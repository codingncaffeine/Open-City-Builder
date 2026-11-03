#include "sc4/LotDeveloper.h"

#include <algorithm>
#include <cfloat>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <vector>

#include "gz/StreamOps.h"
#include "rz/cRZRandom.h"
#include "sc4/DBSegment.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/Lot.h"
#include "sc4/LotBaseTextureOccupant.h"
#include "sc4/LotConfiguration.h"
#include "sc4/OccupantManager.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {
constexpr std::uint32_t kGZIID_cISC4BuildingOccupantQueue = 0x87dda3a9;   // the reference iid of the queue entries (0x100140242)
constexpr std::uint32_t kMaxStreamedQueue = 1u << 20;
}  // namespace

// 0x10013eda8 — the class answers its own interface (0x4818e5d2) and cIGZUnknown at +0, the message
// target 2 (0x452294aa) at +8 and the serializable (0xe4fda3d4) at +0x10. The class id 0xa990bfe0 is
// not answered (as in the original).
bool cSC4LotDeveloper::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cIGZSerializable) *out = static_cast<cIGZSerializable*>(this);
    else if (iid == kGZIID_cIGZMessageTarget2) *out = static_cast<cIGZMessageTarget2*>(this);
    else if (iid == 1 || iid == kGZIID_cISC4LotDeveloper) *out = static_cast<cISC4LotDeveloper*>(this);
    else return false;
    AddRef();
    return true;
}

// 0x10013ee38
std::uint32_t cSC4LotDeveloper::Release() {
    if (refCount_ < 2) {
        delete this;
        return 0;
    }
    return --refCount_;
}

namespace {
const std::list<cSC4Lot*>* (*g_lotList)() = nullptr;
ILotDailyServices* g_daily = nullptr;
constexpr std::uint32_t kOccupantTypeNetwork = 0xc772bf98;
}  // namespace

void cSC4LotDeveloper::SetDailyServices(ILotDailyServices* services) { g_daily = services; }

// §3.2 (0x006CD940). PARTIAL: the day pass, the network latches, the zot embargo and the commute switch; night lights (0x44778234,
// the animation tick), the power-changed rectangle (0x8a07ae00, §9) and the building night deque are not ported.
bool cSC4LotDeveloper::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    switch (message->GetType()) {
    case 0x66956814: return DoDailySimulation();
    case 0x99ef1142:                                     // an occupant inserted / removed: a network piece marks both latches
    case 0x99ef1143:
        if (static_cast<std::uint32_t>(standard->GetData3()) == kOccupantTypeNetwork) netChanged_ = unitChanged_ = true;
        break;
    case 0x26d31ec4:                                     // the city was established: no zots for a month
        noZotUntilDay_ = g_daily ? static_cast<std::uint32_t>(g_daily->SimDateNumber()) + 31 : 0;
        break;
    case 0x69247dc7:                                     // the traffic maps changed: empty lots test the commute when there are jobs
        canCheckCommute_ = g_daily && g_daily->CityHasJobs();
        break;
    default: break;
    }
    return true;
}

void cSC4LotDeveloper::SetLotListProvider(const std::list<cSC4Lot*>* (*provider)()) { g_lotList = provider; }

bool cSC4LotDeveloper::DoDailySimulation() {
    const std::list<cSC4Lot*>* lots = g_lotList ? g_lotList() : nullptr;
    if (!lots) return true;
    auto position = lots->end();
    if (dailyState_ == 0 || dailyState_ == 3) {
        if (lots->empty()) return true;
        position = lots->begin();
        visited_ = 0;
        netSnap_ = netChanged_;
        unitSnap_ = unitChanged_;
        netChanged_ = unitChanged_ = false;
        zotsAllowed_ = g_daily && static_cast<std::uint32_t>(g_daily->SimDateNumber()) > noZotUntilDay_;
        dailyState_ = 1;
    } else if (dailyState_ == 1) {
        // the safe iterator: carry on at the remembered lot (a lot deleted meanwhile restarts the cycle at the first lot)
        position = nextLot_ ? std::find(lots->begin(), lots->end(), nextLot_) : lots->end();
        if (nextLot_ && position == lots->end()) position = lots->begin();
    } else {
        return true;
    }
    if (nextLot_) { nextLot_->Release(); nextLot_ = nullptr; }
    if (lots->empty()) { dailyState_ = 3; return true; }
    const std::uint32_t perDay = std::uint32_t(lots->size()) / 28 + 1;
    while (visited_ < perDay) {
        if (position == lots->end()) { dailyState_ = 3; break; }
        cSC4Lot* lot = *position++;
        if (lot) UpdateLot(lot);
        ++visited_;
    }
    if (dailyState_ == 1 && position != lots->end()) {
        nextLot_ = *position;
        nextLot_->AddRef();
    }
    if (visited_ >= perDay) {
        visited_ = 0;
        return true;
    }
    return false;
}


// §4.2 (Mac i386 0x000f4116). The facing fix after the network refresh is dead in the original (its static byte is only ever
// cleared) and is not ported. A lot in a business unit keeps the travel populations it was saved with: the unit's step
// (UpdateTravelDesignates, then UpdateTravelPopulations onto the designates, §5.7) and PickUnitToJoin are not ported yet.
bool cSC4LotDeveloper::UpdateLot(cSC4Lot* lot) {
    static const bool trace = std::getenv("SC4_LOTDEBUG") != nullptr;
    if (!lot) return false;
    int x = 0, z = 0;
    lot->GetLocation(x, z);                                          // lot vslot 0x54
    if (netSnap_) lot->UpdateRoadAccess();                           // vslot 0x78
    bool changed = false;
    const std::uint32_t type = lot->GetBuildingType(false);          // vslot 0xb8
    if (type == 0) {
        if (zotsAllowed_) {
            // an empty lot standing a month tests its commute (results ignored: only the zots show)
            if (static_cast<std::uint32_t>(lot->GetDaysSinceLastStateChange()) > 30 && canCheckCommute_) {
                const int zone = lot->GetZoneType();
                BuildingProfile probe{};
                probe.purpose = zone >= 1 && zone <= 3 ? 1 : 3;
                if (zone >= 1 && zone <= 6) TestTravel(*lot, probe, x, z);
            }
            if (IsEmptyLotSuitableForZot(*lot, x, z)) TestPower(*lot, x, z);
        }
    } else {
        const BuildingProfile* found = gates_ ? gates_->ProfileOfBuilding(type) : nullptr;   // BDS vslot 0x9c
        const BuildingProfile none{};
        const BuildingProfile& profile = found ? *found : none;
        if (profile.purpose != 0) {
            // a grown building: all four tests run, the last failing one names the reason
            const int state = lot->GetState();                       // vslot 0x84
            int next = state;
            std::uint8_t reason = 0;
            if (!TestDesirability(*lot, x, z)) { next = 4; reason = 1; }
            if (TestPower(*lot, x, z) == 0) { next = 4; reason = 2; }
            if (!TestWater(*lot, x, z)) { next = 4; reason = 3; }
            if (!TestTravel(*lot, profile, x, z)) { next = 4; reason = 4; }
            changed = next != state;
            if (changed) {
                lot->SetState(static_cast<std::uint32_t>(next), 0, reason);   // vslot 0x88
                if (trace) std::fprintf(stderr, "lot pass: (%d,%d) building %08x state %d -> %d reason %u\n", x, z, type, state, next, reason);
            }
            lot->SetCondition(TestCondition(*lot));                  // vslot 0x90
        } else {
            // civic or plopped: only power and the condition it gives
            const bool powered = TestPower(*lot, x, z) == 2;
            const std::uint32_t condition = lot->GetCondition();     // vslot 0x8c
            if (condition == 1) {
                if (powered) {
                    lot->SetCondition(0);
                    TestTravel(*lot, profile, x, z);
                }
            } else if (condition == 0) {
                if (powered) TestTravel(*lot, profile, x, z);
                else lot->SetCondition(1);
            } else {
                if (condition - 5u < 2) lot->SetCondition(0);
                if (powered) TestTravel(*lot, profile, x, z);
            }
        }
    }
    // §4.2 step 5
    if (!lot->GetUnit()) {                                           // vslot 0x64
        lot->ResetTravelPopulation();                                // vslot 0xe4
        lot->AddTravelPopulation(*lot);                              // vslot 0xe8
    }
    return changed;
}

// 0x10014009a: version 1, the vertices per row, the city extent, the construction queue (one serializable reference each), the
// position of the building in progress, the stage byte and a flag.
bool cSC4LotDeveloper::Write(cIGZOStream& stream) const {
    stream.SetUint16(1);
    stream.SetSint32(streamVerticesPerRow_);
    stream.SetFloat32(streamCityWidth_);
    stream.SetFloat32(streamCityDepth_);
    stream.SetUint32(static_cast<std::uint32_t>(constructionQueue_.size()));
    for (const cIGZUnknown* occupant : constructionQueue_) SC4WriteGZSerializable(stream, occupant);
    stream.SetUint32(queueCursor_);
    stream.SetUint8(static_cast<std::uint8_t>(stage_));
    stream << flagF4_;
    return !stream.GetError();
}

