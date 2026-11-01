#include "sc4/ProtectionSimulator.h"
#include "gz/StreamOps.h"
#include "sc4/DBSegment.h"

#include <algorithm>
#include <cmath>

#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"

namespace sc4 {

namespace {
inline int RoundEven(float v) { return static_cast<int>(std::nearbyint(v)); }
inline int ClampByte(int v) { return v < 0 ? 0 : (v > 0xff ? 0xff : v); }
}  // namespace

void AddRadiusEffect(std::vector<std::vector<std::int16_t>>& grid, int width, int height, int cx, int cz,
                     float radius, float peak, float edge) {
    const int r = RoundEven(radius);
    const int x0 = std::max(cx - r, 0);
    const int x1 = std::min(cx + r, width - 1);
    const int z0 = std::max(cz - r, 0);
    const int z1 = std::min(cz + r, height - 1);
    const float slope = (peak - edge) / radius;
    for (int x = x0; x <= x1; ++x) {
        for (int z = z0; z <= z1; ++z) {
            const float dx = static_cast<float>(x - cx), dz = static_cast<float>(z - cz);
            const float distance = std::sqrt(dz * dz + dx * dx);
            if (!(distance < radius)) continue;
            int value = RoundEven(peak - distance * slope) + grid[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)];
            value = std::clamp(value, -0x8000, 0x7fff);
            grid[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)] = static_cast<std::int16_t>(value);
        }
    }
}

cSC4ProtectionSimulator::cSC4ProtectionSimulator(IProtectionHost* host, int tractSize, int cellDivisor)
    : host_(host), tractSize_(tractSize), cellDivisor_(cellDivisor) {}

void cSC4ProtectionSimulator::CreateGrids(int citySizeXInCells, int citySizeZInCells) {
    (void)citySizeXInCells;
    (void)citySizeZInCells;   // the grids size themselves from the registered city (SimGridCity)
    if (!adjusted_) {
        adjusted_ = SimGridPtr<std::uint8_t>(new cSC4SimGrid<std::uint8_t>());
        adjusted_->SetInstanceID(kProtectionAdjustedGridInstance);
        adjusted_->SetTractSize(tractSize_);
        adjusted_->SetTractValues(0);
    }
    if (!unadjusted_) {
        unadjusted_ = SimGridPtr<std::int16_t>(new cSC4SimGrid<std::int16_t>());
        unadjusted_->SetInstanceID(0x49b9e605);
        unadjusted_->SetTractSize(tractSize_);
        unadjusted_->SetTractValues(0);
    }
    const int countX = adjusted_->GetTractCountX(), countZ = adjusted_->GetTractCountZ();
    walk_.SetRect(0, 0, countX < 1 ? 0 : countX - 1, countZ < 1 ? 0 : countZ - 1);
    walk_.SetModular(true);
    cellsPerDay = cellDivisor_ != 0 ? (countX * countZ) / cellDivisor_ : 0;
}

bool cSC4ProtectionSimulator::IsProtectionGridCellZoned(int tx, int tz) const {
    const int size = tractSize_;
    for (int x = size * tx; x < size * (tx + 1); ++x)
        for (int z = size * tz; z < size * (tz + 1); ++z)
            if (zoneColumns[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)] != 0) return true;
    return false;
}

