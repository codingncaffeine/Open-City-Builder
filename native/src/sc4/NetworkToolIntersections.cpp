// cSC4NetworkTool's pre-built intersections ("puzzle pieces"), recovered from the Mac i386 build (symbols kept). A rule of
// the intersection rule file (nSC4Networks::cIntRule) names the existing network cells it replaces and the cells it takes;
// placing one records it (tIntersection), marks its cells (+0x52) so the solver leaves them alone, smooths their corners
// under the rule's constraint layout, and at commit gives every occupied cell a pre-built model piece
// (cSC4NetworkOccupantWithPreBuiltModel, piece AutoTileBase + its tile) anchored at the rule's cell.
//
// i386 offsets in the comments: +0x1c the world cache, +0x48 the occupant manager, +0x50 the place flag (unknown98_), +0x54
// the dragged steps, +0x60 the dragged cells, +0x74 / +0x78 / +0x7c the straight step count, the valid steps and the pen
// direction, +0x84 the set of cells to solve, +0x98 the solved cells, +0xcc the intersections, +0xd8 the bridges, +0xe4 the
// tunnels, +0xf0 the additional draws, +0x230 unknown458_, +0x234 the failure reason, +0x238 / +0x254 the demolition region
// and its cost, +0x398 the type record, +0x39c the network type, +0x3a4 the occupant filter.
#include "sc4/NetworkTool.h"

#include <list>

#include "sc4/NetworkOccupant.h"
#include "sc4/NetworkOccupantWithPreBuiltModel.h"
#include "sc4/NetworkWorldCache.h"