// 0x100140242
bool cSC4LotDeveloper::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 1) return false;
    stream.GetSint32(streamVerticesPerRow_);
    stream.GetFloat32(streamCityWidth_);
    stream.GetFloat32(streamCityDepth_);
    std::uint32_t count = 0;
    stream.GetUint32(count);
    for (cIGZUnknown* occupant : constructionQueue_) if (occupant) occupant->Release();
    constructionQueue_.clear();
    if (count > kMaxStreamedQueue) return false;
    constructionQueue_.resize(count, nullptr);
    for (cIGZUnknown*& slot : constructionQueue_) {
        void* raw = nullptr;
        SC4ReadGZSerializable(stream, kGZIID_cISC4BuildingOccupantQueue, &raw, false);   // one reference
        slot = static_cast<cIGZUnknown*>(raw);
    }
    stream.GetUint32(queueCursor_);
    std::uint8_t stage = 0;
    stream.GetUint8(stage);
    stage_ = stage;
    stream >> flagF4_;
    return !stream.GetError();
}

void cSC4LotDeveloper::ReadSimulatorTunableValues(const cISCPropertyHolder* exemplar) {
    if (!exemplar) return;
    LotDeveloperTunables& t = tunables_;
    cSCPropertyHelp::GetPropertyValue(exemplar, kLotDevPropConditionThreshold, t.conditionThreshold);
    t.ratioA = 0.2f;
    t.ratioB = 0.4f;
    t.ratioC = 0.6f;
    std::vector<float> ratios;
    if (exemplar->HasProperty(kLotDevPropConditionRatios) &&
        cSCPropertyHelp::GetPropertyValue(exemplar, kLotDevPropConditionRatios, ratios) && ratios.size() >= 3) {
        t.ratioA = ratios[0];
        t.ratioB = ratios[1];
        t.ratioC = ratios[2];
    }
    cSCPropertyHelp::GetPropertyValue(exemplar, kLotDevPropFlatTolerance, t.flatTolerance);
    cSCPropertyHelp::GetPropertyValue(exemplar, kLotDevPropFloat12c, t.value12c);
    cSCPropertyHelp::GetPropertyValue(exemplar, kLotDevPropU32_130, t.value130);
    cSCPropertyHelp::GetPropertyValue(exemplar, kLotDevPropU32_134, t.value134);
    cSCPropertyHelp::GetPropertyValue(exemplar, kLotDevPropBool138, t.flag138);
    cSCPropertyHelp::GetPropertyValue(exemplar, kLotDevPropBool139, t.flag139);
    cSCPropertyHelp::GetPropertyValue(exemplar, kLotDevPropLevelTolerance, t.levelTolerance);
    cSCPropertyHelp::GetPropertyValue(exemplar, kLotDevPropMaxLevelling, t.maxLevelling);
    cSCPropertyHelp::GetPropertyValue(exemplar, kLotDevPropU32_0f0, t.value0f0);
    cSCPropertyHelp::GetPropertyValue(exemplar, kLotDevPropU32_0ec, t.value0ec);
    std::vector<float> curve;
    if (exemplar->HasProperty(kLotDevPropCostCurve) &&
        cSCPropertyHelp::GetPropertyValue(exemplar, kLotDevPropCostCurve, curve))
        developmentCostCurve_.SetResponseCurveData(curve.data(), curve.size());
}

float cSC4LotDeveloper::GetTerrainAltitudeByVertex(int vx, int vz, bool includeWater) const {
    const int index = vx + vz * host_->VerticesPerRow();
    const float height = host_->VertexHeight(index);
    if (includeWater) {
        const float water = host_->VertexWaterHeight(index);
        if (height < water) return water;
    }
    return height;
}

// Every vertex of the cell rectangle: the vertex grid is one larger than the cell grid in both directions.
float cSC4LotDeveloper::GetLowestOrHighestAreaAltitude(const LotCellRect& area, bool lowest, bool includeWater) const {
    float best = lowest ? FLT_MAX : -FLT_MAX;
    for (int x = area.x0; x <= area.x1 + 1; ++x) {
        for (int z = area.z0; z <= area.z1 + 1; ++z) {
            const float h = GetTerrainAltitudeByVertex(x, z, includeWater);
            if (lowest ? h < best : best < h) best = h;
        }
    }
    return best;
}

// Metre rectangle: the interior vertices (16 m grid) first, then the four edges at 4 m steps ending exactly on
// the far edge. The rectangle is clamped to [0, cityExtent - 0.1].
float cSC4LotDeveloper::GetLowestOrHighestAreaAltitude(const LotMetreRect& area, bool lowest, bool includeWater) const {
    const float left = area.x0 >= 0.0f ? area.x0 : 0.0f;
    const float top = area.z0 >= 0.0f ? area.z0 : 0.0f;
    const float cityX = host_->CityWidthMetres(), cityZ = host_->CityDepthMetres();
    const float right = area.x1 < cityX ? area.x1 : cityX - 0.1f;
    const float bottom = area.z1 < cityZ ? area.z1 : cityZ - 0.1f;
    float best = lowest ? FLT_MAX : -FLT_MAX;
    auto take = [&](float h) {
        if (lowest ? h < best : best < h) best = h;
    };
    auto sample = [&](float x, float z) {
        float h = host_->HeightAt(x, z);
        if (includeWater) {
            const float water = host_->WaterHeightAt(x, z);
            if (water > h) h = water;
        }
        take(h);
    };
    const int vx0 = static_cast<int>((left + 16.0f) * 0.0625f), vx1 = static_cast<int>((right + 0.0f) * 0.0625f);
    const int vz0 = static_cast<int>((top + 16.0f) * 0.0625f), vz1 = static_cast<int>((bottom + 0.0f) * 0.0625f);
    for (int vx = vx0; vx <= vx1; ++vx)
        for (int vz = vz0; vz <= vz1; ++vz) take(GetTerrainAltitudeByVertex(vx, vz, includeWater));
    // walk one edge in 4 m steps; the step that would reach or pass the far end is replaced by the end itself
    auto walk = [&](float from, float to, auto&& emit) {
        float v = from;
        while (v < to) {
            emit(v);
            const float next = v + 4.0f;
            if (to <= next) {
                emit(to);
                break;
            }
            v = next;
        }
    };
    walk(left, right, [&](float x) { sample(x, top); });
    walk(top, bottom, [&](float z) { sample(right, z); });
    walk(left, right, [&](float x) { sample(x, bottom); });
    walk(top, bottom, [&](float z) { sample(left, z); });
    return best;
}

float cSC4LotDeveloper::GetAverageAreaAltitude(const LotCellRect& area, bool includeWater) const {
    float sum = 0.0f;
    int count = 0;
    for (int x = area.x0; x <= area.x1 + 1; ++x) {
        for (int z = area.z0; z <= area.z1 + 1; ++z) {
            sum += GetTerrainAltitudeByVertex(x, z, includeWater);
            ++count;
        }
    }
    return sum / static_cast<float>(count);   // 0 / 0 = NaN for an empty rectangle, as in the original
}

// Interior vertices only (x0+1..x1, z0+1..z1). A negative height means "average of the area".
int cSC4LotDeveloper::LevelAreaInternalVertices(const LotCellRect& area, float height, bool skipLevelled, bool force,
                                                void* lot) {
    float target = height;
    if (height < 0.0f) target = GetAverageAreaAltitude(area, true);
    int changed = 0;
    for (int x = area.x0 + 1; x <= area.x1; ++x) {
        for (int z = area.z0 + 1; z <= area.z1; ++z) {
            if (skipLevelled && host_->VertexIsLevelled(x, z)) continue;
            const float current = GetTerrainAltitudeByVertex(x, z, true);
            if (force || (current - target > tunables_.levelTolerance)) {
                host_->LevelVertices(target, true, true, x, z, x, z, lot, 5, 0);
                ++changed;
            }
        }
    }
    return changed;
}

int cSC4LotDeveloper::LevelBorderingVertices(const LotCellRect& area, float height, void* lot) {
    return host_->LevelVertices(height, true, true, area.x0, area.z0, area.x1 + 1, area.z1 + 1, lot, 5, 0);
}