// Each tick walks `cellsPerDay` tracts in scrambled order: adjusted = clamp(round(unadjusted * scale)), then
// zoned tracts feed the pass statistics. A finished walk publishes them (average over zoned tracts and the
// share that has any protection) and starts over.
void cSC4ProtectionSimulator::DoDailySimulation() {
    auto& adjusted = adjusted_->GetGridData();
    const auto& unadjusted = unadjusted_->GetGridData();
    for (int n = cellsPerDay; n != 0; --n) {
        int x = 0, z = 0;
        while (!walk_.Next(x, z)) {
            if (zonedCount_ < 1) {
                averageProtection_ = 0.0f;
                coverageShare_ = 0.0f;
            } else {
                const float zoned = static_cast<float>(zonedCount_);
                averageProtection_ = static_cast<float>(protectionSum_) / zoned;
                coverageShare_ = static_cast<float>(protectedCount_) / zoned;
            }
            zonedCount_ = 0;
            protectionSum_ = 0;
            protectedCount_ = 0;
            host_->OnPassFinished();
            walk_.Reset(true);
        }
        const int value = ClampByte(RoundEven(static_cast<float>(unadjusted[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)]) * scale));
        adjusted[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)] = static_cast<std::uint8_t>(value);
        host_->OnAdjustedCell(x, z, value);
        if (IsProtectionGridCellZoned(x, z)) {
            ++zonedCount_;
            protectionSum_ += value;
            if (value != 0) ++protectedCount_;
        }
    }
}

void cSC4ProtectionSimulator::AdjustProtectionEffect(ProtectionEffect& effect, float factor) {
    effect.radius = effect.radius * factor;
    effect.peak = static_cast<int>(static_cast<float>(effect.peak) * factor);
    effect.edge = static_cast<int>(static_cast<float>(effect.edge) * factor);
}

// Rebuilds the unadjusted grid from scratch: one radius effect per station plus one per active dispatch.
void cSC4ProtectionSimulator::UpdateUnadjustedProtectionGrid() {
    const float s = unadjusted_->GetOneOverTractWidthX();
    const int w = unadjusted_->GetTractCountX(), h = unadjusted_->GetTractCountZ();
    unadjusted_->SetTractValues(0);
    auto& grid = unadjusted_->GetGridData();
    for (IProtectionStation* station : stations_) {
        ProtectionEffect e;
        host_->GetStationEffect(station, e);
        AddRadiusEffect(grid, w, h, static_cast<int>(e.x * s), static_cast<int>(e.z * s), e.radius * s,
                        static_cast<float>(e.peak), static_cast<float>(e.edge));
    }
    const std::uint32_t purpose = host_->DispatchPurpose();
    for (std::uint32_t id : host_->ActiveDispatches(purpose)) {
        ProtectionDispatchInfo info;
        if (host_->GetDispatch(id, info) && info.active) {
            ProtectionEffect e;
            host_->GetDispatchEffect(info, e);
            AddRadiusEffect(grid, w, h, static_cast<int>(info.targetX * s), static_cast<int>(info.targetZ * s),
                            e.radius * s, static_cast<float>(e.peak), static_cast<float>(e.edge));
        }
    }
}

void cSC4ProtectionSimulator::CalculateAndWriteAdjustedGridDataByCell(int tx, int tz, int radius) {
    int xEnd = tx + radius, zEnd = tz + radius;
    const int xStart = std::max(tx - radius, 0), zStart = std::max(tz - radius, 0);
    if (adjusted_->GetTractCountX() <= xEnd) xEnd = adjusted_->GetTractCountX() - 1;
    if (adjusted_->GetTractCountZ() <= zEnd) zEnd = adjusted_->GetTractCountZ() - 1;
    const auto& unadjusted = unadjusted_->GetGridData();
    auto& adjusted = adjusted_->GetGridData();
    for (int x = xStart; x <= xEnd; ++x)
        for (int z = zStart; z <= zEnd; ++z)
            adjusted[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)] = static_cast<std::uint8_t>(
                ClampByte(RoundEven(static_cast<float>(unadjusted[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)]) * scale)));
}

void cSC4ProtectionSimulator::UpdateBothGridsByPosition(float x, float z, float radius) {
    int tx = 0, tz = 0;
    adjusted_->PositionToTract(x, z, tx, tz);
    const float tractRadius = adjusted_->GetOneOverTractWidthX() * radius;
    UpdateUnadjustedProtectionGrid();
    CalculateAndWriteAdjustedGridDataByCell(tx, tz, static_cast<int>(tractRadius));
}