namespace sc4 {

namespace {
constexpr std::uint32_t Key(std::uint32_t x, std::uint32_t z) { return x + (z << 16); }
constexpr int kFailReserved = static_cast<int>(0xf0000002), kFailSmooth = static_cast<int>(0xf0000006),
              kFailSolve = static_cast<int>(0xf0000007), kFailSupport = static_cast<int>(0xf0000008);
// The exemplar group of the intersection pieces' models (InsertHighwayIntersectionPiece).
constexpr std::uint32_t kIntersectionPieceGroup = 0xa8434037;

// cSC4NetworkTool::sIntersectionRules.
nSC4Networks::IntersectionRuleMap& Rules() {
    static nSC4Networks::IntersectionRuleMap rules;
    return rules;
}
}  // namespace

const nSC4Networks::cIntRule* cSC4NetworkTool::GetIntersectionRule(std::uint32_t id) {
    const auto found = Rules().find(id);
    return found == Rules().end() ? nullptr : &found->second;
}

const nSC4Networks::IntersectionRuleMap& cSC4NetworkTool::IntersectionRules() { return Rules(); }

void cSC4NetworkTool::ReloadIntersectionRules(cRZIniFile& ini) { nSC4Networks::ReadHighwayIntersectionRules(ini, Rules()); }

void cSC4NetworkTool::AddEdgesToTile(std::uint32_t x, std::uint32_t z, std::uint32_t edges, std::uint32_t networkType,
                                     std::uint8_t oneWayBits, std::uint8_t oneWayMask) {
    tReservedCell draw;
    draw.x = x;
    draw.z = z;
    draw.compact = edges;
    draw.network = networkType;
    draw.oneWayBits = oneWayBits;
    draw.oneWayMask = oneWayMask;
    reservedCells_.push_back(draw);
}

// The drag becomes one step of one cell, the rule's anchor, with the rule recorded at step 0. The rule's new cells are the
// demolition region; then the reserved-tile check (the drag itself skipped), the cell infos and the additional draws (the
// caller's DoAutoComplete), the rule's own test, the marks, the solve of the arms and the height passes 0..2 (pass 3 last,
// with its own failure codes). A real placement then demolishes, moves the terrain and inserts the arms and the pieces.
bool cSC4NetworkTool::InsertIsolatedHighwayIntersection(int x, int z, std::uint32_t id, bool real) {
    solvedCells_.clear();
    solvedCellList_.clear();
    if (worldCache_) worldCache_->Clear();
    intersections_.clear();
    bridges_.clear();
    tunnels_.clear();
    unknown98_ = real;
    const nSC4Networks::cIntRule* rule = GetIntersectionRule(id);
    if (!rule) return false;
    const int type = rule->GetTypeAtOrigin();
    tIntersection record;
    record.ruleId = id;
    record.x = x;
    record.z = z;
    intersections_.push_back(record);
    steps_.clear();
    steps_.push_back(tDraggedStep{});
    cells_.clear();
    cells_.push_back(SC4PointU{static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z)});
    straightSteps_ = 1;
    directionA_ = 0;
    validSteps_ = 1;
    std::vector<SC4PointU> newCells;
    for (const nSC4Networks::cIntPoint& cell : rule->newCells)
        newCells.push_back(SC4PointU{static_cast<std::uint32_t>(x + cell.x), static_cast<std::uint32_t>(z + cell.z)});
    demolitionCells_ = newCells;
    baseCost_ = 0;
    unknown484_ = 0;
    std::int64_t cost = 0;
    INetworkToolHost* host = GetNetworkToolHost();
    if (host && !host->DemolishForNetwork(false, demolitionCells_, cost)) return false;
    baseCost_ = static_cast<std::int32_t>(cost);
    int steps = validSteps_;
    CheckForReservedTiles(steps, static_cast<std::uint32_t>(type), true);              // vslot 0x114
    if (steps < validSteps_) {
        failureReason_ = kFailReserved;
        return false;
    }
    CreateCellInfosForCurrentPlacementTiles(static_cast<std::uint32_t>(type));         // vslot 0x124
    DoAdditionalDraws();
    if (rule->Test(x, z, *this) != nSC4Networks::cIntRule::kMatch) {
        failureReason_ = kFailSolve;
        return false;
    }
    MarkHighwayIntersectionPieces(true);
    BuildSetOfCellsToBeSolved();
    if (SolveNetwork(static_cast<std::uint32_t>(type), false) <= 0) {
        failureReason_ = kFailSolve;
        return false;
    }
    bool smooth = false;
    for (int pass = 0; pass != 3 && !smooth; ++pass)
        smooth = SmoothenNetwork(pass) && CanTilesBeSupportedOnTerrain(static_cast<std::uint32_t>(type));   // vslot 0xe8
    if (!smooth) {
        if (!SmoothenNetwork(3)) {
            failureReason_ = kFailSmooth;
            return false;
        }
        if (!CanTilesBeSupportedOnTerrain(networkType_)) {
            failureReason_ = kFailSupport;
            return false;
        }
    }
    if (unknown98_) {
        std::vector<std::uint32_t> movedVertices;
        StartPlacing();                                                                // vslot 0x120
        DestroyHilightedOccupants();                                                   // vslot 0xf8
        UpdateTerrainHeights(movedVertices);                                           // vslot 0xec
        MarkPositionChangedExistingOccupantsAsNewlySolved();
        MarkTerrainChangedExistingOccupantsAsNewlySolved(movedVertices);
    }
    std::list<cSC4NetworkOccupant*> placed;
    InsertOccupantsForSolvedCells(solvedCellList_, &placed, false);                    // vslot 0xe4
    InsertHighwayIntersectionPieces(&placed);
    MarkOccupantsUsable(placed, true);                                                 // cSC4NetworkConstructionCrew
    return true;
}

// The recorded intersections within the valid steps.
void cSC4NetworkTool::InsertHighwayIntersectionPieces(std::list<cSC4NetworkOccupant*>* out) {
    for (const tIntersection& intersection : intersections_) {
        if (validSteps_ <= intersection.stepB) break;
        InsertHighwayIntersection(intersection.x, intersection.z, intersection.ruleId, out, unknown98_, false);
    }
}