bool cSC4LotDeveloper::AttemptAreaLevelling(const LotCellRect& area, float /*unusedHeight*/, bool includeWater,
                                            bool flag, float maxSlope, void* lot) {
    const float target = GetAverageAreaAltitude(area, includeWater);
    if (maxSlope == kLotMaxLevelling) maxSlope = tunables_.maxLevelling;
    if (maxSlope < 1000.0f && maxSlope < GetAreaSlope(area, includeWater)) return false;
    return host_->LevelVertices(target, true, flag, area.x0, area.z0, area.x1 + 1, area.z1 + 1, lot, 5, 0) != 0;
}

void cSC4LotDeveloper::GetOccupantCellCoverage(void* occupant, LotCellRect& out) const {
    float lo[3] = {}, hi[3] = {};
    host_->OccupantBounds(occupant, lo, hi);
    lo[0] += 0.05f;
    lo[2] += 0.05f;
    hi[0] -= 0.05f;
    hi[2] -= 0.05f;
    host_->CellOfPosition(lo[0], lo[2], out.x0, out.z0);
    host_->CellOfPosition(hi[0], hi[2], out.x1, out.z1);
}

bool cSC4LotDeveloper::IsAreaFlat(const LotCellRect& area, bool includeWater) const {
    const float reference = GetTerrainAltitudeByVertex(area.x0, area.z0, true);   // always with water, as written
    for (int x = area.x0; x <= area.x1 + 1; ++x) {
        for (int z = area.z0; z <= area.z1 + 1; ++z) {
            const float h = GetTerrainAltitudeByVertex(x, z, includeWater);
            if (std::fabs(h - reference) > tunables_.flatTolerance) return false;
        }
    }
    return true;
}

float cSC4LotDeveloper::GetAreaAltitudeRange(const LotCellRect& area, bool includeWater) const {
    float lo = FLT_MAX, hi = -FLT_MAX;
    for (int x = area.x0; x <= area.x1 + 1; ++x) {
        for (int z = area.z0; z <= area.z1 + 1; ++z) {
            const float h = GetTerrainAltitudeByVertex(x, z, includeWater);
            if (h < lo) lo = h;
            if (hi < h) hi = h;
        }
    }
    return hi - lo;
}

// "Slope" is the horizontal distance (in vertex units) between the lowest and the highest vertex found.
float cSC4LotDeveloper::GetAreaSlope(const LotCellRect& area, bool includeWater) const {
    float lo = FLT_MAX, hi = -FLT_MAX;
    float loPos[2] = {}, hiPos[2] = {};
    for (int x = area.x0; x <= area.x1 + 1; ++x) {
        for (int z = area.z0; z <= area.z1 + 1; ++z) {
            const float h = GetTerrainAltitudeByVertex(x, z, includeWater);
            if (h < lo) {
                loPos[0] = static_cast<float>(x);
                loPos[1] = static_cast<float>(z);
                lo = h;
            }
            if (hi < h) {
                hiPos[0] = static_cast<float>(x);
                hiPos[1] = static_cast<float>(z);
                hi = h;
            }
        }
    }
    const float dx = hiPos[0] - loPos[0], dz = hiPos[1] - loPos[1];
    return std::sqrt(dz * dz + dx * dx);
}

std::int64_t cSC4LotDeveloper::GetLotDevelopmentCost(const LotCellRect& area) const {
    const float slope = GetAreaSlope(area, true);
    const float perCell = developmentCostCurve_.GetOutputValue(slope * 0.0625f);
    return static_cast<std::int64_t>(static_cast<float>((area.z1 + 1) - area.z0) *
                                     static_cast<float>((area.x1 + 1) - area.x0) * perCell);
}

LotCellRect cSC4LotDeveloper::RectOfLot(const cSC4Lot& lot) {
    return {lot.RectByte(0), lot.RectByte(1), lot.RectByte(2), lot.RectByte(3)};
}
bool cSC4LotDeveloper::IsLotDeveloped(const cSC4Lot& lot) const { return IsAreaDeveloped(RectOfLot(lot)); }
bool cSC4LotDeveloper::IsLotFlat(const cSC4Lot& lot, bool includeWater) const { return IsAreaFlat(RectOfLot(lot), includeWater); }
float cSC4LotDeveloper::GetLotAltitudeRange(const cSC4Lot& lot, bool includeWater) const {
    return GetAreaAltitudeRange(RectOfLot(lot), includeWater);
}
float cSC4LotDeveloper::GetLotSlope(const cSC4Lot& lot, bool includeWater) const { return GetAreaSlope(RectOfLot(lot), includeWater); }
float cSC4LotDeveloper::GetOccupantSlope(void* occupant, bool includeWater) const {
    LotCellRect area;
    GetOccupantCellCoverage(occupant, area);
    return GetAreaSlope(area, includeWater);
}

std::uint32_t cSC4LotDeveloper::TestCondition(cSC4Lot& lot) const {
    static const std::int32_t kStepDown[4] = {1, 1, 2, 3};                   // 0x100a18ec0
    const int state = lot.GetState();
    std::uint32_t condition = lot.GetCondition();
    std::int32_t grade;
    if (state == 4) {
        if (condition == 0) condition = 1;
        if (static_cast<std::uint32_t>(lot.GetDaysSinceLastStateChange()) <= tunables_.conditionThreshold) return condition;
        if (3 < condition) return condition;
        grade = kStepDown[condition];
    } else {
        if (state != 3) return condition;
        const BuildingProfile* profile = gates_ ? gates_->ProfileOfBuilding(lot.GetBuildingType(false)) : nullptr;
        std::int32_t gap = static_cast<std::int32_t>(profile ? profile->wealth : 0) - static_cast<std::int32_t>(lot.GetOccupantWealth());
        const bool behind = gap < static_cast<std::int32_t>(condition - 1);
        if (behind || tunables_.conditionThreshold < static_cast<std::uint32_t>(lot.GetDaysSinceLastStateChange())) {
            const float occupancy = lot.GetOccupancy();
            if (occupancy < tunables_.ratioB || (behind && occupancy < tunables_.ratioC)) gap = gap + 1;
        }
        grade = 2;
        if (gap < 3) grade = gap;
        if (grade == 0) {
            std::int32_t counter = 0;
            if (!lot.Properties().HasProperty(kLotPropFailureCounter)) return 0;
            cSCPropertyHelp::GetPropertyValue(&lot.Properties(), kLotPropFailureCounter, counter);
            return counter != 0 ? 1u : 0u;
        }
        if (grade < 0) return condition;
    }
    return static_cast<std::uint32_t>(grade + 1);
}

}  // namespace sc4


// ==================================================================================================== lot base textures

