#include "sc4/PollutionSimulator.h"
#include "sc4/SimStreamOps.h"

#include <algorithm>
#include <cmath>

#include "rz/cRZString.h"
#include "sc4/DBSegment.h"
#include "sc4/ProtectionSimulator.h"

namespace sc4 {

namespace {
// Messages posted at the end of a cycle when the matching grid changed.
constexpr std::uint32_t kMessageRadiationChanged = 0xda6dd35d;
constexpr std::uint32_t kMessageAirChanged = 0xfa6dd0bb;
constexpr std::uint32_t kMessageWaterChanged = 0x5a6dd1aa;
constexpr std::uint32_t kMessageGarbageChanged = 0xda6dd2cb;
constexpr std::uint32_t kMessageSanitationEfficiency = 0xea418d8f;
// history records
constexpr std::uint32_t kHistoryAir = 0x0a1100e8;
constexpr std::uint32_t kHistoryWater = 0xea110130;
constexpr std::uint32_t kHistoryGarbage = 0xea110134;
constexpr std::uint32_t kHistoryRadiation = 0xea110137;
constexpr std::uint32_t kHistoryTotal = 0xea110138;

inline std::int32_t RoundToEven(float v) { return static_cast<std::int32_t>(std::nearbyint(v)); }
}  // namespace

cSC4PollutionSimulator::cSC4PollutionSimulator(IPollutionHost* host) : host_(host) {}

// 0x100094c90: the original answers +0 (iid 1 / 0x877181ec), +8 (message target), +0x10 (0x89c498be)
// and +0x18 (serializable). The port keeps a single cIGZSerializable base, so every accepted id resolves
// to `this`; the serializable id is what the city-load serializer (SC4ReadGZSerializable) asks for.
bool cSC4PollutionSimulator::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 0xe4fda3d4) *out = static_cast<cIGZSerializable*>(this);
    else if (iid == 1 || iid == 0x877181ec || iid == kGZCLSID_cSC4PollutionSimulator)
        *out = static_cast<cIGZUnknown*>(this);
    else return false;
    AddRef();
    return true;
}

// 0x100094d48: below 2 destroys the object (the original runs FinalRelease, which Shutdowns and deletes).
std::uint32_t cSC4PollutionSimulator::Release() {
    if (refCount_ < 2) {
        delete this;
        return 0;
    }
    return --refCount_;
}

// 0x1000961d0. PARTIAL: the original writes version 4 then, in order, the four value grids (+0x78),
// the four source grids (+0x98), two s32 (+0xb8/+0xbc), the four occupant ilists (+0xc0/+0xd8/+0xf0/+0x108),
// then the scalars the port keeps, then two u64 (+0x2d0/+0x2d8), two u64 (+0x250/+0x258), one double
// (+0x268), one s32 (+0x2e4), a RandomIterator (+0x2e8) and the prop-request curve (+0x1c0). The occupant
// lists, the two +0xb8/+0xbc counters, the four un-identified u64/double/s32 slots and the RandomIterator
// belong to the unrecovered daily-diffusion state, so they are not serialised here — the field order of
// the scalars below matches the binary.
bool cSC4PollutionSimulator::Write(cIGZOStream& stream) const {
    void* dbStream = nullptr;
    if (stream.QueryInterface(kGZIID_cISC4DBSegmentOStream, &dbStream) && dbStream) {
        static_cast<cIGZUnknown*>(dbStream)->Release();
        return WriteCityRecord(stream);
    }
    stream.SetUint16(4);
    for (auto& grid : valueGrids_) SC4WriteGZSerializable(stream, static_cast<cISC4SimGridBase*>(grid.get()));
    for (auto& grid : sourceGrids_) SC4WriteGZSerializable(stream, static_cast<cISC4SimGridBase*>(grid.get()));
    stream.SetUint32(recyclingCenterCountSnapshot);
    stream.SetSint32(averageAir_);
    stream.SetSint32(averageWater_);
    stream.SetUint32(airSum);
    stream.SetUint32(waterSum);
    stream.SetUint32(periodValue21c[0]);
    stream.SetUint32(periodValue21c[1]);
    stream.SetUint32(periodValue224[0]);
    stream.SetUint32(periodValue224[1]);
    stream.SetUint32(cellCount);
    stream.SetUint16(static_cast<std::uint16_t>(averageGarbage_));
    stream.SetUint32(monthlyIncineratorCapacity_);
    stream.SetUint32(monthlyWasteToEnergyCapacity_);
    stream.SetFloat32(recyclingEffect_);
    stream.SetUint8(garbageUncollected_ ? 1 : 0);
    stream.SetUint32(totalGarbageUncollected_);
    stream.SetSint32(abductedPrev_);
    stream.SetSint64(producedPrev_);
    stream.SetSint64(importedPrev_);
    stream.SetSint64(exportedPrev_);
    stream.SetFloat64(recycledPrev_);
    stream.SetSint64(incineratedPrev_);
    stream.SetSint64(convertedPrev_);
    stream.SetSint64(landfilledPrev_);
    stream.SetSint64(recycledThisMonth_);
    stream.SetSint64(wasteToEnergyThisMonth_);
    stream.SetSint64(incineratedThisMonth_);
    stream.SetSint64(landfilledThisMonth_);
    stream.SetUint32(abductedThisMonth_);
    stream.SetUint32(static_cast<std::uint32_t>(recyclingAllowance_));
    stream.SetSint32(activeLandfillCount_);
    stream.SetUint8(dirtyRadiation ? 1 : 0);
    stream.SetUint8(dirtyAir ? 1 : 0);
    stream.SetUint8(dirtyWater ? 1 : 0);
    stream.SetUint8(dirtyGarbage ? 1 : 0);
    stream.SetFloat32(efficiency_);
    stream.SetUint32(static_cast<std::uint32_t>(garbageSupply_));
    stream.SetUint32(static_cast<std::uint32_t>(garbageCapacity_));
    return !stream.GetError();
}

