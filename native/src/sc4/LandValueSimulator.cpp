#include "sc4/LandValueSimulator.h"

#include <algorithm>

#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "sc4/DBSegment.h"

namespace sc4 {

// ---- cursor ------------------------------------------------------------------------------------------

void TractCursor::Reset() {
    x = 0;
    z = 0;
    state = 0;
    if (minX > 0 || minZ > 0 || maxX < 0 || maxZ < 0) state = 2;
}

void TractCursor::SetBounds(int countX, int countZ) {
    x = 0;
    z = 0;
    minX = 0;
    minZ = 0;
    maxX = countX - 1;
    maxZ = countZ - 1;
    if (countX < 1) maxX = 0;
    if (countZ < 1) maxZ = 0;
}

bool TractCursor::Next(int& outX, int& outZ) {
    if (state == 0) {
        state = 1;
    } else {
        if (state == 2) return false;
        x += step;
        if (maxX < x) {
            x = minX;
            z += step;
            if (maxZ < z) {
                state = 2;
                return false;
            }
        }
    }
    outX = x;
    outZ = z;
    return true;
}

// ---- COM ---------------------------------------------------------------------------------------------

bool cSC4LandValueSimulator::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 0xe4fda3d4) *out = static_cast<cIGZSerializable*>(this);
    else if (iid == 0x452294aa) *out = static_cast<cIGZMessageTarget2*>(this);
    else if (iid == 1 || iid == kGZIID_cISC4LandValueSimulator) *out = static_cast<cISC4LandValueSimulator*>(this);
    else return false;
    AddRef();
    return true;
}

std::uint32_t cSC4LandValueSimulator::Release() {
    if (refCount_ < 2) {
        Shutdown();
        delete this;
        return 0;
    }
    return --refCount_;
}

cSC4SimGrid<std::uint8_t>* cSC4LandValueSimulator::MakeGrid(std::uint32_t id) {
    auto* grid = new cSC4SimGrid<std::uint8_t>();
    grid->AddRef();
    grid->Init();
    grid->SetTractSize(tractSize_);
    grid->SetInstanceID(id);
    return grid;
}

bool cSC4LandValueSimulator::Init() {
    if (initialised_) return true;
    host_->AcquireServices();
    ReadSimulatorTunableValues(host_->SimulatorExemplar());
    if (!intrinsic_) intrinsic_ = MakeGrid(0x89d5bb39);
    if (!total_) total_ = MakeGrid(0x89d5bb51);
    if (!scratch_) scratch_ = MakeGrid(0x89d5bb60);
    if (!slope_) {
        slope_ = new cSC4SimGrid<float>();
        slope_->AddRef();
        slope_->Init();
        slope_->SetTractSize(8);
        slope_->SetInstanceID(0x49d5bb6f);
    }
    stage_ = 0;
    cursorA_ = TractCursor();
    cursorA_.SetBounds(intrinsic_->GetTractCountX(), intrinsic_->GetTractCountZ());
    cursorB_ = TractCursor();
    cursorB_.SetBounds(slope_->GetTractCountX(), slope_->GetTractCountZ());
    histogramShift_ = 5;
    histogram_.resize(static_cast<std::size_t>(static_cast<int>(host_->Terrain()->MaxAltitude()) >> histogramShift_));
    host_->CreateHistory(kLandValueHistoryAverage, 1);
    static const char* const kName = "cSC4LandValueSimulator";
    for (int kind : {0, 10, 4, 6}) host_->AddIdleClient(static_cast<cIGZMessageTarget2*>(this), kind, kName, 2);
    for (std::uint32_t message : {0x99ef1142u, 0x99ef1143u, 0x678e6e3au, 0x278e6003u, 0x7699455u})
        host_->AddNotification(this, message);
    initialised_ = true;
    return true;
}

bool cSC4LandValueSimulator::Shutdown() {
    if (!initialised_) return true;
    initialised_ = false;
    host_->RemoveIdleClient(static_cast<cIGZMessageTarget2*>(this));
    for (std::uint32_t message : {0x99ef1142u, 0x99ef1143u, 0x678e6e3au, 0x278e6003u, 0x7699455u})
        host_->RemoveNotification(this, message);
    host_->ReleaseServices();
    for (auto** grid : {&intrinsic_, &total_, &scratch_}) {
        if (*grid) {
            (*grid)->Shutdown();
            (*grid)->Release();
            *grid = nullptr;
        }
    }
    if (slope_) {
        slope_->Shutdown();
        slope_->Release();
        slope_ = nullptr;
    }
    altitudeCurve_.reset();
    developerCurve_.reset();
    return true;
}