namespace sc4 {
namespace {
ILotBaseTextureServices* g_textureServices = nullptr;
constexpr std::uint32_t kLotBaseTextureOccupantType = 0x497f6d9d;
constexpr std::uint32_t kOccupantInsertedFlag = 1;                 // cISC4Occupant vslot 0x100 IsFlagSet(1)
constexpr std::uint32_t kMsgLotBaseTextureChanged = 0x99ef1145;    // occupant vslot 0x78 after a move
}  // namespace

void cSC4LotDeveloper::SetBaseTextureServices(ILotBaseTextureServices* services) { g_textureServices = services; }

// @ 0x100145e4c. The lot's texture occupant at its anchor cell (occupant manager vslot 0x100 by type 0x497f6d9d); with
// `create` a new one comes from GZCOM (or new), is initialised and set to usage 2. The caller gets one reference.
bool cSC4LotDeveloper::GetLotBaseTextureOccupant(cSC4Lot* lot, cISC4LotBaseTextureOccupant*& out, bool create) {
    out = nullptr;
    ILotBaseTextureServices* services = g_textureServices;
    cISC4OccupantManager* occupants = services ? services->OccupantManager() : nullptr;
    if (!lot || !occupants) return false;
    int x = 0, z = 0;
    lot->GetLocation(x, z);                                                   // lot vslot 0xa8
    cISC4Occupant* found = nullptr;
    if (occupants->GetFirstOccupantByStandardCityCell(found, x, z, kLotBaseTextureOccupantType) && found) {
        void* raw = nullptr;
        if (found->QueryInterface(kGZIID_cISC4LotBaseTextureOccupant, &raw) && raw) {
            out = static_cast<cISC4LotBaseTextureOccupant*>(raw);
            out->SetUsage(2);                                                 // vslot 0x28
        }
        found->Release();
    } else if (create) {
        out = services->CreateBaseTextureOccupant();
        if (out) {
            out->AsOccupant()->Init();                                        // vslot 0x18 -> occupant vslot 0x18
            out->SetUsage(2);
        }
    }
    return out != nullptr;
}

// @ 0x100146066. An empty zoned lot's base texture: one specification per cell, the zone's drag colour (RGB) with the zone
// display alpha and combine mode 3, and the edge texture the boundary rules pick from which sides of the cell face the lot's
// own cells (0), its front (2, the side it faces) or anything else (1). Zone types 0, 11, 12 and 15 carry no texture.
// Landfill / airport / seaport zones (7..9) whose lot reports the (1, 1) pair (lot vslot 0xb0) take plain cells.
void cSC4LotDeveloper::SetEmptyLotBaseTexture(cSC4Lot* lot) {
    ILotBaseTextureServices* services = g_textureServices;
    if (!lot || !services) return;
    const std::uint32_t zoneType = lot->GetZoneType();                       // lot vslot 0xf8
    if (zoneType < 0x10 && ((0x9801u >> zoneType) & 1u) != 0) {
        RemoveLotBaseTexture(lot);
        return;
    }
    const std::uint32_t color = services->ZoneDragColor(static_cast<int>(zoneType));
    const std::uint8_t red = static_cast<std::uint8_t>(color >> 24), green = static_cast<std::uint8_t>(color >> 16);
    const std::uint8_t blue = static_cast<std::uint8_t>(color >> 8), alpha = services->ZoneDisplayAlpha();
    const int cellsX = services->CityCellsX(), cellsZ = services->CityCellsZ();
    std::int32_t rect[4] = {};
    lot->GetBoundingRect(rect);                                               // lot vslot 0x70: x0, z0, x1, z1
    // PARTIAL: lot vslot 0xb0 (the landfill / port pair) is not modelled by the port's cSC4Lot, so zones 7..9 take edges too.
    const bool edges = true;
    const std::uint8_t facing = lot->GetFacing();                             // lot vslot 0xb8
    std::vector<LotBaseTextureSpecification> specs;
    for (int x = rect[0]; x <= rect[2]; ++x) {
        for (int z = rect[1]; z <= rect[3]; ++z) {
            if (x >= cellsX || z >= cellsZ || x < 0 || z < 0) continue;      // (unsigned) +0x88 / +0x84
            int sides[4] = {1, 1, 1, 1};
            if (!edges) {
                sides[0] = sides[1] = sides[2] = sides[3] = 0;
            } else {
                auto neighbour = [&](int nx, int nz) { return services->LotAtCell(nx, nz); };
                cSC4Lot* north = (z >= 1 && z <= cellsZ) ? neighbour(x, z - 1) : nullptr;
                if (north == lot) sides[0] = 0;
                else if (facing == 0) sides[0] = 2;
                cSC4Lot* south = (z >= -1 && z < cellsZ - 1) ? neighbour(x, z + 1) : nullptr;
                if (south == lot) sides[2] = 0;
                else if (facing == 2) sides[2] = 2;
                cSC4Lot* west = (x <= cellsX && x >= 1) ? neighbour(x - 1, z) : nullptr;
                if (west == lot) sides[3] = 0;
                else if (facing == 3) sides[3] = 2;
                cSC4Lot* east = (x < cellsX - 1 && x >= -1) ? neighbour(x + 1, z) : nullptr;
                if (east == lot) sides[1] = 0;
                else if (facing == 1) sides[1] = 2;
            }
            LotBaseTextureSpecification spec;
            spec.cellX = static_cast<std::uint8_t>(x);
            spec.cellZ = static_cast<std::uint8_t>(z);
            bool flip = false;
            services->BoundaryLotTexture(sides, spec.textureId, spec.rotation, flip);   // this vslot 0x150
            spec.flip = flip ? 1 : 0;
            spec.red = red;
            spec.green = green;
            spec.blue = blue;
            spec.alpha = alpha;
            spec.combineMode = 3;
            spec.frame = 0xff;
            specs.push_back(spec);
        }
    }
    cISC4LotBaseTextureOccupant* texture = nullptr;
    if (GetLotBaseTextureOccupant(lot, texture, true)) {                      // this vslot 0x148
        texture->SetTextureSpecification(specs.data(), static_cast<std::uint32_t>(specs.size()), nullptr);   // vslot 0x40
        cISC4Occupant* occupant = texture->AsOccupant();
        cISC4OccupantManager* occupants = services->OccupantManager();
        if (occupants) {
            if (!occupant->IsFlagSet(kOccupantInsertedFlag)) {
                occupants->InsertOccupant(occupant, 0);                       // vslot 0xa0
            } else {
                occupants->MoveOccupant(occupant, false);                     // vslot 0xc0
                occupant->PostOccupantMessage(kMsgLotBaseTextureChanged, 0);
            }
        }
        texture->Release();
    }
}

// @ 0x10014652e (with GetLotBaseTextures @ 0x100146b10 and RemoveAndReleaseOccupants @ 0x100146b72): every texture
// occupant over the lot's cells leaves the occupant manager, is shut down and released.
void cSC4LotDeveloper::RemoveLotBaseTexture(cSC4Lot* lot) {
    ILotBaseTextureServices* services = g_textureServices;
    cISC4OccupantManager* occupants = services ? services->OccupantManager() : nullptr;
    if (!lot || !occupants) return;
    std::int32_t rect[4] = {};
    lot->GetBoundingRect(rect);
    OccupantList list;
    if (!occupants->GetOccupantsByStandardCityCells(list, rect, rect + 2, kLotBaseTextureOccupantType, 0xffffffffu)) return;
    for (cISC4Occupant* occupant : list) {
        if (occupant->IsFlagSet(kOccupantInsertedFlag)) occupants->RemoveOccupant(occupant, false, 0);   // vslot 0xa8
        occupant->Shutdown();                                                 // vslot 0x20
        occupant->Release();
    }
}

// ------------------------------------------------------------------------------------------------ construction

namespace {
ILotConstructionServices* g_construction = nullptr;
}  // namespace

void cSC4LotDeveloper::SetConstructionServices(ILotConstructionServices* services) { g_construction = services; }

// ------------------------------------------------------------------------------------------------ demolition

// Slot 38 (i386 0x000f12cc): the first building occupant on the lot's cells, not referenced for the caller.
cISC4Occupant* cSC4LotDeveloper::GetFirstBuilding(cSC4Lot* lot) {
    return lot && g_construction ? g_construction->FirstBuildingOnLot(lot) : nullptr;
}

// i386 0x000ec5f6: the lot takes `state`, then every building on it goes (its props, optionally its foundation, then the
// occupant itself with `flags`). PARTIAL: building foundations (slot 60) and the zot props (RemoveBuildingZotProp) are not
// ported, as in StartConstruction.
bool cSC4LotDeveloper::ClearLotBuildings(cSC4Lot* lot, std::uint32_t state, bool removeFoundation, std::uint32_t flags) {
    if (!lot || !g_construction) return false;
    (void)removeFoundation;
    cISC4Occupant* building = GetFirstBuilding(lot);
    lot->SetState(state, 0, 0);                                                     // lot vslot 0x88
    for (int guard = 0; building && guard < 64; ++guard) {
        building->AddRef();
        g_construction->RemoveBuildingProps(building);                                // prop manager slot 38
        g_construction->RemoveOccupant(building, flags);                              // occupant manager slot 21 (b, 1, flags)
        building->Release();
        building = GetFirstBuilding(lot);
    }
    return true;
}

// Slot 12 (i386 0x000f1b84): the flora of a lot with flag 1, the lot's props, its network connection nodes and its buildings
// (state 5, foundations too). PARTIAL: RemoveNetworkConnectionNodes (the lot's network pieces with flag 0x200000 handed to the
// network manager) is not ported.
bool cSC4LotDeveloper::StartDemolishLot(cSC4Lot* lot) {
    if (!lot || !g_construction) return false;
    if (lot->GetFlags() & 1) g_construction->RemoveLotFlora(lot);                   // lot vslot 0x30
    g_construction->RemoveLotProps(lot);
    return ClearLotBuildings(lot, 5, true, 0);
}

// i386 0x000f385c: the lot goes back to an empty lot of its zone. PARTIAL: the retaining walls and the lot / surrounding
// foundations (no foundation occupants yet).
void cSC4LotDeveloper::ClearLot(cSC4Lot* lot) {
    lot->SetVerticesAreLocked(false);                                               // lot vslot 0x114
    lot->SetState(1, 0, 0);                                                         // 0x88
    lot->SetBuildingType(0);                                                        // 0xbc
    lot->SetLotConfiguration(nullptr);                                              // 0x28
    SetEmptyLotBaseTexture(lot);                                                    // this vslot 0xac
}

// Slot 13 (i386 0x000f392c): an initialized lot becomes state 6, is cleared, and a lot with no zone left (0, 11, 12, 15:
// plopped, landfill, military, ...) is deleted from the lot manager.
void cSC4LotDeveloper::EndDemolishLot(cSC4Lot* lot) {
    if (!lot || !lot->IsInitialized()) return;                                      // lot vslot 0x18
    lot->SetState(6, 0, 0);
    ClearLot(lot);
    const std::uint32_t zone = lot->GetZoneType();                                  // lot vslot 0x7c
    if ((zone == 0 || zone == 15 || zone == 11 || zone == 12) && g_construction) g_construction->DeleteLot(lot);
}

// Slot 14 (lot-developer.md §7.2): a destroyed lot keeps rubble. PARTIAL: the burnt-building prop, the terrain decal and the
// rubble props (AddLotRubbleProps) are not ported; the buildings, props and the building type go as in the original.
void cSC4LotDeveloper::ExplodeLot(cSC4Lot* lot, bool burnt) {
    if (!lot || lot->GetState() == 5 || lot->GetState() == 6) return;
    ClearLotBuildings(lot, 6, false, 4);
    if (g_construction) g_construction->RemoveLotProps(lot);
    if (burnt) lot->SetCondition(6);
    lot->SetBuildingType(0);
}

std::uint32_t cSC4LotDeveloper::FirstBuildingTypeOf(const std::vector<std::uint32_t>& ids, const std::function<bool(std::uint32_t)>& isType,
                                                    const std::function<void(std::vector<std::uint32_t>&, std::uint32_t)>& familyMembers) {
    std::vector<std::uint32_t> set;   // insertion order, unique
    auto insert = [&set](std::uint32_t id) { if (std::find(set.begin(), set.end(), id) == set.end()) set.push_back(id); };
    for (std::uint32_t id : ids) {
        if (isType(id)) {
            insert(id);
        } else {
            std::vector<std::uint32_t> members;
            familyMembers(members, id);
            for (std::uint32_t member : members) insert(member);
        }
    }
    std::size_t best = set.size();
    for (std::size_t k = 0; k < set.size(); ++k)
        if (best == set.size() || set[k] % 193u <= set[best] % 193u) best = k;   // ties: the later insert heads the bucket
    return best < set.size() ? set[best] : 0;
}

// ConstructDefaultLot (i386 0x000f30f0; vslot 0x38): with no building type given, the type is FirstBuildingTypeOf the primary
// building object's ids; then StartConstruction. No ids, or none resolving, constructs with type 0.
bool cSC4LotDeveloper::ConstructDefaultLot(cSC4Lot* lot, cSC4LotConfiguration* configuration, std::uint32_t buildingType,
                                           bool construct) {
    if (configuration && buildingType == 0) {
        const cSC4LotConfigurationObject* object = configuration->GetPrimaryBuildingConfigurationObject();
        if (object && !object->extra.empty() && g_construction) {
            buildingType = FirstBuildingTypeOf(
                object->extra, [](std::uint32_t id) { return g_construction->BuildingTypeValid(id); },
                [](std::vector<std::uint32_t>& out, std::uint32_t family) { g_construction->BuildingTypesInFamily(out, family); });
        }
    }
    return StartConstruction(lot, configuration, buildingType, construct);
}

// @ i386 0x000f48d0 (x64 0x1001434f8). The lot takes the configuration and building, goes to state 2 (under construction) with
// condition 1 and locked vertices; the footprint is levelled (or given a foundation) when it is steeper than the tunable allows,
// the blocking flora is demolished and the building is placed through the building development simulator. With `construct` a
// construction occupant takes the lot through its stages, else the lot is built at once (state 3). Returns false when the
// building could not be placed.
//   PARTIAL (each named where the original calls it): PickUnitToJoin (business units), RemoveBuildingZotProp, the lot and building
//   foundations (AddLotFoundation vslot 0xdc, AddBuildingFoundation vslot 0xec: no foundation occupants yet, so a steep lot keeps
//   the levelling it got).
bool cSC4LotDeveloper::StartConstruction(cSC4Lot* lot, cSC4LotConfiguration* configuration, std::uint32_t buildingType,
                                         bool construct) {
    if (!lot || !g_construction) return false;
    lot->AddRef();
    lot->SetLotConfiguration(g_construction->ConfigurationRef(configuration));     // lot vslot 0x28
    lot->SetBuildingType(buildingType);                                             // 0xbc
    lot->SetState(2, 0, 0);                                                         // 0x88
    lot->SetCondition(1);                                                           // 0x90
    // PARTIAL: PickUnitToJoin(lot) (i386 0x000f3dbc) joins a commercial / industrial lot to a business unit; not ported.
    lot->SetFlags(lot->GetFlags() & ~7);                                            // 0x30 / 0x34
    lot->SetVerticesAreLocked(true);                                                // 0x114
    // PARTIAL: RemoveBuildingZotProp(lot, 6) (i386 0x000f0684): the lot's zot props are not ported.
    bool result = true;
    if (buildingType != 0) {
        const std::int32_t purpose = g_construction->BuildingPurpose(buildingType);   // +0x18 vslot 0x9c
        const bool water = configuration && configuration->IsCompatibleWithWater();   // configuration vslot 0x38
        bool lotFoundation = false;
        if (!(water && configuration->GetMaximumSlopeBeforeFoundation() == 0.0f && AddLotFoundation(lot, configuration))) {
            if (purpose != 5 && tunables_.flag139) {                                // +0xe1
                const LotCellRect rect = RectOfLot(*lot);
                if (rect.x1 - rect.x0 > 1 && rect.z1 - rect.z0 > 1) LevelAreaInternalVertices(rect, -1.0f, water, false, lot);   // vslot 0x4c
            }
            const float slope = GetLotSlope(*lot, water);                           // vslot 0x78
            if (tunables_.value12c < slope) {                                       // +0xd4
                const float maximum = configuration ? configuration->GetMaximumSlopeBeforeFoundation() : 0.0f;   // configuration vslot 0x28
                if (maximum < slope) {
                    bool levelled = false;
                    if (purpose != 5 && tunables_.flag138) {                        // +0xe0
                        const LotCellRect rect = RectOfLot(*lot);
                        levelled = AttemptAreaLevelling(rect, maximum, true, water, tunables_.maxLevelling, lot) &&   // vslot 0x54, +0xe8
                                   GetLotSlope(*lot, water) <= maximum;
                    }
                    if (!levelled) lotFoundation = AddLotFoundation(lot, configuration);   // vslot 0xdc
                }
            }
        } else {
            lotFoundation = true;
        }
        ClearLotBlockingObjects(lot);
        float position[3] = {0.0f, 0.0f, 0.0f};
        std::int32_t rotation = 0;
        GetPositionForBuilding(lot, position, rotation);                            // vslot 0xa0
        cISC4BuildingOccupant* building = g_construction->AddNewBuildingOccupant(buildingType, position, rotation);   // +0x18 vslot 0x1c
        if (!building) {
            result = false;
        } else {
            if (!lotFoundation) {
                void* occupant = building->AsOccupant();                            // building vslot 0xc
                float buildingSlope = GetOccupantSlope(occupant, water);            // vslot 0x7c
                if (tunables_.value12c < buildingSlope) {
                    LotCellRect rect;
                    GetOccupantCellCoverage(occupant, rect);                        // vslot 0x58
                    if (AttemptAreaLevelling(rect, tunables_.value12c, true, water, tunables_.maxLevelling, lot))
                        buildingSlope = GetOccupantSlope(occupant, water);
                    if (tunables_.value12c < buildingSlope) AddBuildingFoundation(building, configuration);   // vslot 0xec
                }
            }
            if (construct) {
                g_construction->StartConstructionOccupant(lot);
                lot->Release();
                return true;
            }
        }
    } else if (construct) {
        lot->Release();
        return true;
    }
    if (!construct) lot->SetState(3, 0, 0);
    lot->Release();
    return result;
}

// i386 DoMessage 0xe8bda2e7 (x64 0x10014e...): the lot is looked up at the snapshot's location and must still have its size.
//   PARTIAL: AddNetworkConnectionNodes (0x000f14ee) is not ported.
void cSC4LotDeveloper::DoLotStateChanged(const cSC4Lot& snapshot, std::uint32_t oldState, std::uint32_t newState) {
    const bool built = newState - 3 < 2;
    const bool finished = oldState == 2 && newState == 3;
    if (std::getenv("SC4_SIMDEBUG")) std::fprintf(stderr, "lot developer: lot state %u -> %u\n", oldState, newState);
    if ((!built && !finished) || !g_construction) return;
    int x = 0, z = 0;
    snapshot.GetLocation(x, z);                                                     // message lot vslot 0x54
    cSC4Lot* lot = g_construction->LotAt(x, z);
    if (!lot) return;
    cSC4LotConfiguration* configuration = g_construction->Configuration(lot->GetLotConfiguration());   // vslot 0x24
    if (!configuration) return;
    std::uint8_t sizeX = 0, sizeZ = 0, snapshotX = 0, snapshotZ = 0;
    lot->GetSize(sizeX, sizeZ);                                                     // vslot 0x58
    snapshot.GetSize(snapshotX, snapshotZ);
    if (sizeX != snapshotX || sizeZ != snapshotZ) return;
    if (built) SetNormalLotBaseTexture(lot, configuration);                         // vslot 0xb0 (lot, configuration, 1)
    if (finished) {
        AddLotProps(lot, configuration);
        AddLotFlora(lot, configuration);
    }
}

// @ i386 0x000ef4c0. Every prop object (type 1) of the configuration that is not flagged (bit 0) becomes a prop.
void cSC4LotDeveloper::AddLotProps(cSC4Lot* lot, cSC4LotConfiguration* configuration) {
    if (!configuration) return;
    const std::vector<cSC4LotConfigurationObject>& objects = configuration->GetLotConfigurationObjectArray();   // vslot 0x5c
    for (std::uint32_t i = 0; i < objects.size(); ++i)
        if (objects[i].type == 1 && (objects[i].flags & 1) == 0) SelectAndCreateLotProp(lot, i);
}

// @ i386 0x000f2ffe. Every flora object (type 4) becomes a tree of a type PickFloraTypeForLot draws, tagged with the object's id.
void cSC4LotDeveloper::AddLotFlora(cSC4Lot* lot, cSC4LotConfiguration* configuration) {
    if (!lot || !configuration || !g_construction) return;
    for (const cSC4LotConfigurationObject& object : configuration->GetLotConfigurationObjectArray()) {   // vslot 0x5c
        if (object.type != 4) continue;
        float position[3] = {0.0f, 0.0f, 0.0f};
        std::int32_t rotation = 0;
        GetPositionForLotConfigurationObject(lot, configuration, &object, position, rotation);   // vslot 0x9c
        const std::uint32_t type = PickFloraTypeForLot(object.extra);
        if (type == 0) continue;
        if (cISC4Occupant* flora = g_construction->AddFlora(type, position)) flora->SetLotTag(object.id);   // occupant vslot 0x70
    }
}

// @ i386 0x000f2efa. One of the object's ids at random: a flora type as it is, else a family whose members are drawn from at random.
std::uint32_t cSC4LotDeveloper::PickFloraTypeForLot(const std::vector<std::uint32_t>& types) {
    if (types.empty() || !g_construction) return 0;
    const std::uint32_t id = types[g_construction->RandomBelow(static_cast<std::uint32_t>(types.size()))];
    if (g_construction->FloraTypeValid(id)) return id;
    std::vector<std::uint32_t> family;
    g_construction->FloraTypesInFamily(family, id);
    if (family.empty()) return 0;
    return family[g_construction->RandomBelow(static_cast<std::uint32_t>(family.size()))];
}

// @ i386 0x000ef38a. One of the object's prop types at random, placed where the object lands on the lot and tagged with the
// object's id; null when the object has no prop type or the prop could not be added.
cISC4Occupant* cSC4LotDeveloper::SelectAndCreateLotProp(cSC4Lot* lot, std::uint32_t objectIndex) {
    if (!lot || !g_construction) return nullptr;
    cSC4LotConfiguration* configuration = g_construction->Configuration(lot->GetLotConfiguration());   // lot vslot 0x24
    if (!configuration) return nullptr;
    const std::vector<cSC4LotConfigurationObject>& objects = configuration->GetLotConfigurationObjectArray();
    if (objectIndex >= objects.size()) return nullptr;
    const cSC4LotConfigurationObject& object = objects[objectIndex];
    float position[3] = {0.0f, 0.0f, 0.0f};
    std::int32_t rotation = 0;
    GetPositionForLotConfigurationObject(lot, configuration, &object, position, rotation);   // vslot 0x9c
    std::vector<std::uint32_t> types;
    g_construction->ObjectPropTypes(object, types);
    if (types.empty()) return nullptr;
    const std::uint32_t type = types[g_construction->RandomBelow(static_cast<std::uint32_t>(types.size()))];
    if (type == 0) return nullptr;
    cISC4Occupant* prop = g_construction->AddLotProp(type, position, rotation);
    if (prop) prop->SetLotTag(object.id);                                           // occupant vslot 0x70
    if (std::getenv("SC4_SIMDEBUG")) std::fprintf(stderr, "lot developer: prop %08x %s\n", type, prop ? "added" : "FAILED");
    return prop;
}

// @ i386 0x000f2172. One texture per base-texture object (type 2) of the configuration, at the cell the object lands on, turned
// with the lot, picked for the lot's wealth and condition; the lot's texture occupant takes them (inserted, or moved and
// announced when it is already in). With forced empty textures a lot outside zones 0, 11, 12 and 15 shows its zone instead.
void cSC4LotDeveloper::SetNormalLotBaseTexture(cSC4Lot* lot, cSC4LotConfiguration* configuration) {
    if (!lot || !configuration) return;
    if (forceEmptyLotBaseTextures_) {                                               // +0x5c
        const std::uint32_t zoneType = lot->GetZoneType();                          // lot vslot 0x7c
        if (zoneType > 0xf || ((1u << zoneType) & 0x9801u) == 0) {
            SetEmptyLotBaseTexture(lot);                                            // vslot 0xac
            return;
        }
    }
    ILotBaseTextureServices* services = g_textureServices;
    if (!services) return;
    std::vector<LotBaseTextureSpecification> specs;
    for (const cSC4LotConfigurationObject& object : configuration->GetLotConfigurationObjectArray()) {   // configuration vslot 0x5c
        if (object.type != 2) continue;
        float position[3] = {0.0f, 0.0f, 0.0f};
        std::int32_t rotation = 0;
        GetPositionForLotConfigurationObject(lot, configuration, &object, position, rotation);   // vslot 0x9c
        if (object.extra.empty()) continue;
        std::uint32_t texture = 0;
        if (object.extra.front() != 0) {
            texture = PickLotBaseTextureForLot(lot, object.extra);
            if (texture == 0) continue;
        }
        LotBaseTextureSpecification spec;
        spec.textureId = texture;
        spec.cellX = static_cast<std::uint8_t>(static_cast<int>(position[0] * 0.0625f));
        spec.cellZ = static_cast<std::uint8_t>(static_cast<int>(position[2] * 0.0625f));
        spec.rotation = static_cast<std::uint8_t>(rotation);
        spec.flip = object.mirrored;
        spec.red = spec.green = spec.blue = spec.alpha = 0xff;
        spec.combineMode = 0xff;
        spec.frame = 0xff;
        if (spec.cellX < static_cast<unsigned>(services->CityCellsX()) && spec.cellZ < static_cast<unsigned>(services->CityCellsZ()))   // +0x50 / +0x4c
            specs.push_back(spec);
    }
    if (std::getenv("SC4_SIMDEBUG")) {
        std::fprintf(stderr, "lot developer: base textures %08x:", configuration->GetID());
        for (const LotBaseTextureSpecification& spec : specs) std::fprintf(stderr, " %08x@%u,%u/r%u", spec.textureId, spec.cellX, spec.cellZ, spec.rotation);
        std::fprintf(stderr, "\n");
    }
    cISC4LotBaseTextureOccupant* texture = nullptr;
    if (specs.empty() || !GetLotBaseTextureOccupant(lot, texture, true)) return;   // vslot 0xa4
    texture->SetTextureSpecification(specs.data(), static_cast<std::uint32_t>(specs.size()), nullptr);   // vslot 0x20
    cISC4Occupant* occupant = texture->AsOccupant();
    if (cISC4OccupantManager* occupants = services->OccupantManager()) {
        if (!occupant->IsFlagSet(kOccupantInsertedFlag)) {
            occupants->InsertOccupant(occupant, 0);                                 // +0x28 vslot 0x50
        } else {
            occupants->MoveOccupant(occupant, false);                               // vslot 0x60
            occupant->PostOccupantMessage(kMsgLotBaseTextureChanged, 0);            // vslot 0x3c
        }
    }
    texture->Release();
}

// @ i386 0x000ef182. One of the object's textures at random, refined by the lot's wealth (bits 12..15: the occupant wealth, else the
// building's, else 1) and condition (bits 8..11: 0 below 2, 1 for 2..3, 2 for 4..6); the most refined instance that exists wins.
std::uint32_t cSC4LotDeveloper::PickLotBaseTextureForLot(cSC4Lot* lot, const std::vector<std::uint32_t>& textures) {
    if (textures.empty() || !g_construction) return 0;
    const std::uint32_t base = textures[g_construction->RandomBelow(static_cast<std::uint32_t>(textures.size()))];
    std::int32_t wealth = lot->GetOccupantWealth();                                 // lot vslot 0xa4
    const std::int32_t condition = lot->GetCondition();                             // lot vslot 0x8c
    std::uint32_t conditionBits = 0;
    if (condition >= 2 && condition < 4) conditionBits = 0x100;
    else if (condition >= 4 && condition <= 6) conditionBits = 0x200;
    std::uint32_t wealthBits;
    if (wealth == 0) {
        const std::uint32_t type = lot->GetBuildingType(false);                     // lot vslot 0xb8
        if (type == 0) {
            wealthBits = 0x1000;
        } else {
            wealthBits = g_construction->BuildingWealth(type) << 12;                // +0x18 vslot 0x9c -> profile +4
        }
    } else {
        wealthBits = static_cast<std::uint32_t>(wealth) << 12;
    }
    const std::uint32_t family = base & 0xffff0000u;
    std::uint32_t candidate = family | wealthBits | conditionBits;
    if (g_construction->LotTextureExists(candidate)) return candidate;
    candidate = family | wealthBits;
    if (g_construction->LotTextureExists(candidate)) return candidate;
    candidate = family | conditionBits;
    if (g_construction->LotTextureExists(candidate)) return candidate;
    return base;
}

// PARTIAL: AddLotFoundation (vslot 0xdc, i386 0x000ef2d0 -> 0x000f363a) needs the foundation occupant; no foundation is made.
bool cSC4LotDeveloper::AddLotFoundation(cSC4Lot*, cSC4LotConfiguration*) { return false; }
// PARTIAL: AddBuildingFoundation (vslot 0xec, i386 0x000ecdc2) likewise.
void cSC4LotDeveloper::AddBuildingFoundation(cISC4BuildingOccupant*, cSC4LotConfiguration*) {}

// @ i386 0x000f044a. The flora on the lot (the blocking-lot filter, occupant type 0x74758926) inside the lot's cells, inset by half
// a metre, from the ground to 32 m above the lot's highest vertex, is demolished.
void cSC4LotDeveloper::ClearLotBlockingObjects(cSC4Lot* lot) {
    if (!lot || !g_construction) return;
    const LotCellRect rect = RectOfLot(*lot);                                       // lot vslot 0x38
    const float highest = GetLowestOrHighestAreaAltitude(rect, false, true);        // vslot 0x40
    const float minimum[3] = {static_cast<float>(rect.x0 << 4) + 0.5f, 0.0f, static_cast<float>(rect.z0 << 4) + 0.5f};
    const float maximum[3] = {static_cast<float>(rect.x1 * 16 + 16) - 0.5f, highest + 32.0f, static_cast<float>(rect.z1 * 16 + 16) - 0.5f};
    g_construction->DemolishBlockingOccupants(minimum, maximum);
}

// @ i386 0x000ec6de. The position of the lot configuration's primary building (configuration vslot 0x60).
void cSC4LotDeveloper::GetPositionForBuilding(cSC4Lot* lot, float position[3], std::int32_t& rotation) {
    if (!lot || !g_construction) return;
    cSC4LotConfiguration* configuration = g_construction->Configuration(lot->GetLotConfiguration());   // lot vslot 0x24
    if (!configuration) return;
    const cSC4LotConfigurationObject* object = configuration->GetPrimaryBuildingConfigurationObject();
    if (object) GetPositionForLotConfigurationObject(lot, configuration, object, position, rotation);   // vslot 0x9c
}

// @ i386 0x000eeb38. The object's lot-space position turned by the lot's facing into city metres (the lot's corners from the city,
// vslot 0x180); a base texture (type 2) is snapped to its cell centre first. The height is the lot's altitude when the lot is at a
// fixed altitude (flag bit 0), else the lot foundation's top when there is one, the terrain under the point for props, base
// textures and flora (types 1, 2, 4), or the highest point under the object's turned rectangle. The rotation is the object's
// orientation plus the facing, modulo 4.
void cSC4LotDeveloper::GetPositionForLotConfigurationObject(cSC4Lot* lot, cSC4LotConfiguration* configuration,
                                                            const cSC4LotConfigurationObject* object, float position[3],
                                                            std::int32_t& rotation) {
    if (!lot || !object || !g_construction) return;
    std::int32_t cells[4];
    lot->GetBoundingRect(cells);                                                    // lot vslot 0x38
    float minX, minZ, maxX, maxZ;
    g_construction->CellCornerToPosition(cells[0], cells[1], minX, minZ);
    g_construction->CellCornerToPosition(cells[2] + 1, cells[3] + 1, maxX, maxZ);
    const int facing = lot->GetFacing();                                            // lot vslot 0x5c
    float x = object->x;
    const float y = object->y;
    float z = object->z;
    if (object->type == 2) {
        x = (static_cast<float>(static_cast<int>(0.0625f * x)) + 0.5f) * 16.0f;
        z = (static_cast<float>(static_cast<int>(z * 0.0625f)) + 0.5f) * 16.0f;
    }
    // the object's rectangle (+0x10 maxX, +0x14 maxZ, +0x18 minX, +0x1c minZ) turned like the position
    LotMetreRect area;
    switch (facing) {
        case 0:
            position[0] = maxX - x;
            position[2] = maxZ - z;
            area = {maxX - object->maxX, maxZ - object->maxZ, maxX - object->minX, maxZ - object->minZ};
            break;
        case 1:
            position[0] = z + minX;
            position[2] = maxZ - x;
            area = {object->minZ + minX, maxZ - object->maxX, object->maxZ + minX, maxZ - object->minX};
            break;
        case 2:
            position[0] = x + minX;
            position[2] = z + minZ;
            area = {object->minX + minX, object->minZ + minZ, object->maxX + minX, object->maxZ + minZ};
            break;
        case 3:
            position[0] = maxX - z;
            position[2] = x + minZ;
            area = {maxX - object->maxZ, object->minX + minZ, maxX - object->minZ, object->maxX + minZ};
            break;
        default:
            position[0] = 0.0f;
            position[2] = 0.0f;
            area = {0.0f, 1.0f, 0.0f, 1.0f};
            break;
    }
    if ((lot->GetFlags() & 1) != 0) {                                               // lot vslot 0x30
        position[1] = lot->GetAltitude();                                           // lot vslot 0x40
    } else {
        // PARTIAL: GetLotFoundation(lot) (vslot 0xcc) and the foundation top: no foundation occupants yet.
        const bool water = configuration && configuration->IsCompatibleWithWater();
        if (object->type > 4 || ((1u << object->type) & 0x16u) == 0)
            position[1] = GetLowestOrHighestAreaAltitude(area, false, water);      // vslot 0x44
        else
            position[1] = g_construction->TerrainAltitude(position[0], position[2]);   // +0x3c vslot 0x30
    }
    position[1] += y;
    rotation = (static_cast<std::int32_t>(object->orientation) + facing) % 4;
    if (rotation < 0) rotation += 4;
    if (std::getenv("SC4_SIMDEBUG"))
        std::fprintf(stderr, "lot developer: object type %u at (%.1f,%.1f,%.1f) orientation %u, lot (%d,%d)-(%d,%d) facing %d -> (%.1f,%.1f,%.1f) rotation %d\n",
                     object->type, object->x, object->y, object->z, object->orientation, cells[0], cells[1], cells[2], cells[3], facing,
                     position[0], position[1], position[2], rotation);
}

}  // namespace sc4