// 0x100096588. PARTIAL: mirrors Write; the version gate and the occupant-list / RandomIterator / curve
// reads belong to the unrecovered state and are left out (see Write's field list).
// 0x100096588: the record of a city file. The eight grids are kept; the four building lists are object references (consumed: the port
// finds the buildings through the occupant manager); the counters, totals and flags are kept under their original offsets. A stream that is
// not a city-file segment stream (the unit tests) carries the port's own reduced record, read below.
bool cSC4PollutionSimulator::ReadCityRecord(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version > 4) return false;
    for (auto& grid : valueGrids_) {
        void* raw = nullptr;
        if (SC4ReadGZSerializable(stream, kGZIID_cISC4SimGrid, &raw, false)) grid.reset(SimGridCast<std::int16_t>(raw));
    }
    for (auto& grid : sourceGrids_) {
        void* raw = nullptr;
        if (SC4ReadGZSerializable(stream, kGZIID_cISC4SimGrid, &raw, false)) grid.reset(SimGridCast<std::int16_t>(raw));
    }
    std::uint32_t u32 = 0;
    std::int64_t s64 = 0;
    double f64 = 0.0;
    std::uint8_t u8 = 0;
    fromCityFile_ = true;
    stream.GetUint32(fileB8_);                                // +0xb8
    stream.GetUint32(fileBC_);                                // +0xbc
    for (auto& kept : fileLists_) {                           // +0xc0 / +0xd8 / +0xf0 / +0x108: lists of occupants (kept as read)
        kept.clear();
        stream.GetUint32(u32);
        if (u32 < 10000000u)
            for (std::uint32_t i = 0; i < u32 && !stream.GetError(); ++i) { kept.emplace_back(); kept.back().Read(stream); }
    }
    stream.GetUint32(recyclingCenterCountSnapshot);           // +0x208
    stream.GetSint32(averageAir_);                            // +0x20c
    stream.GetSint32(averageWater_);                          // +0x210
    stream.GetUint32(airSum);                                 // +0x214
    stream.GetUint32(waterSum);                               // +0x218
    stream.GetUint32(periodValue21c[0]);                      // +0x21c
    stream.GetUint32(periodValue21c[1]);                      // +0x220
    stream.GetUint32(periodValue224[0]);                      // +0x224
    stream.GetUint32(periodValue224[1]);                      // +0x228
    stream.GetUint32(cellCount);                              // +0x22c
    if (version < 3) {                                        // versions 1-2 (the PC saves) carried a string here, dropped on read
        cRZString dropped;
        stream.GetGZStr(dropped);
    }
    std::uint16_t avgGarbage = 0;
    stream.GetUint16(avgGarbage); averageGarbage_ = static_cast<std::int16_t>(avgGarbage);   // +0x234
    stream.GetUint32(monthlyIncineratorCapacity_);            // +0x238
    stream.GetUint32(monthlyWasteToEnergyCapacity_);          // +0x23c
    stream.GetFloat32(recyclingEffect_);                      // +0x240
    stream.GetUint8(u8); garbageUncollected_ = u8 != 0;       // +0x244
    stream.GetUint32(totalGarbageUncollected_);               // +0x248
    stream.GetSint64(file2d0_);                               // +0x2d0
    stream.GetSint64(file2d8_);                               // +0x2d8
    if (version > 3) { stream.GetUint32(u32); abductedPrev_ = static_cast<std::int32_t>(u32); }   // +0x2c8
    stream.GetSint64(producedPrev_);                          // +0x288
    stream.GetSint64(importedPrev_);                          // +0x290
    stream.GetSint64(exportedPrev_);                          // +0x298
    stream.GetFloat64(recycledPrev_);                         // +0x2a0
    stream.GetSint64(incineratedPrev_);                       // +0x2a8
    stream.GetSint64(convertedPrev_);                         // +0x2b0
    stream.GetSint64(landfilledPrev_);                        // +0x2b8
    stream.GetSint64(file250_);                               // +0x250
    stream.GetSint64(file258_);                               // +0x258
    stream.GetSint64(recycledThisMonth_);                     // +0x260
    stream.GetFloat64(recyclingShare268_);                    // +0x268
    stream.GetSint64(wasteToEnergyThisMonth_);                // +0x270
    stream.GetSint64(incineratedThisMonth_);                  // +0x278
    stream.GetSint64(landfilledThisMonth_);                   // +0x280
    stream.GetUint32(abductedThisMonth_);                     // +0x2c4
    std::int32_t allowance = 0;
    stream.GetSint32(allowance); recyclingAllowance_ = allowance;   // +0x2c0
    std::int32_t landfills = 0;
    stream.GetUint32(u32); landfills = static_cast<std::int32_t>(u32); activeLandfillCount_ = landfills;   // +0x2cc
    stream.GetUint32(file2e4_);                               // +0x2e4
    stream >> fileWalk_;                                      // +0x2e8
    stream.GetUint8(u8); dirtyRadiation = u8 != 0;            // +0x328..
    stream.GetUint8(u8); dirtyAir = u8 != 0;
    stream.GetUint8(u8); dirtyWater = u8 != 0;
    stream.GetUint8(u8); dirtyGarbage = u8 != 0;
    if (version < 3) stream.GetUint32(u32);                   // versions 1-2: one more word, dropped on read
    stream.GetFloat32(efficiency_);                           // +0x348
    std::int32_t supply = 0, capacity = 0;
    stream.GetSint32(supply); garbageSupply_ = supply;        // +0x34c
    stream.GetSint32(capacity); garbageCapacity_ = capacity;  // +0x350
    fileWaterGrid_ = cRetainedReference();
    fileWaterGrid_.Read(stream);                              // +0x1c0: the water grid
    (void)s64;
    return !stream.GetError();
}