void cSC4LandValueSimulator::SetAltitudeCurve(const std::vector<SC4Point>& points) {
    if (!altitudeCurve_) altitudeCurve_ = std::make_unique<cSC4ResponseCurve>();
    altitudeCurve_->SetResponseCurveData(points);
}

// Tunables come from the simulator exemplar {0x6534284a, 0xe7e2c2db, 0xe7e2c8d8} (null = resource missing: nothing changes).
void cSC4LandValueSimulator::ReadSimulatorTunableValues(const cISCPropertyHolder* exemplar) {
    if (!exemplar) return;
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x47e2c200, tractSize_);
    auto readPair = [&](std::uint32_t id, std::uint8_t& first, std::uint8_t& second) {
        std::vector<std::uint8_t> values;
        if (cSCPropertyHelp::GetPropertyValue(exemplar, id, values) && values.size() >= 2) {
            first = values[0];
            second = values[1];
        }
    };
    readPair(0x47e2c300, d1_, d2_);
    readPair(0x47e2c301, d3_, d4_);
    readPair(0x47e2c310, d5_, d6_);
    auto loadCurve = [&](std::unique_ptr<cSC4ResponseCurve>& curve, std::uint32_t id) {
        std::vector<float> values;
        if (!exemplar->HasProperty(id)) {
            curve.reset();
            return;
        }
        if (!curve) curve = std::make_unique<cSC4ResponseCurve>();
        if (cSCPropertyHelp::GetPropertyValue(exemplar, id, values)) curve->SetResponseCurveData(values.data(), values.size());
    };
    loadCurve(altitudeCurve_, 0x47e2c320);
    std::vector<std::uint32_t> developers;
    std::vector<float> weights;
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x47e2c330, developers);
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x47e2c331, weights);
    for (std::size_t i = 0; i < developers.size() && i < weights.size(); ++i)
        developerWeights_[host_->GrowthDeveloperForType(developers[i])] = weights[i];
    loadCurve(developerCurve_, 0x47e2c332);
}

// ---- streams -----------------------------------------------------------------------------------------

bool cSC4LandValueSimulator::Write(cIGZOStream& stream) const {
    stream.SetUint16(1);
    SC4WriteGZSerializable(stream, static_cast<const cISC4SimGridBase*>(intrinsic_));
    SC4WriteGZSerializable(stream, static_cast<const cISC4SimGridBase*>(total_));
    SC4WriteGZSerializable(stream, static_cast<const cISC4SimGridBase*>(slope_));
    bool flag = true;
    if (!dirty_) flag = stage_ != 9;
    stream.SetUint8(flag);
    stream.SetUint8(average_);
    stream.SetFloat32(meanAltitude_);
    return !stream.GetError();
}

bool cSC4LandValueSimulator::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 1) return false;
    SC4ReadGZSerializable(stream, kGZIID_cISC4SimGrid, reinterpret_cast<void**>(&intrinsic_), false);
    SC4ReadGZSerializable(stream, kGZIID_cISC4SimGrid, reinterpret_cast<void**>(&total_), false);
    SC4ReadGZSerializable(stream, kGZIID_cISC4SimGrid, reinterpret_cast<void**>(&slope_), false);
    std::uint8_t flag = 0;
    stream.GetUint8(flag);
    dirty_ = flag != 0;
    stream.GetUint8(average_);
    stream.GetFloat32(meanAltitude_);
    return !stream.GetError();
}

// ---- messages ----------------------------------------------------------------------------------------

bool cSC4LandValueSimulator::DoMessage(cIGZMessage2* message) {
    const std::int32_t type = static_cast<std::int32_t>(message->GetType());
    auto* payload = static_cast<cIGZMessage2Standard*>(message);
    if (type < 0x66956814) {
        if (type < 0x7699455) {
            if (type != -0x6610eebe && type != -0x6610eebd) return true;
        } else if (type != 0x7699455 && type != 0x278e6003) {
            return true;
        }
        dirty_ = true;
        return true;
    }
    std::uint32_t budget;
    if (type < 0x6695681a) {
        if (type != 0x66956814) {
            if (type != 0x66956816) return true;
            if (dirty_ && stage_ == 9) {
                dirty_ = false;
                stage_ = 0;
            }
            return true;
        }
        budget = 5;
    } else {
        if (type == 0x678e6e3a) {
            dirty_ = true;
            return true;
        }
        if (type != 0x6695681a) return true;
        budget = static_cast<std::uint32_t>(payload->GetData3());
    }
    DoIdle(budget);
    return true;
}

// ---- queries -----------------------------------------------------------------------------------------

