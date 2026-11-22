// cSC4NetworkTool's solve pipeline: PlaceNetwork and, in call order, the cell infos of the drag, the major edge types,
// the set of cells to solve, the edge subtypes, the tile rules (ApplyNetworkRules through each network type's
// cSC4TileEvaluator) and the tile neighbourhood the rules are matched in. Recovered from the Mac i386 build with symbols;
// the i386 offsets are in the comments (the map to the ported members is in notes/NETWORK_TOOL_PLAN.md). The i386
// cell info is the 64-bit one minus 8 bytes after +0x0c: +0x10 status, +0x14 stepIndex, +0x18 tileNeighborhoods,
// +0x4c compact54, +0x50 unknown58 (two one-way bits per edge), +0x53 solvedOrImmovable, +0x55 shortStep,
// +0x56 buildable, +0x57 pieceFromSolvedList, +0x58 vertexHeights, +0x68 range70, +0x88 preferred90,
// +0xa8 vertexBaseHeights (vertex ids), +0xb8 solvedCellIndex.
//
// Port note: the original's sets are __gnu_cxx::hash_set; std::unordered_set walks them in another order. Where the
// walk order matters (which unsolved cell ApplyNetworkRules picks first) a different but equally valid order results.
#include "sc4/NetworkTool.h"
#include "platform/Unwired.h"
#include "sc4/NetworkConstructionCrew.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <list>
#include <deque>
#include <unordered_map>

#include "sc4/NetworkOccupant.h"
#include "sc4/NetworkBridgeOccupant.h"
#include "sc4/NetworkOccupantWithPreBuiltModel.h"
#include "gz/cGZPersistResourceKey.h"
#include "gz/cIGZFile.h"
#include "rz/cRZIniFile.h"
#include "sc4/NetworkWorldCache.h"
#include "sc4/NetworkPreview.h"
#include "sc4/NetworkTunnelOccupant.h"
#include "sc4/TileRules.h"
#include "sc4/TileNeighborhood.h"
#include "sc4/GeometryUtilities.h"

namespace sc4 {

namespace {
constexpr std::int32_t kNextX[4] = {-1, 0, 1, 0};   // nSC4Networks::kNextX
constexpr std::int32_t kNextZ[4] = {0, -1, 0, 1};   // nSC4Networks::kNextZ
// AssignEdgeMajorTypes(cISC4NetworkManager::eNetworkType)::kOneWayTypeForEdge (i386 0x008501b4), indexed
// [direction + (edge & 1) * 8].
constexpr std::uint8_t kOneWayTypeForEdge[16] = {2, 2, 0, 1, 1, 1, 0, 2, 0, 2, 2, 2, 0, 1, 1, 1};

constexpr std::uint32_t Key(std::uint32_t x, std::uint32_t z) { return x + (z << 16); }

// The i386 failure codes PlaceNetwork leaves in +0x234.
constexpr int kFailStart = static_cast<int>(0xf0000001), kFailReserved = static_cast<int>(0xf0000002),
              kFailBridge = static_cast<int>(0xf0000003), kFailTunnel = static_cast<int>(0xf0000004),
              kFailHighway = static_cast<int>(0xf0000005), kFailSmooth = static_cast<int>(0xf0000006),
              kFailSolve = static_cast<int>(0xf0000007), kFailSupport = static_cast<int>(0xf0000008);

void SetOneWayBits(cSC4NetworkCellInfo& cell, int edge, std::uint8_t type) {
    const int shift = edge * 2;
    cell.unknown58 = static_cast<std::uint8_t>((~(3u << shift) & cell.unknown58) + (static_cast<std::uint32_t>(type) << shift));
}
}  // namespace

// cSC4NetworkTileNeighborhood (i386 vtable 0x00bb6a58): the neighbourhood of one cell for one network type, read from
// the tool's cell infos. i386 layout: +0x14 x, +0x18 z, +0x1c the tool, +0x20 the type, +0x24 the other types' mask.
class cSC4NetworkTileNeighborhood final : public cSC4TileNeighborhood {
public:
    cSC4NetworkTileNeighborhood(std::uint32_t x, std::uint32_t z, cSC4NetworkTool* tool, std::uint32_t networkType, bool anyType);
    bool IsValidSolutionPos(std::uint8_t meta) override;
    void ComputeCompactRepresentation(std::uint8_t meta, cSC4TileNeighborhoodCompact& out) override;

private:
    void ComputeCompactAt(cSC4TileNeighborhoodCompact& out, int x, int z);
    std::uint32_t x_, z_;
    cSC4NetworkTool* tool_;
    std::uint32_t networkType_;
    std::uint32_t otherTypes_;
};

// ============================================================================================================
// cSC4NetworkTileNeighborhood (i386 0x00125106 ctor, 0x00122efc / 0x001231ba / 0x00123178)
// ============================================================================================================

cSC4NetworkTileNeighborhood::cSC4NetworkTileNeighborhood(std::uint32_t x, std::uint32_t z, cSC4NetworkTool* tool,
                                                         std::uint32_t networkType, bool anyType)
    : x_(x), z_(z), tool_(tool), networkType_(networkType),
      otherTypes_(anyType ? 0u : (kFlagForNetworkType[networkType] ^ 0x1fffu)) {
    cSC4TileNeighborhoodCompact own;
    ComputeCompactAt(own, static_cast<int>(x), static_cast<int>(z));
    center_ = own.Packed();
}

// A solution tile may go where there is no cell yet or where the cell carries none of the other network types.
bool cSC4NetworkTileNeighborhood::IsValidSolutionPos(std::uint8_t meta) {
    std::int8_t dx = 0, dz = 0;
    cSC4TileInfo::ConvertMetaToRelativePos(meta, dx, dz);
    const cSC4NetworkCellInfo* cell = tool_->GetCellInfo(Key(x_ + dx, z_ + dz));
    return cell == nullptr || (otherTypes_ & cell->networkFlags) == 0;
}

void cSC4NetworkTileNeighborhood::ComputeCompactRepresentation(std::uint8_t meta, cSC4TileNeighborhoodCompact& out) {
    std::int8_t dx = 0, dz = 0;
    cSC4TileInfo::ConvertMetaToRelativePos(meta, dx, dz);
    ComputeCompactAt(out, static_cast<int>(x_) + dx, static_cast<int>(z_) + dz);
}

void cSC4NetworkTileNeighborhood::ComputeCompactAt(cSC4TileNeighborhoodCompact& out, int x, int z) {
    out = cSC4TileNeighborhoodCompact(0);
    if (const cSC4NetworkCellInfo* cell = tool_->GetCellInfo(static_cast<std::uint32_t>(x) + (static_cast<std::uint32_t>(z) << 16)))
        out = cSC4TileNeighborhoodCompact(cell->tileNeighborhoods[networkType_]);
}

// ============================================================================================================
// cell infos
// ============================================================================================================

// i386 0x00663f5c / GetCellInfo(key): the cached cell, or one computed from the network occupant there (or null).
cSC4NetworkCellInfo* cSC4NetworkTool::GetCellInfo(std::uint32_t key) {
    if (!worldCache_) return nullptr;
    if (cSC4NetworkCellInfo* cell = worldCache_->GetCell(key)) return cell;
    return ComputeAndStoreCellInfo(key);
}

// GetNetworkOccupant(x, z, filter, &out): the network occupant of a cell inside the city, one reference, or null.
cSC4NetworkOccupant* cSC4NetworkTool::GetNetworkOccupant(std::uint32_t x, std::uint32_t z) {
    if (x >= static_cast<std::uint32_t>(cityWidth_) || z >= static_cast<std::uint32_t>(cityHeight_)) return nullptr;
    INetworkToolHost* host = GetNetworkToolHost();
    return host ? host->NetworkOccupantAt(x, z, placementFlagMask_) : nullptr;
}

// A cell the drag does not cover yet: its record comes from the network occupant already there.
cSC4NetworkCellInfo* cSC4NetworkTool::ComputeAndStoreCellInfo(std::uint32_t key) {
    cSC4NetworkCellInfo info;
    info.x = key & 0xffff;
    info.z = key >> 16;
    const std::int32_t v0 = static_cast<std::int32_t>(info.z) * widthPlusOne_ + static_cast<std::int32_t>(info.x);
    info.vertexBaseHeights[0] = v0;
    info.vertexBaseHeights[1] = v0 + widthPlusOne_;
    info.vertexBaseHeights[2] = v0 + widthPlusOne_ + 1;
    info.vertexBaseHeights[3] = v0 + 1;
    cSC4NetworkOccupant* occupant = GetNetworkOccupant(info.x, info.z);
    if (!occupant) return nullptr;
    for (std::uint32_t type = 0; type < 13; ++type) {
        if (occupant->IsOfType(type)) info.tileNeighborhoods[type] = occupant->GetEdgeTypesFlag(static_cast<std::uint8_t>(type));   // vslot 0x58 / 0x98
    }
    info.unknown58 = occupant->GetOneWayDirections();                  // vslot 0x114
    info.occupant = occupant;
    info.networkFlags = occupant->GetNetworkFlag() & 0x1fff;           // vslot 0x30
    info.status = 0;
    info.stepIndex = -1;
    info.hasOccupant = true;
    info.unknown5a = occupant->HasNetworkFlag(0x100000);               // vslot 0x20
    info.solvedOrImmovable = occupant->IsImmovable();                  // vslot 0x44
    info.occupantId = 0;
    info.shortStep = false;
    info.buildable = occupant->HasNetworkFlag(0x200000);
    info.pieceFromSolvedList = false;
    info.solvedCellIndex = -1;
    occupant->GetVertexHeights(info.vertexHeights);                    // vslot 0x88
    for (int i = 0; i < 4; ++i) {
        info.range70[i] = {-FLT_MAX, FLT_MAX};
        info.preferred90[i] = {-FLT_MAX, FLT_MAX};
    }
    info.RecomputeEdgesFromAllTypes();
    cSC4NetworkCellInfo* cell = worldCache_->CreateCell(key);
    *cell = info;
    occupant->Release();   // the record keeps a bare pointer; the occupant manager owns the occupant
    return cell;
}

// i386: a record for a cell an additional draw touches that has neither a drag cell nor an occupant.
cSC4NetworkCellInfo* cSC4NetworkTool::CreatePlaceholderCellInfo(std::uint32_t x, std::uint32_t z, std::uint32_t networkType) {
    const std::uint32_t key = Key(x, z);
    if (cSC4NetworkCellInfo* cell = worldCache_->GetCell(key)) return cell;
    INetworkToolHost* host = GetNetworkToolHost();
    const INetworkTypeInfo* info = host ? host->NetworkTypeInfo(networkType) : nullptr;
    const float deviation = info ? info->PlacementParams().float40[0] : 0.0f;   // type record +0x84 = params +0x34
    cSC4NetworkCellInfo cell;
    cell.x = x;
    cell.z = z;
    cell.networkFlags = kFlagForNetworkType[networkType];
    cell.status = 1;
    cell.stepIndex = 0;
    cell.solvedCellIndex = -1;
    const std::int32_t v0 = static_cast<std::int32_t>(x) + static_cast<std::int32_t>(z) * widthPlusOne_;
    const std::int32_t ids[4] = {v0, v0 + widthPlusOne_, v0 + widthPlusOne_ + 1, v0 + 1};
    for (int i = 0; i < 4; ++i) {
        cell.vertexBaseHeights[i] = ids[i];
        cell.vertexHeights[i] = host ? host->TerrainVertexHeight(ids[i]) : 0.0f;
        cell.range70[i] = {cell.vertexHeights[i] - deviation, deviation + cell.vertexHeights[i]};
    }
    cSC4NetworkCellInfo* stored = worldCache_->CreateCell(key);
    *stored = cell;
    return stored;
}

// i386 vslot 0x124: a record for every cell of the valid steps, marked solved-this-drag, with the corner height windows
// (terrain +- the placement deviation, never below the water surface).
void cSC4NetworkTool::CreateCellInfosForCurrentPlacementTiles(std::uint32_t networkType) {
    INetworkToolHost* host = GetNetworkToolHost();
    for (int s = 0; s < validSteps_; ++s) {
        const tDraggedStep& step = steps_[static_cast<std::size_t>(s)];
        for (std::uint32_t c = step.firstCell; c <= step.lastCell; ++c) {
            const std::uint32_t x = cells_[c].x, z = cells_[c].z;
            cSC4NetworkCellInfo* cell = GetCellInfo(Key(x, z));
            if (!cell) {
                cSC4NetworkCellInfo info;
                info.x = x;
                info.z = z;
                info.networkFlags = kFlagForNetworkType[networkType];
                info.status = 1;
                info.stepIndex = s;
                info.hasOccupant = false;
                info.unknown5a = 0;
                info.solvedOrImmovable = step.direction != 0;
                info.occupantId = 0;
                info.shortStep = step.direction == 3;
                info.buildable = false;
                info.pieceFromSolvedList = false;
                info.solvedCellIndex = -1;
                const std::int32_t v0 = static_cast<std::int32_t>(x) + static_cast<std::int32_t>(z) * widthPlusOne_;
                const std::int32_t ids[4] = {v0, v0 + widthPlusOne_, v0 + widthPlusOne_ + 1, v0 + 1};
                const float deviation = placement_.float40[0];   // i386 +0x304 = params +0x34
                for (int i = 0; i < 4; ++i) {
                    info.vertexBaseHeights[i] = ids[i];
                    info.vertexHeights[i] = host ? host->TerrainVertexHeight(ids[i]) : 0.0f;
                    info.range70[i] = {info.vertexHeights[i] - deviation, info.vertexHeights[i] + deviation};
                    info.preferred90[i] = {-4.2535295e37f, 4.2535295e37f};   // 0xfdffffff / 0x7dffffff
                }
                cell = worldCache_->CreateCell(Key(x, z));
                *cell = info;
            } else {
                cell->networkFlags |= kFlagForNetworkType[networkType];
                cell->status = 1;
                cell->stepIndex = s;
                if (step.direction != 0) cell->solvedOrImmovable = true;
                const float deviation = placement_.float10[1];   // i386 +0x2e4 = params +0x14
                for (int i = 0; i < 4; ++i) {
                    cell->range70[i] = {cell->vertexHeights[i] - deviation, cell->vertexHeights[i] + deviation};
                    cell->preferred90[i] = {cell->vertexHeights[i], cell->vertexHeights[i]};
                }
            }
            if (step.direction == 1) cell->networkFlags |= 0x40000;
            else if (step.direction == 2) cell->networkFlags |= 0x80000;
            // Never below the water surface (SL::spSurfaceWater vslot 0x28 at the corner).
            for (int i = 0; i < 4; ++i) {
                const std::int32_t id = cell->vertexBaseHeights[i];
                const float water = host ? host->WaterSurfaceAtVertex(id % widthPlusOne_, id / widthPlusOne_) : -FLT_MAX;
                if (cell->range70[i].low < water) cell->range70[i].low = water;
                if (cell->preferred90[i].low < water) cell->preferred90[i].low = water;
            }
        }
    }
}

// ============================================================================================================
// edges
// ============================================================================================================

// i386: the edges the drag itself makes. On the straight part each cell connects to the previous and the next step
// (major type = direction A, the last straight cell = direction B); on the diagonal part a cell connects to every
// neighbour solved this drag. One-way roads (type 10) also get their one-way bits per edge.
void cSC4NetworkTool::AssignEdgeMajorTypes(std::uint32_t networkType) {
    const int straight = straightSteps_, valid = validSteps_;
    const std::uint32_t forward = static_cast<std::uint32_t>(directionA_ / 2);
    const std::uint32_t backward = (forward + 2) & 3;
    const bool oneWay = networkType == 10;
    for (int s = 0; s < validSteps_; ++s) {
        const tDraggedStep& step = steps_[static_cast<std::size_t>(s)];
        for (std::uint32_t c = step.firstCell; c <= step.lastCell; ++c) {
            const std::int32_t x = static_cast<std::int32_t>(cells_[c].x), z = static_cast<std::int32_t>(cells_[c].z);
            cSC4NetworkCellInfo* cell = GetCellInfo(Key(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z)));
            cSC4TileNeighborhoodCompact edges(0);
            if (s < straightSteps_) {
                if (s < valid - 1) {
                    const std::uint8_t major = static_cast<std::uint8_t>(s < straight - 1 ? directionA_ : directionB_);
                    edges.SetEdgeConnectionDirection(static_cast<int>(forward), major);
                    if (oneWay) SetOneWayBits(*cell, static_cast<int>(forward), kOneWayTypeForEdge[directionA_ + (forward & 1) * 8]);
                }
                if (s > 0) {
                    edges.SetEdgeConnectionDirection(static_cast<int>(backward), static_cast<std::uint8_t>(directionA_));
                    if (oneWay) SetOneWayBits(*cell, static_cast<int>(backward), kOneWayTypeForEdge[directionA_ + (backward & 1) * 8]);
                }
            } else {
                for (int e = 0; e < 4; ++e) {
                    const cSC4NetworkCellInfo* n = GetCellInfo(Key(static_cast<std::uint32_t>(x + kNextX[e]), static_cast<std::uint32_t>(z + kNextZ[e])));
                    if (n && n->status != 0) {
                        edges.SetEdgeConnectionDirection(e, static_cast<std::uint8_t>(directionB_));
                        if (oneWay) SetOneWayBits(*cell, e, kOneWayTypeForEdge[(e & 1) * 8 + directionB_]);
                    }
                }
            }
            cSC4TileNeighborhoodCompact own(cell->tileNeighborhoods[networkType]);
            own.ResetEdgeMinorTypes();
            own.Merge(edges);
            cell->tileNeighborhoods[networkType] = own.Packed();
            cell->RecomputeEdgesFromAllTypes();
        }
    }
}

// i386: the extra cells (+0xf0) a derived tool asks to draw get their edges merged in.
void cSC4NetworkTool::DoAdditionalDraws() {
    for (const tReservedCell& draw : reservedCells_) {
        if (draw.x >= static_cast<std::uint32_t>(cityWidth_) || draw.z >= static_cast<std::uint32_t>(cityHeight_)) continue;
        cSC4NetworkCellInfo* cell = GetCellInfo(Key(draw.x, draw.z));
        if (!cell) cell = CreatePlaceholderCellInfo(draw.x, draw.z, draw.network);
        if (!cell) continue;
        cell->networkFlags |= kFlagForNetworkType[draw.network];
        cSC4TileNeighborhoodCompact own(cell->tileNeighborhoods[draw.network]);
        own.ResetEdgeMinorTypes();
        own.Merge(cSC4TileNeighborhoodCompact(draw.compact));
        std::uint32_t packed = own.Packed();
        const std::uint32_t directions = draw.compact & 0x4040404;
        if (directions != 0) packed = directions + (~((directions >> 2) * 0xff) & packed);
        cell->tileNeighborhoods[draw.network] = packed;
        cell->RecomputeEdgesFromAllTypes();
        if (draw.oneWayMask != 0) cell->unknown58 = static_cast<std::uint8_t>((~draw.oneWayMask & cell->unknown58) + draw.oneWayBits);
    }
}

// i386: an edge of a simple network gets the diagonal subtype when the neighbour across it is a simple diagonal; edges
// that extend a diagonal run (MarkExtendingDiagonalEdges) are left alone. False when a fixed cell would have to change.
bool cSC4NetworkTool::AssignSimpleEdgeSubtypes(cSC4NetworkCellInfo& cell, std::uint32_t type,
                                               std::unordered_set<std::uint32_t>& set, bool assign) {
    int straight[4], diagonal[4], straightCount = 0, diagonalCount = 0;
    cSC4TileNeighborhoodCompact(cell.tileNeighborhoods[type]).GetStraightAndDiagonalEdges(straight, diagonal, straightCount, diagonalCount);
    if (straightCount == 0 || diagonalCount == 0) return true;
    bool extending[4];
    MarkExtendingDiagonalEdges(cell, extending, type);
    for (int i = 0; i < diagonalCount; ++i) {
        const int e = diagonal[i];
        if (extending[e]) continue;
        const std::uint32_t key = Key(cell.x + static_cast<std::uint32_t>(kNextX[e]), cell.z + static_cast<std::uint32_t>(kNextZ[e]));
        cSC4NetworkCellInfo* n = GetCellInfo(key);
        if (!n) continue;
        const bool simple = IsSimpleDiagonalNetwork(*n, type);
        if (assign) {
            if (cell.solvedOrImmovable && simple) return false;
            cSC4TileNeighborhoodCompact own(cell.tileNeighborhoods[type]);
            own.SetEdgeTypeMinor(e, simple);
            if (!simple) own.SetEdgeCondition(e, static_cast<std::uint8_t>((own.Packed() >> (e * 8)) & 0xf8 | 2));
            cell.tileNeighborhoods[type] = own.Packed();
            cell.RecomputeEdgesFromAllTypes();
        }
        if (set.find(key) == set.end()) {
            if (simple && n->solvedOrImmovable) return false;
        } else {
            const int back = (e + 2) & 3;
            cSC4TileNeighborhoodCompact other(n->tileNeighborhoods[type]);
            other.SetEdgeTypeMinor(back, simple);
            if (!simple) other.SetEdgeCondition(back, static_cast<std::uint8_t>((other.Packed() >> (back * 8)) & 0xf8 | 2));
            n->tileNeighborhoods[type] = other.Packed();
            n->RecomputeEdgesFromAllTypes();
        }
    }
    return true;
}