void cSC4ProtectionSimulator::DoOccupantInserted(IProtectionStation* station) {
    if (station->OccupantType() != kProtectionBuildingOccupantType) return;
    ProtectionEffect effect;
    if (!host_->GetStationEffect(station, effect)) return;
    stations_.push_back(station);
    std::uint32_t bits = 0;
    host_->GetStationInfo(station, nullptr, nullptr, &bits);
    host_->SetStationDispatchMask(station, (1u << (bits & 31)) - 1);

    // first free run of `bits` dispatch bits above bit 0
    std::vector<bool> used(static_cast<std::size_t>(dispatchBitsInUse) + 64, false);
    for (IProtectionStation* other : stations_) {
        if (other == station) continue;
        std::uint32_t offset = 0, count = 0;
        host_->GetStationInfo(other, nullptr, &offset, &count);
        if (used.size() < static_cast<std::size_t>(offset) + count) used.resize(static_cast<std::size_t>(offset) + count, false);
        for (std::uint32_t i = 0; i < count; ++i) used[offset + i] = true;
    }
    if (used.size() < static_cast<std::size_t>(dispatchBitsInUse) + bits + 64) used.resize(static_cast<std::size_t>(dispatchBitsInUse) + bits + 64, false);
    std::uint32_t offset = 1;
    if (dispatchBitsInUse > 1) {
        std::uint32_t start;
        std::uint32_t i = 0;
        do {
            start = offset;
            offset = 1;
            if (bits == 0) break;
            i = 0;
            while (!used[start + i]) {
                ++i;
                offset = start;
                if (bits <= i) goto found;
            }
            offset = start + 1 + i;
        } while (offset < dispatchBitsInUse);
        offset = start + 1 + i;
    }
found:
    host_->SetStationDispatchOffset(station, offset);
    dispatchBitsInUse = std::max(dispatchBitsInUse, offset + bits);
    host_->RegisterStationProperty(station, true);
    UpdateBothGridsByPosition(effect.x, effect.z, effect.radius);
}

void cSC4ProtectionSimulator::DoOccupantRemoved(IProtectionStation* station) {
    if (station->OccupantType() != kProtectionBuildingOccupantType) return;
    auto it = std::find(stations_.begin(), stations_.end(), station);
    if (it == stations_.end()) return;
    stations_.erase(it);
    host_->RegisterStationProperty(station, false);
    ProtectionEffect effect;
    host_->GetStationEffect(station, effect);
    UpdateBothGridsByPosition(effect.x, effect.z, effect.radius);
}

void cSC4ProtectionSimulator::DoOccupantChanged(IProtectionStation* station) {
    if (station->OccupantType() != kProtectionBuildingOccupantType) return;
    if (std::find(stations_.begin(), stations_.end(), station) == stations_.end()) return;
    ProtectionEffect effect;
    host_->GetStationEffect(station, effect);
    UpdateBothGridsByPosition(effect.x, effect.z, effect.radius);
}

bool cSC4ProtectionSimulator::GetStations(std::list<IProtectionStation*>& out) const {
    out = stations_;
    return !out.empty();
}

int cSC4ProtectionSimulator::GetMaxPossibleDispatchCount() {
    int total = 0;
    for (IProtectionStation* station : stations_) {
        std::uint32_t count = 0;
        host_->GetStationInfo(station, nullptr, nullptr, &count);
        total += static_cast<int>(count);
    }
    return total;
}

bool cSC4ProtectionSimulator::CanDispatch(float x, float z, bool* enough) {
    const int needed = host_->MinimumDispatchesForService();
    if (needed <= 0 || !host_->IsAboveWater(x, z)) return false;
    *enough = static_cast<unsigned>(needed) <= static_cast<unsigned>(host_->AvailableDispatchCount(host_->DispatchPurpose()));
    return true;
}