// Nothing unless `real`: every occupied cell of the rule that has a cell info and a tile gets its piece, AutoTileBase + tile,
// with the path AutoPathBase + tile when the rule has a path base. The place flag is the caller's for the loop.
void cSC4NetworkTool::InsertHighwayIntersection(int x, int z, std::uint32_t id, std::list<cSC4NetworkOccupant*>* out, bool real,
                                                bool usable) {
    const nSC4Networks::cIntRule* rule = GetIntersectionRule(id);   // the original's operator[] inserts an empty rule
    const bool saved = unknown98_;
    unknown98_ = real;
    if (real && rule) {
        for (const nSC4Networks::cIntPoint& cell : rule->occupiedCells) {
            cSC4NetworkCellInfo* info = GetCellInfo(Key(static_cast<std::uint32_t>(x + cell.x), static_cast<std::uint32_t>(z + cell.z)));
            if (!info) continue;
            const nSC4Networks::cIntRule::cTileDef* tile = rule->GetTileInfo(cell.x, cell.z);
            if (!tile) continue;
            const std::uint32_t path = rule->autoPathBase != 0 ? rule->autoPathBase + tile->tile : 0;
            InsertHighwayIntersectionPiece(info, out, usable, rule->autoTileBase + tile->tile, tile->orientation, x, z, path);
        }
    }
    unknown98_ = saved;
}

// A pre-built model occupant over the cell with the piece's model (group 0xa8434037) at the piece's orientation, drawn
// without quad triangles, its split diagonal from the corner heights, the path override, the cell cleared, inserted, marked
// as part of an intersection (0x500000) and anchored at the rule's cell.
void cSC4NetworkTool::InsertHighwayIntersectionPiece(cSC4NetworkCellInfo* cell, std::list<cSC4NetworkOccupant*>* out, bool usable,
                                                     std::uint32_t pieceId, std::uint8_t orientation, int x, int z,
                                                     std::uint32_t pathId) {
    INetworkToolHost* host = GetNetworkToolHost();
    float corners[4][3];
    cSC4NetworkOccupant* occupant = nullptr;
    CreateNetworkOccupant(cell, &occupant, corners, kGZCLSID_cSC4NetworkOccupantWithPreBuiltModel);   // vslot 0xe0
    if (!occupant) return;
    const float offset[3] = {0.0f, 0.0f, 0.0f};
    SetExemplarForOccupant(occupant, pieceId, offset, orientation, kIntersectionPieceGroup);
    occupant->SetQuadDraw(pieceId, orientation);                                       // vslot 0x78
    occupant->ClearNetworkFlag(0x2000000);
    occupant->ClearNetworkFlag(0x4000000);
    occupant->SetNetworkFlag(0x8000000);
    if (cell->vertexHeights[1] + cell->vertexHeights[3] < cell->vertexHeights[0] + cell->vertexHeights[2])
        occupant->SetNetworkFlag(0x1000000);
    if (auto* prebuilt = dynamic_cast<cSC4NetworkOccupantWithPreBuiltModel*>(occupant)) prebuilt->RecomputeShearTransform();
    if (pathId != 0) occupant->SetPathIDOverride(pathId);                              // vslot 0xcc
    ClearNetworkOccsFromCell(cell->x, cell->z);
    if (host) cell->occupantId = static_cast<std::uint8_t>(host->InsertNetworkOccupant(occupant, static_cast<std::uint32_t>(unknown458_)));
    if (usable) {
        occupant->SetNetworkFlag(0x10000000);
        occupant->ClearNetworkFlag(0x4000);
    }
    occupant->SetNetworkFlag(0x500000);
    occupant->SetComplexPieceOrigin(x, z);                                             // vslot 0x120
    if (out) {
        occupant->AddRef();
        out->push_back(occupant);
    }
    occupant->Release();
}