// The city-file record, as ReadCityRecord reads it (version 4); what the port does not model is written as the file had it.
bool cSC4PollutionSimulator::WriteCityRecord(cIGZOStream& stream) const {
    stream.SetUint16(4);
    for (auto& grid : valueGrids_) SC4WriteGZSerializable(stream, static_cast<cISC4SimGridBase*>(grid.get()));
    for (auto& grid : sourceGrids_) SC4WriteGZSerializable(stream, static_cast<cISC4SimGridBase*>(grid.get()));
    stream.SetUint32(fileB8_);                                // +0xb8
    stream.SetUint32(fileBC_);                                // +0xbc
    for (const auto& kept : fileLists_) {                     // +0xc0 / +0xd8 / +0xf0 / +0x108
        stream.SetUint32(std::uint32_t(kept.size()));
        for (const cRetainedReference& occupant : kept) occupant.Write(stream);
    }
    stream.SetUint32(recyclingCenterCountSnapshot);           // +0x208
    stream.SetSint32(averageAir_);                            // +0x20c
    stream.SetSint32(averageWater_);                          // +0x210
    stream.SetUint32(airSum);                                 // +0x214
    stream.SetUint32(waterSum);                               // +0x218
    stream.SetUint32(periodValue21c[0]);                      // +0x21c
    stream.SetUint32(periodValue21c[1]);                      // +0x220
    stream.SetUint32(periodValue224[0]);                      // +0x224
    stream.SetUint32(periodValue224[1]);                      // +0x228
    stream.SetUint32(cellCount);                              // +0x22c
    stream.SetUint16(static_cast<std::uint16_t>(averageGarbage_));   // +0x234
    stream.SetUint32(monthlyIncineratorCapacity_);            // +0x238
    stream.SetUint32(monthlyWasteToEnergyCapacity_);          // +0x23c
    stream.SetFloat32(recyclingEffect_);                      // +0x240
    stream.SetUint8(garbageUncollected_ ? 1 : 0);             // +0x244
    stream.SetUint32(totalGarbageUncollected_);               // +0x248
    stream.SetSint64(file2d0_);                               // +0x2d0
    stream.SetSint64(file2d8_);                               // +0x2d8
    stream.SetUint32(static_cast<std::uint32_t>(abductedPrev_));   // +0x2c8
    stream.SetSint64(producedPrev_);                          // +0x288
    stream.SetSint64(importedPrev_);                          // +0x290
    stream.SetSint64(exportedPrev_);                          // +0x298
    stream.SetFloat64(recycledPrev_);                         // +0x2a0
    stream.SetSint64(incineratedPrev_);                       // +0x2a8
    stream.SetSint64(convertedPrev_);                         // +0x2b0
    stream.SetSint64(landfilledPrev_);                        // +0x2b8
    stream.SetSint64(file250_);                               // +0x250
    stream.SetSint64(file258_);                               // +0x258
    stream.SetSint64(recycledThisMonth_);                     // +0x260
    stream.SetFloat64(recyclingShare268_);                    // +0x268
    stream.SetSint64(wasteToEnergyThisMonth_);                // +0x270
    stream.SetSint64(incineratedThisMonth_);                  // +0x278
    stream.SetSint64(landfilledThisMonth_);                   // +0x280
    stream.SetUint32(abductedThisMonth_);                     // +0x2c4
    stream.SetSint32(static_cast<std::int32_t>(recyclingAllowance_));   // +0x2c0
    stream.SetUint32(static_cast<std::uint32_t>(activeLandfillCount_)); // +0x2cc
    stream.SetUint32(file2e4_);                               // +0x2e4
    stream << fileWalk_;                                      // +0x2e8
    stream.SetUint8(dirtyRadiation ? 1 : 0);                  // +0x328..
    stream.SetUint8(dirtyAir ? 1 : 0);
    stream.SetUint8(dirtyWater ? 1 : 0);
    stream.SetUint8(dirtyGarbage ? 1 : 0);
    stream.SetFloat32(efficiency_);                           // +0x348
    stream.SetSint32(static_cast<std::int32_t>(garbageSupply_));     // +0x34c
    stream.SetSint32(static_cast<std::int32_t>(garbageCapacity_));   // +0x350
    fileWaterGrid_.Write(stream);                             // +0x1c0
    return !stream.GetError();
}

bool cSC4PollutionSimulator::Read(cIGZIStream& stream) {
    void* dbStream = nullptr;
    if (stream.QueryInterface(kGZIID_cISC4DBSegmentIStream, &dbStream) && dbStream) {
        static_cast<cIGZUnknown*>(dbStream)->Release();
        return ReadCityRecord(stream);
    }
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version > 4) return false;
    for (auto& grid : valueGrids_) {
        void* raw = nullptr;
        if (SC4ReadGZSerializable(stream, kGZIID_cISC4SimGrid, &raw, false)) {
            grid.reset(SimGridCast<std::int16_t>(raw));   // keeps the reference the read took
        }
    }
    for (auto& grid : sourceGrids_) {
        void* raw = nullptr;
        if (SC4ReadGZSerializable(stream, kGZIID_cISC4SimGrid, &raw, false)) {
            grid.reset(SimGridCast<std::int16_t>(raw));   // keeps the reference the read took
        }
    }
    stream.GetUint32(recyclingCenterCountSnapshot);
    stream.GetSint32(averageAir_);
    stream.GetSint32(averageWater_);
    stream.GetUint32(airSum);
    stream.GetUint32(waterSum);
    stream.GetUint32(periodValue21c[0]);
    stream.GetUint32(periodValue21c[1]);
    stream.GetUint32(periodValue224[0]);
    stream.GetUint32(periodValue224[1]);
    stream.GetUint32(cellCount);
    std::uint16_t avgGarbage = 0;
    stream.GetUint16(avgGarbage);
    averageGarbage_ = static_cast<std::int16_t>(avgGarbage);
    stream.GetUint32(monthlyIncineratorCapacity_);
    stream.GetUint32(monthlyWasteToEnergyCapacity_);
    stream.GetFloat32(recyclingEffect_);
    std::uint8_t uncollected = 0;
    stream.GetUint8(uncollected);
    garbageUncollected_ = uncollected != 0;
    stream.GetUint32(totalGarbageUncollected_);
    stream.GetSint32(abductedPrev_);
    stream.GetSint64(producedPrev_);
    stream.GetSint64(importedPrev_);
    stream.GetSint64(exportedPrev_);
    stream.GetFloat64(recycledPrev_);
    stream.GetSint64(incineratedPrev_);
    stream.GetSint64(convertedPrev_);
    stream.GetSint64(landfilledPrev_);
    stream.GetSint64(recycledThisMonth_);
    stream.GetSint64(wasteToEnergyThisMonth_);
    stream.GetSint64(incineratedThisMonth_);
    stream.GetSint64(landfilledThisMonth_);
    stream.GetUint32(abductedThisMonth_);
    std::uint32_t recyclingAllowance = 0;
    stream.GetUint32(recyclingAllowance);
    recyclingAllowance_ = static_cast<std::int32_t>(recyclingAllowance);
    stream.GetSint32(activeLandfillCount_);
    std::uint8_t b = 0;
    stream.GetUint8(b); dirtyRadiation = b != 0;
    stream.GetUint8(b); dirtyAir = b != 0;
    stream.GetUint8(b); dirtyWater = b != 0;
    stream.GetUint8(b); dirtyGarbage = b != 0;
    stream.GetFloat32(efficiency_);
    std::uint32_t supply = 0, capacity = 0;
    stream.GetUint32(supply); garbageSupply_ = static_cast<int>(supply);
    stream.GetUint32(capacity); garbageCapacity_ = static_cast<int>(capacity);
    return !stream.GetError();
}

