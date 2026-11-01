#include "sc4/LotDeveloper.h"

#include <cfloat>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <vector>

#include "gz/StreamOps.h"
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

// The original's DoMessage drives construction/demolition; that stage machine is unrecovered.
bool cSC4LotDeveloper::DoMessage(cIGZMessage2*) {
    return false;
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