// i386: complex networks (rail, monorail, light rail) mark a diagonal edge's subtype from how the other edges bend.
bool cSC4NetworkTool::AssignComplexEdgeSubtypes(cSC4NetworkCellInfo& cell, std::uint32_t type,
                                                std::unordered_set<std::uint32_t>& set, bool assign) {
    for (int e = 0; e < 4; ++e) {
        const std::uint32_t opposite = static_cast<std::uint32_t>(e + 2) & 3;
        const std::uint32_t packed = cell.tileNeighborhoods[type];
        const std::uint32_t edge = packed >> (e * 8);
        const std::uint8_t kind = static_cast<std::uint8_t>(edge & 3);
        if (kind == 0) continue;
        std::uint8_t subtype = 0;
        if ((edge & 1) == 0) {
            if (kind != 2) continue;
            subtype = (packed >> (opposite * 8) & 3) != 0;
            if (((packed >> (((e + 3) & 3) * 8)) & 3) == 1) subtype |= 2;
            if (((packed >> (((e + 1) & 3) * 8)) & 3) == 3) subtype |= 4;
            if (subtype == 1 || subtype == 0) continue;
        } else {
            const std::uint32_t across = packed >> (opposite * 8);
            bool bends = false;
            if ((across & 3) != 0)
                bends = !(kind == (across & 3) && kind == 4 - ((packed >> (((e + 3) & 3) * 8)) & 3));
            const std::uint32_t sideEdge = kind == 1 ? static_cast<std::uint32_t>(e + 1) : static_cast<std::uint32_t>(e + 3);
            const std::uint8_t side = static_cast<std::uint8_t>((packed >> ((sideEdge & 3) * 8)) & 3);
            const bool continues = side == 2 || side + kind == 4;
            if (!bends) continue;
            subtype = static_cast<std::uint8_t>(2 - !continues);
        }
        const std::uint32_t key = Key(cell.x + static_cast<std::uint32_t>(kNextX[e]), cell.z + static_cast<std::uint32_t>(kNextZ[e]));
        if (assign) {
            if (cell.solvedOrImmovable) return false;
            cSC4TileNeighborhoodCompact own(cell.tileNeighborhoods[type]);
            own.SetEdgeCondition(e, static_cast<std::uint8_t>(subtype * 8 | kind));
            cell.tileNeighborhoods[type] = own.Packed();
            cell.RecomputeEdgesFromAllTypes();
        }
        if (set.find(key) == set.end()) {
            const cSC4NetworkCellInfo* n = GetCellInfo(key);
            if (n && n->solvedOrImmovable) return false;
        } else if (cSC4NetworkCellInfo* n = GetCellInfo(key)) {
            cSC4TileNeighborhoodCompact other(n->tileNeighborhoods[type]);
            other.SetEdgeCondition(static_cast<int>(opposite), static_cast<std::uint8_t>(subtype * 8 | kind));
            n->tileNeighborhoods[type] = other.Packed();
            n->RecomputeEdgesFromAllTypes();
        }
    }
    return true;
}

// i386: an edge pair (e, e2) that turns a diagonal through this cell is "extending" when both walks along the diagonal
// (one per edge) reach a simple diagonal cell.
void cSC4NetworkTool::MarkExtendingDiagonalEdges(const cSC4NetworkCellInfo& cell, bool* extending, std::uint32_t type) {
    for (int i = 0; i < 4; ++i) extending[i] = false;
    const std::uint32_t packed = cell.tileNeighborhoods[type];
    for (int e = 0; e < 4; ++e) {
        if (extending[e]) continue;
        const std::uint32_t kind = (packed >> (e * 8)) & 3;
        int e2;
        if (kind == 1) e2 = e + 1;
        else if (kind == 3) e2 = e + 3;
        else continue;
        e2 &= 3;
        const std::uint32_t kind2 = (packed >> (e2 * 8)) & 3;
        if (kind + kind2 != 4) continue;
        int dir = e;
        std::uint32_t current = kind;
        for (int pass = 0;; ++pass) {
            std::int32_t x = static_cast<std::int32_t>(cell.x), z = static_cast<std::int32_t>(cell.z);
            bool found = false;
            std::uint32_t sum;
            do {
                x += kNextX[dir];
                z += kNextZ[dir];
                const cSC4NetworkCellInfo* n = GetCellInfo(Key(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z)));
                if (!n) break;
                if (IsSimpleDiagonalNetwork(*n, type)) { found = true; break; }
                dir = (current == 3 ? dir + 1 : dir + 3) & 3;
                const std::uint32_t next = (n->tileNeighborhoods[type] >> (dir * 8)) & 3;
                sum = next + current;
                current = next;
            } while (sum == 4);
            if (pass == 1) {
                if (found) { extending[e] = true; extending[e2] = true; }
                break;
            }
            if (!found) break;
            dir = e2;
            current = kind2;
        }
    }
}

// i386 0x001477f2: subtypes for every cell around the solve set (padded by one); `removed` gets the cells whose subtype
// could not change. With `reset` the minor types of the set's own single-type cells are cleared first.
void cSC4NetworkTool::AssignEdgeSubtypes(std::unordered_set<std::uint32_t>& set, std::unordered_set<std::uint32_t>& removed, bool reset) {
    std::unordered_set<std::uint32_t> padded;
    BuildPaddedNetworkSet(set, padded, 1);
    const auto singleType = [](std::uint32_t flags, int& type) {
        if ((flags - 1 & flags) != 0) return false;
        for (int t = 0; t < 13; ++t) if ((kFlagForNetworkType[t] & flags) != 0) { type = t; return true; }
        return false;
    };
    INetworkToolHost* host = GetNetworkToolHost();
    if (reset) {
        for (std::uint32_t key : padded) {
            const bool inSet = set.count(key) != 0;
            cSC4NetworkCellInfo* cell = GetCellInfo(key);
            int type = 0;
            if (cell && singleType(cell->networkFlags, type) && inSet) {
                cSC4TileNeighborhoodCompact own(cell->tileNeighborhoods[type]);
                own.ResetEdgeMinorTypes();
                cell->tileNeighborhoods[type] = own.Packed();
                cell->RecomputeEdgesFromAllTypes();
            }
        }
    }
    for (std::uint32_t key : padded) {
        const bool inSet = set.count(key) != 0;
        cSC4NetworkCellInfo* cell = GetCellInfo(key);
        int type = 0;
        if (!cell || !singleType(cell->networkFlags, type)) continue;
        const int kind = host ? host->NetworkTypeKind(static_cast<std::uint32_t>(type)) : 0;   // type record +0
        bool ok;
        if (kind == 1) ok = AssignSimpleEdgeSubtypes(*cell, static_cast<std::uint32_t>(type), set, inSet);
        else if (kind == 2) ok = AssignComplexEdgeSubtypes(*cell, static_cast<std::uint32_t>(type), set, inSet);
        else continue;
        if (!ok) {
            set.erase(key);
            removed.insert(key);
        }
    }
}

// ============================================================================================================
// the solve set
// ============================================================================================================

void cSC4NetworkTool::BuildSetOfCellsToBeSolved() {
    std::vector<std::uint32_t> seeds;
    for (int s = 0; s < validSteps_; ++s) {
        const tDraggedStep& step = steps_[static_cast<std::size_t>(s)];
        for (std::uint32_t c = step.firstCell; c <= step.lastCell; ++c) {
            const std::uint32_t key = Key(cells_[c].x, cells_[c].z);
            cSC4NetworkCellInfo* cell = GetCellInfo(key);
            if (!cell->solvedOrImmovable) {
                solvedCells_.insert(key);
                cell->pieceFromSolvedList = true;
                seeds.push_back(key);
            }
        }
    }
    for (const tReservedCell& draw : reservedCells_) {
        if (draw.x >= static_cast<std::uint32_t>(cityWidth_) || draw.z >= static_cast<std::uint32_t>(cityHeight_)) continue;
        const std::uint32_t key = Key(draw.x, draw.z);
        GetCellInfo(key)->pieceFromSolvedList = true;
        seeds.push_back(key);
        solvedCells_.insert(key);
    }
    ExpandToSolveSetBasedOnConnectivity(seeds, 4);
    RemoveHighwayIntersectionPiecesFromSetOfCellsToBeSolved(solvedCells_);
    RemoveBridgePiecesFromSetOfCellsToBeSolved(solvedCells_);
    RemoveTunnelPiecesFromSetOfCellsToBeSolved(solvedCells_);
}

// i386: `rings` times, every cell connected to the frontier that is not fixed (and not a short step) joins the set.
void cSC4NetworkTool::ExpandToSolveSetBasedOnConnectivity(std::vector<std::uint32_t>& frontier, int rings) {
    std::vector<std::uint32_t> current;
    for (int ring = 0; ring < rings; ++ring) {
        current.clear();
        current.swap(frontier);
        for (std::uint32_t key : current) {
            frontier.push_back(key);
            const cSC4NetworkCellInfo* cell = GetCellInfo(key);
            if (!cell) continue;
            for (int e = 0; e < 4; ++e) {
                if (!cSC4TileNeighborhoodCompact::HasConnectionOnEdge(cell->compact54, e)) continue;
                const std::uint32_t nx = cell->x + static_cast<std::uint32_t>(kNextX[e]), nz = cell->z + static_cast<std::uint32_t>(kNextZ[e]);
                if (nx >= static_cast<std::uint32_t>(cityWidth_) || nz >= static_cast<std::uint32_t>(cityHeight_)) continue;
                const std::uint32_t nkey = Key(nx, nz);
                if (solvedCells_.count(nkey)) continue;
                cSC4NetworkCellInfo* n = GetCellInfo(nkey);
                if (n && !n->solvedOrImmovable && !n->shortStep) {
                    solvedCells_.insert(nkey);
                    n->pieceFromSolvedList = true;
                    frontier.push_back(nkey);
                }
            }
        }
    }
}

// ============================================================================================================
// one-way roads
// ============================================================================================================

namespace {
// (anonymous namespace)::ResolveOneWayConflictsWorkUnit (12 bytes in the i386 build): a cell whose one-way bits
// changed and the edge the change came in through (-1 for the seeds).
struct ResolveOneWayConflictsWorkUnit {
    std::uint32_t x = 0;
    std::uint32_t z = 0;
    std::int32_t incomingEdge = -1;
};

// The two one-way bits of an edge (the cell info's i386 +0x50 byte; 1 and 2 are the two directions along the axis).
constexpr std::uint32_t OneWayBitsOnEdge(std::uint8_t bits, std::uint32_t edge) { return (static_cast<std::uint32_t>(bits) >> (edge * 2)) & 3u; }
}  // namespace

// i386 0x004cde90 (PlaceNetwork calls it for type 10 only, after BuildSetOfCellsToBeSolved): the one-way directions the
// drag assigned are carried along the existing one-way road at both ends of the drag, so a new piece that joins a one-way
// road against its direction turns the road it joins around. The walk starts at the cells of the first and the last
// dragged step; a cell with one or two edges of the type passes the direction it got in through one edge out through its
// other (flipped on a turn between the edge pairs 0/1 and 2/3), into the neighbour across that edge (the other portal for
// a tunnel portal), which joins the solve set. A cell with more edges (an intersection) stops the walk. Then every cell of
// the solve set drops the one-way bits of edges that contradict another edge with the same connection direction, on its
// own side and on the neighbour's, until no new cell is touched.
void cSC4NetworkTool::ResolveOneWayConflicts(std::uint32_t networkType) {
    if (!worldCache_ || steps_.empty()) return;
    std::vector<ResolveOneWayConflictsWorkUnit> work, next;
    // The seeds: the cells of step 0 and of the last step (i386 +0x70, the total step count).
    for (std::size_t s = 0;;) {
        const tDraggedStep& step = steps_[s];
        for (std::uint32_t c = step.firstCell; c <= step.lastCell; ++c) work.push_back({cells_[c].x, cells_[c].z, -1});
        const std::size_t last = static_cast<std::size_t>(totalSteps_) - 1;
        if (s == last || last >= steps_.size()) break;
        s = last;
    }
    INetworkToolHost* host = GetNetworkToolHost();
    const auto inCity = [this](std::uint32_t x, std::uint32_t z) {
        return x < static_cast<std::uint32_t>(cityWidth_) && z < static_cast<std::uint32_t>(cityHeight_);   // i386 +0x280 / +0x284
    };
    while (!work.empty()) {
        for (const ResolveOneWayConflictsWorkUnit& unit : work) {
            const std::uint32_t key = Key(unit.x, unit.z);
            cSC4NetworkCellInfo* cell = worldCache_->GetCell(key);   // cSC4NetworkWorldCache::GetCell (the world cache at i386 +0x1c)
            if (!cell) continue;
            cell->status = 1;
            if (!cell->pieceFromSolvedList) {                          // i386 +0x57: in the solve set (+0x84)
                cell->pieceFromSolvedList = true;
                solvedCells_.insert(key);
            }
            // Only a cell with one or two edges of the type carries the direction through.
            const std::uint32_t edges = cell->tileNeighborhoods[networkType];
            const std::uint32_t edgeCount = (((edges & 0x07070707u) + 0x07070707u) & 0x08080808u) * 0x01010101u >> 27;
            if (edgeCount - 1u >= 2u) continue;
            // A tunnel portal's edge into the hill leads to the other portal: the edge that points toward it.
            std::int32_t tunnelEdge = -1;
            std::uint32_t otherX = 0, otherZ = 0;
            if (cell->occupant && cell->occupant->HasNetworkFlag(0x80000)) {   // vslot 0x20: a tunnel portal
                if (cSC4NetworkOccupant* other = host ? host->TunnelOtherEnd(cell->occupant) : nullptr) {   // QI 0x8a4bd52b, GetOtherEndOccupant
                    other->GetOccupiedCell(otherX, otherZ);                                                  // vslot 0xec
                    const std::int32_t ox = static_cast<std::int32_t>(otherX), oz = static_cast<std::int32_t>(otherZ);
                    const std::int32_t ux = static_cast<std::int32_t>(unit.x), uz = static_cast<std::int32_t>(unit.z);
                    if (ox < ux) tunnelEdge = 0;
                    else if (oz < uz) tunnelEdge = 1;
                    else if (ux < ox) tunnelEdge = 2;
                    else if (uz < oz) tunnelEdge = 3;
                    other->Release();
                }
            }
            // The edge the direction comes from (kept: the incoming edge, or the first edge whose neighbour was already
            // solved) and the edge it goes out through (the last other edge toward a neighbour that may change).
            std::int32_t fromEdge = unit.incomingEdge, toEdge = -1;
            cSC4NetworkCellInfo* toCell = nullptr;
            for (std::uint32_t e = 0; e < 4; ++e) {
                if (OneWayBitsOnEdge(cell->unknown58, e) == 0) continue;
                std::uint32_t nx = unit.x + static_cast<std::uint32_t>(kNextX[e]), nz = unit.z + static_cast<std::uint32_t>(kNextZ[e]);
                if (static_cast<std::int32_t>(e) == tunnelEdge) {
                    nx = otherX;
                    nz = otherZ;
                }
                if (!inCity(nx, nz)) continue;
                cSC4NetworkCellInfo* n = GetCellInfo(Key(nx, nz));
                if (!n || n->unknown5a != 0) continue;                // i386 +0x52
                if (n->status == 0 || (fromEdge >= 0 && static_cast<std::int32_t>(e) != fromEdge)) {
                    toEdge = static_cast<std::int32_t>(e);
                    toCell = n;
                } else {
                    fromEdge = static_cast<std::int32_t>(e);
                }
            }
            if (fromEdge < 0 || toEdge < 0) continue;
            std::uint32_t direction = OneWayBitsOnEdge(cell->unknown58, static_cast<std::uint32_t>(fromEdge));
            if (toEdge == (fromEdge ^ 1)) direction ^= 3;
            if (direction == OneWayBitsOnEdge(cell->unknown58, static_cast<std::uint32_t>(toEdge))) continue;
            SetOneWayBits(*cell, toEdge, static_cast<std::uint8_t>(direction));
            if (!toCell) continue;
            const std::int32_t back = (toEdge + 2) & 3;
            SetOneWayBits(*toCell, back, static_cast<std::uint8_t>(direction));
            next.push_back({toCell->x, toCell->z, back});
        }
        work.swap(next);
        next.clear();
    }
    // The conflicts: per cell of the solve set, two edges with the same connection direction (the edge's byte plus the
    // edge index, mod 4) must agree (flipped unless the two differ in bit 1); a contradicting edge loses its bits on both
    // sides. A neighbour not yet in the solve set brings the cell back for another round.
    std::unordered_set<std::uint32_t> current(solvedCells_), added;
    while (!current.empty()) {
        for (std::uint32_t key : current) {
            cSC4NetworkCellInfo* cell = GetCellInfo(key);
            if (!cell) continue;
            std::uint32_t seen[4] = {0, 0, 0, 0};
            for (std::uint32_t e = 0; e < 4; ++e) {
                const std::uint32_t slot = ((cell->tileNeighborhoods[networkType] >> (e * 8)) + e) & 3u;
                std::uint32_t direction = OneWayBitsOnEdge(cell->unknown58, e);
                if (direction == 0) continue;
                if (((slot ^ e) & 2u) == 0) direction ^= 3u;
                if (seen[slot] != 0 && direction != seen[slot]) {
                    cell->unknown58 = static_cast<std::uint8_t>(cell->unknown58 & ~(3u << (slot * 2)));
                    const std::uint32_t nx = (key & 0xffffu) + static_cast<std::uint32_t>(kNextX[slot]);
                    const std::uint32_t nz = (key >> 16) + static_cast<std::uint32_t>(kNextZ[slot]);
                    if (inCity(nx, nz)) {
                        if (cSC4NetworkCellInfo* n = GetCellInfo(nx, nz)) {
                            n->unknown58 = static_cast<std::uint8_t>(n->unknown58 & ~(3u << (((slot + 2) & 3u) * 2)));
                            if (!n->pieceFromSolvedList) {
                                n->pieceFromSolvedList = true;
                                added.insert(key);   // the original inserts the cell being walked, not the neighbour
                            }
                        }
                    }
                }
                seen[slot] = direction;
            }
        }
        solvedCells_.insert(added.begin(), added.end());
        current.swap(added);
        added.clear();
    }
}

// i386: the neighbour-city connections (+0x100) open their edge on the cell; a connection onto a straight edge turns
// every connection bit into the next kind.
void cSC4NetworkTool::MarkNeighborConnections() {
    for (tNeighborConnection& connection : pendingNeighborConnections_) {
        cSC4NetworkCellInfo* cell = GetCellInfo(connection.unknown0);
        if (!cell) continue;
        std::uint32_t packed = cell->tileNeighborhoods[networkType_];
        if ((packed & 0x3000000) == 0x2000000 || (packed & 0x30000) == 0x20000 || (packed & 0x300) == 0x200 || (packed & 3) == 2) {
            std::uint32_t bits = connection.unknown4;
            for (std::uint32_t bit = 1, i = 0; i < 4; ++i, bit <<= 8)
                if (bit & bits) bits = (bits & ~bit) | bit * 2;
            connection.unknown4 = bits;
        }
        cell->tileNeighborhoods[networkType_] = packed | connection.unknown4;
        cell->RecomputeEdgesFromAllTypes();
    }
    pendingNeighborConnections_.clear();
}