void cSC4PollutionSimulator::ReadSimulatorTunableValues(const cISCPropertyHolder* exemplar) {
    tunables_ = PollutionTunables();
    // default response curves: the prop request / effect curves start as the single point (0, 0)
    const std::vector<SC4Point> zero{{0.0f, 0.0f}};
    propRequestCurve_.SetResponseCurveData(zero);
    garbageTransportCapacity_.SetResponseCurveData(zero);
    const std::vector<SC4Point> identity{{0.0f, 1.0f}, {1.0f, 1.0f}};
    ageCurve_.SetResponseCurveData(identity);
    efficiencyCurve_.SetResponseCurveData(identity);
    if (!exemplar) return;
    PollutionTunables& t = tunables_;
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropTrafficAirFactor, t.trafficAirFactor);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropMaxGarbageForUI, t.maxGarbageForUI);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropMaxAirForUI, t.maxAirForUI);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropMaxWaterForUI, t.maxWaterForUI);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropMaxRadiationForUI, t.maxRadiationForUI);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropMaxAirDelta, t.maxAirDelta);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropMaxWaterDelta, t.maxWaterDelta);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropGarbageThreshold, t.garbageThreshold);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropAirThreshold, t.airThreshold);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropWaterThreshold, t.waterThreshold);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropRadiationThreshold, t.radiationThreshold);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropAbductionAmount, t.abductionAmount);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropGarbageScaling, t.garbageScalingFactor);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropFloat2e0, t.value2e0);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropMaxGarbageOnValve, t.maxGarbageOnValve);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropLandfillTileCapacity, t.landfillTileCapacity);
    auto loadCurve = [&](cSC4ResponseCurve& curve, std::uint32_t id) {
        std::vector<float> values;
        if (exemplar->HasProperty(id) && cSCPropertyHelp::GetPropertyValue(exemplar, id, values))
            curve.SetResponseCurveData(values.data(), values.size());
    };
    loadCurve(propRequestCurve_, 0x69a437f6);
    loadCurve(garbageTransportCapacity_, 0x8c19c3e9);
    loadCurve(ageCurve_, 0x0911e2e8);
    loadCurve(efficiencyCurve_, 0x0911e2e9);
    loadCurve(fundingEfficiencyCurve_, kPollutionPropFundingEfficiencyCurve);
}

void cSC4PollutionSimulator::CreateGrids() {
    for (int i = 0; i < 4; ++i) {
        if (!valueGrids_[i]) {
            valueGrids_[i] = SimGridPtr<std::int16_t>(new cSC4SimGrid<std::int16_t>());
            valueGrids_[i]->SetTractSize(2);
            valueGrids_[i]->SetInstanceID(kPollutionGridInstanceValueBase + static_cast<std::uint32_t>(i));
        }
        if (!sourceGrids_[i]) {
            sourceGrids_[i] = SimGridPtr<std::int16_t>(new cSC4SimGrid<std::int16_t>());
            sourceGrids_[i]->SetTractSize(i == 2 ? 1 : 2);
            sourceGrids_[i]->SetInstanceID(kPollutionGridInstanceSourceBase + static_cast<std::uint32_t>(i));
        }
    }
}

cSC4SimGrid<std::int16_t>* cSC4PollutionSimulator::GetPollutionGrid(PollutionType type) const {
    return type == kPollutionRadiation ? valueGrids_[3].get() : sourceGrids_[type].get();
}

std::int16_t cSC4PollutionSimulator::GetPollutionValue(PollutionType type, int x, int z) const {
    const std::int16_t v = valueGrids_[type]->GetCellValue(x, z);
    return v < 0 ? 0 : v;
}

bool cSC4PollutionSimulator::IsRadioactive(unsigned x, unsigned z) const {
    return valueGrids_[3]->GetCellValue(static_cast<int>(x), static_cast<int>(z)) != 0;
}

void cSC4PollutionSimulator::GetAirValue(unsigned x, unsigned z, int& out) const {
    out = sourceGrids_[0]->GetCellValue(static_cast<int>(x), static_cast<int>(z));
}

void cSC4PollutionSimulator::GetWaterValue(unsigned x, unsigned z, int& out) const {
    out = sourceGrids_[1]->GetCellValue(static_cast<int>(x), static_cast<int>(z));
}

// The garbage grid keeps the "active landfill" flag in bit 15; the value is read through the sign-extended short
// with that bit cleared, which leaves negative results for flagged cells (original quirk).
void cSC4PollutionSimulator::GetGarbageValue(unsigned x, unsigned z, int& out) const {
    const int v = sourceGrids_[2]->GetCellValue(static_cast<int>(x), static_cast<int>(z));
    out = v & static_cast<int>(0xffff7fff);
}

bool cSC4PollutionSimulator::IsAirPolluted(unsigned x, unsigned z) const {
    int value = 0;
    GetAirValue(x, z, value);
    return tunables_.airThreshold <= static_cast<std::uint32_t>(value);
}

bool cSC4PollutionSimulator::IsWaterPolluted(unsigned x, unsigned z) const {
    int value = 0;
    GetWaterValue(x, z, value);
    return tunables_.waterThreshold <= static_cast<std::uint32_t>(value);
}

bool cSC4PollutionSimulator::IsGarbageAccumulated(unsigned x, unsigned z) const {
    int value = 0;
    GetGarbageValue(x, z, value);
    return tunables_.garbageThreshold <= static_cast<std::uint32_t>(value);
}

void cSC4PollutionSimulator::SetIsRadioactive(unsigned x, unsigned z, bool on) {
    auto& grid = *valueGrids_[3];
    const int shift = grid.GetTractShift();
    grid.SetTractValue(static_cast<int>(x >> shift), static_cast<int>(z >> shift), on ? 1 : 0);
    dirtyRadiation = true;
}