// +0x52 on the cell infos of the rule's occupied cells.
void cSC4NetworkTool::MarkIntersection(const tIntersection& intersection, bool mark) {
    const nSC4Networks::cIntRule* rule = GetIntersectionRule(intersection.ruleId);
    if (!rule) return;
    for (const nSC4Networks::cIntPoint& cell : rule->occupiedCells) {
        cSC4NetworkCellInfo* info = GetCellInfo(static_cast<std::uint32_t>(intersection.x + cell.x + (intersection.z + cell.z) * 0x10000));
        if (info) info->unknown5a = mark;
    }
}

void cSC4NetworkTool::MarkHighwayIntersectionPieces(bool mark) {
    for (const tIntersection& intersection : intersections_) {
        if (validSteps_ <= intersection.stepB) break;
        MarkIntersection(intersection, mark);
    }
}

// The occupied cells of the recorded intersections leave the set to solve; the record keeps them.
void cSC4NetworkTool::RemoveHighwayIntersectionPiecesFromSetOfCellsToBeSolved(std::unordered_set<std::uint32_t>& cells) {
    for (tIntersection& intersection : intersections_) {
        if (validSteps_ <= intersection.stepB) break;
        const nSC4Networks::cIntRule* rule = GetIntersectionRule(intersection.ruleId);
        if (!rule) continue;
        for (const nSC4Networks::cIntPoint& cell : rule->occupiedCells) {
            const auto key = static_cast<std::uint32_t>(intersection.x + cell.x + (intersection.z + cell.z) * 0x10000);
            cells.erase(key);
            intersection.cells.push_back(key);
        }
    }
}

// SmoothenNetwork's first step: every occupied cell of every recorded intersection gets a (placeholder) cell info marked
// +0x52 (a new cell takes the rule's network types) and joins the solved list with the first model's orientation, so the
// smoothing set grows around it. The caller cuts the solved list back afterwards.
void cSC4NetworkTool::InsertIntersectionCellsIntoSolvedSet() {
    for (const tIntersection& intersection : intersections_) {
        const nSC4Networks::cIntRule* rule = GetIntersectionRule(intersection.ruleId);
        if (!rule) continue;
        GetCellInfo(Key(static_cast<std::uint32_t>(intersection.x), static_cast<std::uint32_t>(intersection.z)));
        std::uint8_t orientation = 0;
        if (!rule->pieces.empty())
            orientation = static_cast<std::uint8_t>(rule->pieces.front().rotation + (rule->pieces.front().flip != 0 ? 0x80 : 0));
        for (const nSC4Networks::cIntPoint& cell : rule->occupiedCells) {
            cSC4NetworkCellInfo* info = CreatePlaceholderCellInfo(static_cast<std::uint32_t>(intersection.x + cell.x),
                                                                  static_cast<std::uint32_t>(intersection.z + cell.z), networkType_);
            if (!info) continue;
            info->unknown5a = 1;
            const nSC4Networks::cIntRule::cTileDef* tile = rule->GetTileInfo(cell.x, cell.z);
            if (tile && tile->newCell) info->networkFlags = rule->typeMask;
            tSolvedCell solved;
            solved.pieceType = 0;
            solved.rotationFlip = orientation;
            solved.cellKey = info->x + info->z * 0x10000;
            solvedCellList_.push_back(solved);
        }
    }
}

