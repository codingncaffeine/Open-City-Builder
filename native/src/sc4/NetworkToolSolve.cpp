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

#include <cfloat>
#include <cstdio>
#include <cstring>

#include "sc4/NetworkOccupant.h"
#include "sc4/NetworkWorldCache.h"
#include "sc4/NetworkPreview.h"
#include "sc4/TileRules.h"

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
    // RemoveHighwayIntersectionPieces / RemoveTunnelPieces / RemoveBridgePiecesFromSetOfCellsToBeSolved: no such pieces
    // exist until bridges, tunnels and highway intersections are ported.
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

// i386 0x00147a.. Returns the number of steps that solved (validSteps_ when all did).
int cSC4NetworkTool::SolveNetwork(std::uint32_t networkType, bool retry) {
    (void)networkType;
    std::unordered_set<std::uint32_t> padded, removed;
    BuildPaddedNetworkSet(solvedCells_, padded, 2);
    AssignEdgeSubtypes(padded, removed, true);
    for (std::uint32_t key : removed) solvedCells_.erase(key);
    // SolveMultiTypeIntersections (two network types crossing) is not ported: it only acts on multi-type cells.
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
bool cSC4NetworkTool::AdjustTileSubsets(std::vector<tSolvedCell>&, bool) { return true; }

// i386 0x00145f40: the tile pattern verifiers (sTilePatternVerifyRules, "TileVerifiers"). With no verifiers loaded it
// returns true (0x00145f68), as here (NOT PORTED: the pattern walk).
bool cSC4NetworkTool::VerifyTileSubsets(std::vector<tSolvedCell>&) { return true; }

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
void cSC4NetworkTool::UpdateTerrainHeights(std::vector<std::uint32_t>&) {}  // i386 vslot 0xec: NOT PORTED YET

// i386 vslot 0xe8. NOT PORTED YET: the support check of the solved heights; every solve is accepted.
bool cSC4NetworkTool::CanTilesBeSupportedOnTerrain(std::uint32_t) { return true; }

// i386 SmoothenNetwork. NOT PORTED YET: the vertex height solve (ComputeInitialSmoothingSet + the constraint inserters +
// cSC4VertexHtConstraintSatisfier); the cells keep their terrain heights.
bool cSC4NetworkTool::SmoothenNetwork(int) { return true; }

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
    } else {
        // cSC4NetworkBridgeOccupant (0x49cc1bcd), cSC4NetworkTunnelOccupant (0x8a4bd52b), cSC4NetworkOccupantWithPreBuiltModel
        // (0x49c1a034): not created by the land path yet.
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

// i386 0x0012b6d8 (vslot 0x128). The plain-piece path: a new occupant with the solved piece and rotation, its triangles,
// the old occupants of the cell removed, the new one inserted (its handle at +0x54), translucent until it is usable.
// NOT PORTED YET: the pre-built model pieces (an exemplar {0x6534284a, type exemplar, piece} exists: support models,
// shear transform), the support structures of raised pieces (AddSupportStructures) and SetExemplarForOccupant.
void cSC4NetworkTool::InsertNetworkOccupant(cSC4NetworkCellInfo& cell, const tSolvedCell& solved, std::list<cSC4NetworkOccupant*>* out) {
    INetworkToolHost* host = GetNetworkToolHost();
    if (host) {
        for (std::uint32_t type = 0; type < 13; ++type) {
            if ((kFlagForNetworkType[type] & cell.networkFlags) && host->HasPrebuiltModelExemplar(type, solved.pieceType)) {
                static int warned = 0;
                if (warned++ < 4) std::fprintf(stderr, "network tool: piece %08x has a pre-built model (not ported), drawn as a quad\n", solved.pieceType);
                break;
            }
        }
    }
    float corners[4][3];
    cSC4NetworkOccupant* occupant = nullptr;
    CreateNetworkOccupant(&cell, &occupant, corners, 0);
    occupant->SetQuadDraw(solved.pieceType, solved.rotationFlip);                      // vslot 0x78
    SetQuadTrianglesToDraw(occupant, cell.tileNeighborhoods[networkType_]);
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
bool cSC4NetworkTool::PlaceNetwork(bool place, bool allowDiagonals, bool connectNeighbor, std::uint32_t networkType) {
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
        // IdentifyTunnelCells: not ported (no tunnels).
        CheckForReservedTiles(steps, networkType, false);                           // vslot 0x114
        if (steps < validSteps_) { failureReason_ = kFailReserved; validSteps_ = 0; }
        steps = UpdateOccupantHighlights(validSteps_);                              // vslot 0xf4
        if (steps < validSteps_) { failureReason_ = kFailReserved; validSteps_ = 0; }
        // IdentifyBridgeCells: not ported (no bridges; water stops the drag in CheckForGoodTerrain).
        CheckForGoodTerrain(steps);                                                 // vslot 0x11c
        if (steps < validSteps_) { failureReason_ = kFailStart; validSteps_ = 0; }
        // IdentifyLandBridgeCells (hasBridgeTypes_): not ported.
        bool solved = false;
        while (validSteps_ > 0) {
            solvedCells_.clear();
            solvedCellList_.clear();
            if (auto* cache = dynamic_cast<cSC4NetworkWorldCache*>(worldCache_)) cache->ResetSolvedCellIDs();
            unknown10_.clear();                                                     // +0x08 potential intersections
            CreateCellInfosForCurrentPlacementTiles(networkType);
            AssignEdgeMajorTypes(networkType);
            // AddGroupingInfo (pen width > 1, the highways): not ported.
            DoAdditionalDraws();
            // IdentifyHighwayIntersections: not ported (no highway crossings); the valid count stays.
            BuildSetOfCellsToBeSolved();
            // ResolveOneWayConflicts (type 10): not ported.
            MarkNeighborConnections();
            const int solvedSteps = SolveNetwork(networkType, false);
            if (solvedSteps < validSteps_) {
                failureReason_ = kFailSolve;
            } else {
                int code = 0;
                for (int pass = bridges_.empty() ? 0 : 1; pass != 4; ++pass) {
                    if (!SmoothenNetwork(pass)) code = kFailSmooth;
                    else if (CanTilesBeSupportedOnTerrain(networkType)) { solved = true; break; }
                    else code = kFailSupport;
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
            // PlaceNeighborConnections (place): not ported.
            break;
        }
        // AttemptNeighborConnections: not ported, so the drag never retries with a neighbour connection.
        break;
    }
    std::list<cSC4NetworkOccupant*> placed;
    if (validSteps_ > 0) {
        if (!place) {
            // UpdatePositionChangedPreviewsForPreviousSegments: not ported (anchored multi-segment previews).
        } else {
            StartPlacing();                                                          // vslot 0x120
            DestroyHilightedOccupants();                                             // vslot 0xf8
            // UpdateTerrainHeights / Mark*ExistingOccupantsAsNewlySolved: not ported yet.
        }
        if (previewUpdateEnabled_) InsertOccupantsForSolvedCells(solvedCellList_, &placed, false);
    }
    if (!place) {
        // The preview occupants (+0x25c) are kept so ClearCellPreviews can take them out again.
        for (cSC4NetworkOccupant* occupant : placed) previewOccupants_.push_back(occupant);
        // SetTerrainHilight (previewUpdateEnabled_): not ported.
    } else {
        // The construction crew (cSC4NetworkConstructionCrew::AddNetworkSegment) is not ported: the pieces are usable at once.
        MarkOccupantsUsable(placed, true);
    }
    return true;
}

}  // namespace sc4