// i386: every cell within `pad` of a set cell that has a record and is not fixed (or is the set cell itself).
void cSC4NetworkTool::BuildPaddedNetworkSet(const std::unordered_set<std::uint32_t>& set, std::unordered_set<std::uint32_t>& out, std::uint32_t pad) {
    for (std::uint32_t key : set) {
        SC4RectU rect;
        GetPaddedRectAroundCell(key & 0xffff, key >> 16, rect, pad);
        for (std::uint32_t z = rect.z0; z <= rect.z1; ++z) {
            for (std::uint32_t x = rect.x0; x <= rect.x1; ++x) {
                const std::uint32_t k = Key(x, z);
                const cSC4NetworkCellInfo* cell = GetCellInfo(k);
                if (cell && (!cell->solvedOrImmovable || key == k)) out.insert(k);
            }
        }
    }
}

// ============================================================================================================
// the solve
// ============================================================================================================

// i386 0x00147a26. Returns the number of steps that solved (validSteps_ when all did).
int cSC4NetworkTool::SolveNetwork(std::uint32_t networkType, bool retry) {
    std::unordered_set<std::uint32_t> padded, removed;
    BuildPaddedNetworkSet(solvedCells_, padded, 2);
    AssignEdgeSubtypes(padded, removed, true);
    for (std::uint32_t key : removed) solvedCells_.erase(key);
    std::vector<std::uint32_t> crossings;
    SolveMultiTypeIntersections(solvedCells_, crossings, networkType);
    for (std::uint32_t key : crossings) {
        solvedCells_.erase(key);
        padded.erase(key);
    }
    for (std::uint32_t type = 0; type < 13; ++type) {
        for (cSC4TileEvaluator* evaluator : NetworkTileEvaluators(type)) {
            if (!evaluator) break;
            SolveUsingEvaluator(padded, solvedCells_, *evaluator, type);
        }
    }
    for (std::uint32_t key : removed) solvedCells_.insert(key);
    int result;
    if (retry || solvedCells_.empty()) {
        if (!AdjustTileSubsets(solvedCellList_, retry) || !VerifyTileSubsets(solvedCellList_)) return validSteps_ - 1;
    }
    // Cells left unsolved cut the drag back to the earliest step they belong to.
    result = validSteps_;
    for (std::uint32_t key : solvedCells_) {
        const cSC4NetworkCellInfo* cell = GetCellInfo(key);
        if (!cell || cell->status == 0) {
            if (result == validSteps_) --result;
        } else if (cell->stepIndex < result) {
            result = cell->stepIndex;
        }
    }
    return result;
}

// ============================================================================================================
// the multitype rules
// ============================================================================================================

namespace {

// AppendNetworkName (i386 0x00152352): the names the multitype rule sections and AllowedOverrides use.
constexpr const char* kNetworkNames[kNetworkTypeCount] = {"Road", "Rail", "Highway", "Street", "Pipe", "PowerLine", "Avenue",
                                                       "Subway", "LightRail", "MonoRail", "OneWayRoad", "DirtRoad", "GroundHighway"};

// tMixedTypeIntxSolution: the piece and its rotation (| 0x80 flipped) for one pair of edge configurations.
struct tMixedTypeIntxSolution {
    std::uint32_t pieceType = 0;
    std::uint8_t rotationFlip = 0;
};
// cSC4NetworkTool::sAllMixedTypeSolutions: per type pair (lower type first), {lower type's edges, higher type's} ->
// the solution.
std::map<std::pair<std::uint32_t, std::uint32_t>, std::unordered_map<std::uint64_t, tMixedTypeIntxSolution>> g_mixedTypeSolutions;
// The AllowedOverrides matrix (sNetworkTypeInfo +0xfc per type): bit t = this type may override type t.
std::uint32_t g_overrideMatrix[kNetworkTypeCount] = {};

// nSC4Networks::kNetworkMultitypePriority (i386 0x00845520): the higher value keeps its pieces in a shared cell.
constexpr int kNetworkMultitypePriority[kNetworkTypeCount] = {3, 5, 6, 2, 1, 0, 4, 1, 7, 7, 3, 0, 5};

// cRZIniFile's view of the text: section -> (key, value) lines in file order, ';' comments and blanks skipped.
using IniSections = std::map<std::string, std::vector<std::pair<std::string, std::string>>>;
std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string Trim(const std::string& s) {
    const std::size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
IniSections ParseIni(const std::string& text) {
    IniSections sections;
    std::vector<std::pair<std::string, std::string>>* current = nullptr;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = Trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[') {
            const std::size_t close = line.find(']');
            current = &sections[Lower(line.substr(1, close == std::string::npos ? std::string::npos : close - 1))];
        } else if (current) {
            const std::size_t eq = line.find('=');
            if (eq != std::string::npos) current->emplace_back(Trim(line.substr(0, eq)), Trim(line.substr(eq + 1)));
        }
    }
    return sections;
}
// NetworkNameToID: the type of a name in kNetworkNames (case-insensitive), or -1.
int NetworkNameToID(const std::string& name) {
    for (std::uint32_t t = 0; t < kNetworkTypeCount; ++t)
        if (Lower(name) == Lower(kNetworkNames[t])) return static_cast<int>(t);
    return -1;
}

}  // namespace

// kNetworkTypeToNeighborConnectionType (i386 0x00844d20): the neighbour connection type of each network type (0: the street
// and the one-way road make no connection of their own).
constexpr std::uint32_t kNetworkTypeToNeighborConnectionType[kNetworkTypeCount] = {2, 1, 3, 0, 5, 4, 8, 6, 7, 11, 0, 10, 12};

// i386 0x00125a22. The drag's last anchor segment is drawn again; when its end steps out of the city (the cell next to the
// end, in the drag direction, is outside it) and the pen's cells there all face the same neighbour city, every cell of the
// last step gets a connection record (in both vectors: the pending one opens the cells' edges in the next solve, the
// other is what PlaceNeighborConnections creates). A diagonal drag (fewer straight steps than steps) records one corner
// connection. Refused: an existing connection on the edge, a cell holding a network that does not fit the edge, or a
// higher-priority network the override rules do not let this one replace.
// i386 0x004cc34e. For a network drawn with a pen wider than one cell (sNetworkTypeInfo +0x1c, the highways), the cells
// a step covers belong together: every cell but the step's last gets condition 4 (grouped) on the edge toward the next
// cell of the step (edge 3 for a horizontal step, else 2), every cell but the first on the edge back (edge 1, else 0),
// in that type's neighbourhood; then the cell's all-types neighbourhood is recomputed.
void cSC4NetworkTool::AddGroupingInfo(std::uint32_t networkType) {
    INetworkToolHost* host = GetNetworkToolHost();
    const INetworkTypeInfo* info = host && networkType < kNetworkTypeCount ? host->NetworkTypeInfo(networkType) : nullptr;
    if (!info || info->PenWidthX() < 2) return;
    const int forwardEdge = stepIsHorizontal_ ? 3 : 2;
    const int backEdge = stepIsHorizontal_ ? 1 : 0;
    const int stepCount = std::min(straightSteps_, validSteps_);
    for (int i = 0; i < stepCount; ++i) {
        const tDraggedStep& step = steps_[static_cast<std::size_t>(i)];
        for (std::uint32_t c = step.firstCell; c <= step.lastCell; ++c) {
            const SC4PointU& point = cells_[c];
            cSC4NetworkCellInfo* cell = GetCellInfo(point.x + (point.z << 16));
            if (!cell) continue;
            cSC4TileNeighborhoodCompact compact(cell->tileNeighborhoods[networkType]);
            if (c < step.lastCell)
                compact.SetEdgeCondition(forwardEdge, static_cast<std::uint8_t>((compact.GetEdgeCondition(forwardEdge) & 0xf8u) | 4u));
            if (step.firstCell < c)
                compact.SetEdgeCondition(backEdge, static_cast<std::uint8_t>((compact.GetEdgeCondition(backEdge) & 0xf8u) | 4u));
            cell->tileNeighborhoods[networkType] = compact.Packed();
            cell->RecomputeEdgesFromAllTypes();
        }
    }
}

bool cSC4NetworkTool::AttemptNeighborConnections(bool allowDiagonals, std::uint32_t networkType) {
    INetworkToolHost* host = GetNetworkToolHost();
    if (!host || anchors_.size() < 2 || networkType >= kNetworkTypeCount) return false;
    const INetworkTypeInfo* info = host->NetworkTypeInfo(networkType);   // sNetworkTypeInfo[networkType] +0x1c / +0x20
    const int penX = info ? info->PenWidthX() : 1, penZ = info ? info->PenWidthZ() : 1;
    SC4PointU start = anchors_[anchors_.size() - 2];
    SC4PointU end = anchors_[anchors_.size() - 1];
    DrawNetworkLine(start, end, allowDiagonals, networkType);                     // vslot 0x48 (i386)
    if (start.x == end.x && start.z == end.z) return false;
    if (steps_.empty() || cells_.empty()) return false;
    int dir = directionA_ >> 1;
    int nextX = 0, nextZ = 0;
    std::uint32_t edgeBits = 0;
    bool sameCity = true;
    const tDraggedStep& last = steps_.back();
    auto across = [&](int x, int z) { return host->NeighborCityAcross(x, z); };
    if (straightSteps_ < totalSteps_) {                                            // the drag ends on a diagonal
        if (penZ != 1) return false;
        if (last.lastCell - last.firstCell + 1 < 2) dir = (dir + directionB_ - directionA_) & 3;
        // the decompile names the anchor count here; the end cell's x is what the z half and the straight branch use
        nextX = static_cast<int>(end.x) + kNextX[dir];
        nextZ = static_cast<int>(end.z) + kNextZ[dir];
        if (static_cast<std::uint32_t>(nextX) < static_cast<std::uint32_t>(cityWidth_) &&
            static_cast<std::uint32_t>(nextZ) < static_cast<std::uint32_t>(cityHeight_)) return false;
        edgeBits = (directionB_ - dir * 2 < 5 ? 3u : 1u) << (dir * 8);
        if (penX >= 2)
            sameCity = across(static_cast<int>(cells_[last.firstCell].x), nextZ) == across(static_cast<int>(cells_[last.lastCell].x), nextZ);
    } else {
        nextX = static_cast<int>(end.x) + kNextX[dir];
        nextZ = static_cast<int>(end.z) + kNextZ[dir];
        if (static_cast<std::uint32_t>(nextX) < static_cast<std::uint32_t>(cityWidth_) &&
            static_cast<std::uint32_t>(nextZ) < static_cast<std::uint32_t>(cityHeight_)) return false;
        edgeBits = 2u << (dir * 8);
        if (penX >= 2) {
            if ((dir & 1) == 0)
                sameCity = across(nextX, static_cast<int>(cells_[last.firstCell].z)) == across(nextX, static_cast<int>(cells_[last.lastCell].z));
            else
                sameCity = across(static_cast<int>(cells_[last.firstCell].x), nextZ) == across(static_cast<int>(cells_[last.lastCell].x), nextZ);
        }
    }
    if (!sameCity) return false;
    if ((edgeBits & 0x1010101u) != 0 && penZ >= 2) return false;
    const std::uint32_t connectionType = kNetworkTypeToNeighborConnectionType[networkType_ < kNetworkTypeCount ? networkType_ : 0];
    if (!host->HasNeighborsSimulator() || connectionType == 0) return false;
    bool flag = false;
    if (connectionType == 3 || connectionType == 0xc || connectionType == 8) {     // the two-sided networks: the driving side
        flag = nextX == cityWidth_ || nextZ == -1;
        if (!host->RoadDrivingSideRight()) flag = !flag;
    }
    const bool atXEdge = nextX == -1 || nextX == cityWidth_;
    const bool atZEdge = nextZ == -1 || nextZ == cityHeight_;
    for (std::uint32_t index = last.firstCell;; ++index) {
        if (last.lastCell < index) {                                                // every cell checked: record the connections
            auto record = [&](std::uint32_t key, int x, int z, bool side) {
                tNeighborConnection connection;
                connection.unknown0 = key;
                connection.unknown4 = edgeBits;
                connection.info.x = static_cast<std::uint32_t>(x);
                connection.info.z = static_cast<std::uint32_t>(z);
                connection.info.unknown8 = connectionType;
                connection.info.unknownC = side ? 1 : 0;
                pendingNeighborConnections_.push_back(connection);
                neighborConnections_.push_back(connection);
            };
            if ((edgeBits & 0x1010101u) == 0) {
                for (std::uint32_t c = last.firstCell; c <= last.lastCell; ++c) {
                    int x = nextX, z = nextZ;
                    if (atXEdge) z = static_cast<int>(cells_[c].z);
                    else if (atZEdge) x = static_cast<int>(cells_[c].x);
                    record(cells_[c].z * 0x10000u + cells_[c].x, x, z, c == last.firstCell ? flag : !flag);
                }
            } else {
                if (penZ != 1) return false;
                const int cx = nextX < 0 ? 0 : nextX == cityWidth_ ? nextX - 1 : nextX;
                const int cz = nextZ < 0 ? 0 : nextZ == cityHeight_ ? nextZ - 1 : nextZ;
                record(static_cast<std::uint32_t>(cx) + static_cast<std::uint32_t>(cz) * 0x10000u, nextX, nextZ, false);
            }
            return true;
        }
        const SC4PointU& cell = cells_[index];
        if (cSC4NetworkCellInfo* cellInfo = GetCellInfo(cell.x, cell.z)) {
            if ((kFlagForNetworkType[networkType_] & cellInfo->networkFlags) == 0) return false;
            auto edge = [&](std::uint32_t packed) { return (packed >> ((dir & 3) << 3)) & 0xffu; };
            if (edge(cellInfo->compact54) != 0) {
                for (std::uint32_t t = 0; t < kNetworkTypeCount; ++t) {
                    const std::uint32_t condition = edge(cellInfo->tileNeighborhoods[t]);
                    if (condition == 0) continue;
                    if (condition != ((edgeBits >> (dir * 8)) & 0xffu)) return false;
                    if (networkType_ != t && kNetworkMultitypePriority[networkType_] < kNetworkMultitypePriority[t] &&
                        (g_overrideMatrix[networkType_] & (1u << t)) == 0) return false;
                }
            }
        }
        if (atXEdge) {
            if (host->NeighborConnectionAt(connectionType, nextX, static_cast<int>(cell.z))) return false;
        } else if (atZEdge) {
            if (host->NeighborConnectionAt(connectionType, static_cast<int>(cell.x), nextZ)) return false;
        }
    }
}

// i386 0x00123e36: each recorded connection replaces the connections of the other network types this drag may replace
// (the placement mask) on its cell, then is created in the neighbours simulator.
void cSC4NetworkTool::PlaceNeighborConnections() {
    INetworkToolHost* host = GetNetworkToolHost();
    if (!host || !host->HasNeighborsSimulator()) return;
    for (const tNeighborConnection& connection : neighborConnections_) {
        const int x = static_cast<int>(connection.info.x), z = static_cast<int>(connection.info.z);
        for (std::uint32_t t = 0; t < kNetworkTypeCount; ++t)
            if (networkType_ != t && (placementFlagMask_ & kFlagForNetworkType[t]) != 0 && kNetworkTypeToNeighborConnectionType[t] != 0)
                host->DeleteNeighborConnection(kNetworkTypeToNeighborConnectionType[t], x, z);
        host->CreateNeighborConnection(connection.info.unknown8, x, z, connection.info.unknownC != 0);
    }
}