bool cSC4LandValueSimulator::TractIsZoned(int tractX, int tractZ) {
    const int shift = intrinsic_->GetTractShift();
    const int size = intrinsic_->GetTractSize();
    const int x0 = tractX << shift, z0 = tractZ << shift;
    for (int x = x0; x < x0 + size; ++x)
        for (int z = z0; z < z0 + size; ++z)
            if (host_->CellZoneType(x, z) != 0) return true;
    return false;
}

// The original scans size+1 cells in each direction (inclusive bounds) and is kept that way.
bool cSC4LandValueSimulator::TractHasLand(int tractX, int tractZ) {
    const int shift = intrinsic_->GetTractShift();
    const int size = intrinsic_->GetTractSize();
    const int x0 = tractX << shift, z0 = tractZ << shift;
    if (size < 0) return false;
    for (int x = x0; x <= x0 + size; ++x)
        for (int z = z0; z <= z0 + size; ++z)
            if (host_->Terrain()->CellHasLand(x, z)) return true;
    return false;
}

void cSC4LandValueSimulator::SetupAltitudeEffect() {
    float pivot = std::min(meanAltitude_, modeAltitude_);
    effectPivot_ = pivot;
    effectLowSpan_ = std::max(1.0f, pivot - minAltitude_);
    effectHighSpan_ = std::max(1.0f, maxAltitude_ - pivot);
    effectScale_ = 100;
    if (100.0f < effectHighSpan_) {
        const float top = host_->Terrain()->MaxAltitude();
        const int scaled = static_cast<int>(static_cast<float>(effectScale_) +
                                            ((effectHighSpan_ - 100.0f) / (top - 100.0f)) * 100.0f);
        effectScale_ = scaled < 0x97 ? scaled : 0x96;
    }
}

std::uint32_t cSC4LandValueSimulator::LookupAltitudeEffect(float altitude) {
    if (minAltitude_ > altitude) return 0;
    float v;
    if (effectPivot_ <= altitude)
        v = ((altitude - effectPivot_) * static_cast<float>(effectScale_ - 50)) / effectHighSpan_ + 50.0f;
    else
        v = ((altitude - minAltitude_) * 50.0f) / effectLowSpan_;
    const float out = altitudeCurve_->GetOutputValue(static_cast<float>(static_cast<int>(v)));
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(out)) & 0xff;
}

int cSC4LandValueSimulator::GetLandValueType(int x, int z) const {
    const std::uint8_t value = total_->GetCellValue(x, z);
    if (value < d3_) return 1;
    return 3 - (value < d4_ ? 1 : 0);
}

// ---- the stage machine -------------------------------------------------------------------------------