// The constraint layout of every recorded intersection: each occupied cell's corners join the satisfier at their heights,
// then its code ties them ('-' / '|' level across the cell and limit the slope along it, '/' '\' the diagonals, 'F' 'R' a
// level diagonal with a straight slope along the other two corners, '#' all four level, 'X' slope limits on every edge).
// Corners: 0 (x, z), 1 (x, z+1), 2 (x+1, z+1), 3 (x+1, z).
void cSC4NetworkTool::InsertIntersectionConstraints(INetworkConstraintSatisfier& satisfier) {
    static const cSC4NetworkSlopeLimits kDefaultLimits;
    const cSC4NetworkSlopeLimits& limits = typeInfo_ ? typeInfo_->SlopeLimits() : kDefaultLimits;
    const float straight = limits.maxSlope, diagonal = limits.maxSlopeDiagonal;
    for (const tIntersection& intersection : intersections_) {
        const nSC4Networks::cIntRule* rule = GetIntersectionRule(intersection.ruleId);
        if (!rule) continue;
        GetCellInfo(Key(static_cast<std::uint32_t>(intersection.x), static_cast<std::uint32_t>(intersection.z)));
        for (const nSC4Networks::cIntPoint& cell : rule->occupiedCells) {
            const std::uint8_t code = rule->GetConstraintAtCell(cell.x, cell.z);
            const cSC4NetworkCellInfo* info = GetCellInfo(Key(static_cast<std::uint32_t>(intersection.x + cell.x),
                                                              static_cast<std::uint32_t>(intersection.z + cell.z)));
            if (!info) continue;
            const std::int32_t* v = info->vertexBaseHeights;
            for (int i = 0; i < 4; ++i) satisfier.AddVertexToSolve(v[i], info->vertexHeights[i]);
            switch (code) {
                case 1:
                    satisfier.InsertEqualityConstraint(static_cast<std::uint32_t>(v[0]), static_cast<std::uint32_t>(v[1]));
                    satisfier.InsertEqualityConstraint(static_cast<std::uint32_t>(v[2]), static_cast<std::uint32_t>(v[3]));
                    satisfier.InsertSlopeConstraint(v[0], v[3], straight);
                    satisfier.InsertSlopeConstraint(v[1], v[2], straight);
                    break;
                case 2:
                    satisfier.InsertEqualityConstraint(static_cast<std::uint32_t>(v[0]), static_cast<std::uint32_t>(v[3]));
                    satisfier.InsertEqualityConstraint(static_cast<std::uint32_t>(v[1]), static_cast<std::uint32_t>(v[2]));
                    satisfier.InsertSlopeConstraint(v[0], v[1], straight);
                    satisfier.InsertSlopeConstraint(v[3], v[2], straight);
                    break;
                case 3:
                    satisfier.InsertEqualityConstraint(static_cast<std::uint32_t>(v[0]), static_cast<std::uint32_t>(v[2]));
                    satisfier.InsertSlopeConstraint(v[1], v[3], diagonal);
                    break;
                case 4:
                    satisfier.InsertEqualityConstraint(static_cast<std::uint32_t>(v[1]), static_cast<std::uint32_t>(v[3]));
                    satisfier.InsertSlopeConstraint(v[0], v[2], diagonal);
                    break;
                case 5:
                    satisfier.InsertEqualityConstraint(static_cast<std::uint32_t>(v[0]), static_cast<std::uint32_t>(v[2]));
                    satisfier.InsertSlopeConstraint(v[1], v[3], diagonal);
                    satisfier.InsertSmoothnessConstraint(v[1], v[0], v[3], 0.0f);
                    break;
                case 6:
                    satisfier.InsertEqualityConstraint(static_cast<std::uint32_t>(v[1]), static_cast<std::uint32_t>(v[3]));
                    satisfier.InsertSlopeConstraint(v[0], v[2], diagonal);
                    satisfier.InsertSmoothnessConstraint(v[0], v[1], v[2], 0.0f);
                    break;
                case 7:
                    for (int i = 1; i < 4; ++i) satisfier.InsertEqualityConstraint(static_cast<std::uint32_t>(v[0]), static_cast<std::uint32_t>(v[i]));
                    break;
                case 8:
                    satisfier.InsertSlopeConstraint(v[0], v[1], straight);
                    satisfier.InsertSlopeConstraint(v[0], v[3], straight);
                    satisfier.InsertSlopeConstraint(v[1], v[2], straight);
                    satisfier.InsertSlopeConstraint(v[3], v[2], straight);
                    break;
                default:
                    break;
            }
        }
    }
}

}  // namespace sc4