// i386 0x00125220 (0x100186d56). PORTED: the multitype rules {0x0a5bcf4b, 0xaa5bcf57, 0x10000001}
// (ReadAllMultiTypeIntersectionSolutions + SC4NetworkLoadOverrideMatrix) and the highway intersection rules (0x10000000,
// ReadHighwayIntersectionRules). Not yet: the network type info ini (0x8a5971c5/0x8a5993b9, the host holds those records)
// and the tile-conflict overrides (0x10000002).
void cSC4NetworkTool::ReloadRules() {
    if (INetworkToolHost* host = GetNetworkToolHost()) {
        if (cIGZFile* file = host->OpenResourceFile(cGZPersistResourceKey{kNetworkRulesType, kNetworkRulesGroup, 0x10000000})) {
            cRZIniFile ini(file);
            ReloadIntersectionRules(ini);
            file->Release();
        }
    }
    std::string text;
    if (!LoadTileRuleResourceText(cGZPersistResourceKey{kNetworkRulesType, kNetworkRulesGroup, 0x10000001}, text)) return;
    const IniSections sections = ParseIni(text);
    // ReadAllMultiTypeIntersectionSolutions (0x0015486c): every pair a < b reads "[<a><b>IntersectionSolutions]",
    // each value "edgesA,edgesB,piece,rotation,flip" (AddMultiTypeIntersectionSolution 0x001567e2).
    g_mixedTypeSolutions.clear();
    for (std::uint32_t a = 0; a < kNetworkTypeCount; ++a) {
        for (std::uint32_t b = a + 1; b < kNetworkTypeCount; ++b) {
            auto& solutions = g_mixedTypeSolutions[{a, b}];
            const auto it = sections.find(Lower(std::string(kNetworkNames[a]) + kNetworkNames[b] + "IntersectionSolutions"));
            if (it == sections.end()) continue;
            for (const auto& entry : it->second) {
                std::uint32_t fields[5];
                int count = 0;
                std::size_t start = 0;
                while (count < 5) {
                    const std::size_t comma = entry.second.find(',', start);
                    fields[count++] = static_cast<std::uint32_t>(std::strtoul(entry.second.substr(start, comma - start).c_str(), nullptr, 0));
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
                if (count != 5) continue;
                tMixedTypeIntxSolution& solution = solutions[static_cast<std::uint64_t>(fields[0]) | static_cast<std::uint64_t>(fields[1]) << 32];
                solution.pieceType = fields[2];
                solution.rotationFlip = static_cast<std::uint8_t>(fields[3] | (fields[4] != 0 ? 0x80 : 0));
            }
        }
    }
    // SC4NetworkLoadOverrideMatrix (0x00151b60): "[AllowedOverrides]" n=TypeA,TypeB marks both directions.
    std::fill(std::begin(g_overrideMatrix), std::end(g_overrideMatrix), 0u);
    if (const auto it = sections.find("allowedoverrides"); it != sections.end()) {
        for (const auto& entry : it->second) {
            const std::size_t comma = entry.second.find(',');
            if (comma == std::string::npos) continue;
            const int a = NetworkNameToID(Trim(entry.second.substr(0, comma)));
            const int b = NetworkNameToID(Trim(entry.second.substr(comma + 1)));
            if (a < 0 || b < 0) continue;
            g_overrideMatrix[a] |= 1u << b;
            g_overrideMatrix[b] |= 1u << a;
        }
    }
}

void cSC4NetworkTool::SolveMultiTypeIntersections(const std::unordered_set<std::uint32_t>& cells, std::vector<std::uint32_t>& solved,
                                                  std::uint32_t networkType) {
    // One bit per edge whose two-bit connection is set (the ((x & 0x3030303) + 0x3030303 & 0x4040404) * 0xc30c3 >> 20 idiom).
    const auto edgeMask = [](std::uint32_t edges) {
        return static_cast<std::uint8_t>((((edges & 0x3030303u) + 0x3030303u) & 0x4040404u) * 0xc30c3u >> 20);
    };
    // The edges of `inner` that are set all match `outer` (each edge's connection bits, either of the two set).
    const auto runsAlong = [](std::uint32_t outer, std::uint32_t inner) {
        return ((outer ^ inner) & (((inner & 0x2020202u) >> 1 | (inner & 0x1010101u)) * 3)) == 0;
    };
    const auto raised = [](std::uint32_t flag) { return (flag & 0x304) != 0; };
    const int drawn = static_cast<int>(networkType);
    for (std::uint32_t key : cells) {
        cSC4NetworkCellInfo* cell = GetCellInfo(key);
        if (!cell) continue;
        for (;;) {
            const std::uint32_t flags = cell->networkFlags;
            std::uint32_t rest = (flags - 1) & flags;
            const std::uint32_t firstFlag = flags ^ rest;
            if (rest == 0) break;                                                    // one type: nothing shared
            const int typeA = GetNetworkTypeFromFlag(firstFlag);
            const std::uint32_t rest2 = (rest - 1) & rest;
            const std::uint32_t secondFlag = rest ^ rest2;
            const int typeB = GetNetworkTypeFromFlag(secondFlag);
            if (rest2 != 0) {
                // Three or more types: drop a type whose edges run along a higher-priority one of the same level.
                const int types[3] = {typeA, typeB, GetNetworkTypeFromFlag(rest2)};
                const std::uint32_t edges[3] = {cell->tileNeighborhoods[types[0]], cell->tileNeighborhoods[types[1]],
                                                cell->tileNeighborhoods[types[2]]};
                bool dropped = false;
                for (int i = 0; i < 3 && !dropped; ++i) {
                    const int ti = types[i];
                    for (int j = 0; j < 3; ++j) {
                        const int tj = types[j];
                        const bool lower = kNetworkMultitypePriority[ti] < kNetworkMultitypePriority[tj] ||
                                           (kNetworkMultitypePriority[ti] == kNetworkMultitypePriority[tj] && drawn != ti && drawn == tj);
                        if (!lower || !runsAlong(edges[j], edges[i]) || raised(kFlagForNetworkType[ti]) != raised(kFlagForNetworkType[tj])) continue;
                        cell->networkFlags &= ~kFlagForNetworkType[ti];
                        cell->tileNeighborhoods[ti] = 0;
                        cell->RecomputeEdgesFromAllTypes();
                        if (ti == 10) cell->unknown58 = 0;
                        dropped = true;
                        break;
                    }
                }
                if (dropped) continue;                                               // look at the cell again
                break;
            }
            if (cell->buildable) break;                                              // i386 +0x56
            // The winner keeps its pieces: the higher priority, or on a tie the type being drawn (else the first).
            int winner = typeB, loser = typeA;
            std::uint32_t winnerFlag = secondFlag;
            if (kNetworkMultitypePriority[typeB] <= kNetworkMultitypePriority[typeA] &&
                !(kNetworkMultitypePriority[typeA] == kNetworkMultitypePriority[typeB] && typeA != drawn && typeB == drawn)) {
                winner = typeA;
                loser = typeB;
                winnerFlag = firstFlag;
            }
            const std::uint32_t loserFlag = winnerFlag == firstFlag ? secondFlag : firstFlag;
            const std::uint32_t winnerEdges = cell->tileNeighborhoods[winner];
            std::uint32_t loserEdges = cell->tileNeighborhoods[loser];
            if (runsAlong(winnerEdges, loserEdges)) {
                // The loser only follows the winner: an allowed override hands the cell to the winner.
                if (g_overrideMatrix[winner] & (1u << loser)) {
                    cell->tileNeighborhoods[loser] = 0;
                    cell->RecomputeEdgesFromAllTypes();
                    cell->networkFlags = winnerFlag;
                    if (loser == 10) cell->unknown58 = 0;
                }
                break;
            }
            if (raised(winnerFlag) == raised(loserFlag)) {
                // On the same level the loser keeps only the edges the winner does not already connect.
                for (int edge = 0; edge < 4; ++edge) {
                    const std::uint32_t bits = 3u << (edge * 8);
                    if ((winnerEdges & bits) == (loserEdges & bits) && (winnerEdges & bits) != 0) loserEdges &= ~(0xffu << (edge * 8));
                }
                cell->tileNeighborhoods[loser] = loserEdges;
                cell->RecomputeEdgesFromAllTypes();
                if (loser == 10) cell->unknown58 &= edgeMask(loserEdges);
            }
            std::uint32_t streetLike = 0;                                            // road, street, avenue, dirt road
            for (std::uint32_t t = 0; t < kNetworkTypeCount; ++t)
                if (kFlagForNetworkType[t] & 0x849) streetLike |= cell->tileNeighborhoods[t];
            cell->unknown58 &= static_cast<std::uint8_t>(~edgeMask(streetLike));
            const std::uint32_t low = static_cast<std::uint32_t>(std::min(winner, loser)), high = static_cast<std::uint32_t>(std::max(winner, loser));
            const auto pair = g_mixedTypeSolutions.find({low, high});
            if (pair == g_mixedTypeSolutions.end()) break;
            const std::uint32_t lowEdges = low == static_cast<std::uint32_t>(loser) ? loserEdges : winnerEdges;
            const std::uint32_t highEdges = low == static_cast<std::uint32_t>(loser) ? winnerEdges : loserEdges;
            const auto solution = pair->second.find(static_cast<std::uint64_t>(lowEdges) | static_cast<std::uint64_t>(highEdges) << 32);
            if (solution == pair->second.end()) break;
            cell->solvedCellIndex = static_cast<std::int32_t>(solvedCellList_.size());
            tSolvedCell piece;
            piece.pieceType = solution->second.pieceType;
            piece.rotationFlip = solution->second.rotationFlip;
            piece.cellKey = key;
            solvedCellList_.push_back(piece);
            solved.push_back(key);
            unknown10_.push_back({key & 0xffff, key >> 16, flags});
            break;
        }
    }
}

void cSC4NetworkTool::SolveUsingEvaluator(std::unordered_set<std::uint32_t>& padded, std::unordered_set<std::uint32_t>& toSolve,
                                          const cSC4TileEvaluator& evaluator, std::uint32_t networkType) {
    std::unordered_set<std::uint32_t> unsolved;
    ApplyNetworkRules(padded, toSolve, unsolved, evaluator, networkType);
    if (!toSolve.empty()) padded.insert(unsolved.begin(), unsolved.end());
}

// i386 0x00146ce4: take cells from `padded` one at a time and look their neighbourhood up in the rule set. A match places
// the rule's tiles: every tile cell still to solve is removed from the sets and its piece goes to the solved list
// (+0x98). A cell without a match, or whose own cell the rule does not cover, moves to `unsolved`.
void cSC4NetworkTool::ApplyNetworkRules(std::unordered_set<std::uint32_t>& padded, std::unordered_set<std::uint32_t>& toSolve,
                                        std::unordered_set<std::uint32_t>& unsolved, const cSC4TileEvaluator& evaluator,
                                        std::uint32_t networkType) {
    while (!toSolve.empty() && !padded.empty()) {
        const std::uint32_t key = *padded.begin();
        const std::uint32_t x = key & 0xffff, z = key >> 16;
        cSC4NetworkCellInfo* cell = GetCellInfo(key);
        const std::vector<cSC4TileInfo>* solutions = nullptr;
        bool matched = false;
        if (cell) {
            cSC4NetworkTileNeighborhood neighborhood(x, z, this, networkType, false);
            if ((cell->networkFlags & 0x1fff) == kFlagForNetworkType[networkType]) {
                bool ok = true;
                if (cell->buildable) {
                    // A connection piece keeps its node edge types: the cell's edges must agree with both.
                    ok = false;
                    cSC4NetworkOccupant* occupant = cell->occupant;
                    if (occupant && (occupant->GetNetworkFlag() & 0x1fff) == cell->networkFlags) {
                        std::uint32_t a = 0, b = 0;
                        const std::uint32_t own = cell->tileNeighborhoods[networkType];
                        if (!occupant->GetConnectionNodeEdgeTypes(a, b)) {
                            ok = true;
                        } else if ((own & 0xf8f8f8f8) == 0) {
                            std::uint32_t m = a + 0xf0f0f0f & 0x10101010;
                            if (((a ^ own) & (m - (m >> 4))) == 0) {
                                m = own + 0xf0f0f0f & 0x10101010;
                                ok = ((own ^ b) & (m - (m >> 4))) == 0;
                            }
                        }
                    }
                }
                matched = ok && evaluator.Lookup(neighborhood, solutions);
            }
        }
        if (!matched) {
            unsolved.insert(key);
            padded.erase(key);
            continue;
        }
        bool coversCell = false;
        for (const cSC4TileInfo& tile : *solutions) {
            std::uint32_t tx = 0, tz = 0;
            tile.GetCell(x, z, tx, tz);
            if (tx == x && tz == z) coversCell = true;
            const std::uint32_t k = Key(tx, tz);
            std::unordered_set<std::uint32_t>* holder = padded.count(k) ? &padded : (unsolved.count(k) ? &unsolved : nullptr);
            if (!holder) continue;
            holder->erase(k);
            if (toSolve.erase(k) == 0) continue;
            if (cSC4NetworkCellInfo* target = GetCellInfo(k)) target->solvedCellIndex = static_cast<std::int32_t>(solvedCellList_.size());
            tSolvedCell solved;
            solved.pieceType = tile.id;
            solved.rotationFlip = tile.rotation;
            solved.cellKey = k;
            solvedCellList_.push_back(solved);
        }
        if (!coversCell) {
            padded.erase(key);
            unsolved.insert(key);
        }
    }
}

// i386 0x00146290: the tile conflict rules (cSC4NetworkTypeInfo::LoadTileConflictRules, sTileConflictRules) patch pairs of
// neighbouring pieces that may not meet. With no rules loaded it returns true at once, which is what this port does until
// the conflict rules are loaded (NOT PORTED: the patch loop and PatchTilePair).
bool cSC4NetworkTool::AdjustTileSubsets(std::vector<tSolvedCell>&, bool) {
    SC4_UNWIRED("cSC4NetworkTool::AdjustTileSubsets: tile conflict rules (PatchTilePair patch loop) not ported");
    return true;
}

// i386 0x00145f40: the tile pattern verifiers (sTilePatternVerifyRules, "TileVerifiers"). With no verifiers loaded it
// returns true (0x00145f68), as here (NOT PORTED: the pattern walk).
bool cSC4NetworkTool::VerifyTileSubsets(std::vector<tSolvedCell>&) {
    SC4_UNWIRED("cSC4NetworkTool::VerifyTileSubsets: TileVerifiers pattern walk not ported (verifiers not loaded)");
    return true;
}

// ============================================================================================================
// the checks before the solve, and the steps not ported yet
// ============================================================================================================

// i386 vslot 0x114 (0x004cc20c): the first step with a reserved cell ends the drag; a reserved additional draw cancels it.
void cSC4NetworkTool::CheckForReservedTiles(int& steps, std::uint32_t networkType, bool skipDrag) {
    INetworkToolHost* host = GetNetworkToolHost();
    if (!host) return;
    if (!skipDrag) {
        for (int s = 0; s < steps; ++s) {
            const tDraggedStep& step = steps_[static_cast<std::size_t>(s)];
            if (step.direction == 3) continue;
            for (std::uint32_t c = step.firstCell; c <= step.lastCell; ++c) {
                if (host->CellReserved(cells_[c].x, cells_[c].z, networkType)) {   // vslot 0x118
                    steps = s;
                    return;
                }
            }
        }
    }
    for (const tReservedCell& draw : reservedCells_) {
        if (host->CellReserved(draw.x, draw.z, draw.network)) {
            steps = 0;
            return;
        }
    }
}

// i386 vslot 0x11c: the first regular step on bad terrain (water, for the types the water stops) ends the drag.
bool cSC4NetworkTool::CheckForGoodTerrain(int& steps) {
    if (!IsStoppedByWater()) return true;
    for (int s = 0; s < steps; ++s) {
        const tDraggedStep& step = steps_[static_cast<std::size_t>(s)];
        if (step.direction != 0) continue;
        for (std::uint32_t c = step.firstCell; c <= step.lastCell; ++c) {
            if (!IsTerrainOK(cells_[c].x, cells_[c].z)) {
                steps = s;
                return false;
            }
        }
    }
    // Quirk kept from the i386 code (0x0012...): an additional draw on GOOD terrain cancels the drag.
    for (const tReservedCell& draw : reservedCells_) {
        if (IsTerrainOK(draw.x, draw.z)) {
            steps = 0;
            return false;
        }
    }
    return true;
}

// i386 vslot 0xf4 (0x0012...): the regular cells of the valid steps that hold no buildable network piece form the
// demolition region (cSC4NetworkRoutines::ComputeDemolitionRegionForPlacementCells); its cost goes to +0x254. A region
// the network may not clear (a zone of type 10..15) costs one step.
// NOT PORTED: cSC4NetworkRoutines::UpdateHighlights (the highlight of what will go: highlight 5 on, 0 off).
int cSC4NetworkTool::UpdateOccupantHighlights(int validSteps) {
    if (validSteps_ == 0) {
        ClearOccupantHighlights();
        return 0;
    }
    std::vector<SC4PointU> cells;
    for (int s = 0; s < validSteps_; ++s) {
        const tDraggedStep& step = steps_[static_cast<std::size_t>(s)];
        if (step.direction == 3) continue;
        for (std::uint32_t c = step.firstCell; c <= step.lastCell; ++c) {
            const cSC4NetworkCellInfo* cell = GetCellInfo(cells_[c].x, cells_[c].z);
            if (!cell || !cell->buildable) cells.push_back(cells_[c]);
        }
    }
    demolitionCells_ = cells;
    SC4_UNWIRED("cSC4NetworkTool::UpdateOccupantHighlights: cSC4NetworkRoutines::UpdateHighlights (highlight what the network will demolish) not ported");
    std::int64_t cost = 0;
    INetworkToolHost* host = GetNetworkToolHost();
    const bool ok = !host || host->DemolishForNetwork(false, demolitionCells_, cost);
    baseCost_ = static_cast<std::int32_t>(cost);
    return validSteps - (ok ? 0 : 1);
}

// i386 vslot 0xf8: the demolition region goes (spDemolition vslot 0x18 with cSC4DemolishByNetworksOccupantFilter), then
// the highlights.
void cSC4NetworkTool::DestroyHilightedOccupants() {
    if (!demolitionCells_.empty()) {
        std::int64_t cost = 0;
        if (INetworkToolHost* host = GetNetworkToolHost()) host->DemolishForNetwork(true, demolitionCells_, cost);
        demolitionCells_.clear();
    }
    ClearOccupantHighlights();
}
// ============================================================================================================
// the height solve: SmoothenNetwork, CanTilesBeSupportedOnTerrain, UpdateTerrainHeights
// ============================================================================================================
//
// The first constraint satisfier (+0x228, i386 +0x124) settles the network's own vertex heights: every cell of the
// smoothing set (the drag and the network cells around it, ComputeInitialSmoothingSet) gets its corner windows
// (InsertRangeConstraintsForCell, after ApplyPlaneConstraintToRegularCells pulled the regular steps toward the plane
// between the drag's ends) and the slope / smoothness / equality constraints of its piece; the boundary vertices
// around the set (+0x150, i386 +0xb8) stay fixed. The second satisfier (+0x328, i386 +0x19c) then finds terrain
// heights that carry those network heights (CanTilesBeSupportedOnTerrain), and placing writes them into the terrain
// (UpdateTerrainHeights). The cell records keep i386 offsets in the comments (64-bit = i386 + 8 after +0x0c).

namespace {
// ComputeReferencePlaneForVertexMovement::kVert1Offset / kVert2Offset (i386 0x008453a0 / 0x00845360): per drag
// direction, the two vertices (x, z offsets from the cell) of the edge the drag leaves a cell by.
constexpr std::int32_t kVert1Offset[8][2] = {{1, 1}, {0, 1}, {0, 1}, {0, 0}, {0, 0}, {1, 0}, {1, 0}, {1, 1}};
constexpr std::int32_t kVert2Offset[8][2] = {{1, 0}, {1, 0}, {1, 1}, {1, 1}, {0, 1}, {0, 1}, {0, 0}, {0, 0}};
// ApplyPlaneConstraintToRegularCells::kXOffsetForVert / kZOffsetForVert (i386 0x00845414 / 0x00845404): the cell
// corners in vertex-id order (x, z), (x, z+1), (x+1, z+1), (x+1, z).
constexpr std::int32_t kXOffsetForVert[4] = {0, 0, 1, 1};
constexpr std::int32_t kZOffsetForVert[4] = {0, 1, 1, 0};
// GetElevationForReferencePlaneVertex::kXIncrToNeighbor / kZIncrToNeighbor (i386 0x00845340 / 0x00845330): the four
// cells around a vertex.
constexpr std::int32_t kXIncrToNeighbor[4] = {0, 0, -1, -1};
constexpr std::int32_t kZIncrToNeighbor[4] = {0, -1, -1, 0};
// InsertRangeConstraintsForCell::kEdgesToFixVertex (i386 0x00845400): the sides (bit = side) whose connection pins a
// lot-connection cell's corner to the terrain.
constexpr std::uint8_t kRangeEdgesToFixVertex[4] = {0x03, 0x09, 0x0c, 0x06};
// The offset the vertex ids of a lot-connection cell (network flag 0x200000, the record's +0x5e / i386 +0x56) carry,
// so their corners solve apart from the terrain vertices they sit on.
constexpr std::int32_t kLotConnectionVertexOffset = 100000;
// tHtRange's kErrorTolerance (i386 0x008344f0), which ApplyPlaneConstraintToRegularCells reads through a pointer.
constexpr float kErrorTolerance = 0.01f;
// cSC4TrafficAutomaton::kfMinTravelDistance (i386 0x00a70798): the smallest terrain change UpdateTerrainHeights writes.
constexpr float kMinTerrainChange = 0.001f;
// kDefaultMaxDensity (i386 0x00a70808): the headroom above the terrain's highest allowed height SmoothenNetwork gives
// the first satisfier.
constexpr float kSmoothingHeadroom = 20.0f;

float AsFloat(std::uint32_t word) {
    float value = 0.0f;
    std::memcpy(&value, &word, sizeof value);
    return value;
}
// The sides of a packed neighbourhood with any of bits 0-2 set, one bit per side.
std::uint32_t ConnectedSideMask(std::uint32_t edges) {
    return (((edges & 0x07070707u) + 0x07070707u) & 0x08080808u) * 0x01020408u >> 27;
}
bool VertexOfCell(const cSC4NetworkCellInfo& cell, std::int32_t vertex) {
    for (std::int32_t id : cell.vertexBaseHeights)
        if (id == vertex) return true;
    return false;
}
// A cross section of a cell the neighbour is not smoothed with keeps only the vertices it shares with the cell.
void KeepSharedVertices(const cSC4NetworkCellInfo& cell, std::int32_t& a, std::int32_t& b) {
    const bool hasA = VertexOfCell(cell, a), hasB = VertexOfCell(cell, b);
    if (hasA) {
        if (!hasB) b = a;
    } else if (hasB) {
        a = b;
    } else {
        a = b = -1;
    }
}
}  // namespace

// i386 0x0067b176
bool cSC4NetworkTool::IsCellBeingSmoothed(std::uint32_t x, std::uint32_t z) const {
    return unknown128_.count(Key(x, z)) != 0;
}

// i386 0x00142c92. The smoothing set (+0x128) and its fixed boundary vertices (+0x150). A straight drag whose solve
// produced one piece per step takes the drag's bounding rectangle grown by `pad`; anything else grows every solved
// cell by `pad` row by row (the per-row spans are the static `scans`, in vertex coordinates) and fixes the outline.
void cSC4NetworkTool::ComputeInitialSmoothingSet(int pad) {
    unknown128_.clear();
    unknown150_.clear();
    INetworkToolHost* host = GetNetworkToolHost();
    auto fixVertex = [&](std::int32_t x, std::int32_t z) {
        unknown150_.insert(static_cast<std::uint32_t>(host ? host->TerrainVertexIndex(x, z) : -1));   // terrain vslot 0x24
    };
    auto addCell = [&](std::int32_t x, std::int32_t z) {
        const std::uint32_t key = static_cast<std::uint32_t>(x) + static_cast<std::uint32_t>(z) * 0x10000u;
        const cSC4NetworkCellInfo* cell = GetCellInfo(key);
        if (cell && !cell->shortStep) unknown128_.insert(key);                                   // i386 +0x55
    };
    if (totalSteps_ == straightSteps_ && static_cast<int>(solvedCellList_.size()) == totalSteps_) {
        std::int32_t minX = 10000, maxX = -1, minZ = 10000, maxZ = -1;
        for (int s = 0; s < validSteps_; ++s) {
            const tDraggedStep& step = steps_[static_cast<std::size_t>(s)];
            for (std::uint32_t c = step.firstCell; c <= step.lastCell; ++c) {
                const std::int32_t x = static_cast<std::int32_t>(cells_[c].x), z = static_cast<std::int32_t>(cells_[c].z);
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minZ = std::min(minZ, z);
                maxZ = std::max(maxZ, z);
            }
        }
        maxX += pad + 1;
        maxZ += pad + 1;
        minX = std::max(minX - pad, 0);
        minZ = std::max(minZ - pad, 0);
        if (static_cast<std::uint32_t>(maxX) > static_cast<std::uint32_t>(cityWidth_)) maxX = widthPlusOne_ - 1;
        if (static_cast<std::uint32_t>(maxZ) > static_cast<std::uint32_t>(cityHeight_)) maxZ = heightPlusOne_ - 1;
        for (std::int32_t z = minZ; z <= maxZ; ++z) {
            for (std::int32_t x = minX; x <= maxX; ++x) {
                addCell(x, z);
                const bool outline = x == minX || x == maxX || z == minZ || z == maxZ;
                if (outline && x != 0 && x != widthPlusOne_ - 1 && z != 0 && z != heightPlusOne_ - 1) fixVertex(x, z);
            }
        }
        return;
    }
    if (solvedCellList_.empty()) return;
    // ComputeInitialSmoothingSet(long)::scans (i386 0x00bfb500): {first, last} vertex column per vertex row.
    static std::vector<std::int16_t> scans;
    scans.assign(static_cast<std::size_t>(std::max(heightPlusOne_, 0)) * 2, 0);
    for (int row = 0; row < heightPlusOne_; ++row) {
        scans[row * 2] = 10000;
        scans[row * 2 + 1] = -1;
    }
    std::int32_t firstRow = 10000, lastRow = -1;
    for (const tSolvedCell& solved : solvedCellList_) {
        const cSC4NetworkCellInfo* cell = GetCellInfo(solved.cellKey);
        if (!cell) continue;
        std::uint32_t rowHi = cell->z + 1 + static_cast<std::uint32_t>(pad);
        std::int32_t rowLo = static_cast<std::int32_t>(cell->z) - pad;
        if (rowLo < 0) rowLo = 0;
        if (static_cast<std::uint32_t>(heightPlusOne_) <= rowHi) rowHi = static_cast<std::uint32_t>(heightPlusOne_) - 1;
        firstRow = std::min(firstRow, rowLo);
        lastRow = std::max(lastRow, static_cast<std::int32_t>(rowHi));
        std::uint32_t colHi = cell->x + 1 + static_cast<std::uint32_t>(pad);
        std::int32_t colLo = static_cast<std::int32_t>(cell->x) - pad;
        if (colLo < 0) colLo = 0;
        if (static_cast<std::uint32_t>(widthPlusOne_) <= colHi) colHi = static_cast<std::uint32_t>(widthPlusOne_) - 1;
        for (std::int32_t row = rowLo; row <= static_cast<std::int32_t>(rowHi); ++row) {
            if (colLo < scans[row * 2]) scans[row * 2] = static_cast<std::int16_t>(colLo);
            if (scans[row * 2 + 1] < static_cast<std::int32_t>(colHi)) scans[row * 2 + 1] = static_cast<std::int16_t>(colHi);
        }
    }
    // The cells inside the spans (the last row and each span's last column are vertex bounds, not cells).
    for (std::int32_t row = firstRow; row < lastRow; ++row)
        for (std::int32_t x = scans[row * 2]; x < scans[row * 2 + 1]; ++x) addCell(x, row);
    // The outline: the first and last rows whole, then each row's ends and the parts its neighbours do not cover.
    auto fixRow = [&](std::int32_t row) {
        for (std::int32_t x = scans[row * 2]; x <= scans[row * 2 + 1]; ++x)
            if (x != 0 && x != widthPlusOne_ - 1) fixVertex(x, row);
    };
    if (firstRow != 0) fixRow(firstRow);
    if (lastRow != heightPlusOne_ - 1) fixRow(lastRow);
    for (std::int32_t row = firstRow + 1; row <= lastRow - 1; ++row) {
        const std::int32_t lo = scans[row * 2], hi = scans[row * 2 + 1];
        if (hi < 0) continue;
        const std::int32_t neighbors[4] = {scans[(row - 1) * 2], scans[(row - 1) * 2 + 1], scans[(row + 1) * 2], scans[(row + 1) * 2 + 1]};
        if (scans[(row - 1) * 2 + 1] < 0 || scans[(row + 1) * 2 + 1] < 0) {
            for (std::uint32_t x = lo != 0 ? static_cast<std::uint32_t>(lo) : 1u;
                 static_cast<std::int32_t>(x) <= hi && x < static_cast<std::uint32_t>(widthPlusOne_) - 1; ++x)
                fixVertex(static_cast<std::int32_t>(x), row);
            continue;
        }
        if (lo != 0) fixVertex(lo, row);
        if (hi != widthPlusOne_ - 1) fixVertex(hi, row);
        for (int k = 0; k < 4; ++k) {
            const std::int32_t v = neighbors[k];
            if (!(lo < v && v < hi)) continue;
            if ((k & 1) == 0) {
                for (std::int32_t x = lo + 1; x <= v; ++x) fixVertex(x, row);
            } else {
                for (std::int32_t x = v; x != hi; ++x) fixVertex(x, row);
            }
        }
    }
}

// i386 0x0014363e (vslot 0x138). A free cell's corners get their windows (a fixed boundary vertex keeps its height),
// a solved or immovable cell's corners are fixed, and a lot-connection cell fixes its offset corners plus the terrain
// corners of the sides that connect outside the lot.
void cSC4NetworkTool::InsertRangeConstraintsForCell(std::uint32_t cellKey) {
    cSC4NetworkCellInfo* cell = GetCellInfo(cellKey);
    INetworkConstraintSatisfier* satisfier = constraintA_;
    if (!cell || !satisfier) return;
    if (!cell->buildable) {                                                                       // i386 +0x56
        if (!cell->solvedOrImmovable) {                                                           // i386 +0x53
            for (int i = 0; i < 4; ++i) {
                const std::int32_t vertex = cell->vertexBaseHeights[i];
                if (unknown150_.count(static_cast<std::uint32_t>(vertex)) == 0) {
                    satisfier->AddVertexToSolve(vertex, cell->vertexHeights[i]);
                    satisfier->InsertRangeConstraint(vertex, cell->range70[i].low, cell->range70[i].high);
                    satisfier->InsertPreferredRangeConstraint(vertex, cell->preferred90[i].low, cell->preferred90[i].high);
                    if (cell->hasOccupant) satisfier->InsertPreferredValueConstraint(vertex, cell->vertexHeights[i]);   // i386 +0x51
                } else {
                    satisfier->InsertFixedVertex(vertex, cell->vertexHeights[i]);
                }
            }
        } else {
            for (int i = 0; i < 4; ++i) satisfier->InsertFixedVertex(cell->vertexBaseHeights[i], cell->vertexHeights[i]);
        }
        return;
    }
    INetworkToolHost* host = GetNetworkToolHost();
    std::int32_t lot[4] = {0, 0, 0, 0};
    if (!host || !host->LotBoundingRectAt(cell->x, cell->z, lot)) return;                         // spLotManager vslot 0x14
    std::uint32_t edges = 0;
    for (std::uint32_t compact : cell->tileNeighborhoods) edges |= compact;
    for (int side = 0; side < 4; ++side) {
        const std::int32_t nx = static_cast<std::int32_t>(cell->x) + kNextX[side];
        const std::int32_t nz = static_cast<std::int32_t>(cell->z) + kNextZ[side];
        if (lot[0] <= nx && lot[1] <= nz && nx <= lot[2] && nz <= lot[3]) edges &= ~(0xffu << (side * 8));
    }
    const std::uint32_t sides = ConnectedSideMask(edges);
    for (int i = 0; i < 4; ++i) {
        const std::int32_t vertex = cell->vertexBaseHeights[i];
        satisfier->AddVertexToSolve(vertex + kLotConnectionVertexOffset, cell->vertexHeights[i]);
        satisfier->InsertFixedVertex(vertex + kLotConnectionVertexOffset, cell->vertexHeights[i]);
        if (sides & kRangeEdgesToFixVertex[i]) {
            satisfier->AddVertexToSolve(vertex, cell->vertexHeights[i]);
            satisfier->InsertFixedVertex(vertex, cell->vertexHeights[i]);
        }
    }
}

// i386 0x00142758. A drag end's height comes from the terrain; the vertex is "pinned" when a lot leaves it no
// adjustment range or an existing network piece outside the smoothing set touches it.
void cSC4NetworkTool::GetElevationForReferencePlaneVertex(std::int32_t vertex, float& height, bool& pinned) {
    INetworkToolHost* host = GetNetworkToolHost();
    const std::uint32_t z = static_cast<std::uint32_t>(vertex) / static_cast<std::uint32_t>(widthPlusOne_);
    const std::uint32_t x = static_cast<std::uint32_t>(vertex) % static_cast<std::uint32_t>(widthPlusOne_);
    pinned = false;
    height = host ? host->TerrainVertexHeight(vertex) : 0.0f;                                     // terrain vslot 0x34
    float minAltitude = 0.0f, maxAltitude = 0.0f;
    if (host) host->LotVertexAltitudeAdjustmentRange(static_cast<std::int32_t>(x), static_cast<std::int32_t>(z), minAltitude, maxAltitude);
    if (maxAltitude == 0.0f) {
        pinned = true;
        return;
    }
    for (int k = 0; k < 4; ++k) {
        const std::uint32_t nx = x + static_cast<std::uint32_t>(kXIncrToNeighbor[k]);
        const std::uint32_t nz = z + static_cast<std::uint32_t>(kZIncrToNeighbor[k]);
        if (nx >= static_cast<std::uint32_t>(cityWidth_) || nz >= static_cast<std::uint32_t>(cityHeight_)) continue;
        if (unknown128_.count(nx + nz * 0x10000u)) continue;
        if (cSC4NetworkOccupant* occupant = GetNetworkOccupant(nx, nz)) {   // occupant manager vslot 0x84 + the type mask
            occupant->Release();
            pinned = true;
            break;
        }
    }
}

// i386 0x00142956. The plane through the edge the first step leaves its cell by and the edge the last step enters
// its cell by, at the given heights (a height <= 0 is taken from the drag end: a pinned vertex wins, else the mean).
void cSC4NetworkTool::ComputeReferencePlaneForVertexMovement(float* plane, int firstStep, int lastStep, float startHeight,
                                                             float endHeight) {
    const SC4PointU& start = cells_[steps_[static_cast<std::size_t>(firstStep)].firstCell];
    const SC4PointU& end = cells_[steps_[static_cast<std::size_t>(lastStep)].firstCell];
    const int direction = straightSteps_ < 1 ? directionB_ : directionA_;
    const int opposite = (direction + 4) % 8;
    const std::int32_t s1x = static_cast<std::int32_t>(start.x) + kVert1Offset[direction][0];
    const std::int32_t s1z = static_cast<std::int32_t>(start.z) + kVert1Offset[direction][1];
    const std::int32_t s2x = static_cast<std::int32_t>(start.x) + kVert2Offset[direction][0];
    const std::int32_t s2z = static_cast<std::int32_t>(start.z) + kVert2Offset[direction][1];
    const std::int32_t e1x = static_cast<std::int32_t>(end.x) + kVert1Offset[opposite][0];
    const std::int32_t e1z = static_cast<std::int32_t>(end.z) + kVert1Offset[opposite][1];
    const std::int32_t e2x = static_cast<std::int32_t>(end.x) + kVert2Offset[opposite][0];
    const std::int32_t e2z = static_cast<std::int32_t>(end.z) + kVert2Offset[opposite][1];
    auto endHeightAt = [&](std::int32_t ax, std::int32_t az, std::int32_t bx, std::int32_t bz) {
        float a = 0.0f, b = 0.0f;
        bool pinned = false;
        GetElevationForReferencePlaneVertex(widthPlusOne_ * az + ax, a, pinned);
        if (pinned) return a;
        GetElevationForReferencePlaneVertex(bz * widthPlusOne_ + bx, b, pinned);
        return pinned ? b : (a + b) * 0.5f;
    };
    if (startHeight <= 0.0f) startHeight = endHeightAt(s1x, s1z, s2x, s2z);
    if (endHeight <= 0.0f) endHeight = endHeightAt(e1x, e1z, e2x, e2z);
    if (firstStep == lastStep && straightSteps_ <= firstStep) {
        plane[0] = 0.0f;
        plane[1] = 1.0f;
        plane[2] = 0.0f;
        plane[3] = -startHeight;
        return;
    }
    const float cs = cellSize_;
    const float p0[3] = {static_cast<float>(s1x) * cs, startHeight, static_cast<float>(s1z) * cs};
    const float p1[3] = {static_cast<float>(s2x) * cs, startHeight, static_cast<float>(s2z) * cs};
    const float p2[3] = {static_cast<float>(e1x) * cs, endHeight, static_cast<float>(e1z) * cs};
    SC4GeometryUtilities::TriangleToPlaneEquation(p0, p1, p2, plane);
}

// i386 0x00143c30 (vslot 0x13c). Every regular step's corners move toward the plane between the immovable portions
// (bridges, tunnels) around it, or between the drag's ends: the window narrows to the plane height +- params +0x20
// (widened to keep the corner's current height) and the preferred range spans the plane height and the current one.
void cSC4NetworkTool::ApplyPlaneConstraintToRegularCells(std::vector<tImmovablePortion>& portions) {
    int planeEnd = -1;
    bool havePlane = false;
    float plane[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float scale = 0.0f;
    const float deviation = AsFloat(placement_.unknown20);                                       // i386 +0x2f0
    for (int s = 0; s < validSteps_; ++s) {
        const tDraggedStep& step = steps_[static_cast<std::size_t>(s)];
        if (step.direction != 0) continue;
        if (planeEnd < s || !havePlane) {
            int first = -1;
            float startHeight = 0.0f, endHeight = 0.0f;
            planeEnd = 0x7fffffff;
            for (const tImmovablePortion& portion : portions) {
                if (portion.step < s && first < portion.step) {
                    startHeight = portion.valueB;
                    first = portion.step;
                }
                if (s < portion.x && portion.x < planeEnd) {
                    endHeight = portion.valueA;
                    planeEnd = portion.x;
                }
            }
            if (first == -1) {
                startHeight = 0.0f;
                first = s;
            }
            if (planeEnd == 0x7fffffff) {
                planeEnd = validSteps_ - 1;
                endHeight = 0.0f;
            }
            ComputeReferencePlaneForVertexMovement(plane, first, planeEnd, startHeight, endHeight);
            if (plane[1] == 0.0f) return;
            scale = -1.0f / plane[1];
            havePlane = true;
        }
        for (std::uint32_t c = step.firstCell; c <= step.lastCell; ++c) {
            cSC4NetworkCellInfo* cell = GetCellInfo(Key(cells_[c].x, cells_[c].z));
            if (!cell) continue;
            for (int i = 0; i < 4; ++i) {
                const float h = (cellSize_ * static_cast<float>(cell->z + kZOffsetForVert[i]) * plane[2] +
                                 static_cast<float>(cell->x + kXOffsetForVert[i]) * cellSize_ * plane[0] + plane[3]) * scale;
                float lo = h - deviation, hi = h + deviation;
                const float current = cell->vertexHeights[i];
                if (lo <= current) {
                    if (hi < current) hi = current;
                } else {
                    lo = current;
                }
                tVertexHeightRange& range = cell->range70[i];
                if (range.low < lo) range.low = lo;
                if (hi < range.high) range.high = hi;
                if (range.high < range.low && range.low - range.high <= kErrorTolerance) range.high = range.low;
                if (h <= current) cell->preferred90[i] = {h, current};
                else cell->preferred90[i] = {current, h};
            }
        }
    }
}

// i386 0x0067af84
void cSC4NetworkTool::InsertSmoothnessConstraint(int vertexA, int vertexB, int vertexC, float limit) {
    if (vertexA < 0 || vertexB < 0 || vertexC < 0 || vertexA == vertexB || vertexB == vertexC || vertexA == vertexC) return;
    if (constraintA_) constraintA_->InsertSmoothnessConstraint(vertexA, vertexB, vertexC, limit);
}

// i386 0x00143918. For the edge of `side` (when the network of `networkType` crosses it): the cross section of the
// cell at that edge (`here`), of the neighbour across it (`after`) and of the cell the piece continues to on the other
// side (`before`). `diagonal` is set for a diagonal edge. A cross section of a cell outside the smoothing set keeps
// only the vertices it shares with this cell.
bool cSC4NetworkTool::GetCrossSections(const cSC4NetworkCellInfo& cell, std::uint32_t networkType, int side, tCrossSection& here,
                                       tCrossSection& before, tCrossSection& after, bool& diagonal) {
    const std::uint32_t edge = cell.tileNeighborhoods[networkType] >> (side * 8);
    const std::uint32_t condition = edge & 3;
    if (condition == 0) return false;
    const std::uint32_t turn = condition + 6 + static_cast<std::uint32_t>(side) * 2;
    const std::uint32_t corner = turn & 3;
    diagonal = (turn & 1) != 0;
    // Init's table at +0x4bc (i386 +0x290 / +0x2b0): the two corner offsets of each cross section direction.
    const std::int32_t offsetA = initScratch_[corner], offsetB = initScratch_[8 + corner];
    const std::int32_t own = cell.buildable ? kLotConnectionVertexOffset : 0;
    const std::int32_t base = static_cast<std::int32_t>(cell.z) * widthPlusOne_ + static_cast<std::int32_t>(cell.x);
    here.a = base + offsetA + own;
    here.b = own + base + offsetB;
    const std::uint32_t nx = cell.x + static_cast<std::uint32_t>(kNextX[side]);
    const std::uint32_t nz = cell.z + static_cast<std::uint32_t>(kNextZ[side]);
    const std::int32_t next = static_cast<std::int32_t>(nz) * widthPlusOne_ + static_cast<std::int32_t>(nx);
    after.a = next + offsetA;
    after.b = next + offsetB;
    if (nx < static_cast<std::uint32_t>(cityWidth_) && nz < static_cast<std::uint32_t>(cityHeight_)) {
        const cSC4NetworkCellInfo* neighbor = GetCellInfo(nx, nz);
        if (neighbor && neighbor->buildable) {
            after.a += kLotConnectionVertexOffset;
            after.b += kLotConnectionVertexOffset;
        }
    }
    if (!IsCellBeingSmoothed(nx, nz)) KeepSharedVertices(cell, after.a, after.b);
    int otherSide = side;
    if (condition == 2) otherSide = side + 2;
    else if (condition == 3) otherSide = side + 3;
    else if (condition == 1) otherSide = side + 1;
    otherSide &= 3;
    const std::uint32_t ox = cell.x + static_cast<std::uint32_t>(kNextX[otherSide]);
    const std::uint32_t oz = cell.z + static_cast<std::uint32_t>(kNextZ[otherSide]);
    const std::int32_t other = static_cast<std::int32_t>(oz) * widthPlusOne_ + static_cast<std::int32_t>(ox);
    before.a = offsetA + other;
    before.b = other + offsetB;
    const bool connected = cSC4TileNeighborhoodCompact(cell.tileNeighborhoods[networkType]).GetEdgeCondition(otherSide) != 0;
    if (!connected || ox >= static_cast<std::uint32_t>(cityWidth_) || oz >= static_cast<std::uint32_t>(cityHeight_) || !IsCellBeingSmoothed(ox, oz)) {
        before.a += own;
        before.b += own;
        KeepSharedVertices(cell, before.a, before.b);
    } else {
        const cSC4NetworkCellInfo* neighbor = GetCellInfo(ox, oz);
        if (neighbor && neighbor->buildable) {
            before.a += kLotConnectionVertexOffset;
            before.b += kLotConnectionVertexOffset;
        }
    }
    return true;
}

// i386 0x001440c2. A piece with three or four sides (other than a straight crossing of one type), or a two-sided piece
// of a type with special pieces (+0xf0) whose edges carry a special bit, gets one height for all four corners. The
// others get, per crossed edge, level cross sections, the slope limit between consecutive sections and the smoothness
// limit over three of them.
void cSC4NetworkTool::InsertSlopeAndSmoothnessConstraintsForCell(std::uint32_t cellKey) {
    const cSC4NetworkCellInfo* cell = GetCellInfo(cellKey);
    if (!cell || cell->unknown5a || cell->solvedOrImmovable || !constraintA_) return;            // i386 +0x52 / +0x53
    INetworkConstraintSatisfier* satisfier = constraintA_;
    const std::int32_t own = cell->buildable ? kLotConnectionVertexOffset : 0;
    const std::uint32_t flags = cell->networkFlags;
    const int type = GetFirstNetworkTypeFromFlags(flags);
    INetworkToolHost* host = GetNetworkToolHost();
    const INetworkTypeInfo* info = host ? host->NetworkTypeInfo(static_cast<std::uint32_t>(type)) : nullptr;
    static const cSC4NetworkSlopeLimits kDefaultLimits;
    const cSC4NetworkSlopeLimits& limits = info ? info->SlopeLimits() : kDefaultLimits;
    const std::uint32_t compact = cell->compact54;                                               // i386 +0x4c
    const int sides = ((compact & 0x3u) != 0) + ((compact & 0x300u) != 0) + ((compact & 0x3000000u) != 0) + ((compact & 0x30000u) != 0);
    const bool simple = static_cast<unsigned>(sides - 1) < 2u ||
                        ((flags & (flags - 1)) == 0 && (compact == 0x3010301u || compact == 0x1030103u));
    const bool specialPieces = info && !info->PlacementParams().listD0.empty();                  // +0xf0
    if ((specialPieces && (compact & 0xf8f8f8f8u) != 0 && sides == 2) || !simple) {
        for (int i = 0; i < 3; ++i) {
            const std::int32_t a = own + cell->vertexBaseHeights[i], b = own + cell->vertexBaseHeights[i + 1];
            if (b != a && a >= 0 && b >= 0) satisfier->InsertEqualityConstraint(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b));
        }
        return;
    }
    for (int side = 0; side < 4; ++side) {
        tCrossSection here, before, after;
        bool diagonal = false;
        if (!GetCrossSections(*cell, static_cast<std::uint32_t>(type), side, here, before, after, diagonal)) continue;
        const float slope = diagonal ? limits.maxSlopeDiagonal : limits.maxSlope;
        const float smoothness = diagonal ? limits.maxSmoothnessDiagonal : limits.maxSmoothness;
        auto level = [&](const tCrossSection& section) {
            if (section.b != section.a && section.a >= 0 && section.b >= 0)
                satisfier->InsertEqualityConstraint(static_cast<std::uint32_t>(section.a), static_cast<std::uint32_t>(section.b));
        };
        level(here);
        level(before);
        level(after);
        if (before.a >= 0 && here.a >= 0 && here.a != before.a) satisfier->InsertSlopeConstraint(before.a, here.a, slope);
        if (here.a >= 0 && after.a >= 0 && after.a != here.a) satisfier->InsertSlopeConstraint(here.a, after.a, slope);
        InsertSmoothnessConstraint(before.a, here.a, after.a, smoothness);
    }
}

// i386 0x001443f6: the windows, then the slope / smoothness constraints, then the derived tool's own constraints.
void cSC4NetworkTool::InsertConstraintsForCellsToBeSmoothed(const std::unordered_set<std::uint32_t>& cells) {
    for (std::uint32_t key : cells) InsertRangeConstraintsForCell(key);                          // vslot 0x138
    for (std::uint32_t key : cells) InsertSlopeAndSmoothnessConstraintsForCell(key);
    for (std::uint32_t key : cells) InsertNetworkSpecificConstraintsForCell(key);                // vslot 0x140
}

// i386 0x001444ae. The recorded intersections' cells join the solved list for the smoothing set only (cut back after it); the
// bridges' deck heights and the tunnel portals' heights become immovable portions; the intersections' constraint layouts
// come after the cells' own constraints.
bool cSC4NetworkTool::SmoothenNetwork(int pad) {
    if (!constraintA_) return true;
    const std::size_t solvedCount = solvedCellList_.size();
    InsertIntersectionCellsIntoSolvedSet();
    PropagateSolvedCellsForSmoothing();                                                          // vslot 0x134
    ComputeInitialSmoothingSet(pad);
    solvedCellList_.resize(solvedCount);
    std::vector<tImmovablePortion> portions;
    CopyBridgeElevationConstraintsIntoCellInfo(portions);
    CopyTunnelElevationConstraintsIntoCellInfo(portions);
    ApplyPlaneConstraintToRegularCells(portions);                                                // vslot 0x13c
    INetworkToolHost* host = GetNetworkToolHost();
    constraintA_->Shutdown();
    constraintA_->Init(0.0f, (host ? host->TerrainMaxAllowedHeight() : 0.0f) + kSmoothingHeadroom);   // terrain vslot 0x60
    InsertConstraintsForCellsToBeSmoothed(unknown128_);
    InsertIntersectionConstraints(*constraintA_);
    return constraintA_->Solve();
}

// i386 0x001454d0 (vslot 0xe8). The terrain under every smoothed cell must carry the network: a free corner's terrain
// may rise at most params +0x0c above the terrain and sink to params +0x10 below it but not above the road (a piece
// with a support structure may also hang params +0x3c above it); otherwise it is pulled to the road height. The
// boundary vertices and the corners of placed immovable pieces keep their terrain height.
bool cSC4NetworkTool::CanTilesBeSupportedOnTerrain(std::uint32_t networkType) {
    INetworkConstraintSatisfier* support = constraintB_;
    if (!support) return true;
    INetworkToolHost* host = GetNetworkToolHost();
    support->Shutdown();
    support->Init(-3.4028235e38f, 3.4028235e38f);
    const INetworkTypeInfo* info = host ? host->NetworkTypeInfo(networkType) : nullptr;
    const cSC4NetworkPlacementParams* params = info ? &info->PlacementParams() : nullptr;
    const float maxRise = params ? AsFloat(params->unknown00[3]) : 0.0f;                         // record +0x5c
    const float maxCut = params ? params->float10[0] : 0.0f;                                     // record +0x60
    const float maxSupportDrop = params ? params->float40[2] : 0.0f;                             // record +0x8c
    for (std::uint32_t key : unknown128_) {
        const cSC4NetworkCellInfo* cell = GetCellInfo(key);
        if (!cell || cell->buildable) continue;                                                  // i386 +0x56
        for (int i = 0; i < 4; ++i) {
            const std::int32_t vertex = cell->vertexBaseHeights[i];
            float network = 0.0f;
            if (!constraintA_ || !constraintA_->GetHeight(vertex, network)) network = cell->vertexHeights[i];
            const float terrain = host ? host->TerrainVertexHeight(vertex) : 0.0f;
            if (!support->IsInToSolveSet(vertex)) support->AddVertexToSolve(vertex, terrain);
            float fixed = terrain;
            if (unknown150_.count(static_cast<std::uint32_t>(vertex)) == 0 && (!cell->solvedOrImmovable || !cell->hasOccupant)) {
                float low = terrain - maxCut;
                float high = network;
                if (terrain + maxRise <= network) high = terrain + maxRise;
                std::uint32_t piece = 0;
                bool hasPiece = false;
                if (cell->solvedCellIndex < 0) {
                    if (cell->occupant) {
                        piece = cell->occupant->PieceId();                                       // vslot 0x5c
                        hasPiece = true;
                    }
                } else {
                    piece = solvedCellList_[static_cast<std::size_t>(cell->solvedCellIndex)].pieceType;
                    hasPiece = true;
                }
                if (hasPiece && GetSupportStructureInfo(piece)) {
                    const float hanging = network - maxSupportDrop;
                    if (low < hanging) low = hanging;
                    if (high < low) return false;
                } else if (!cell->solvedOrImmovable) {
                    if (high < network) return false;
                    low = network;
                    high = network;
                }
                if (!(high == low)) {
                    support->InsertRangeConstraint(vertex, low, high);
                    support->InsertPreferredValueConstraint(vertex, cell->solvedOrImmovable ? terrain : network);
                    continue;
                }
                fixed = high;
            }
            support->InsertFixedVertex(vertex, fixed);
        }
    }
    InsertIntersectionConstraints(*support);
    for (std::uint32_t vertex : unknown150_) {
        const std::int32_t id = static_cast<std::int32_t>(vertex);
        if (support->IsInToSolveSet(id)) continue;
        const float terrain = host ? host->TerrainVertexHeight(id) : 0.0f;
        support->AddVertexToSolve(id, terrain);
        support->InsertFixedVertex(id, terrain);
    }
    return support->Solve();
}

// i386 0x00145306 (vslot 0xec). Every terrain vertex the support solve moved by more than 0.001 is written and listed;
// the touched rectangle (grown by one vertex) is redisplayed inside a long terrain operation. The row of a vertex is
// its id divided by the vertex row count (+0x4b8), not the row length, as the original computes it.
void cSC4NetworkTool::UpdateTerrainHeights(std::vector<std::uint32_t>& vertices) {
    vertices.clear();
    INetworkConstraintSatisfier* support = constraintB_;
    INetworkToolHost* host = GetNetworkToolHost();
    if (!support || !host) return;
    std::int32_t minX = widthPlusOne_, minZ = heightPlusOne_, maxX = -1, maxZ = -1;
    support->RewindSolvedVertexList();
    int vertex = 0;
    float height = 0.0f;
    while (support->GetNextSolvedVertexAndItsHeight(vertex, height)) {
        if (!(std::fabs(height - host->TerrainVertexHeight(vertex)) > kMinTerrainChange)) continue;
        host->SetTerrainVertexHeight(vertex, height);                                            // terrain vslot 0xc4
        vertices.push_back(static_cast<std::uint32_t>(vertex));
        const std::int32_t x = static_cast<std::int32_t>(static_cast<std::uint32_t>(vertex) % static_cast<std::uint32_t>(widthPlusOne_));
        const std::int32_t z = static_cast<std::int32_t>(static_cast<std::uint32_t>(vertex) / static_cast<std::uint32_t>(heightPlusOne_));
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minZ = std::min(minZ, z);
        maxZ = std::max(maxZ, z);
    }
    if (minZ != 0) --minZ;
    if (minX != 0) --minX;
    if (maxX < host->CityWidth()) ++maxX;                                                        // spCity vslot 0x174
    if (maxZ < host->CityHeight()) ++maxZ;                                                       // spCity vslot 0x178
    if (minX <= maxX && minZ <= maxZ) {
        host->BeginLongTerrainOperation();                                                       // terrain vslot 0x15c
        host->RedisplayTerrain(minX, minZ, maxX, maxZ, 4);                                       // terrain vslot 0x114
        host->EndLongTerrainOperation();                                                         // terrain vslot 0x160
    }
}

// i386 0x0012ab34. The smoothing-set cells whose network height moved re-read it; a placed piece among them (not in
// the solved list yet) joins the solved list so it is rebuilt at its new heights.
void cSC4NetworkTool::MarkPositionChangedExistingOccupantsAsNewlySolved() {
    for (std::uint32_t key : unknown128_) {
        cSC4NetworkCellInfo* cell = GetCellInfo(key);
        if (!cell) continue;
        const std::int32_t own = cell->buildable ? kLotConnectionVertexOffset : 0;
        bool moved = false;
        for (int i = 0; i < 4; ++i) {
            const float height = GetNetworkVertexHeight(own + cell->vertexBaseHeights[i]);      // vslot 0x12c
            if (!(height == cell->vertexHeights[i])) moved = true;
            cell->vertexHeights[i] = height;
        }
        if (!moved || cell->solvedCellIndex >= 0 || !cell->hasOccupant) continue;
        cSC4NetworkOccupant* occupant = GetNetworkOccupant(cell->x, cell->z);
        if (!occupant) continue;
        tSolvedCell solved;
        solved.pieceType = occupant->PieceId();                                                  // vslot 0x5c
        solved.rotationFlip = occupant->GetRotationAndFlip();                                    // vslot 0x68
        solved.cellKey = key;
        cell->solvedCellIndex = static_cast<std::int32_t>(solvedCellList_.size());
        solvedCellList_.push_back(solved);
        occupant->Release();
    }
}

// i386 0x00127d48. The placed pieces on the cells around every terrain vertex that moved (and not in the solved list
// yet) join the solved list with the network heights the first satisfier solved for their corners.
void cSC4NetworkTool::MarkTerrainChangedExistingOccupantsAsNewlySolved(const std::vector<std::uint32_t>& vertices) {
    for (std::uint32_t vertex : vertices) {
        std::int32_t z1 = static_cast<std::int32_t>(vertex / static_cast<std::uint32_t>(widthPlusOne_));
        std::int32_t x1 = static_cast<std::int32_t>(vertex % static_cast<std::uint32_t>(widthPlusOne_));
        const std::int32_t x0 = x1 < 1 ? 0 : x1 - 1;
        std::int32_t z0 = z1 < 1 ? 0 : z1 - 1;
        if (static_cast<std::uint32_t>(cityWidth_) <= static_cast<std::uint32_t>(x1)) x1 = cityWidth_ - 1;
        if (static_cast<std::uint32_t>(cityHeight_) <= static_cast<std::uint32_t>(z1)) z1 = cityHeight_ - 1;
        for (; z0 <= z1; ++z0) {
            for (std::int32_t x = x0; x <= x1; ++x) {
                const std::uint32_t key = static_cast<std::uint32_t>(z0) * 0x10000u + static_cast<std::uint32_t>(x);
                cSC4NetworkCellInfo* cell = GetCellInfo(key);
                if (!cell || cell->solvedCellIndex >= 0 || !cell->hasOccupant) continue;
                cSC4NetworkOccupant* occupant = GetNetworkOccupant(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z0));
                if (!occupant) continue;
                tSolvedCell solved;
                solved.pieceType = occupant->PieceId();
                solved.rotationFlip = occupant->GetRotationAndFlip();
                solved.cellKey = key;
                cell->solvedCellIndex = static_cast<std::int32_t>(solvedCellList_.size());
                for (int i = 0; i < 4; ++i)
                    if (constraintA_) constraintA_->GetHeight(cell->vertexBaseHeights[i], cell->vertexHeights[i]);
                solvedCellList_.push_back(solved);
                occupant->Release();
            }
        }
    }
}