void cSC4LandValueSimulator::DoIdle(std::uint32_t budgetMilliseconds) {
    int tx = 0, tz = 0;
    switch (stage_) {
    case 0:
        cursorA_.Reset();
        stage_ = 1;
        break;
    case 1: {   // land mask
        const auto slice = host_->BeginSlice(budgetMilliseconds);
        auto& data = scratch_->GetGridData();
        for (;;) {
            if (host_->SliceExpired(slice)) return;
            if (!cursorA_.Next(tx, tz)) {
                stage_ = 2;
                return;
            }
            data[tx][tz] = TractHasLand(tx, tz) ? d1_ : 0;
        }
    }
    case 2:
        host_->AddDistanceEffect(scratch_->GetGridData(), intrinsic_->GetTractCountX(), intrinsic_->GetTractCountZ(),
                                 static_cast<float>(d6_), static_cast<float>(d5_));
        minAltitude_ = host_->Terrain()->MaxAltitude();
        maxAltitude_ = host_->Terrain()->MinAltitude();
        cursorA_.Reset();
        allLand_ = false;
        sum_ = 0;
        count_ = 0;
        std::fill(histogram_.begin(), histogram_.end(), 0);
        stage_ = 3;
        break;
    case 3: {   // altitude statistics
        const auto slice = host_->BeginSlice(budgetMilliseconds);
        for (;;) {
            if (host_->SliceExpired(slice)) return;
            if (!cursorA_.Next(tx, tz)) break;
            bool use = TractHasLand(tx, tz);
            float px, pz;
            intrinsic_->TractCenterToPosition(tx, tz, px, pz);
            const float altitude = host_->Terrain()->AltitudeAt(px, pz);
            if (use) {
                minAltitude_ = std::min(minAltitude_, altitude);
                if (altitude > maxAltitude_) maxAltitude_ = altitude;
            }
            if (!allLand_) use = use && TractIsZoned(tx, tz);
            if (use) {
                sum_ = static_cast<int>(static_cast<float>(sum_) + altitude);
                ++count_;
                std::int64_t bin = static_cast<std::int32_t>(altitude) >> histogramShift_;
                if (bin < 0) bin = 0;
                if (static_cast<std::uint64_t>(bin) >= histogram_.size()) bin = static_cast<std::int64_t>(histogram_.size()) - 1;
                ++histogram_[static_cast<std::size_t>(bin)];
            }
        }
        meanAltitude_ = 0;
            if (count_ < 1) {
            if (!allLand_) {   // nothing zoned yet: redo the walk over every land tract
                cursorA_.Reset();
                sum_ = 0;
                count_ = 0;
                allLand_ = true;
                std::fill(histogram_.begin(), histogram_.end(), 0);
                stage_ = 3;
                return;
            }
        } else {
            meanAltitude_ = static_cast<float>(sum_) / static_cast<float>(count_);
        }
        int best = 0, bestCount = 0;
        for (int i = 0; i < static_cast<int>(histogram_.size()); ++i) {
            if (bestCount < histogram_[static_cast<std::size_t>(i)]) best = i;
            if (bestCount <= histogram_[static_cast<std::size_t>(i)]) bestCount = histogram_[static_cast<std::size_t>(i)];
        }
        modeAltitude_ = static_cast<float>((1 << (histogramShift_ - 1)) + (best << histogramShift_));
        if (modeAltitude_ < minAltitude_) modeAltitude_ = minAltitude_;
        if (maxAltitude_ < modeAltitude_) modeAltitude_ = maxAltitude_;
        SetupAltitudeEffect();
        cursorB_.Reset();
        stage_ = 5;
        return;
    }
    case 5: {   // slope map
        const auto slice = host_->BeginSlice(budgetMilliseconds);
        const int shift = slope_->GetTractShift();
        auto& data = slope_->GetGridData();
        for (;;) {
            if (host_->SliceExpired(slice)) return;
            if (!cursorB_.Next(tx, tz)) {
                cursorA_.Reset();
                stage_ = 4;
                return;
            }
            data[tx][tz] = host_->AverageSlope(tx << shift, tz << shift, ((tx + 1) << shift) - 1, ((tz + 1) << shift) - 1);
        }
    }
    case 4: {   // altitude effect
        const auto slice = host_->BeginSlice(budgetMilliseconds);
        auto& scratch = scratch_->GetGridData();
        auto& intrinsic = intrinsic_->GetGridData();
        for (;;) {
            if (host_->SliceExpired(slice)) return;
            if (!altitudeCurve_ || !cursorA_.Next(tx, tz)) {
                cursorA_.Reset();
                stage_ = 6;
                return;
            }
            float px, pz;
            intrinsic_->TractCenterToPosition(tx, tz, px, pz);
            const std::uint32_t effect = LookupAltitudeEffect(host_->Terrain()->AltitudeAt(px, pz));
            std::uint32_t value = scratch[tx][tz] + effect;
            if (value > 0xfe) value = 0xff;
            intrinsic[tx][tz] = static_cast<std::uint8_t>(value);
        }
    }
    case 6:
        scratch_->GetGridData() = intrinsic_->GetGridData();
        developerCursor_ = developerWeights_.begin();
        stage_ = 7;
        break;
    case 7:
        if (developerCursor_ == developerWeights_.end()) {
            total_->GetGridData() = scratch_->GetGridData();
            cursorA_.Reset();
            sum_ = 0;
            count_ = 0;
            stage_ = 8;
        } else {
            DesirabilityContext context{developerCursor_->second, developerCurve_.get()};
            if (cSC4SimGrid<std::int16_t>* map = host_->DesirabilityMap(developerCursor_->first))   // the port's host: null until the developer exists
                ProcessTwoGridsWithCallback<std::uint8_t, std::int16_t>(*scratch_, *map, AddDesirability, &context);
            ++developerCursor_;
        }
        break;
    case 8: {   // average
        const auto slice = host_->BeginSlice(budgetMilliseconds);
        const auto& total = total_->GetGridData();
        for (;;) {
            if (host_->SliceExpired(slice)) return;
            if (!cursorA_.Next(tx, tz)) break;
            if ((allLand_ || TractIsZoned(tx, tz)) && TractHasLand(tx, tz)) {
                sum_ += total[tx][tz];
                ++count_;
            }
        }
        average_ = count_ < 1 ? 0 : static_cast<std::uint8_t>(sum_ / count_);
        host_->AddHistory(kLandValueHistoryAverage, average_);
        stage_ = 9;
        DoEndOfSimulationCycle();
        return;
    }
    default:
        break;
    }
}

}  // namespace sc4