void cSC4PollutionSimulator::SetAirValue(unsigned x, unsigned z, int value) {
    auto& grid = *sourceGrids_[0];
    const int shift = grid.GetTractShift();
    grid.SetTractValue(static_cast<int>(x >> shift), static_cast<int>(z >> shift), static_cast<std::int16_t>(value));
    dirtyAir = true;
}

void cSC4PollutionSimulator::SetWaterValue(unsigned x, unsigned z, int value) {
    auto& grid = *sourceGrids_[1];
    const int shift = grid.GetTractShift();
    grid.SetTractValue(static_cast<int>(x >> shift), static_cast<int>(z >> shift), static_cast<std::int16_t>(value));
    dirtyWater = true;
}

void cSC4PollutionSimulator::SetGarbageValue(unsigned x, unsigned z, int value) {
    auto& grid = *sourceGrids_[2];
    const int shift = grid.GetTractShift();
    const std::uint16_t flag = static_cast<std::uint16_t>(grid.GetCellValue(static_cast<int>(x), static_cast<int>(z))) & 0x8000;
    grid.SetTractValue(static_cast<int>(x >> shift), static_cast<int>(z >> shift),
                       static_cast<std::int16_t>(flag | static_cast<std::uint16_t>(value)));
    dirtyGarbage = true;
}

bool cSC4PollutionSimulator::IsActiveLandfill(unsigned x, unsigned z) const {
    return (static_cast<std::uint16_t>(sourceGrids_[2]->GetCellValue(static_cast<int>(x), static_cast<int>(z))) &
            0x8000) != 0;
}

// Turning a landfill on needs the cell to be zoned landfill (type 0xe); turning it off always works. The write
// goes straight to the tract (x, z) without shifting the cell coordinates, as in the original.
void cSC4PollutionSimulator::SetIsActiveLandfill(unsigned x, unsigned z, bool on, std::uint32_t zoneTypeAtCell) {
    if (zoneTypeAtCell != 0xe && on) return;
    auto& grid = *sourceGrids_[2];
    const bool active = IsActiveLandfill(x, z);
    std::int16_t value;
    if (!active && on) {
        value = static_cast<std::int16_t>(grid.GetCellValue(static_cast<int>(x), static_cast<int>(z)) | 0x8000);
        ++activeLandfillCount_;
    } else {
        if (!active || on) return;
        const std::int16_t current = grid.GetCellValue(static_cast<int>(x), static_cast<int>(z));
        if (activeLandfillCount_ != 0) --activeLandfillCount_;
        value = static_cast<std::int16_t>(current & 0x7fff);
    }
    grid.SetTractValue(static_cast<int>(x), static_cast<int>(z), value);
    dirtyGarbage = true;
}

std::int32_t cSC4PollutionSimulator::GetBuildingGarbageCapacity(cISCPropertyHolder* occupant) const {
    std::int32_t value = 0;
    if (!cSCPropertyHelp::GetPropertyValue(occupant, kPollutionOccGarbageCapacity, value)) return 0;
    return value;
}

void cSC4PollutionSimulator::AddToGarbageSupply(int amount) {
    if (amount < 0 && garbageSupply_ + amount < 0) amount = -garbageSupply_;
    garbageSupply_ = std::max(0, garbageSupply_ + amount);
}

int cSC4PollutionSimulator::GetGarbageSupply() {
    if (garbageSupply_ < 0) garbageSupply_ = 0;
    return garbageSupply_;
}

// Average garbage value over the rectangle through the prop-request curve, rounded to the nearest int.
int cSC4PollutionSimulator::GetRequestedShare(int x0, int z0, int x1, int z1) const {
    std::uint32_t total = 0;
    for (int x = x0; x <= x1; ++x) {
        for (int z = z0; z <= z1; ++z) {
            int value = 0;
            GetGarbageValue(static_cast<unsigned>(x), static_cast<unsigned>(z), value);
            total += static_cast<std::uint32_t>(value);
        }
    }
    const std::uint32_t area = static_cast<std::uint32_t>(((z1 + 1) - z0) * ((x1 + 1) - x0));
    const float average = static_cast<float>(static_cast<std::uint64_t>(total) / area) / 32767.0f;
    return RoundToEven(propRequestCurve_.GetOutputValue(average));
}

bool cSC4PollutionSimulator::AddOrRemoveOccupant(std::list<IPollutionOccupant*>& list, IPollutionOccupant* occupant,
                                                 bool add) {
    if (add) {
        list.push_back(occupant);
        return true;
    }
    auto it = std::find(list.begin(), list.end(), occupant);
    if (it == list.end()) return false;
    list.erase(it);
    return true;
}

// ------------------------------------------------------------------------------------- occupant helpers

void cSC4PollutionSimulator::GetRecyclingCenterMaxCapacity(IPollutionOccupant* occupant, float& percentage) const {
    percentage = 0.0f;
    SC4Percentage p;
    if (cSCPropertyHelp::GetPropertyValue(occupant->PropertyHolder(), kPollutionOccRecyclingMax, p)) percentage = p.value;
}

void cSC4PollutionSimulator::GetRecyclingCenterCurrentCapacity(IPollutionOccupant* occupant, float& percentage) const {
    std::uint32_t value = 0;
    if (GetCurrentCapacity(kPollutionOccRecyclingMax, occupant, value)) percentage = static_cast<float>(value) * 0.01f;
    else percentage = 0.0f;
}

void cSC4PollutionSimulator::GetMaxCapacity(std::uint32_t property, IPollutionOccupant* occupant,
                                            std::uint32_t& out) const {
    out = 0;
    cSCPropertyHelp::GetPropertyValue(occupant->PropertyHolder(), property, out);
}

// capacity * efficiency (zero when the occupant has no power), then scaled by the age curve of the occupant
bool cSC4PollutionSimulator::GetCurrentCapacity(std::uint32_t property, IPollutionOccupant* occupant,
                                                std::uint32_t& out) const {
    cISCPropertyHolder* holder = occupant->PropertyHolder();
    if (!cSCPropertyHelp::GetPropertyValue(holder, property, out)) {
        out = 0;
        return false;
    }
    if (!host_->OccupantPowered(occupant)) out = 0;
    out = static_cast<std::uint32_t>(static_cast<std::int64_t>(static_cast<float>(out) * efficiency_));
    if (holder->HasProperty(kPollutionOccAgeCurve)) {
        cSC4ResponseCurve curve;
        std::vector<float> values;
        if (cSCPropertyHelp::GetPropertyValue(holder, kPollutionOccAgeCurve, values))
            curve.SetResponseCurveData(values.data(), values.size());
        float age = 0.0f;
        cSCPropertyHelp::GetPropertyValue(holder, kPollutionOccAge, age);
        out = static_cast<std::uint32_t>(RoundToEven(curve.GetOutputValue(age) * static_cast<float>(out)));
    }
    return true;
}