// i386 0x00124878 (vslot 0xe4). The preview half (not placing) asks AddCellPreview for every solved cell, the placement
// half inserts a network occupant per cell, or updates the occupant already there.
// NOT PORTED YET: the intersection previews (+0xcc, multi-type crossings), the connection-piece update of an existing
// occupant (network flag 0x200000) and the one-way update (0x400000): those cells get a new occupant instead.
void cSC4NetworkTool::InsertOccupantsForSolvedCells(std::vector<tSolvedCell>& cells, std::list<cSC4NetworkOccupant*>* out, bool preview) {
    if (!unknown98_ || preview) {
        for (const tSolvedCell& solved : cells) {
            cSC4NetworkCellInfo* cell = GetCellInfo(solved.cellKey);
            if (!cell) continue;
            for (int i = 0; i < 4; ++i) cell->vertexHeights[i] = GetNetworkVertexHeight((cell->buildable ? 100000 : 0) + cell->vertexBaseHeights[i]);   // vslot 300
            if (!preview) cell->hasOccupant = true;
            AddCellPreview(*cell, solved);                                              // vslot 0xf0
        }
        return;
    }
    for (const tSolvedCell& solved : cells) {
        cSC4NetworkCellInfo* cell = GetCellInfo(solved.cellKey);
        if (!cell) continue;
        if (cell->buildable)
            SC4_UNWIRED("cSC4NetworkTool::InsertOccupantsForSolvedCells: connection-piece / one-way update of an existing occupant not ported (a new occupant is made)");
        cell->hasOccupant = true;
        InsertNetworkOccupant(*cell, solved, out);                                       // vslot 0x128
    }
}