IProtectionStation* cSC4ProtectionSimulator::DispatchFromNearestStation(float x, float z, std::uint32_t* dispatchId) {
    IProtectionStation* best = nullptr;
    float bestDistance = 3.4028235e+38f;
    for (IProtectionStation* station : stations_) {
        int available = 0;
        host_->GetStationInfo(station, &available, nullptr, nullptr);
        if (available == 0) continue;
        if (!host_->LotAllowsDispatch(station)) continue;
        float position[3];
        station->Position(position);
        const float dx = position[0] - x, dz = position[2] - z;
        const float distance = std::sqrt(dx * dx + dz * dz);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = station;
        }
    }
    *dispatchId = 0;
    if (best) host_->TakeStationDispatch(best);
    return best;
}

// ---- streams ------------------------------------------------------------------------------------------------------

// The record of a city file (the layout ReadCityRecord reads): the grid and station references a loaded file named are
// written back as they were read; the walk iterator follows the first grid.
bool cSC4ProtectionSimulator::WriteCityRecord(cIGZOStream& stream) const {
    stream.SetUint16(3);
    if (fromCityFile_ || !unadjusted_.get()) fileUnadjusted_.Write(stream); else SC4WriteGZSerializable(stream, static_cast<cISC4SimGridBase*>(unadjusted_.get()));   // +0xa0
    stream << walk_;                                                      // +0x40
    if (fromCityFile_ || !adjusted_.get()) fileAdjusted_.Write(stream); else SC4WriteGZSerializable(stream, static_cast<cISC4SimGridBase*>(adjusted_.get()));       // +0xa8
    fileThird_.Write(stream);                                             // +0xb0
    stream.SetUint32(std::uint32_t(fileStations_.size()));                // +0xb8 (the port's stations are not COM objects yet)
    for (const cRetainedReference& station : fileStations_) station.Write(stream);
    stream.SetSint32(zonedCount_);                                        // +0xd0
    stream.SetSint32(protectionSum_);                                     // +0xd4
    stream.SetSint32(protectedCount_);                                    // +0xd8
    stream.SetFloat32(averageProtection_);                                // +0xdc
    stream.SetFloat32(coverageShare_);                                    // +0xe0
    stream.SetUint8(flagE8_ ? 1 : 0);                                     // +0xe8
    stream.SetUint32(valueEC_);                                           // +0xec
    stream.SetUint32(dispatchBitsInUse);                                  // +0xf0
    return !stream.GetError();
}

bool cSC4ProtectionSimulator::Write(cIGZOStream& stream) const {
    void* dbStream = nullptr;
    if (stream.QueryInterface(kGZIID_cISC4DBSegmentOStream, &dbStream) && dbStream) {
        static_cast<cIGZUnknown*>(dbStream)->Release();
        return WriteCityRecord(stream);
    }
    stream.SetUint16(3);
    host_->WriteGridInt16(stream, unadjusted_.get());                     // +0xa0
    // +0x40 RandomIterator walk state is not streamed here: the operator lives in SimStreamOps.cpp, which is
    // not linked everywhere the base is (the walk is rebuilt by CreateGrids anyway).
    host_->WriteGridUint8(stream, adjusted_.get());                       // +0xa8
    host_->WriteGridUint8(stream, nullptr);                               // +0xb0 third grid (unrecovered)
    stream.SetUint32(std::uint32_t(stations_.size()));                    // +0xb8
    for (IProtectionStation* station : stations_) host_->WriteStation(stream, station);
    stream.SetSint32(zonedCount_);                                        // +0xd0
    stream.SetSint32(protectionSum_);                                     // +0xd4
    stream.SetSint32(protectedCount_);                                    // +0xd8
    stream.SetFloat32(averageProtection_);                                // +0xdc
    stream.SetFloat32(coverageShare_);                                    // +0xe0
    stream.SetUint8(flagE8_ ? 1 : 0);                                     // +0xe8
    stream.SetUint32(valueEC_);                                           // +0xec
    stream.SetUint32(dispatchBitsInUse);                                  // +0xf0
    return !stream.GetError();
}

