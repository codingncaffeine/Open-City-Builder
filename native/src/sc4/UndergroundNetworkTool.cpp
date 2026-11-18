// cSC4UndergroundNetworkTool — see UndergroundNetworkTool.h for the object layout, the two pure virtuals that
// make the class abstract (the pipe/subway tools fill them) and the quirks kept with their addresses.
#include "sc4/UndergroundNetworkTool.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "sc4/SCPropertyHelp.h"

namespace sc4 {
namespace {

IUndergroundNetworkToolHost* g_undergroundNetworkToolHost = nullptr;

}  // namespace

void SetUndergroundNetworkToolHost(IUndergroundNetworkToolHost* host) { g_undergroundNetworkToolHost = host; }
IUndergroundNetworkToolHost* GetUndergroundNetworkToolHost() { return g_undergroundNetworkToolHost; }

// 0x1001a92df
bool IsUndergroundStraightNeighbourhood(const cSC4NetworkCellInfo& cell, std::uint32_t networkType) {
    // Quirk: no bounds check on networkType (the original indexes +0x20 + type * 4 the same way).
    switch (cell.tileNeighborhoods[networkType]) {
        case 0x0000301:
        case 0x0020002:
        case 0x0030100:
        case 0x1000003:
        case 0x2000200:
        case 0x3010000:
            return true;
        default:
            return false;
    }
}

// ============================================================================================================
// construction / init / shutdown
// ============================================================================================================

// 0x1001a74c8
cSC4UndergroundNetworkTool::cSC4UndergroundNetworkTool(std::uint32_t networkType)
    : cSC4NetworkTool(networkType) {
    inited_ = false;
    networkDepth_ = 0.0f;
    tuningA_ = 0.0f;
    tuningFlag_ = false;
    tuningB_ = 0.0f;
    // The two lists are constructed empty by their own constructors.
}

// 0x1001a9ab2 (complete) / 0x1001a9ab8 (deleting): both are `push rbp; mov rbp, rsp; ud2`. The tool is never
// destroyed in the game; the port aborts so a mistaken delete cannot silently run the base destructor.
cSC4UndergroundNetworkTool::~cSC4UndergroundNetworkTool() { std::abort(); }

// 0x1001a754c
bool cSC4UndergroundNetworkTool::Init() {
    // Quirk: the guard is set *before* the base Init runs, so a failing base Init still leaves the tool inited.
    if (!inited_) {
        inited_ = true;
        cSC4NetworkTool::Init();   // calls ReloadNetworkSpecificInfo (vtable +0x288) once it is up
    }
    return true;
}

// 0x1001a756a
bool cSC4UndergroundNetworkTool::Shutdown() {
    if (!inited_) return true;
    inited_ = false;
    for (cSC4UndergroundOccupant* occupant : previewList_) {
        if (occupant != nullptr) occupant->Release();   // vslot +0x10
    }
    previewList_.clear();
    for (cSC4UndergroundOccupant* occupant : highlightList_) {
        if (occupant != nullptr) occupant->Release();   // vslot +0x10
    }
    highlightList_.clear();
    return cSC4NetworkTool::Shutdown();
}

// 0x1001a7c7e
void cSC4UndergroundNetworkTool::ReloadNetworkSpecificInfo(std::uint32_t exemplarId) {
    IUndergroundNetworkToolHost* host = g_undergroundNetworkToolHost;
    if (host == nullptr) return;
    // SL::spRM slot +0x18 with the key {kUndergroundTuningType, kUndergroundTuningGroup, exemplarId} and
    // property kUndergroundTuningProperty; the resource's property holder (+0x30) is read four times.
    cISCPropertyHolder* holder = host->AcquireNetworkTuningProperties(exemplarId);
    if (holder == nullptr) return;
    cSCPropertyHelp::GetPropertyValue(holder, kUndergroundPropNetworkDepth, networkDepth_);   // +0x63c
    cSCPropertyHelp::GetPropertyValue(holder, kUndergroundPropTuningA, tuningA_);             // +0x640
    cSCPropertyHelp::GetPropertyValue(holder, kUndergroundPropTuningB, tuningB_);             // +0x648
    cSCPropertyHelp::GetPropertyValue(holder, kUndergroundPropTuningFlag, tuningFlag_);       // +0x644
    holder->Release();
}

// ============================================================================================================
// the trivial network-type answers (all folded onto shared return stubs in the binary)
// ============================================================================================================

// 0x100192f9e
bool cSC4UndergroundNetworkTool::StopPickAtWater() { return false; }

// 0x100193078
bool cSC4UndergroundNetworkTool::AllowBridges() { return false; }

// 0x100193080
bool cSC4UndergroundNetworkTool::AllowTunnels() { return false; }

// 0x100193088
bool cSC4UndergroundNetworkTool::IsStoppedByWater() { return false; }

// 0x100193090
bool cSC4UndergroundNetworkTool::ShouldAutoConnect() { return true; }

// 0x100193098 (the failure reason is not touched)
bool cSC4UndergroundNetworkTool::CheckForGoodTerrain(int& failureReason) {
    (void)failureReason;
    return true;
}

// 0x1001930a0
float cSC4UndergroundNetworkTool::GetNetworkHeightForTerrainHeight(float terrainHeight) const {
    return terrainHeight - networkDepth_;
}

// 0x1001937b8
bool cSC4UndergroundNetworkTool::CanTilesBeSupportedOnTerrain(std::uint32_t networkType) {
    (void)networkType;
    return true;
}

// 0x1001937c0
void cSC4UndergroundNetworkTool::UpdateTerrainHeights(std::vector<std::uint32_t>& heights) { (void)heights; }

// 0x1001a71e0
void cSC4UndergroundNetworkTool::CheckForReservedTiles(int& cell, std::uint32_t networkType, bool flag) {
    (void)cell;
    (void)networkType;
    (void)flag;
}

// 0x100192f96
void cSC4UndergroundNetworkTool::ClearOccupantHighlights() {}

// 0x1001a9ac4: the identity function (the original returns its int argument).
int cSC4UndergroundNetworkTool::UpdateOccupantHighlights(int value) { return value; }

// 0x1001a9acc
void cSC4UndergroundNetworkTool::DestroyHilightedOccupants() {}

// 0x1001a78e8
void cSC4UndergroundNetworkTool::ApplyPlaneConstraintToRegularCells(std::vector<tImmovablePortion>& portions) {
    (void)portions;
}

// ============================================================================================================
// occupant lookups
// ============================================================================================================

// 0x1001a7d3e
bool cSC4UndergroundNetworkTool::FindUndergroundOccupant(int x, int z, cSC4UndergroundOccupant*& out,
                                                         std::uint32_t gziid) {
    IUndergroundNetworkToolHost* host = g_undergroundNetworkToolHost;
    std::list<cISC4Occupant*> occupants;
    if (host != nullptr) host->QueryCellOccupants(x, z, occupants);   // SL::spOccupantManager +0x148
    out = nullptr;
    bool found = false;
    for (cISC4Occupant* occupant : occupants) {
        void* queried = nullptr;
        if (occupant->QueryInterface(gziid, &queried)) {
            if (out != nullptr) out->Release();
            out = static_cast<cSC4UndergroundOccupant*>(queried);
            found = true;
        }
        occupant->Release();   // the list element's own reference
    }
    return found;
}

// 0x1001a81d6
bool cSC4UndergroundNetworkTool::FindUndergroundConnectionOccupant(
    int x, int z, std::uint32_t gziid, cSC4UndergroundConnectionOccupant*& out) {
    IUndergroundNetworkToolHost* host = g_undergroundNetworkToolHost;
    std::list<cISC4Occupant*> occupants;
    if (host != nullptr) host->QueryCellOccupants(x, z, occupants);   // SL::spOccupantManager +0x148
    out = nullptr;
    bool found = false;
    for (cISC4Occupant* occupant : occupants) {
        void* queried = nullptr;
        if (occupant->QueryInterface(kGZIID_cISC4UndergroundConnectionOccupant, &queried)) {
            auto* connection = static_cast<cSC4UndergroundConnectionOccupant*>(queried);
            if (connection->GetNetworkOccupantGZIID() == gziid) {   // vslot 0x190
                if (out != nullptr) out->Release();
                out = connection;
                found = true;
            } else {
                connection->Release();
            }
        }
        occupant->Release();   // the list element's own reference
    }
    return found;
}

// 0x1001a93a6
bool cSC4UndergroundNetworkTool::TestForAutoConnectOccupant(int x, int z) {
    const std::uint32_t gziid = NetworkGZIID();
    cSC4UndergroundOccupant* underground = nullptr;
    if (FindUndergroundOccupant(x, z, underground, gziid)) {
        underground->Release();
        return true;
    }
    cSC4UndergroundConnectionOccupant* connection = nullptr;
    if (!FindUndergroundConnectionOccupant(x, z, gziid, connection)) return false;
    connection->Release();
    return true;
}

// ============================================================================================================
// occupant creation
// ============================================================================================================

// 0x1001a7362
bool cSC4UndergroundNetworkTool::CreateNewUndergroundOccupant(cSC4NetworkCellInfo* cell, const tSolvedCell& solved,
                                                              cSC4UndergroundOccupant*& out, bool& existing) {
    existing = true;   // the original's *param_4 starts true and is cleared once a new occupant was built
    const int x = static_cast<int>(cell->x);
    const int z = static_cast<int>(cell->z);
    if (FindUndergroundOccupant(x, z, out, NetworkGZIID())) {
        // The cell already holds one: only the "come from the tool" flag is refreshed.
        if (!unknown98_) {
            out->SetNetworkFlag(kUndergroundToolFlagBuilt);     // vslot 0x50
        } else {
            out->ClearNetworkFlag(kUndergroundToolFlagBuilt);   // vslot 0x58
        }
        return true;
    }
    float heights[4][3] = {};
    CreateNetworkOccupant(cell, out, heights, 0);   // pure virtual 0x2a0: builds the pipe/subway occupant
    cSC4UndergroundOccupant* occupant = out;
    if (occupant == nullptr) return false;
    if (!unknown98_) {
        occupant->SetNetworkFlag(kUndergroundToolFlagBuilt);
    } else {
        occupant->ClearNetworkFlag(kUndergroundToolFlagBuilt);
    }
    occupant->SetQuadDraw(solved.pieceType, solved.rotationFlip);   // vslot 0xf0
    IUndergroundNetworkToolHost* host = g_undergroundNetworkToolHost;
    float terrain[4] = {};
    for (int v = 0; v < 4; ++v) {
        // Quirk: no depth subtraction here (unlike every other terrain query of the tool).
        terrain[v] = host != nullptr ? host->TerrainHeight(static_cast<std::uint32_t>(cell->vertexBaseHeights[v]))
                                     : 0.0f;
    }
    occupant->SetTerrainHeights(terrain);   // vslot 0x390
    occupant->CreateWalls(false);           // vslot 0x360
    occupant->CreateFloor(false);           // vslot 0x368
    existing = false;
    return true;
}

// 0x1001a71e6
void cSC4UndergroundNetworkTool::CreateNetworkOccupant(cSC4NetworkCellInfo* cell, cSC4NetworkOccupant** out,
                                                       float (*heights)[3], std::uint32_t networkType) {
    if (out == nullptr) return;
    cSC4UndergroundOccupant* created = nullptr;
    CreateNetworkOccupant(cell, created, heights, networkType);   // the pure virtual of the concrete tool
    *out = created;
}

// 0x1001a7218
void cSC4UndergroundNetworkTool::InsertOccupantsForSolvedCells(std::vector<tSolvedCell>& cells,
                                                               std::list<cSC4NetworkOccupant*>* out, bool preview) {
    IUndergroundNetworkToolHost* host = g_undergroundNetworkToolHost;
    cSC4NetworkTool::InsertOccupantsForSolvedCells(cells, out, preview);   // 0x10018a13c
    if (!unknown98_ && !preview) return;
    for (tSolvedCell& solved : cells) {
        cSC4NetworkCellInfo* cell = GetCellInfo(solved.cellKey);
        if (cell == nullptr) continue;   // the original dereferences the result unchecked
        cSC4UndergroundOccupant* occupant = nullptr;
        bool existing = false;
        if (!CreateNewUndergroundOccupant(cell, solved, occupant, existing)) continue;
        occupant->SetVertexHeights(cell->vertexHeights);   // vslot 0x338; cell +0x60
        occupant->CreateGeometry(existing);                // vslot 0x348
        occupant->SetEdgeTypesFlag(static_cast<std::uint8_t>(networkType_),
                                   cell->tileNeighborhoods[networkType_]);   // vslot 0x128
        occupant->ClearNetworkFlag(kUndergroundToolFlagTranslucent);         // vslot 0x58
        occupant->SetNetworkFlag(kUndergroundToolFlagUsable);                // vslot 0x50
        if (!existing && occupantManager_ != nullptr && host != nullptr) {
            // Quirk: the manager's handle is kept in a single byte (+0x5c).
            cell->occupantId = static_cast<std::uint8_t>(
                host->AddOccupantToManager(static_cast<cISC4Occupant*>(occupant), unknown458_));
        }
        occupant->Release();   // vslot 0x10
    }
}

// ============================================================================================================
// previews / highlights
// ============================================================================================================

// 0x1001a7e56
void cSC4UndergroundNetworkTool::AddCellPreview(cSC4NetworkCellInfo& cell, const tSolvedCell& solved) {
    if (cell.status == 0) return;
    cSC4NetworkCellInfo copy = cell;   // the original rebuilds the record on the stack, keeping the occupant
    if (copy.occupant != nullptr) copy.occupant->AddRef();
    IUndergroundNetworkToolHost* host = g_undergroundNetworkToolHost;
    for (int v = 0; v < 4; ++v) {
        const float height = host != nullptr
                                 ? host->TerrainHeight(static_cast<std::uint32_t>(cell.vertexBaseHeights[v]))
                                 : 0.0f;
        copy.vertexHeights[v] = height - networkDepth_;
    }
    cSC4UndergroundOccupant* occupant = nullptr;
    if (FindUndergroundOccupant(static_cast<int>(cell.x), static_cast<int>(cell.z), occupant, NetworkGZIID())) {
        highlightList_.push_back(occupant);   // the cell already had one: highlight it, do not build again
    } else {
        bool existing = false;
        if (CreateNewUndergroundOccupant(&copy, solved, occupant, existing)) {
            cISC4Occupant* occupantInterface = static_cast<cISC4Occupant*>(occupant);   // AsOccupant (+0x180)
            occupantInterface->ClearFlag(1);                                           // [31] +0xf8
            occupant->CreateHighlight(true);                                           // vslot 0x370
            if (!existing && occupantManager_ != nullptr && host != nullptr) {
                cell.occupantId = static_cast<std::uint8_t>(
                    host->AddOccupantToManager(occupantInterface, unknown458_));   // +0xa0
            }
            previewList_.push_back(occupant);
        }
    }
    if (occupant != nullptr) occupant->SetHighlight(1, false);   // vslot 0x288
    if (copy.occupant != nullptr) copy.occupant->Release();
}

// 0x1001a80ec
void cSC4UndergroundNetworkTool::ClearCellPreviews() {
    IUndergroundNetworkToolHost* host = g_undergroundNetworkToolHost;
    for (cSC4UndergroundOccupant* occupant : previewList_) {
        if (occupant == nullptr) continue;
        occupant->SetHighlight(0, false);   // vslot 0x288
        if (occupantManager_ != nullptr && host != nullptr) {
            cISC4Occupant* occupantInterface = static_cast<cISC4Occupant*>(occupant);
            if ((occupantInterface->GetFlags() & 1u) != 0) {   // [33] +0x108
                host->RemoveOccupantFromManager(occupantInterface, false, unknown458_);   // +0xa8
            }
        }
        occupant->Shutdown();   // vslot 0x20
        occupant->Release();    // vslot 0x10
    }
    previewList_.clear();
    for (cSC4UndergroundOccupant* occupant : highlightList_) {
        if (occupant == nullptr) continue;
        occupant->SetHighlight(0, false);
        occupant->Release();
    }
    highlightList_.clear();
    cSC4NetworkTool::ClearCellPreviews();   // 0x10018803e
}

// ============================================================================================================
// solved-cell propagation
// ============================================================================================================

// 0x1001a913c
void cSC4UndergroundNetworkTool::PropagateFromCell(cSC4NetworkCellInfo* cell, int depth) {
    if (cell == nullptr) return;
    const std::uint32_t networkType = networkType_;
    // Quirk: the "is a plain piece" stop only applies from the fifth level on.
    const bool recurseAlways =
        !IsUndergroundStraightNeighbourhood(*cell, networkType) || depth < 4;
    for (int edge = 0; edge < 4; ++edge) {
        const std::uint32_t packed = cell->tileNeighborhoods[networkType_];
        if (!cSC4TileNeighborhoodCompact::HasConnectionOnEdge(packed, edge)) continue;
        const int neighbourX = static_cast<int>(cell->x) + kUndergroundNextX[edge];
        const int neighbourZ = static_cast<int>(cell->z) + kUndergroundNextZ[edge];
        const std::uint32_t neighbourKey =
            static_cast<std::uint32_t>(neighbourZ * 0x10000 + neighbourX);
        bool alreadySolved = false;
        for (const tSolvedCell& solved : solvedCellList_) {
            if (solved.cellKey == neighbourKey) {
                alreadySolved = true;
                break;
            }
        }
        if (alreadySolved) continue;
        cSC4NetworkCellInfo* neighbour = GetCellInfo(neighbourKey);
        if (neighbour == nullptr || neighbour->occupant == nullptr) continue;
        tSolvedCell solved;
        solved.pieceType = neighbour->occupant->PieceId();                 // +0xb8
        solved.rotationFlip = neighbour->occupant->GetRotationAndFlip();   // +0xd0
        solved.cellKey = neighbourKey;
        neighbour->solvedCellIndex = static_cast<std::int32_t>(solvedCellList_.size());
        solvedCellList_.push_back(solved);
        if (recurseAlways || !IsUndergroundStraightNeighbourhood(*neighbour, networkType)) {
            PropagateFromCell(neighbour, depth + 1);
        }
    }
}

// 0x1001a932a
void cSC4UndergroundNetworkTool::PropagateSolvedCellsForSmoothing() {
    const std::size_t count = solvedCellList_.size();
    for (std::size_t i = 0; i < count; ++i) {
        // The list may grow while this runs; the original re-reads the begin pointer every iteration and uses
        // the same index, so the new entries are visited too (up to the captured count).
        const std::uint32_t cellKey = solvedCellList_[i].cellKey;
        cSC4NetworkCellInfo* cell = GetCellInfo(cellKey);
        if (cell != nullptr) PropagateFromCell(cell, 0);
    }
}

// ============================================================================================================
// vertical constraints
// ============================================================================================================

// 0x1001a7616
void cSC4UndergroundNetworkTool::InsertRangeConstraintsForCell(std::uint32_t cellKey) {
    cSC4NetworkCellInfo* cell = GetCellInfo(cellKey);
    if (cell == nullptr) return;
    if (cell->buildable) {
        // 0x1001a7643 tail call: a buildable cell goes through the base tool's constraints instead.
        IUndergroundNetworkToolHost* host = g_undergroundNetworkToolHost;
        if (host != nullptr) host->InsertRangeConstraintsForCellBase(cellKey);
        return;
    }
    INetworkConstraintSatisfier* satisfier = constraintA_;   // this + 0x228
    if (satisfier == nullptr) return;
    if (cell->solvedOrImmovable) {
        // 0x1001a766d (the +0x5b branch): a cell that already belongs to a solved/immovable step has no free
        // corner heights: the original only pins the four vertices, without AddVertexToSolve or the ranges.
        for (int v = 0; v < 4; ++v) {
            satisfier->InsertFixedVertex(cell->vertexBaseHeights[v], cell->vertexHeights[v]);
        }
        return;
    }
    IUndergroundNetworkToolHost* host = g_undergroundNetworkToolHost;
    for (int v = 0; v < 4; ++v) {
        const int vertexId = cell->vertexBaseHeights[v];
        satisfier->AddVertexToSolve(vertexId, cell->vertexHeights[v]);
        if (unknown150_.find(static_cast<std::uint32_t>(vertexId)) != unknown150_.end()) {
            // Already pinned by an earlier pass: the height is fixed instead of ranged.
            satisfier->InsertFixedVertex(vertexId, cell->vertexHeights[v]);
            continue;
        }
        const float terrainHeight =
            (host != nullptr ? host->TerrainHeight(static_cast<std::uint32_t>(vertexId)) : 0.0f) - networkDepth_;
        satisfier->InsertRangeConstraint(vertexId, std::min(cell->range70[v].low, terrainHeight),
                                         std::min(cell->range70[v].high, terrainHeight));
        satisfier->InsertPreferredRangeConstraint(vertexId, std::min(cell->preferred90[v].low, terrainHeight),
                                                  std::min(cell->preferred90[v].high, terrainHeight));
        if (cell->hasOccupant) satisfier->InsertPreferredValueConstraint(vertexId, cell->vertexHeights[v]);
    }
}

// 0x1001a779a
void cSC4UndergroundNetworkTool::InsertNetworkSpecificConstraintsForCell(std::uint32_t cellKey) {
    cSC4NetworkCellInfo* cell = GetCellInfo(cellKey);
    if (cell == nullptr) return;
    if (cell->solvedOrImmovable) return;   // +0x5b
    std::uint32_t pieceId = 0;
    if (cell->pieceFromSolvedList) {   // +0x5f
        // Quirk: no bounds check on the index into the solved-cell vector.
        pieceId = solvedCellList_[static_cast<std::size_t>(cell->solvedCellIndex)].pieceType;
    } else {
        if (cell->occupant == nullptr) return;   // +0x10
        pieceId = cell->occupant->PieceId();     // +0xb8
        std::uint32_t occupiedX = 0, occupiedZ = 0;
        cell->occupant->GetOccupiedCell(occupiedX, occupiedZ);   // +0x1d8 (result unused)
    }
    switch (pieceId) {
        case 0x0000:
        case 0x0200:
        case 0x0300:
        case 0x0800:
        case 0x0a00:
        case 0x1400:
        case 0x4b00:
        case 0x14e00:
            return;   // plain pieces do not need the equality constraints
        default:
            break;
    }
    INetworkConstraintSatisfier* satisfier = constraintA_;   // this + 0x228
    if (satisfier == nullptr) return;
    // Quirk: a buildable cell shifts every vertex id by 100000, and both ids must look non-negative as ints.
    const std::uint32_t offset = cell->buildable ? 100000u : 0u;
    for (int i = 0; i < 3; ++i) {
        const std::int32_t a = cell->vertexBaseHeights[i];
        const std::int32_t b = cell->vertexBaseHeights[i + 1];
        if (a == b) continue;
        const std::uint32_t shiftedA = static_cast<std::uint32_t>(a) + offset;
        const std::uint32_t shiftedB = static_cast<std::uint32_t>(b) + offset;
        if (static_cast<std::int32_t>(shiftedA | shiftedB) < 0) continue;
        satisfier->InsertEqualityConstraint(shiftedA, shiftedB);
    }
}

// 0x1001a7c30
float cSC4UndergroundNetworkTool::GetNetworkVertexHt(int vertexId) const {
    float height = 0.0f;
    if (constraintA_ != nullptr && constraintA_->GetHeight(vertexId, height)) return height;
    IUndergroundNetworkToolHost* host = g_undergroundNetworkToolHost;
    return (host != nullptr ? host->TerrainHeight(static_cast<std::uint32_t>(vertexId)) : 0.0f) - networkDepth_;
}

// ============================================================================================================
// placement tiles
// ============================================================================================================

// 0x1001a78ee
void cSC4UndergroundNetworkTool::CreateCellInfosForCurrentPlacementTiles(std::uint32_t networkType) {
    (void)networkType;
    if (validSteps_ <= 0) return;
    IUndergroundNetworkToolHost* host = g_undergroundNetworkToolHost;
    for (int stepIndex = 0; stepIndex < validSteps_; ++stepIndex) {
        const tDraggedStep& step = steps_[static_cast<std::size_t>(stepIndex)];
        for (std::uint32_t c = step.firstCell; c <= step.lastCell; ++c) {
            if (c >= cells_.size()) break;   // the original trusts the drag state
            const SC4PointU& point = cells_[c];
            const int x = static_cast<int>(point.x);
            const int z = static_cast<int>(point.z);
            const std::uint32_t cellKey = static_cast<std::uint32_t>(z * 0x10000 + x);
            cSC4NetworkCellInfo* cell = GetCellInfo(cellKey);
            if (cell == nullptr) {
                cSC4NetworkCellInfo fresh;
                fresh.x = point.x;
                fresh.z = point.z;
                fresh.networkFlags = kFlagForNetworkType[networkType_];
                fresh.status = 1;
                fresh.stepIndex = stepIndex;
                fresh.hasOccupant = false;
                fresh.unknown5a = 0;
                fresh.solvedOrImmovable = step.direction != 0;
                fresh.occupantId = 0;
                fresh.shortStep = step.direction == 3;
                fresh.buildable = false;
                const std::int32_t width = widthPlusOne_;
                const std::int32_t base = z * width + x;
                fresh.vertexBaseHeights[0] = base;
                fresh.vertexBaseHeights[1] = base + width;
                fresh.vertexBaseHeights[2] = base + width + 1;
                fresh.vertexBaseHeights[3] = base + 1;
                for (int v = 0; v < 4; ++v) {
                    const float height =
                        (host != nullptr
                             ? host->TerrainHeight(static_cast<std::uint32_t>(fresh.vertexBaseHeights[v]))
                             : 0.0f) -
                        networkDepth_;
                    fresh.vertexHeights[v] = height;
                    // Quirk: a new cell uses placement +0x40 as the depth of its allowed range.
                    fresh.range70[v].low = height - placement_.float40[0];
                    fresh.range70[v].high = height;
                    fresh.preferred90[v].low = height;
                    fresh.preferred90[v].high = height;
                }
                if (worldCache_ == nullptr) continue;
                cSC4NetworkCellInfo* created = worldCache_->CreateCell(cellKey);
                if (created == nullptr) continue;
                created->x = fresh.x;
                created->z = fresh.z;
                created->networkFlags = fresh.networkFlags;
                created->unknown0c = fresh.unknown0c;
                // The occupant of the fresh record is replaced by the (null) occupant of the stack copy.
                if (created->occupant != nullptr) {
                    created->occupant->Release();
                    created->occupant = nullptr;
                }
                created->status = fresh.status;
                created->stepIndex = fresh.stepIndex;
                for (int t = 0; t < 13; ++t) created->tileNeighborhoods[t] = fresh.tileNeighborhoods[t];
                created->compact54 = fresh.compact54;
                created->unknown58 = fresh.unknown58;
                created->hasOccupant = fresh.hasOccupant;
                created->unknown5a = fresh.unknown5a;
                created->solvedOrImmovable = fresh.solvedOrImmovable;
                created->occupantId = fresh.occupantId;
                created->shortStep = fresh.shortStep;
                created->buildable = fresh.buildable;
                created->pieceFromSolvedList = fresh.pieceFromSolvedList;
                for (int v = 0; v < 4; ++v) {
                    created->vertexHeights[v] = fresh.vertexHeights[v];
                    created->range70[v] = fresh.range70[v];
                    created->preferred90[v] = fresh.preferred90[v];
                    created->vertexBaseHeights[v] = fresh.vertexBaseHeights[v];
                }
                created->solvedCellIndex = fresh.solvedCellIndex;
            } else {
                cell->networkFlags |= kFlagForNetworkType[networkType_];
                cell->status = 1;
                cell->stepIndex = stepIndex;
                if (step.direction != 0) cell->solvedOrImmovable = true;
                for (int v = 0; v < 4; ++v) {
                    const float height =
                        (host != nullptr
                             ? host->TerrainHeight(static_cast<std::uint32_t>(cell->vertexBaseHeights[v]))
                             : 0.0f) -
                        networkDepth_;
                    // Quirk: an existing cell uses placement +0x14, and its preferred range collapses to a
                    // point at min(vertex height, terrain height).
                    cell->range70[v].low = height - placement_.float10[1];
                    cell->range70[v].high = height;
                    const float preferred = std::min(cell->vertexHeights[v], height);
                    cell->preferred90[v].low = preferred;
                    cell->preferred90[v].high = preferred;
                }
            }
        }
    }
}

// 0x1001a8310
void cSC4UndergroundNetworkTool::StartPlacing() {
    std::vector<cSC4NetworkCellInfo> allCells;
    if (worldCache_ != nullptr) worldCache_->GetAllCells(allCells);   // cSC4NetworkWorldCache 0x10079ddbe
    for (const cSC4NetworkCellInfo& cell : allCells) {
        // Quirk: the skip test looks at the low bytes of +0x5c and +0x18.
        if (cell.occupantId != 0 && cell.status == 0) continue;
        const std::uint32_t cellKey = (cell.z << 16) + cell.x;
        if (solvedCells_.find(cellKey) == solvedCells_.end()) continue;   // this + 0x128
        cSC4UndergroundOccupant* occupant = nullptr;
        if (!FindUndergroundOccupant(static_cast<int>(cell.x), static_cast<int>(cell.z), occupant, NetworkGZIID())) {
            continue;
        }
        if (occupant->HasNetworkFlag(kUndergroundToolFlagNoPiece)) {
            occupant->Release();
            continue;
        }
        float oldHeights[4] = {};
        occupant->GetVertexHeights(oldHeights);   // vslot 0x110
        float newHeights[4] = {};
        bool changed = false;
        for (int v = 0; v < 4; ++v) {
            const float height = GetNetworkVertexHt(cell.vertexBaseHeights[v]);
            newHeights[v] = height;
            if (std::fabs(height - oldHeights[v]) > 0.001f) changed = true;
        }
        if (changed) {
            occupant->SetVertexHeights(newHeights);   // vslot 0x338
            occupant->CreateGeometry(true);           // vslot 0x348
        }
        occupant->Release();
    }
}

// ============================================================================================================
// triangles / terrain heights
// ============================================================================================================

// 0x1001a8500
void cSC4UndergroundNetworkTool::SetQuadTrianglesToDraw(cSC4NetworkOccupant* occupant,
                                                         const cSC4TileNeighborhoodCompact& compact) {
    (void)compact;   // the original does not read it either
    if (occupant == nullptr) return;
    void* queried = nullptr;
    if (!occupant->QueryInterface(kGZIID_cISC4UndergroundOccupant3, &queried)) return;
    auto* underground = static_cast<cSC4UndergroundOccupant*>(queried);
    occupant->ClearNetworkFlag(kNetworkFlagHideTriangle0);   // +0x58
    occupant->ClearNetworkFlag(kNetworkFlagHideTriangle1);
    occupant->ClearNetworkFlag(kNetworkFlagHideTriangle2);
    bool hidden0 = false, hidden1 = false, hidden2 = false;
    underground->GetHiddenTriangles(hidden0, hidden1, hidden2);   // vslot 0x378
    if (hidden0) occupant->SetNetworkFlag(kNetworkFlagHideTriangle0);          // +0x50
    // Quirks: triangles 1 and 2 are inverted (the flag marks the visible triangle).
    if (!hidden1) occupant->SetNetworkFlag(kNetworkFlagHideTriangle1);
    if (!hidden2) occupant->SetNetworkFlag(kNetworkFlagHideTriangle2);
    underground->Release();
}

// 0x1001a85e2 — PARTIAL. See the header: this function is 2788 bytes and its body is mostly
//   * cSC4NetworkTool::SmoothenNetwork @ 0x1001b14a0 (the base class's vertical solver pass, not recovered),
//   * SL::NetworkManager() (0x100041188) slot +0x30 and SL::spOccupantManager slot +0x180 (batch move/remove of
//     cISC4NetworkOccupant lists; neither object has a seam yet),
//   * an inlined libc++ __hash_table rehash for the solved-cell dedupe (the port uses the base's
//     unknown128_/std::unordered_set and solvedCellList_ instead of reproducing the pointer juggling).
// Only the three cache resets at the top of the original are ported (0x1001a85e2..0x1001a8638); everything
// after them (the per-occupant terrain re-read, the smoothing, and the two update/destroy branches) is left
// out. The function is recovered-partial in notes/ledger_pending/cSC4UndergroundNetworkTool.txt.
void cSC4UndergroundNetworkTool::UpdateUndergroundOccupantTerrainHeights(std::vector<SC4PointU> cells,
                                                                         std::list<cSC4NetworkOccupant*>& occupants,
                                                                         std::uint32_t networkType) {
    (void)cells;
    (void)occupants;
    (void)networkType;
    if (worldCache_ != nullptr) worldCache_->Clear();   // cSC4NetworkWorldCache::Clear 0x10079d868
    unknown128_.clear();
    solvedCellList_.clear();
}

}  // namespace sc4