// i386 vslot 300 (0x12c): the solved height of a vertex, or the terrain's.
float cSC4NetworkTool::GetNetworkVertexHeight(std::int32_t vertex) {
    float height = 0.0f;
    if (constraintA_ && constraintA_->GetHeight(vertex, height)) return height;
    INetworkToolHost* host = GetNetworkToolHost();
    return host ? host->TerrainVertexHeight(vertex) : 0.0f;
}

// i386 vslot 0xf0 (0x0012...): the solved piece's texture on the cell, tinted (blue for a +0x52 cell, else orange),
// raised by the type's elevation (kAltitudeForNetworkType: the elevated highway, light rail and monorail float 15.5).
// NOT PORTED: the cells of two network types (SolveMultiTypeSeparation); they get no preview.
void cSC4NetworkTool::AddCellPreview(cSC4NetworkCellInfo& cell, const tSolvedCell& solved) {
    static constexpr float kAltitudeForNetworkType[13] = {0, 0, 15.5f, 0, 0, 0, 0, 0, 15.5f, 15.5f, 0, 0, 0};
    if (cell.status == 0 || !preview_) return;
    auto* preview = dynamic_cast<cSC4NetworkPreview*>(preview_);
    if (!preview) return;
    INetworkToolHost* host = GetNetworkToolHost();
    const bool flip = (solved.rotationFlip >> 7) != 0;
    int rotation = solved.rotationFlip & 0x7f;
    // The network manager's piece texture (vslot 0x34) for the piece at the renderer's zoom.
    const std::uint32_t instance = host ? host->PieceTexture(solved.pieceType, rotation) : solved.pieceType;
    const cGZPersistResourceKey key{0x7ab50e44, 0x1abe787d, instance};
    const std::uint32_t color = (cell.unknown5a == 0 ? 0x664c01u : 0u) - 0xffff01u;
    const SC4PointU at{cell.x, cell.z};
    preview->Erase(at);
    const std::uint32_t flags = cell.networkFlags;
    if ((flags & 0x304) == 0 || (flags & 0xfffffcfbu) == 0) {
        const std::uint32_t lowest = (0u - flags) & flags;
        for (int type = 0; type < 13; ++type) {
            if (lowest != kFlagForNetworkType[type]) continue;
            preview->AddTexturedSelection(at, color, cell.vertexHeights, key, rotation, flip, kAltitudeForNetworkType[type], cell.buildable);
            return;
        }
    }
    if (flags & (flags - 1))
        SC4_UNWIRED("cSC4NetworkTool::AddCellPreview: preview of a two-network-type cell (SolveMultiTypeSeparation) not ported");
}

// i386 0x001262b4 (vslot 0xe0): a new occupant of the class `clsid` (0 = cSC4NetworkOccupant) over the cell's four corners.
void cSC4NetworkTool::CreateNetworkOccupant(cSC4NetworkCellInfo* cell, cSC4NetworkOccupant** out, float (*corners)[3], std::uint32_t clsid) {
    INetworkToolHost* host = GetNetworkToolHost();
    const float size = host ? host->TerrainCellSize() : 16.0f;                       // SL::spTerrain vslot 0x18
    const float x0 = static_cast<float>(cell->x) * size, z0 = static_cast<float>(cell->z) * size;
    const float c[4][3] = {{x0, cell->vertexHeights[0], z0},
                           {x0, cell->vertexHeights[1], size + z0},
                           {size + x0, cell->vertexHeights[2], size + z0},
                           {size + x0, cell->vertexHeights[3], z0}};
    std::memcpy(corners, c, sizeof c);
    cSC4NetworkOccupant* occupant = nullptr;
    if (clsid == 0) {
        occupant = new cSC4NetworkOccupant();
    } else if (clsid == kGZCLSID_cSC4NetworkBridgeOccupant) {
        occupant = new cSC4NetworkBridgeOccupant();
    } else if (clsid == kGZCLSID_cSC4NetworkTunnelOccupant) {
        occupant = new cSC4NetworkTunnelOccupant();
    } else if (clsid == kGZCLSID_cSC4NetworkOccupantWithPreBuiltModel) {
        occupant = new cSC4NetworkOccupantWithPreBuiltModel();                           // the intersection pieces
    } else {
        SC4_UNWIRED("cSC4NetworkTool::CreateNetworkOccupant: network occupant class not ported, a plain network occupant stands in");
        std::fprintf(stderr, "network tool: occupant class %08x not ported, using a plain network occupant\n", clsid);
        occupant = new cSC4NetworkOccupant();
    }
    *out = occupant;
    occupant->AddRef();
    if (!unknown98_) {
        occupant->SetNetworkFlag(cell->networkFlags & 0xc0000);
    } else {
        cell->occupant = occupant;
        occupant->SetNetworkFlag(cell->networkFlags);
    }
    occupant->Init(c);                                                                  // vslot 0xc
    if (!unknown98_) occupant->SetNetworkFlag(0x80000000u);
    else occupant->ClearNetworkFlag(0x80000000u);
    if (clsid == 0x8a4bd52b) {
        occupant->SetNetworkFlag(0x10000000);
        occupant->ClearNetworkFlag(0x4000);
    } else {
        occupant->SetNetworkFlag(0x4000);
    }
    if (unknown98_) {
        for (std::uint32_t type = 0; type < 13; ++type)
            if (kFlagForNetworkType[type] & cell->networkFlags) occupant->SetEdgeTypesFlag(static_cast<std::uint8_t>(type), cell->tileNeighborhoods[type]);
        occupant->SetOneWayDirections(cell->unknown58);
    }
    occupant->SetCostOfDemolition(static_cast<std::int64_t>(placement_.unknownA8[9]));   // i386 +0x36c = params +0x9c
}