// 0x1000aa362: the record of a city file. The grids and the station occupants are references (consumed here: the grids belong to the
// unrecovered coverage model, the stations are resolved by the host elsewhere); the walk iterator and the counters are kept. A stream that is
// not a city-file segment stream (the unit tests) carries the port's own reduced format, read below.
bool cSC4ProtectionSimulator::ReadCityRecord(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version > 3) return false;
    const auto skipReference = [&stream](std::uint32_t iid) {
        void* raw = nullptr;
        SC4ReadGZSerializable(stream, iid, &raw, false);
        if (raw) static_cast<cIGZUnknown*>(raw)->Release();
    };
    (void)skipReference;
    fromCityFile_ = true;
    fileUnadjusted_.Read(stream);                                         // +0xa0
    stream >> walk_;                                                      // +0x40
    fileAdjusted_.Read(stream);                                           // +0xa8
    fileThird_.Read(stream);                                              // +0xb0
    std::uint32_t stationCount = 0;
    stream.GetUint32(stationCount);                                       // +0xb8: the occupants of the stations
    fileStations_.clear();
    if (stationCount < 10000000u)
        for (std::uint32_t i = 0; i < stationCount && !stream.GetError(); ++i) { fileStations_.emplace_back(); fileStations_.back().Read(stream); }
    stream.GetSint32(zonedCount_);                                        // +0xd0
    stream.GetSint32(protectionSum_);                                     // +0xd4
    stream.GetSint32(protectedCount_);                                    // +0xd8
    stream.GetFloat32(averageProtection_);                                // +0xdc
    if (version < 3) {
        float share = 0.0f;
        stream.GetFloat32(share);
        coverageShare_ = share * 0.01f;
    } else {
        stream.GetFloat32(coverageShare_);                                // +0xe0
    }
    std::uint8_t flag = 0;
    stream.GetUint8(flag); flagE8_ = flag != 0;                           // +0xe8
    stream.GetUint32(valueEC_);                                           // +0xec
    stream.GetUint32(dispatchBitsInUse);                                  // +0xf0
    return !stream.GetError();
}

bool cSC4ProtectionSimulator::Read(cIGZIStream& stream) {
    void* dbStream = nullptr;
    if (stream.QueryInterface(kGZIID_cISC4DBSegmentIStream, &dbStream) && dbStream) {
        static_cast<cIGZUnknown*>(dbStream)->Release();
        return ReadCityRecord(stream);
    }
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version > 3) return false;
    cSC4SimGrid<std::int16_t>* unadjusted = nullptr;
    host_->ReadGridInt16(stream, &unadjusted);
    unadjusted_.reset(unadjusted);                                        // +0xa0
    // +0x40 RandomIterator walk state skipped (see Write).
    cSC4SimGrid<std::uint8_t>* adjusted = nullptr;
    host_->ReadGridUint8(stream, &adjusted);
    adjusted_.reset(adjusted);                                            // +0xa8
    cSC4SimGrid<std::uint8_t>* third = nullptr;
    host_->ReadGridUint8(stream, &third);                                 // +0xb0 (unrecovered)
    if (third) third->Release();
    std::uint32_t stationCount = 0;
    stream.GetUint32(stationCount);
    stations_.clear();
    for (std::uint32_t i = 0; i < stationCount; ++i) {
        IProtectionStation* station = nullptr;
        host_->ReadStation(stream, &station);
        if (station) stations_.push_back(station);
    }
    stream.GetSint32(zonedCount_);                                        // +0xd0
    stream.GetSint32(protectionSum_);                                     // +0xd4
    stream.GetSint32(protectedCount_);                                    // +0xd8
    stream.GetFloat32(averageProtection_);                                // +0xdc
    if (version < 3) {
        float share = 0.0f;
        stream.GetFloat32(share);
        coverageShare_ = share * 0.01f;
    } else {
        stream.GetFloat32(coverageShare_);                                // +0xe0
    }
    std::uint8_t flag = 0;
    stream.GetUint8(flag); flagE8_ = flag != 0;                           // +0xe8
    stream.GetUint32(valueEC_);                                           // +0xec
    stream.GetUint32(dispatchBitsInUse);                                  // +0xf0
    return !stream.GetError();
}

}  // namespace sc4