void cSC4PollutionSimulator::GetIncineratorCurrentCapacity(IPollutionOccupant* occupant, std::uint32_t& out) const {
    out = 0;
    GetCurrentCapacity(kPollutionOccIncinerator, occupant, out);
}

void cSC4PollutionSimulator::GetWasteToEnergyCurrentCapacity(IPollutionOccupant* occupant, std::uint32_t& out) const {
    out = 0;
    GetCurrentCapacity(kPollutionOccWasteToEnergy, occupant, out);
}

void cSC4PollutionSimulator::GetWaterTreatmentMaxCapacity(IPollutionOccupant* occupant, std::uint32_t& out) const {
    out = 0;
    GetMaxCapacity(kPollutionOccWaterTreatment, occupant, out);
}

void cSC4PollutionSimulator::GetWaterTreatmentCurrentCapacity(IPollutionOccupant* occupant, std::uint32_t& out) const {
    out = 0;
    GetCurrentCapacity(kPollutionOccWaterTreatment, occupant, out);
}

void cSC4PollutionSimulator::GetWaterTreatmentUsedCapacity(IPollutionOccupant* occupant, std::uint32_t& out) const {
    out = 0;
    GetMaxCapacity(kPollutionOccWaterTreatmentUsed, occupant, out);
}

// 100 % scaled by the age curve; fails (leaving 100 %) when the occupant has no age curve
bool cSC4PollutionSimulator::GetCurrentOccupantCondition(IPollutionOccupant* occupant, float& percentage) const {
    percentage = 1.0f;                                                  // kSC4PercentageOneHundred
    cISCPropertyHolder* holder = occupant->PropertyHolder();
    if (!holder->HasProperty(kPollutionOccAgeCurve)) return false;
    cSC4ResponseCurve curve;
    std::vector<float> values;
    if (cSCPropertyHelp::GetPropertyValue(holder, kPollutionOccAgeCurve, values))
        curve.SetResponseCurveData(values.data(), values.size());
    float age = 0.0f;
    cSCPropertyHelp::GetPropertyValue(holder, kPollutionOccAge, age);
    percentage = curve.GetOutputValue(age) * percentage;
    return true;
}

bool cSC4PollutionSimulator::GetCurrentOccupantEfficiency(IPollutionOccupant* occupant, float& percentage) const {
    if (host_->PowerAvailable()) {
        percentage = efficiency_;
        float condition = 0.0f;
        if (GetCurrentOccupantCondition(occupant, condition)) {
            percentage = condition * percentage;
            return true;
        }
    }
    percentage = 0.0f;
    return false;
}

bool cSC4PollutionSimulator::GetUnusedCapacity(std::uint32_t maxProperty, std::uint32_t usedProperty,
                                               IPollutionOccupant* occupant, std::uint32_t& out) const {
    std::uint32_t maximum = 0;
    if (!GetCurrentCapacity(maxProperty, occupant, maximum)) return false;
    std::uint32_t used = 0;
    if (!cSCPropertyHelp::GetPropertyValue(occupant->PropertyHolder(), usedProperty, used)) return false;
    out = maximum < used ? 0 : maximum - used;
    return true;
}

void cSC4PollutionSimulator::GetIncineratorUsedCapacity(IPollutionOccupant* occupant, std::uint32_t& out) const {
    out = 0;
    cSCPropertyHelp::GetPropertyValue(occupant->PropertyHolder(), kPollutionOccIncineratorUsed, out);
}

void cSC4PollutionSimulator::GetWasteToEnergyUsedCapacity(IPollutionOccupant* occupant, std::uint32_t& out) const {
    out = 0;
    cSCPropertyHelp::GetPropertyValue(occupant->PropertyHolder(), kPollutionOccWasteToEnergyUsed, out);
}

bool cSC4PollutionSimulator::CalcOccupantMaintenanceCost(IPollutionOccupant* occupant, std::int64_t& out) const {
    cISCPropertyHolder* holder = occupant->PropertyHolder();
    out = 0;
    float age = 0.0f;
    if (!cSCPropertyHelp::GetPropertyValue(holder, kPollutionOccMaintenanceBase, out)) return false;
    if (!cSCPropertyHelp::GetPropertyValue(holder, kPollutionOccAge, age)) return false;
    if (!holder->HasProperty(kPollutionOccMaintenanceCurve)) return false;
    cSC4ResponseCurve curve;
    std::vector<float> values;
    if (cSCPropertyHelp::GetPropertyValue(holder, kPollutionOccMaintenanceCurve, values))
        curve.SetResponseCurveData(values.data(), values.size());
    out = static_cast<std::int64_t>(curve.GetOutputValue(age) * static_cast<float>(out));
    return true;
}

// Copies each occupant's "capacity used" counter into the saved property and zeroes the counter.
void cSC4PollutionSimulator::ResetOccupantListCapacityUsedProperty(std::list<IPollutionOccupant*>& list,
                                                                    std::uint32_t currentProperty,
                                                                    std::uint32_t savedProperty) const {
    for (IPollutionOccupant* occupant : list) {
        cISCPropertyHolder* holder = occupant->PropertyHolder();
        std::uint32_t current = 0;
        if (!holder->HasProperty(currentProperty)) continue;
        cSCPropertyHelp::GetPropertyValue(holder, currentProperty, current);
        if (holder->HasProperty(savedProperty)) cSCPropertyHelp::SetPropertyValue(holder, savedProperty, current);
        cSCPropertyHelp::SetPropertyValue(holder, currentProperty, std::uint32_t{0});
    }
}