// i386 0x00123f2a (vslot 0x148): which of the quad's triangles a piece draws. Without supports (network flag 0x40000000)
// both; a supported piece of one of the listed edge patterns draws one triangle (0x2000000 / 0x4000000 = the triangles,
// 0x1000000 = the shared edge).
void cSC4NetworkTool::SetQuadTrianglesToDraw(cSC4NetworkOccupant* occupant, std::uint32_t compact) {
    const auto set = [&](std::uint32_t f) { occupant->SetNetworkFlag(f); };
    const auto clear = [&](std::uint32_t f) { occupant->ClearNetworkFlag(f); };
    if (occupant->HasNetworkFlag(0x40000000u)) {
        switch (compact) {
        case 0x9: case 0x301: case 0x309: case 0xb00: case 0xb01:
            set(0x1000000); set(0x2000000); clear(0x4000000); return;
        case 0xb: case 0x1000003: case 0x100000b: case 0x9000003: case 0x9000000:
            set(0x2000000); clear(0x4000000); return;
        case 0x90000: case 0x3010000: case 0x3090000: case 0xb000000: case 0xb010000:
            set(0x1000000); set(0x4000000); clear(0x2000000); return;
        case 0x900: case 0x30100: case 0x30900: case 0xb0000: case 0xb0100:
            set(0x4000000); clear(0x2000000); return;
        default:
            break;
        }
    }
    set(0x2000000);
    set(0x4000000);
}

// i386 0x00125444: the network occupants of the cell the new piece replaces leave the occupant manager (and a flora
// occupant standing exactly on the cell is demolished).
void cSC4NetworkTool::ClearNetworkOccsFromCell(std::uint32_t x, std::uint32_t z) {
    if (INetworkToolHost* host = GetNetworkToolHost()) host->ClearCellForNetwork(x, z, placementFlagMask_, networkType_);
}

// i386 0x0012306c: the cell above (z - 1) or below (z + 1) carries a network type outside `networkFlags`.
bool cSC4NetworkTool::AdjacentVerticallyToMultitype(std::uint32_t x, std::uint32_t z, std::uint32_t networkFlags) {
    if (z != 0) {
        const cSC4NetworkCellInfo* cell = GetCellInfo(Key(x, z - 1));
        if (cell && (~networkFlags & cell->networkFlags & 0x1fff) != 0) return true;
    }
    if (z < static_cast<std::uint32_t>(cityHeight_) - 1u) {
        const cSC4NetworkCellInfo* cell = GetCellInfo(Key(x, z + 1));
        if (cell && (~networkFlags & cell->networkFlags & 0x1fff) != 0) return true;
    }
    return false;
}

// i386 0x001230f2: the same along x.
bool cSC4NetworkTool::AdjacentHorizontallyToMultitype(std::uint32_t x, std::uint32_t z, std::uint32_t networkFlags) {
    if (x != 0) {
        const cSC4NetworkCellInfo* cell = GetCellInfo(Key(x - 1, z));
        if (cell && (~networkFlags & cell->networkFlags & 0x1fff) != 0) return true;
    }
    if (x < static_cast<std::uint32_t>(cityWidth_) - 1u) {
        const cSC4NetworkCellInfo* cell = GetCellInfo(Key(x + 1, z));
        if (cell && (~networkFlags & cell->networkFlags & 0x1fff) != 0) return true;
    }
    return false;
}

// The pre-built model half of InsertNetworkOccupant (i386 0x0012b8c9..0x0012c1fb). A piece of one network type marks its
// split diagonal (0x1000000) for the end, curve and crossing patterns listed, gets its shear transform, and when the
// type's placement block lists support models (0xe974eaa7; the tool's style index picks one, else the first) a support
// model: a two-wide type (the elevated highway, the avenue) under the straight and the T patterns listed, at the middle of
// the cell's corners 1 and 3, else under the piece's straight, diagonal or curve, offset to its middle and sunk by 0.65 of
// the steeper corner normal's slope below the piece's altitude there.
bool cSC4NetworkTool::ComputePreBuiltSupportModel(cSC4NetworkCellInfo& cell, const tSolvedCell& solved, cSC4NetworkOccupant* occupant,
                                                  cSC4NetworkOccupantWithPreBuiltModel* prebuilt, const float (*corners)[3],
                                                  std::uint32_t& modelId, float& rotation, float (&offset)[3]) {
    constexpr float kQuarterPi = 0.785398f;          // i386 0x00a708ac
    constexpr float kSixteenthPi = 0.19635f;         // 0x00a708c0
    constexpr float kSlopeSink = 0.65f;              // 0x00a708b8
    float position[3] = {0.0f, 0.0f, 0.0f};
    prebuilt->GetPosition(position);                                                    // i386 vslot 0x148
    const std::uint32_t flags = occupant->GetNetworkFlag() & 0x1fff;
    if ((flags - 1) & flags) return false;                                              // two types: no support model
    std::uint32_t type = 0;
    while (type < 13 && !(kFlagForNetworkType[type] & flags)) ++type;
    if (type >= 13) return false;
    const std::uint32_t compact = cell.tileNeighborhoods[type];
    const bool twoWide = flags == 4 || flags == 0x40;
    // The connected edges: one bit per byte whose kind or minor type is set.
    const std::uint32_t present = ((compact & 0x7070707u) + 0x7070707u) & 0x8080808u;
    const std::uint32_t edgeCount = (present * 0x1010101u) >> 27;
    const std::uint32_t edgeMask = (present * 0x1020408u) >> 27;
    bool diagonal = false;
    if (edgeCount == 1) {
        const std::uint32_t kinds = compact & 0x3030303u;
        diagonal = kinds == 0x100 || kinds == 3 || kinds == 0x30000 || kinds == 0x1000000;
    } else if (edgeCount == 2) {
        if (compact == 0x10300 || compact == 0x103 || compact == 0x1030000 || compact == 0x3000001) diagonal = edgeMask == 3 || edgeMask == 0xc;
        else diagonal = edgeMask == 9 || edgeMask == 6;
    } else if (edgeCount == 4) {
        diagonal = compact == 0x1030103;
    }
    if (diagonal) occupant->SetNetworkFlag(0x1000000);
    prebuilt->RecomputeShearTransform();
    INetworkToolHost* host = GetNetworkToolHost();
    const INetworkTypeInfo* info = host ? host->NetworkTypeInfo(type) : nullptr;
    if (!info) return false;
    const std::vector<std::uint32_t>& models = info->PlacementParams().listD0;          // record +0x50 + 0xa0
    if (models.empty()) return false;
    modelId = rendererAltitudeScale_ < models.size() ? models[rendererAltitudeScale_] : models[0];   // i386 +0xfc
    const float middle = (cell.vertexHeights[1] + cell.vertexHeights[3]) * 0.5f - position[1];
    if (twoWide) {
        switch (compact) {
        case 0x1030103: case 0x3010301: case 0x3000001: case 0x103: case 0x10300: case 0x1030000:
            rotation = kQuarterPi;
            offset[0] = 0.0f; offset[1] = middle; offset[2] = 0.0f;
            return true;
        case 0x2040200:
            if (!(cell.z & 1) && !AdjacentVerticallyToMultitype(cell.x, cell.z, cell.networkFlags)) return false;
            [[fallthrough]];
        case 0x40200: case 0x2040000:
            rotation = 0.0f;
            offset[0] = 8.0f; offset[1] = middle; offset[2] = 0.0f;
            return true;
        case 0x4020002:
            if (!(cell.x & 1) && !AdjacentHorizontallyToMultitype(cell.x, cell.z, cell.networkFlags)) return false;
            [[fallthrough]];
        case 0x4000002: case 0x4020000:
            rotation = 0.0f;
            offset[0] = 0.0f; offset[1] = middle; offset[2] = 8.0f;
            return true;
        default:
            return false;
        }
    }
    float v[3] = {0.0f, 0.0f, 0.0f};
    rotation = 0.0f;
    bool none = false;   // the piece stands on its own: no offset, no rotation
    if (edgeCount == 1 && (compact & 0x1010101u)) {
        // A diagonal end piece: the support sits off the diagonal toward its open corner.
        float a = -4.0f, b = 4.0f, c = 4.0f, d = -4.0f;   // XMM0, XMM3, XMM1, XMM2 of the original
        if (compact & 0xffff00u) { a = 4.0f; b = -4.0f; }
        float e = 0.0f;
        if (compact & 0xffff0000u) { e = c; c = 2.6f; }
        else { e = d; c = -2.6f; }
        if ((compact & 0x2020202u) == 0) {
            d = c;
            c = b * kSlopeSink;
        } else {
            d = a * kSlopeSink;
            a = e;
            e = b;
        }
        v[0] = a + d;
        v[2] = c + e;
        rotation = kQuarterPi;
    } else if (compact == 0x301) {
        v[0] = -4.0f; v[2] = -4.0f;
        rotation = -kQuarterPi;
    } else if (compact == 0x30100) {
        v[0] = 4.0f; v[2] = -4.0f;
        rotation = -kQuarterPi;
    } else if (edgeCount == 2 && ((1u << edgeMask) & 0x1248u) && ((compact & 0xf8f8f8f8u) || (~(compact * 2) & compact & 0x2020202u))) {
        // A curve: the support turns to the mean of the two edge directions (in sixteenths of a turn) of the piece as it
        // is drawn (mirrored, then rotated), and sits toward the curve's inner corner.
        std::uint32_t d = compact;
        if (solved.rotationFlip & 0x80) {
            cSC4TileNeighborhoodCompact mirrored;
            cSC4TileNeighborhoodCompact(compact).GetMirrorImageEquivalent(mirrored);
            d = mirrored.Packed();
        }
        const std::uint32_t turn = solved.rotationFlip & 3;
        if (turn) d = (d >> (turn * 8)) | (d << (32 - turn * 8));
        int base = 0;
        while ((d & 0xff) == 0 || (d & 0xff00) == 0) {
            base -= 4;
            d = (d >> 8) | (d << 24);
        }
        const int kindA = static_cast<int>(d & 3), kindB = static_cast<int>((d >> 8) & 3);
        int dirA = (base - 2 * kindA + 4) & 0xf;
        int dirB = (base - 2 * kindB + 8) & 0xf;
        if (d & 0xf8) {
            if (kindA == 2) dirA += (d & 0x10) ? -1 : 1;
            else dirA = kindA + dirA - 2;
        }
        if (d & 0xf800) {
            if (kindB == 2) dirB += (d & 0x1000) ? -1 : 1;
            else dirB = kindB + dirB - 2;
        }
        if (dirB < dirA) std::swap(dirA, dirB);
        if (dirB - dirA >= 9) dirA += 16;
        rotation = static_cast<float>(dirA + dirB + 8) * kSixteenthPi;
        switch (edgeMask) {
        case 6: v[0] = 4.0f; v[2] = -4.0f; break;
        case 3: v[0] = -4.0f; v[2] = -4.0f; break;
        case 9: v[0] = -4.0f; v[2] = 4.0f; break;
        case 0xc: v[0] = 4.0f; v[2] = 4.0f; break;
        default: break;
        }
    } else if (edgeCount > 2 || (edgeCount == 2 && (compact & 0xf8f8f8f8u)) || (edgeCount == 1 && (compact & ~(compact * 2) & 0x2020202u))) {
        none = true;
    } else if (compact == 0x2000200) {
        if (!(cell.z & 1) && !AdjacentVerticallyToMultitype(cell.x, cell.z, cell.networkFlags)) return false;
        none = true;
    } else if (compact == 0x20002) {
        if (!(cell.x & 1) && !AdjacentHorizontallyToMultitype(cell.x, cell.z, cell.networkFlags)) return false;
        none = true;
    } else {
        return false;
    }
    if (none) {
        v[0] = v[1] = v[2] = 0.0f;
        rotation = 0.0f;
    }
    // The steeper of the two corner normals along the piece's split diagonal.
    float normalA[3] = {0, 1, 0}, normalB[3] = {0, 1, 0};
    if (prebuilt->HasNetworkFlag(0x1000000)) {                                          // i386 vslot 0x20
        prebuilt->GetNormal(corners[0][0], corners[0][2], normalA);                     // vslot 0xe4
        prebuilt->GetNormal(corners[2][0], corners[2][2], normalB);
    } else {
        prebuilt->GetNormal(corners[1][0], corners[1][2], normalA);
        prebuilt->GetNormal(corners[3][0], corners[3][2], normalB);
    }
    const float slopeA = static_cast<float>((static_cast<double>(std::fabs(normalA[0])) + std::fabs(normalA[2])) / std::fabs(normalA[1]));
    const float slopeB = static_cast<float>((static_cast<double>(std::fabs(normalB[0])) + std::fabs(normalB[2])) / std::fabs(normalB[1]));
    const float altitude = occupant->GetAltitude(position[0] + v[0], position[2] + v[2]);   // vslot 0xe0
    offset[0] = v[0];
    offset[1] = altitude - position[1] - kSlopeSink * (slopeB > slopeA ? slopeB : slopeA);
    offset[2] = v[2];
    return true;
}

// i386 0x0012b6d8 (vslot 0x128). A piece whose exemplar {0x6534284a, the group of one of the cell's network types, piece}
// exists is a pre-built model occupant: no quad triangles, its model from that exemplar (SetExemplarForOccupant) and the
// support model ComputePreBuiltSupportModel picks; any other piece is a plain occupant drawn as the piece's quad with the
// triangles SetQuadTrianglesToDraw picks. Then the old occupants of the cell leave, the new one is inserted (its handle at
// +0x54), translucent until it is usable.
// A piece with a support record (GetSupportStructureInfo by piece id, from the records of the types LoadSupports loads) that stands
// above the terrain at a corner gets its support structures: level 1 (side lips and retaining walls) up to the type's
// wall height (placement +0x38), level 2 (side lips and a pylon) above it.
void cSC4NetworkTool::InsertNetworkOccupant(cSC4NetworkCellInfo& cell, const tSolvedCell& solved, std::list<cSC4NetworkOccupant*>* out) {
    INetworkToolHost* host = GetNetworkToolHost();
    std::uint32_t group = 0;
    bool prebuiltPiece = false;
    if (host) {
        for (std::uint32_t type = 0; type < 13; ++type) {
            if (!(kFlagForNetworkType[type] & cell.networkFlags)) continue;
            const INetworkTypeInfo* info = host->NetworkTypeInfo(type);
            group = info ? info->DefaultExemplarId() : 0;                               // sNetworkTypeInfo[type] +0xc
            if (host->HasPrebuiltModelExemplar(type, solved.pieceType)) {               // spRM vslot 0xc, iid 0xa52160f5
                prebuiltPiece = true;
                break;
            }
        }
    }
    const cSC4NetworkSupportStructInfo* support = GetSupportStructureInfo(solved.pieceType);
    float corners[4][3];
    cSC4NetworkOccupant* occupant = nullptr;
    if (!prebuiltPiece) {
        CreateNetworkOccupant(&cell, &occupant, corners, 0);
        occupant->SetQuadDraw(solved.pieceType, solved.rotationFlip);                  // vslot 0x78
    } else {
        CreateNetworkOccupant(&cell, &occupant, corners, kGZCLSID_cSC4NetworkOccupantWithPreBuiltModel);
        occupant->SetQuadDraw(solved.pieceType, solved.rotationFlip);
        occupant->ClearNetworkFlag(0x2000000);
        occupant->ClearNetworkFlag(0x4000000);
        occupant->SetNetworkFlag(0x8000000);
        const float offset[3] = {0.0f, 0.0f, 0.0f};
        if (auto* prebuilt = dynamic_cast<cSC4NetworkOccupantWithPreBuiltModel*>(occupant)) {   // QI 0x49c1a034
            std::uint32_t modelId = 0;
            float rotation = 0.0f, supportOffset[3] = {0.0f, 0.0f, 0.0f};
            if (ComputePreBuiltSupportModel(cell, solved, occupant, prebuilt, corners, modelId, rotation, supportOffset))
                prebuilt->AddSupportModel(modelId, rotation, cS3DVector3(supportOffset[0], supportOffset[1], supportOffset[2]));
        }
        SetExemplarForOccupant(occupant, solved.pieceType, offset, solved.rotationFlip, group);
    }
    if (support) {
        int level = 0;
        for (int i = 0; i != 4; ++i) {
            const float above = cell.vertexHeights[i] - (host ? host->TerrainVertexHeight(cell.vertexBaseHeights[i]) : 0.0f);
            if (above > 0.01f) level = std::max(level, placement_.float40[1] <= above ? 2 : 1);
        }
        if (level != 0) {
            AddSupportStructures(occupant, cell, support, level);
            occupant->SetNetworkFlag(0x40000000);
            occupant->SetNetworkFlag(0x8000000);
        }
    }
    if (!prebuiltPiece) SetQuadTrianglesToDraw(occupant, cell.tileNeighborhoods[networkType_]);   // vslot 0x148
    occupant->ClearNetworkFlag(0x400000);
    ClearNetworkOccsFromCell(cell.x, cell.z);
    if (host) cell.occupantId = static_cast<std::uint8_t>(host->InsertNetworkOccupant(occupant, static_cast<std::uint32_t>(unknown458_)));
    if (occupant->GetNetworkFlag() & 0x10000000u) occupant->ClearNetworkFlag(0x4000);
    else occupant->SetNetworkFlag(0x4000);
    if (out) {
        occupant->AddRef();
        out->push_back(occupant);
    }
    occupant->Release();
}

// cSC4NetworkConstructionCrew::MarkOccupantsUsable (i386 0x00164690) / MarkOccupantUsable (0x0016429e): opaque in the
// normal view (view level 1), visible, usable; `release` drops the list's references.
void cSC4NetworkTool::MarkOccupantsUsable(std::list<cSC4NetworkOccupant*>& occupants, bool release) {
    INetworkToolHost* host = GetNetworkToolHost();
    for (cSC4NetworkOccupant* occupant : occupants) {
        if (!occupant) continue;
        if (host && host->CurrentViewLevel() == 1) occupant->ClearNetworkFlag(0x4000);
        occupant->SetVisibility(true, true);                                            // cISC4Occupant vslot 0x48
        occupant->SetNetworkFlag(0x10000000);
        if (release) occupant->Release();
    }
}

// ============================================================================================================
// PlaceNetwork
// ============================================================================================================