// ==================================================================================================== the daily tests and zots

namespace sc4 {
namespace {
// A lot's failure counter (a uint32 property, created as 0 on first use).
std::uint32_t Counter(cSC4Lot& lot, std::uint32_t id) {
    std::uint32_t value = 0;
    if (!lot.Properties().HasProperty(id)) cSCPropertyHelp::SetPropertyValue(&lot.Properties(), id, value);
    else cSCPropertyHelp::GetPropertyValue(&lot.Properties(), id, value);
    return value;
}
void SetCounter(cSC4Lot& lot, std::uint32_t id, std::uint32_t value) { cSCPropertyHelp::SetPropertyValue(&lot.Properties(), id, value); }
}  // namespace

// §5.1: a livable lot whose developer finds the cell below the abandon threshold is tolerated for 3..9 visits in a row, the
// tolerance fixed per location by a generator seeded with it.
bool cSC4LotDeveloper::TestDesirability(cSC4Lot& lot, int x, int z) {
    const std::uint32_t count = Counter(lot, kLotPropDesirabilityCounter);
    if (lot.IsInLivableState() && g_daily && gates_) {                                  // lot vslot 0x94
        const BuildingProfile* profile = gates_->ProfileOfBuilding(lot.GetBuildingType(false));
        std::int32_t developerPurpose = 0;
        std::int16_t desirability = 0;
        if (profile && g_daily->DeveloperDesirability(profile->purpose, lot.GetOccupantWealth(), x, z, developerPurpose, desirability)) {
            if (developerPurpose == 5) return true;                                    // farms: the counter is left alone
            if (static_cast<float>(desirability) / 100.0f < tunables_.ratioA) {
                cRZRandom random(static_cast<std::uint32_t>(z + x * 0x10000));
                const std::int32_t tolerance = random.RandomSint32RangeUniform(0, 7);
                SetCounter(lot, kLotPropDesirabilityCounter, count + 1);
                return count + 1 <= static_cast<std::uint32_t>(tolerance + 2);
            }
        }
    }
    SetCounter(lot, kLotPropDesirabilityCounter, 0);
    return true;
}

// §5.2: tested at the building's centre cell (the lot's own cell without a building); two unpowered visits are tolerated.
int cSC4LotDeveloper::TestPower(cSC4Lot& lot, int x, int z) {
    const std::uint32_t count = Counter(lot, kLotPropFailureCounter);
    float lo[3] = {}, hi[3] = {};
    const bool hasBuilding = g_daily && g_daily->BuildingBounds(lot, lo, hi);           // lot vslot 0xc0
    const bool needs = !hasBuilding || lot.GetNeedsPower();                            // vslot 0x104
    bool powered = true;
    if (g_daily && !g_daily->CheatIgnorePower() && needs) {
        int cellX = x, cellZ = z;
        if (hasBuilding) {
            float widthX = 16.0f, widthZ = 16.0f;
            g_daily->CellWidths(widthX, widthZ);
            cellX = static_cast<int>((lo[0] + hi[0]) / (widthX + widthX));
            cellZ = static_cast<int>((lo[2] + hi[2]) / (widthZ + widthZ));
        }
        powered = g_daily->IsPowered(cellX, cellZ);
    }
    if (powered != lot.GetHasPower()) {                                                  // vslot 0x108
        lot.SetHasPower(powered);                                                        // vslot 0x10c
        if (powered) {
            if (lot.GetCondition() == 1) lot.SetCondition(0);
        } else if (lot.GetCondition() == 0) {
            lot.SetCondition(1);
        }
    }
    if (!powered) {
        SetCounter(lot, kLotPropFailureCounter, count + 1);
        AddBuildingZotProp(lot, 0x0fd10000, 4);
        return count + 1 < 3 ? 1 : 0;
    }
    SetCounter(lot, kLotPropFailureCounter, 0);
    if (hasBuilding) RemoveBuildingZotProp(lot, 4);
    return 2;
}

// §5.3: an occupied or abandoned lot needs water when it is civic or its growth stage is above its developer's water cap; tested at
// the lot's own cell.
bool cSC4LotDeveloper::TestWater(cSC4Lot& lot, int x, int z) {
    const std::uint32_t count = Counter(lot, kLotPropWaterCounter);
    bool needs = false;
    if (static_cast<std::uint32_t>(lot.GetState()) - 2u < 3) {
        const BuildingProfile* profile = gates_ ? gates_->ProfileOfBuilding(lot.GetBuildingType(false)) : nullptr;
        if (!profile || profile->purpose == 0) {
            needs = true;
        } else {
            std::uint8_t cap = 0, stage = 0;
            if (g_daily && g_daily->DeveloperWaterSupplyCap(profile->purpose, profile->wealth, cap) && g_daily->LotGrowthStage(lot, stage))
                needs = cap < stage;
        }
    }
    if (needs && g_daily && !g_daily->CheatIgnorePlumbing() && !g_daily->IsCellWatered(x, z)) {
        SetCounter(lot, kLotPropWaterCounter, count + 1);
        AddBuildingZotProp(lot, 0x1c430000, 5);
        return count + 1 < 3;
    }
    SetCounter(lot, kLotPropWaterCounter, 0);
    RemoveBuildingZotProp(lot, 5);
    return true;
}

// §5.4: road access (and, for residences, a job reachable and a trip length) for a lot under construction, occupied or abandoned.
// Road and commute share one counter: no road is tolerated twice; an occupied residence without a commute shows the zot from the
// third visit and fails on the seventh.
bool cSC4LotDeveloper::TestTravel(cSC4Lot& lot, const BuildingProfile& profile, int x, int z) {
    const int state = lot.GetState();
    bool keepCounter = false;
    if (static_cast<std::uint32_t>(state) - 3u < 2 || state == 1) {
        const std::uint32_t purpose = profile.purpose;
        bool road = false, tripOk = true, test = true;
        if (purpose == 1) {
            road = lot.HasRoadAccess() && lot.HasJobAccess();                           // vslots 0x70 / 0x6c
            tripOk = g_daily && g_daily->TripLengthAt(x, z) != 0;
        } else if (purpose - 5u <= 3) {
            road = lot.HasOrganicRoadAccess(purpose);                                    // vslot 0x74
        } else {
            float jobs[4] = {};
            lot.GetJobs(jobs);                                                           // vslot 0x100 (unused)
            if (!(lot.GetTravelJobs(1) > 0) && !(lot.GetTravelJobs(2) > 0) && !(lot.GetTravelJobs(3) > 0)) test = false;
            else road = lot.HasRoadAccess();
        }
        if (test && (!road || !tripOk)) {
            const std::uint32_t count = Counter(lot, kLotPropTravelCounter) + 1;
            static const bool trace = std::getenv("SC4_LOTDEBUG") != nullptr;
            static int shown = 0;
            if (trace && shown < 200 && ++shown)
                std::fprintf(stderr, "lot travel: (%d,%d) purpose %u state %d road %d job %d trip %d count %u\n", x, z, purpose, state,
                             int(lot.HasRoadAccess()), int(lot.HasJobAccess()), g_daily ? int(g_daily->TripLengthAt(x, z)) : -1, count);
            SetCounter(lot, kLotPropTravelCounter, count);
            if (!road) {
                AddBuildingZotProp(lot, 0x107a0000, 6);
                return count < 3;
            }
            if (state == 3) {
                if (count > 6) { AddBuildingZotProp(lot, 0x1c440000, 7); return false; }
                if (count > 2) { AddBuildingZotProp(lot, 0x1c440000, 7); return true; }
            }
            RemoveBuildingZotProp(lot, 6);
            keepCounter = true;
        }
    }
    if (!keepCounter) {
        if (lot.Properties().HasProperty(kLotPropTravelCounter)) SetCounter(lot, kLotPropTravelCounter, 0);
        RemoveBuildingZotProp(lot, 6);
    }
    RemoveBuildingZotProp(lot, 7);
    return true;
}

// §5.6: lower zots win (power 4 < water 5 < road 6 < commute 7); the prop floats 3 m over the building (or the lot's centre).
void cSC4LotDeveloper::AddBuildingZotProp(cSC4Lot& lot, std::uint32_t propType, std::uint32_t zot) {
    if (!g_daily) return;
    if (zot == 6) g_daily->PostLotNotification(lot, 1);                                // before the priority test (quirk)
    const std::uint32_t current = Counter(lot, kLotPropZot);
    if (current != 0) {
        if (current <= zot) return;
        g_daily->RemoveLotPropsWithZot(lot, current);
    }
    SetCounter(lot, kLotPropZot, zot);
    float position[3] = {};
    float lo[3] = {}, hi[3] = {};
    cSC4LotConfiguration* configuration = g_construction ? g_construction->Configuration(lot.GetLotConfiguration()) : nullptr;
    if (g_daily->BuildingBounds(lot, lo, hi) && configuration && configuration->GetPrimaryBuildingConfigurationObject()) {
        std::int32_t rotation = 0;
        GetPositionForBuilding(&lot, position, rotation);                               // vslot 0xa0
        position[1] = hi[1];
    } else {
        lot.GetCenter(position);                                                         // vslot 0x3c
    }
    position[1] += 3.0f;
    cISC4Occupant* prop = g_daily->AddZotProp(propType, position);                      // prop manager vslot 0x64
    if (!prop) return;
    prop->SetRemovalFlags(3);                                                            // occupant vslot 0x30
    if (cISCPropertyHolder* holder = prop->AsPropertyHolder()) cSCPropertyHelp::SetPropertyValue(holder, kPropPropZot, zot);
}

void cSC4LotDeveloper::RemoveBuildingZotProp(cSC4Lot& lot, std::uint32_t zot) {
    if (!lot.Properties().HasProperty(kLotPropZot)) return;
    std::uint32_t current = 0;
    cSCPropertyHelp::GetPropertyValue(&lot.Properties(), kLotPropZot, current);
    if (zot == 6 && g_daily) g_daily->PostLotNotification(lot, 2);
    if (zot != 0 && current != zot) return;
    if (current != 0 && g_daily) g_daily->RemoveLotPropsWithZot(lot, current);
    SetCounter(lot, kLotPropZot, 0);
}

// Empty lots show zots on a sparse pattern: a diagonal for residential and commercial, a grid for industry and farms.
bool cSC4LotDeveloper::IsEmptyLotSuitableForZot(const cSC4Lot& lot, int x, int z) {
    const int zone = lot.GetZoneType();
    if (zone >= 1 && zone <= 6) return (x + z) % 4 == 0;
    if (zone >= 7 && zone <= 9) return x % 4 == 0 && z % 4 == 0;
    return false;
}

}  // namespace sc4