void cSC4PollutionSimulator::ComputeWasteToEnergyListCurrentCapacity() {
    for (IPollutionOccupant* occupant : wasteToEnergy_) {
        cISCPropertyHolder* holder = occupant->PropertyHolder();
        std::uint32_t rate = 0, capacity = 0, used = 0;
        if (cSCPropertyHelp::GetPropertyValue(holder, kPollutionOccWasteToEnergyRate, rate) &&
            cSCPropertyHelp::GetPropertyValue(holder, kPollutionOccWasteToEnergy, capacity) &&
            cSCPropertyHelp::GetPropertyValue(holder, kPollutionOccWasteToEnergyUsed, used) && capacity != 0) {
            cSCPropertyHelp::SetPropertyValue(
                holder, kPollutionOccWasteToEnergyProduced,
                static_cast<std::uint32_t>(static_cast<std::uint64_t>(used) * rate / capacity));
        }
    }
}

// Recycling centres sorted by efficiency (best first); the best centres worth `slots` units of capacity decide
// the city-wide effect: sum(factor * efficiency) / max(slots, sum(factor)).
void cSC4PollutionSimulator::CalculateRecyclingCenterEffect() {
    const int slots = static_cast<int>(host_->RecyclingCapacitySlots());
    std::list<IPollutionOccupant*> sorted;
    if (recyclingCenterCountSnapshot == 0) {
        recyclingEffect_ = 0.0f;
        return;
    }
    for (IPollutionOccupant* occupant : recyclingCenters_) {
        float efficiency = 0.0f;
        if (!GetCurrentOccupantEfficiency(occupant, efficiency)) continue;
        auto position = sorted.begin();
        for (; position != sorted.end(); ++position) {
            float other = 0.0f;
            if (GetCurrentOccupantEfficiency(*position, other) && other <= efficiency) break;
        }
        sorted.insert(position, occupant);
    }
    recyclingCenters_ = sorted;
    float factorSum = 0.0f, weighted = 0.0f;
    if (slots > 0 && !recyclingCenters_.empty()) {
        const float limit = static_cast<float>(slots);
        for (IPollutionOccupant* occupant : recyclingCenters_) {
            float factor = 0.0f;
            if (cSCPropertyHelp::GetPropertyValue(occupant->PropertyHolder(), kPollutionOccRecyclingFactor, factor)) {
                factorSum += factor;
                float efficiency = 0.0f;
                if (GetCurrentOccupantEfficiency(occupant, efficiency)) weighted += factor * efficiency;
            }
            if (!(factorSum < limit)) break;
        }
        if (0.0f < factorSum) {
            recyclingEffect_ = weighted / std::max(limit, factorSum);
            return;
        }
    }
    recyclingEffect_ = 0.0f;
}

// Disposal priority: free recycling allowance, then incinerators, then waste-to-energy, then the landfill
// transport (limited by the landfill free space and the transport curve), then alien abduction.
float cSC4PollutionSimulator::eatGarbage(float amount, std::uint32_t& wasteToEnergyBudget,
                                         std::uint32_t& incineratorBudget) {
    if (amount <= 0.0f) return 0.0f;
    float eaten = 0.0f;
    const int recyclable = recyclingAllowance_ - static_cast<int>(recycledThisMonth_);
    if (recyclable > 0) {
        const float room = static_cast<float>(recyclable);
        const float taken = room < amount ? room : amount;
        recycledThisMonth_ = static_cast<std::int64_t>(static_cast<float>(recycledThisMonth_) + taken);
        eaten += taken;
        if (amount <= room) return eaten;
        amount -= room;
    }
    auto burn = [&](std::uint32_t& budget, std::int64_t& counter) {
        if (!(0.0f < amount) || budget == 0) return;
        float taken;
        if (static_cast<float>(budget) <= amount) {
            // original quirk: a budget no larger than the request is emptied without counting any garbage eaten
            taken = 0.0f;
            budget = 0;
        } else {
            budget -= static_cast<std::uint32_t>(RoundToEven(amount));
            taken = amount;
            amount = 0.0f;
        }
        counter = static_cast<std::int64_t>(static_cast<float>(counter) + taken);
        eaten += taken;
    };
    burn(incineratorBudget, incineratedThisMonth_);
    burn(wasteToEnergyBudget, wasteToEnergyThisMonth_);
    if (0.0f < amount) {
        int low = 0, high = 0;
        host_->LandfillRange(low, high);
        const int free = high - low;
        const float transport = garbageTransportCapacity_.GetOutputValue(static_cast<float>(host_->LandfillTransportLoad()));
        int room = RoundToEven(transport * efficiency_) - garbageSupply_;
        if (free < room) room = free;
        if (0 < room) {
            float moved;
            if (amount <= static_cast<float>(room)) {
                AddToGarbageSupply(RoundToEven(amount));
                moved = amount;
                amount = 0.0f;
            } else {
                moved = static_cast<float>(room);
                AddToGarbageSupply(room);
                amount -= moved;
            }
            eaten += moved;
        }
    }
    if (0.0f < amount) {
        const int allowance = static_cast<int>(tunables_.abductionAmount) - static_cast<int>(abductedThisMonth_);
        if (0 < allowance) {
            const float room = static_cast<float>(allowance);
            const float taken = amount <= room ? amount : room;
            abductedThisMonth_ = static_cast<std::uint32_t>(static_cast<std::int64_t>(static_cast<float>(abductedThisMonth_) + taken));
            eaten += taken;
        }
    }
    return eaten;
}