// i386 0x004ceb..: solve the current line, retrying with neighbour-city connections when asked, then insert the solved
// pieces (as previews when !place). The checks before the solve each cut the valid step count with a failure code.
bool cSC4NetworkTool::PlaceNetwork(bool place, cSC4NetworkConstructionCrew* crew, bool allowDiagonals, bool connectNeighbor,
                                   std::uint32_t networkType) {
    if (totalSteps_ == 0) return false;
    unknown98_ = place;
    pendingNeighborConnections_.clear();
    neighborConnections_.clear();
    if (DoNetworkSpecificPlacement(allowDiagonals, connectNeighbor)) return true;   // vslot 0xdc: the derived tools
    bool connected = false;
    for (;;) {
        if (auto* cache = dynamic_cast<cSC4NetworkWorldCache*>(worldCache_)) cache->Push();
        bridges_.clear();
        tunnels_.clear();
        intersections_.clear();
        int steps = totalSteps_;
        validSteps_ = steps;
        IdentifyStartingConditions(steps);                                          // vslot 0x110
        if (steps < validSteps_) { failureReason_ = kFailStart; validSteps_ = 0; }
        IdentifyTunnelCells(steps);                                                 // the count is read, never cut
        if (steps < validSteps_) { failureReason_ = kFailTunnel; validSteps_ = 0; }
        CheckForReservedTiles(steps, networkType, false);                           // vslot 0x114
        if (steps < validSteps_) { failureReason_ = kFailReserved; validSteps_ = 0; }
        steps = UpdateOccupantHighlights(validSteps_);                              // vslot 0xf4
        if (steps < validSteps_) { failureReason_ = kFailReserved; validSteps_ = 0; }
        IdentifyBridgeCells(steps, networkType);                                    // the bridge steps (direction 1) leave the terrain check
        if (steps < validSteps_) { failureReason_ = kFailBridge; validSteps_ = 0; }
        CheckForGoodTerrain(steps);                                                 // vslot 0x11c
        if (steps < validSteps_) { failureReason_ = kFailStart; validSteps_ = 0; }
        if (hasBridgeTypes_) IdentifyLandBridgeCells(validSteps_, networkType);     // i386 +0x394
        bool solved = false;
        while (validSteps_ > 0) {
            solvedCells_.clear();
            solvedCellList_.clear();
            if (auto* cache = dynamic_cast<cSC4NetworkWorldCache*>(worldCache_)) cache->ResetSolvedCellIDs();
            unknown10_.clear();                                                     // +0x08 potential intersections
            CreateCellInfosForCurrentPlacementTiles(networkType);
            AssignEdgeMajorTypes(networkType);
            AddGroupingInfo(networkType);
            DoAdditionalDraws();
            const int crossingSteps = IdentifyHighwayIntersections(validSteps_, networkType);
            const int solvedSteps = crossingSteps < validSteps_ ? crossingSteps : [&] {
                BuildSetOfCellsToBeSolved();
                if (networkType == kNetworkOneWayRoad) ResolveOneWayConflicts(networkType);
                MarkNeighborConnections();
                return SolveNetwork(networkType, false);
            }();
            if (crossingSteps < validSteps_) {
                failureReason_ = static_cast<int>(0xf0000005);
            } else if (solvedSteps < validSteps_) {
                if (std::getenv("SC4_NETDEBUG")) std::fprintf(stderr, "network solve: %d of %d steps solved\n", solvedSteps, validSteps_);
                failureReason_ = kFailSolve;
            } else {
                int code = 0;
                for (int pass = bridges_.empty() ? 0 : 1; pass != 4; ++pass) {
                    if (!SmoothenNetwork(pass)) code = kFailSmooth;
                    else if (CanTilesBeSupportedOnTerrain(networkType)) { solved = true; break; }
                    else code = kFailSupport;
                    if (std::getenv("SC4_NETDEBUG")) std::fprintf(stderr, "network solve: pass %d failed %08x\n", pass, static_cast<unsigned>(code));
                }
                if (!solved) failureReason_ = code;
            }
            if (solved) break;
            validSteps_ = 0;
            if (auto* cache = dynamic_cast<cSC4NetworkWorldCache*>(worldCache_)) cache->Pop();
            if (!unknown9a_) return false;
        }
        if (!connectNeighbor || validSteps_ != totalSteps_) break;
        if (connected) {
            if (place) PlaceNeighborConnections();
            break;
        }
        // the whole drag solved: when it ends at the city edge, solve it again with the neighbour connections' edges open
        if (anchors_.size() < 2 || !AttemptNeighborConnections(allowDiagonals, networkType)) break;
        MarkHighwayIntersectionPieces(false);
        connected = true;
    }
    std::list<cSC4NetworkOccupant*> placed;
    if (validSteps_ > 0) {
        if (!place) {
            // UpdatePositionChangedPreviewsForPreviousSegments: not ported (anchored multi-segment previews).
            SC4_UNWIRED("cSC4NetworkTool::PlaceNetwork: UpdatePositionChangedPreviewsForPreviousSegments (anchored multi-segment previews) not ported");
        } else {
            StartPlacing();                                                          // vslot 0x120
            DestroyHilightedOccupants();                                             // vslot 0xf8
            std::vector<std::uint32_t> movedVertices;
            UpdateTerrainHeights(movedVertices);                                     // vslot 0xec
            MarkPositionChangedExistingOccupantsAsNewlySolved();
            MarkTerrainChangedExistingOccupantsAsNewlySolved(movedVertices);
        }
        if (previewUpdateEnabled_) {
            InsertOccupantsForSolvedCells(solvedCellList_, &placed, false);
            InsertBridgePieces(&placed);
            InsertTunnelPieces(networkType);
            InsertHighwayIntersectionPieces(&placed);
        }
    }
    if (!place) {
        // The preview occupants (+0x25c) are kept so ClearCellPreviews can take them out again.
        for (cSC4NetworkOccupant* occupant : placed) previewOccupants_.push_back(occupant);
        if (previewUpdateEnabled_) SetTerrainHilight(0);                         // the invalid rest of the drag in red
    } else {
        // The crew takes the segment's pieces when the whole drag solved (or nothing did), or always for a one-cell pen; the
        // pieces it does not take are usable at once (cSC4NetworkConstructionCrew::MarkOccupantsUsable).
        const auto onePen = [&] {   // sNetworkTypeInfo +0x1c / +0x20: the pen widths
            INetworkToolHost* host = GetNetworkToolHost();
            const INetworkTypeInfo* info = host ? host->NetworkTypeInfo(networkType) : nullptr;
            return info && info->PenWidthX() == 1 && info->PenWidthZ() == 1;
        };
        if (crew && (totalSteps_ == validSteps_ || totalSteps_ == 0 || onePen())) crew->AddNetworkSegment(placed, steps_, cells_);
        MarkOccupantsUsable(placed, true);
    }
    return true;
}

// ============================================================================================================
// demolition: DestroyOccupantsAndResolve
// ============================================================================================================

namespace {
constexpr std::uint32_t kNetworkFlagTunnelPortal = 0x80000;      // network-manager.md §5 flag table
constexpr std::uint32_t kNetworkFlagComplexPiece = 0x100000;     // part of an intersection or complex piece
constexpr std::uint32_t kNetworkFlagConnectionNode = 0x200000;   // a lot connection node
// DestroyOccupantsAndResolve::kEdgesToFixVertex (i386 0x00844dd4): the sides whose edges pin each corner (corner 0
// (x, z), 1 (x, z+1), 2 (x+1, z+1), 3 (x+1, z); side 0 -x, 1 -z, 2 +x, 3 +z).
constexpr std::uint8_t kEdgesToFixVertex[4] = {0x03, 0x09, 0x0c, 0x06};

std::uint32_t CellKeyOf(const cSC4NetworkOccupant* occupant) {
    std::uint32_t x = 0, z = 0;
    occupant->GetOccupiedCell(x, z);
    return Key(x, z);
}
bool HasSeveralTypes(cSC4NetworkOccupant* occupant) {
    const std::uint32_t types = occupant->GetNetworkFlag() & 0x1fff;
    return (types & (types - 1)) != 0;
}
// The sides (bytes) of an edge-types word with any of bits 0-2 set.
std::uint32_t ConnectedSideCount(std::uint32_t edges) {
    return (((edges & 0x07070707u) + 0x07070707u) & 0x08080808u) * 0x01010101u >> 27;
}
// The first network type whose flag meets `flags` (the scan stops at 12, as the original's).
std::uint32_t FirstTypeOf(std::uint32_t flags) {
    std::uint32_t type = 0;
    while (type != 12 && (kFlagForNetworkType[type] & flags) == 0) ++type;
    return type;
}
}  // namespace

// Mac i386 0x00128fae (network-tools.md §10). Rounds until nothing is queued and no cell waits for a re-solve:
//   1. close the set: every queued piece drops its edge from the neighbours it connects to (their cells are re-solved);
//      a grouped edge, an immovable neighbour, an intersection or a tunnel portal pulls more pieces in;
//   2. remove the pieces in pointer order (std::set): those on a destroyed cell (the caller's cells, and the cells a
//      destroyed piece's grouped edges lead to) go outright, the others through RemoveOrReplaceOccupant;
//   3. re-solve: the movable pieces within two cells of every re-solve cell, then insert the solved pieces;
//   4. a piece the solve left unsolved loses its weaker network type (two types) or is queued for removal; a lot
//      connection node is only marked unusable.
// PARTIAL: the first constraint satisfier's Shutdown / Init(0, terrain maximum + kDefaultMaxDensity) are not on the
// INetworkConstraintSatisfier seam; the fixed corners go in only when the host supplies one (natively it does not, as
// for placement).
void cSC4NetworkTool::DestroyOccupantsAndResolve(const std::vector<cSC4NetworkOccupant*>& occupants) {
    if (occupants.empty()) return;
    SC4_UNWIRED("cSC4NetworkTool::DestroyOccupantsAndResolve: constraint satisfier Shutdown / Init before the re-solve not ported");
    INetworkToolHost* host = GetNetworkToolHost();
    forceReload_ = true;                       // +0x53
    const bool saved = unknown98_;             // +0x50, restored at the end
    unknown98_ = true;

    std::deque<cSC4NetworkOccupant*> queue;              // one reference each
    std::unordered_set<std::uint32_t> resolveCells;      // the cells to re-solve
    std::unordered_set<std::uint32_t> destroyedCells;    // the cells whose pieces go outright
    for (cSC4NetworkOccupant* occupant : occupants) {
        if (!occupant) continue;
        occupant->AddRef();
        queue.push_back(occupant);
        destroyedCells.insert(CellKeyOf(occupant));
    }

    int rounds = 0;
    do {
        std::set<cSC4NetworkOccupant*> removal;   // one reference each
        std::set<cSC4NetworkOccupant*> visited;
        auto addToRemoval = [&removal](cSC4NetworkOccupant* piece) {   // takes over the caller's reference
            if (!removal.insert(piece).second) piece->Release();
        };
        auto addIntersectionPieces = [&](cSC4NetworkOccupant* piece) {
            std::vector<cSC4NetworkOccupant*> pieces;
            if (host) host->GetIntersectionPieces(piece, pieces);
            for (cSC4NetworkOccupant* p : pieces)
                if (p) addToRemoval(p);
        };

        // 1. close the set
        while (!queue.empty()) {
            cSC4NetworkOccupant* occupant = queue.front();
            queue.pop_front();
            if (!visited.insert(occupant).second) {
                occupant->Release();
                continue;
            }
            occupant->AddRef();
            addToRemoval(occupant);
            const std::uint32_t flags = occupant->GetNetworkFlag();
            std::uint32_t x = 0, z = 0;
            occupant->GetOccupiedCell(x, z);
            for (int side = 0; side < 4; ++side) {
                const std::uint32_t nx = x + static_cast<std::uint32_t>(kNextX[side]);
                const std::uint32_t nz = z + static_cast<std::uint32_t>(kNextZ[side]);
                if (nx >= static_cast<std::uint32_t>(cityWidth_) || nz >= static_cast<std::uint32_t>(cityHeight_)) continue;
                cSC4NetworkOccupant* neighbor = GetNetworkOccupant(nx, nz);
                if (!neighbor) continue;
                const int opposite = (side + 2) & 3;
                bool connected = false;
                for (std::uint32_t type = 0; type < kNetworkTypeCount; ++type) {
                    if ((flags & kFlagForNetworkType[type]) == 0) continue;
                    const std::uint32_t edge = occupant->GetEdgeTypesFlag(static_cast<std::uint8_t>(type)) >> (8 * side);
                    if (edge & 4) {   // a grouped edge: the neighbour goes as well
                        neighbor->AddRef();
                        queue.push_back(neighbor);
                        neighbor->SetEdgeType(static_cast<std::uint8_t>(type), opposite, 0);
                        if (destroyedCells.count(Key(x, z))) destroyedCells.insert(Key(nx, nz));
                    } else if ((edge & 0xff) != 0) {
                        neighbor->SetEdgeType(static_cast<std::uint8_t>(type), opposite, 0);
                        if (type == 10)   // one-way road: the arrows through that side go too
                            neighbor->SetOneWayDirections(static_cast<std::uint8_t>(neighbor->GetOneWayDirections() & ~(3u << (opposite * 2))));
                        if (neighbor->GetEdgeTypesFlag(static_cast<std::uint8_t>(type)) == 0 && HasSeveralTypes(neighbor))
                            neighbor->ClearNetworkFlag(kFlagForNetworkType[type]);
                        connected = true;
                    }
                }
                if (connected) {
                    if (neighbor->HasNetworkFlag(kNetworkFlagConnectionNode) || !neighbor->IsImmovable()) {
                        resolveCells.insert(Key(nx, nz));
                    } else if (!neighbor->HasNetworkFlag(kNetworkFlagComplexPiece)) {
                        neighbor->AddRef();
                        queue.push_back(neighbor);
                    } else {
                        addIntersectionPieces(neighbor);
                    }
                }
                neighbor->Release();
            }
            if (flags & kNetworkFlagTunnelPortal) {
                if (cSC4NetworkOccupant* other = host ? host->TunnelOtherEnd(occupant) : nullptr) queue.push_back(other);
            }
            if (flags & kNetworkFlagComplexPiece) addIntersectionPieces(occupant);
            occupant->Release();
        }
        if (worldCache_) worldCache_->Clear();

        // 2. remove
        for (cSC4NetworkOccupant* occupant : removal) {
            if (host) host->SendOccupantDemolished(occupant);
            bool replaced = false;
            const std::uint32_t key = CellKeyOf(occupant);
            if (destroyedCells.count(key) == 0) {
                RemoveOrReplaceOccupant(occupant, replaced);
            } else if (host) {
                host->RemoveNetworkOccupant(occupant);
            }
            if (replaced) resolveCells.insert(key);
            else resolveCells.erase(key);
            occupant->Shutdown();
            occupant->Release();
        }
        removal.clear();

        // 3. re-solve around the cells
        solvedCells_.clear();
        solvedCellList_.clear();
        for (std::uint32_t key : resolveCells) {
            const std::uint32_t cx = key & 0xffff, cz = key >> 16;
            const std::uint32_t x0 = cx < 3 ? 0 : cx - 2, z0 = cz < 3 ? 0 : cz - 2;
            const std::uint32_t x1 = std::min<std::uint32_t>(cx + 2, static_cast<std::uint32_t>(cityWidth_) - 1);
            const std::uint32_t z1 = std::min<std::uint32_t>(cz + 2, static_cast<std::uint32_t>(cityHeight_) - 1);
            for (std::uint32_t z = z0; z <= z1; ++z) {
                for (std::uint32_t x = x0; x <= x1; ++x) {
                    cSC4NetworkOccupant* piece = GetNetworkOccupant(x, z);
                    if (!piece) continue;
                    if (constraintA_) {
                        std::uint32_t fixed = 0xf;
                        if (piece->HasNetworkFlag(kNetworkFlagConnectionNode)) {
                            const std::uint32_t active = piece->GetActiveEdges();
                            for (int corner = 0; corner < 4; ++corner)
                                if ((kEdgesToFixVertex[corner] & active) == 0) fixed &= ~(1u << corner);
                        }
                        float heights[4] = {};
                        piece->GetVertexHeights(heights);
                        const int vertex = static_cast<int>(x) + static_cast<int>(z) * widthPlusOne_;
                        if (fixed & 1) constraintA_->InsertFixedVertex(vertex, heights[0]);
                        if (fixed & 2) constraintA_->InsertFixedVertex(vertex + widthPlusOne_, heights[1]);
                        if (fixed & 4) constraintA_->InsertFixedVertex(vertex + widthPlusOne_ + 1, heights[2]);
                        if (fixed & 8) constraintA_->InsertFixedVertex(vertex + 1, heights[3]);
                    }
                    if (!piece->IsImmovable()) solvedCells_.insert(Key(x, z));
                    piece->Release();
                }
            }
        }
        resolveCells.clear();
        validSteps_ = 0;
        SolveNetwork(0xffffffffu, true);
        std::list<cSC4NetworkOccupant*> inserted;
        InsertOccupantsForSolvedCells(solvedCellList_, &inserted, false);

        // 4. what the solve left unsolved
        for (std::uint32_t key : solvedCells_) {
            cSC4NetworkOccupant* piece = GetNetworkOccupant(key & 0xffff, key >> 16);
            if (!piece) continue;
            if (piece->HasNetworkFlag(kNetworkFlagConnectionNode)) {
                piece->SetUsable(false);
                piece->Release();
                continue;
            }
            std::uint32_t px = 0, pz = 0;
            piece->GetOccupiedCell(px, pz);
            const std::uint32_t types = piece->GetNetworkFlag() & 0x1fff;
            const std::uint32_t second = types & (types - 1);
            const std::uint32_t first = types ^ second;
            if (second != 0 && (second & (second - 1)) == 0) {   // exactly two types: the one with fewer sides goes
                const std::uint32_t typeA = FirstTypeOf(first), typeB = FirstTypeOf(second);
                const std::uint32_t edgesA = piece->GetEdgeTypesFlag(static_cast<std::uint8_t>(typeA));
                const std::uint32_t edgesB = piece->GetEdgeTypesFlag(static_cast<std::uint8_t>(typeB));
                const std::uint32_t sidesA = ConnectedSideCount(edgesA), sidesB = ConnectedSideCount(edgesB);
                if (sidesA != sidesB) {
                    const bool dropA = sidesA < sidesB;
                    const std::uint32_t flag = dropA ? first : second;
                    const std::uint32_t type = dropA ? typeA : typeB;
                    const std::uint32_t edges = dropA ? edgesA : edgesB;
                    for (int side = 0; side < 4; ++side) {
                        if ((edges & (0xffu << (8 * side))) == 0) continue;
                        const std::uint32_t nx = px + static_cast<std::uint32_t>(kNextX[side]);
                        const std::uint32_t nz = pz + static_cast<std::uint32_t>(kNextZ[side]);
                        cSC4NetworkOccupant* neighbor = GetNetworkOccupant(nx, nz);
                        if (!neighbor) continue;
                        neighbor->SetEdgeType(static_cast<std::uint8_t>(type), (side + 2) & 3, 0);
                        if (neighbor->GetEdgeTypesFlag(static_cast<std::uint8_t>(type)) == 0 && HasSeveralTypes(neighbor))
                            neighbor->ClearNetworkFlag(flag);
                        resolveCells.insert(Key(nx, nz));
                        neighbor->Release();
                    }
                    piece->SetEdgeTypesFlag(static_cast<std::uint8_t>(type), 0);
                    piece->ClearNetworkFlag(flag);
                    resolveCells.insert(Key(px, pz));
                    piece->Release();
                    continue;
                }
            }
            queue.push_back(piece);   // keeps the reference: removed next round
        }
        MarkOccupantsUsable(inserted, true);
        // PORT: a guard the original does not have; a solve that never settles stops here instead of spinning.
        if (++rounds >= 64) {
            std::fprintf(stderr, "network tool: demolition re-solve did not settle after %d rounds\n", rounds);
            for (cSC4NetworkOccupant* left : queue) left->Release();
            queue.clear();
            resolveCells.clear();
        }
    } while (!resolveCells.empty() || !queue.empty());
    unknown98_ = saved;
}

// network-manager.md §7.4 with this tool: a piece that is not in the city or not a network piece is refused; a piece
// of an intersection is removed and a plain piece built from its cell's record (taken before the removal) goes back.
bool cSC4NetworkTool::RemoveOrReplaceOccupant(cSC4NetworkOccupant* occupant, bool& replaced) {
    INetworkToolHost* host = GetNetworkToolHost();
    if (!occupant || !host || (occupant->GetFlags() & 1) == 0) return false;
    if (!occupant->HasNetworkFlag(kNetworkFlagComplexPiece)) return host->RemoveNetworkOccupant(occupant);
    cSC4NetworkCellInfo* cell = GetCellInfo(CellKeyOf(occupant));
    if (!host->RemoveNetworkOccupant(occupant)) return false;
    unknown98_ = true;
    if (cell) InsertNetworkOccupant(*cell, tSolvedCell{}, nullptr);
    replaced = true;
    return true;
}

}  // namespace sc4