// Averages the period's sums into the published values, records the history, tells the UI which grids changed
// and refreshes the sanitation efficiency from the department funding.
void cSC4PollutionSimulator::EndOfCycle() {
    CalculateRecyclingCenterEffect();
    garbageUncollected_ = totalGarbageUncollected_ != 0;
    std::uint32_t radiationPercent = 0;
    std::uint64_t avgAir = 0, avgWater = 0;
    std::uint16_t avgGarbage = 0;
    if (cellCount != 0) {
        avgAir = airSum / cellCount;
        avgWater = waterSum / cellCount;
        avgGarbage = static_cast<std::uint16_t>(totalGarbageUncollected_ / cellCount);
        radiationPercent = (radiationCount * 100u) / cellCount;
    }
    averageAir_ = static_cast<std::int32_t>(avgAir);
    averageWater_ = static_cast<std::int32_t>(avgWater);
    averageGarbage_ = static_cast<std::int16_t>(avgGarbage);
    periodValue224[0] = periodValue21c[0];
    periodValue224[1] = periodValue21c[1];
    host_->AddHistory(kHistoryAir, static_cast<std::int64_t>(avgAir));
    host_->AddHistory(kHistoryWater, averageWater_);
    host_->AddHistory(kHistoryGarbage, static_cast<std::uint16_t>(averageGarbage_));
    host_->AddHistory(kHistoryRadiation, radiationCount);
    host_->AddHistory(kHistoryTotal, static_cast<std::int64_t>(radiationPercent) + averageAir_);
    cellCount = 0;
    radiationCount = 0;
    periodValue21c[0] = periodValue21c[1] = 0;
    airSum = waterSum = 0;
    if (dirtyRadiation) host_->PostMessage(kMessageRadiationChanged, 0);
    if (dirtyAir) host_->PostMessage(kMessageAirChanged, 0);
    if (dirtyWater) host_->PostMessage(kMessageWaterChanged, 0);
    if (dirtyGarbage) host_->PostMessage(kMessageGarbageChanged, 0);
    dirtyRadiation = dirtyAir = dirtyWater = dirtyGarbage = false;
    efficiency_ = fundingEfficiencyCurve_.GetOutputValue(host_->SanitationFunding() * 100.0f) * 0.01f;
    host_->PostMessage(kMessageSanitationEfficiency, 8);
}

// ---------------------------------------------------------------------------- occupant list passes

void cSC4PollutionSimulator::AgeOccupantList(std::list<IPollutionOccupant*>& list, std::uint32_t usedProperty,
                                             std::uint32_t capacityProperty) {
    for (IPollutionOccupant* occupant : list) {
        cISCPropertyHolder* holder = occupant->PropertyHolder();
        float age = 0.0f, wear = 0.0f;
        std::uint32_t capacity = 0;
        std::int32_t used = 0;
        float warnAt = 99.0f, abandonAt = 99.0f;
        const float funding = host_->SanitationFunding();
        float load;
        if (usedProperty == 0) {
            load = 0.0f;
            if (recyclingCenterCountSnapshot != 0) load = static_cast<float>(recyclingShare268_) / static_cast<float>(recyclingCenterCountSnapshot);
        } else {
            cSCPropertyHelp::GetPropertyValue(holder, usedProperty, used);
            GetCurrentCapacity(capacityProperty, occupant, capacity);
            load = 0.0f;
            if (capacity != 0 && 0 < used) load = static_cast<float>(used) / static_cast<float>(capacity);
        }
        cSCPropertyHelp::GetPropertyValue(holder, kPollutionOccAge, age);
        cSCPropertyHelp::GetPropertyValue(holder, 0xea1cf220, wear);
        cSCPropertyHelp::GetPropertyValue(holder, 0xea1cf221, warnAt);
        cSCPropertyHelp::GetPropertyValue(holder, 0xea1cf222, abandonAt);
        float step = wear;
        step = ageCurve_.GetOutputValue(funding) * step;
        age = efficiencyCurve_.GetOutputValue(load) * step + age;
        cSCPropertyHelp::SetPropertyValue(holder, kPollutionOccAge, age);
        if (abandonAt <= age || warnAt <= age) {
            if (!services_) continue;
            float position[3];
            occupant->Position(position);
            IPollutionLot* lot = services_->LotAtPosition(position);
            if (!lot) continue;
            if (abandonAt <= age) lot->SetState(4);
            else lot->SetState(2);
        }
    }
}

void cSC4PollutionSimulator::SetOccupantListUsedCapacity(std::list<IPollutionOccupant*>& list, std::uint32_t capacityProperty,
                                                         std::uint32_t usedProperty, std::uint32_t total,
                                                         std::uint32_t& remaining, std::uint32_t count) {
    if (total == 0 || count == 0 || remaining == 0) return;
    auto it = list.begin();
    std::uint32_t budget = remaining;
    if (total < budget) budget = total;
    std::uint64_t share = static_cast<std::uint64_t>(budget) / count;
    do {
        int portion = static_cast<int>(share);
        --count;
        if (it == list.end()) return;
        IPollutionOccupant* occupant = *it;
        cISCPropertyHolder* holder = occupant->PropertyHolder();
        std::uint32_t capacity = 0;
        if (GetCurrentCapacity(capacityProperty, occupant, capacity) && holder->HasProperty(usedProperty)) {
            std::uint32_t used = 0;
            cSCPropertyHelp::GetPropertyValue(holder, usedProperty, used);
            int room = static_cast<int>(capacity - used);
            if (capacity < used || capacity - used == 0) room = 0;
            if (room < portion) {
                portion = room;
                if (count != 0) share = static_cast<std::uint64_t>(remaining) / count;     // re-spread what is left
            }
            remaining -= static_cast<std::uint32_t>(portion);
            cSCPropertyHelp::SetPropertyValue(holder, usedProperty, static_cast<std::uint32_t>(used + portion));
        }
        ++it;
    } while (remaining != 0);
}

void cSC4PollutionSimulator::CalcMaintenanceCosts(std::list<IPollutionOccupant*>& list) {
    if (!services_) return;
    for (IPollutionOccupant* occupant : list) {
        cISCPropertyHolder* holder = occupant->PropertyHolder();
        std::int64_t cost = 0;
        if (!CalcOccupantMaintenanceCost(occupant, cost)) continue;
        PollutionBudgetRecord record;
        record.zero = 0;
        if (!services_->FindBudgetLineItem(holder, 0xca58edc7, record) || cost == record.cost) continue;
        if (IPollutionDepartment* department = services_->DepartmentFor(record)) {
            department->AddCost(cost - record.cost);
            services_->SetBudgetLineItem(holder, record.id, cost);
        }
    }
}

void cSC4PollutionSimulator::AddRadiationToCity(const float position[3], float amount, float radius) {
    if ((amount != 0.0f || std::isnan(amount)) && 0.0f < radius) {
        cSC4SimGrid<std::int16_t>* grid = valueGrids_[kPollutionRadiation].get();
        std::int32_t tractX = 0, tractZ = 0;
        grid->PositionToTract(position[0], position[2], tractX, tractZ);
        AddRadiusEffect(grid->GetGridData(), grid->GetTractCountX(), grid->GetTractCountZ(), tractX, tractZ, radius, amount, 0.0f);
        if (visualEffectUpdate_) visualEffectUpdate_();
        host_->PostMessage(0xda6dd35d, 0);
    }
}

}  // namespace sc4
