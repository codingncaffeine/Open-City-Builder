// The pre-built intersection rules (nSC4Networks::cIntRule) and the tool steps around them: the CheckType grammar, the rule
// file reader (layouts, CopyFrom, Rotate, Transpose, the ordering rings), the bounds and tile queries, Test and
// DoAutoComplete over a world cache of cell infos, and InsertIntersectionConstraints' constraint codes. The rule file is
// written to TMPDIR and read through cRZIniFile as the game reads it.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "rz/cRZIniFile.h"
#include "rz/cRZString.h"
#include "sc4/NetworkIntersectionRules.h"
#include "sc4/NetworkTool.h"
#include "sc4/NetworkWorldCache.h"

static int failures = 0;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);           \
            ++failures;                                                \
        }                                                              \
    } while (0)

using namespace sc4;
using namespace sc4::nSC4Networks;

namespace sc4 {
struct NetworkToolTestAccess {
    static void SetWorldCache(cSC4NetworkTool& t, INetworkWorldCache* cache) { t.worldCache_ = cache; }
    static void SetCity(cSC4NetworkTool& t, int width) {
        t.cityWidth_ = t.cityHeight_ = width;
        t.widthPlusOne_ = t.heightPlusOne_ = width + 1;
    }
    static std::vector<tReservedCell>& Draws(cSC4NetworkTool& t) { return t.reservedCells_; }
    static std::vector<tIntersection>& Intersections(cSC4NetworkTool& t) { return t.intersections_; }
    static int& ValidSteps(cSC4NetworkTool& t) { return t.validSteps_; }
};
}  // namespace sc4

namespace {

struct Host : INetworkToolHost {
    void RegisterShutdownCallback(bool (*)(void*), void*) override {}
    cIGZUnknown* AcquireTerrain() override { return nullptr; }
    float TerrainCellSize() override { return 16.0f; }
    bool TerrainAllowsNetwork(std::uint32_t x, std::uint32_t z) override { return x == waterX && z == waterZ; }
    int CityWidth() override { return 64; }
    int CityHeight() override { return 64; }
    cIGZUnknown* AcquireOccupantManager() override { return nullptr; }
    cIGZUnknown* AcquireCityViewModel() override { return nullptr; }
    void AddRenderLayer(INetworkPreview*, std::uint32_t, std::int32_t) override {}
    void RemoveRenderLayer(INetworkPreview*) override {}
    std::uint32_t RendererAltitudeScale() override { return 0; }
    void RegisterMessage(cIGZMessageTarget2*, std::uint32_t) override {}
    void UnregisterMessage(cIGZMessageTarget2*, std::uint32_t) override {}
    cIGZUnknown* CreateNetworkOrFilter(std::uint32_t) override { return nullptr; }
    INetworkPreview* CreatePreview() override { return nullptr; }
    INetworkWorldCache* CreateWorldCache() override { return nullptr; }
    INetworkConstraintSatisfier* CreateConstraintSatisfier() override { return nullptr; }
    const INetworkTypeInfo* NetworkTypeInfo(std::uint32_t) override { return nullptr; }
    cSC4NetworkCellInfo* GetCellInfo(std::uint32_t) override { return nullptr; }
    cIGZUnknown* GetTuningProperty(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) override { return nullptr; }
    bool LoadString(std::uint32_t, std::uint32_t, cIGZString&) override { return false; }
    bool DemolishForNetwork(bool, const std::vector<SC4PointU>& cells, std::int64_t& cost) override {
        demolished = cells;
        cost = 0;
        return demolishAllowed;
    }
    std::uint32_t waterX = 1000, waterZ = 1000;
    bool demolishAllowed = true;
    std::vector<SC4PointU> demolished;
};

// The constraint calls InsertIntersectionConstraints makes, in order.
struct RecordingSatisfier : INetworkConstraintSatisfier {
    std::vector<std::string> calls;
    void AddVertexToSolve(int, float) override {}
    bool GetHeight(int, float&) override { return false; }
    void InsertRangeConstraint(int, float, float) override {}
    void InsertPreferredRangeConstraint(int, float, float) override {}
    void InsertPreferredValueConstraint(int, float) override {}
    void InsertFixedVertex(int, float) override {}
    void InsertEqualityConstraint(std::uint32_t a, std::uint32_t b) override { calls.push_back("eq " + std::to_string(a) + " " + std::to_string(b)); }
    void InsertSlopeConstraint(int a, int b, float limit) override {
        calls.push_back("slope " + std::to_string(a) + " " + std::to_string(b) + " " + std::to_string(static_cast<int>(limit * 100)));
    }
    void InsertSmoothnessConstraint(int a, int b, int c, float) override {
        calls.push_back("smooth " + std::to_string(a) + " " + std::to_string(b) + " " + std::to_string(c));
    }
};

// The rule file: 0x10 a road crossing a highway (A / C checks, B the crossing the piece takes, a / b road cells it takes) with
// a new cell, 0x11 its quarter turn, 0x12 its mirror, 0x20 a one-cell rule ring, and the ordering of the three.
const char* kRules =
    "; the ordering comes first in the file, the reader takes the sections in name order\n"
    "[Ordering]\n"
    "RotationRing = 10, 11, 99, 13\n"
    "AddTypes = 12, 20, 21\n"
    "[HighwayIntersectionInfo_0x00000010]\n"
    "Piece = 8.0, -16.0, 1, 0, 0x0a000001\n"
    "CellLayout = .A+\n"
    "CellLayout = aBb<\n"
    "CellLayout = .C.\n"
    "CellLayout = .^.\n"
    "CheckType = A - check highway: 0x00020000, 0x00ff0000\n"
    "CheckType = B - road: 0x02000200 highway: 0x00020002\n"
    "CheckType = C - highway: 0x00000002, 0x000000ff optional\n"
    "CheckType = a - road: 0x00000200, 0x0000ff00 multiok\n"
    "CheckType = b - road: 0x02000000, 0xff000000\n"
    "ConsLayout = .-|\n"
    "ConsLayout = /#X<\n"
    "ConsLayout = .^\n"
    "AutoTileBase = 0x0c00a000\n"
    "AutoPathBase = 0x0d000000\n"
    "PlaceQueryID = 0xA0000004\n"
    "HandleOffset = 1, 0\n"
    "PreviewEffect = preview_test\n"
    "OneWayDir = 1\n"
    "Costs = 1500\n"
    "AutoPlace = yes\n"
    "StepOffsets = 2, 3\n"
    "ReplacementIntersection = 4, -1\n"
    "PieceType = ignored\n"
    "[HighwayIntersectionInfo_0x00000011]\n"
    "CopyFrom = 0x10\n"
    "Rotate = 1\n"
    "[HighwayIntersectionInfo_0x00000012]\n"
    "CopyFrom = 0x00000010\n"
    "Transpose = 1\n"
    "Translate = 1, 2\n"
    "[HighwayIntersectionInfo_0x00000013]\n"
    "CopyFrom = 0x10\n"
    "Rotate = 2\n"
    "[HighwayIntersectionInfo_0x00000021]\n"
    "CopyFrom = 0x20\n"
    "[HighwayIntersectionInfo_0x00000020]\n"
    "CellLayout = A<\n"
    "CellLayout = ^\n"
    "CheckType = A - highway: 0x02000200\n";

bool HasPoint(const std::vector<cIntPoint>& points, int x, int z) {
    for (const cIntPoint& p : points)
        if (p.x == x && p.z == z) return true;
    return false;
}

cSC4NetworkCellInfo* MakeCell(cSC4NetworkWorldCache& cache, std::uint32_t x, std::uint32_t z) {
    cSC4NetworkCellInfo* cell = cache.CreateCell(x + (z << 16));
    cell->x = x;
    cell->z = z;
    const std::int32_t v0 = static_cast<std::int32_t>(z * 65 + x);
    cell->vertexBaseHeights[0] = v0;
    cell->vertexBaseHeights[1] = v0 + 65;
    cell->vertexBaseHeights[2] = v0 + 66;
    cell->vertexBaseHeights[3] = v0 + 1;
    return cell;
}

}  // namespace

int main() {
    Host host;
    SetNetworkToolHost(&host);

    // ---- the CheckType grammar ----
    {
        std::uint8_t tag = 0;
        cIntRule::CheckType check;
        CHECK(ParseCheckType("d - road: 0x00020002 HighWay: 0x02040200, 0x00ff00ff  # comment", tag, check));
        CHECK(tag == 'd');
        CHECK(check.types[0] == kNetworkRoad && check.types[1] == kNetworkHighway);
        CHECK(check.checks[0].edges == 0x00020002u && check.checks[0].mask == 0xffffffffu);   // the mask defaults to all
        CHECK(check.checks[1].edges == 0x02040200u && check.checks[1].mask == 0x00ff00ffu);
        CHECK(check.occupies && !check.optional && !check.multiOk);
        CHECK(ParseCheckType("E - check groundhighway: 0x03010000 optional multiok", tag, check));
        CHECK(tag == 'E' && check.types[0] == kNetworkGroundHighway && check.types[1] == -1);
        CHECK(!check.occupies && check.optional && check.multiOk);
        CHECK(ParseCheckType("F - onewayroad: 0x02000200 check ; the rest is ignored road: 1", tag, check));
        CHECK(check.types[0] == kNetworkOneWayRoad && !check.occupies && !check.optional);
        CHECK(!ParseCheckType("ab - road: 0x2", tag, check));                          // "Bad map tag"
        CHECK(!ParseCheckType("a - road: 0x2 road: 0x2 road: 0x2", tag, check));       // "Too many network checks"
        CHECK(!ParseCheckType("a - boulevard: 0x2", tag, check));                      // "Expected network name"
        CHECK(!ParseCheckType("1 - road: 0x2", tag, check));                           // a digit is a number, not a tag
        CHECK(!ParseCheckType("a - road: 0x2, 7 9", tag, check));                      // a stray number ends the value
    }

    // ---- the file ----
    const char* tmp = std::getenv("TMPDIR");
    const std::string path = std::string(tmp ? tmp : "/tmp") + "/sc4_networkintersections_rules.ini";
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fputs(kRules, f);
        std::fclose(f);
    }
    IntersectionRuleMap rules;
    {
        cRZIniFile ini{cRZString(path.c_str())};
        ReadHighwayIntersectionRules(ini, rules);
    }
    CHECK(rules.size() == 6);
    const cIntRule& base = rules[0x10];
    // The layout: centre column 1 ('^'), centre row 1 ('<'). Occupied: '+' (1,-1), 'a' (-1,0), 'B' (0,0), 'b' (1,0); 'A' and 'C'
    // only check.
    CHECK(base.occupiedCells.size() == 4);
    CHECK(HasPoint(base.occupiedCells, 1, -1) && HasPoint(base.occupiedCells, -1, 0) && HasPoint(base.occupiedCells, 0, 0) &&
          HasPoint(base.occupiedCells, 1, 0));
    CHECK(base.newCells.size() == 1 && HasPoint(base.newCells, 1, -1));
    CHECK(base.checkCells.size() == 5);
    CHECK(base.minX == -1 && base.maxX == 1 && base.minZ == -1 && base.maxZ == 0);
    CHECK(base.tileWidth == 3 && base.tiles.size() == 6);
    // tile codes row * 16 + column + 1 from the bounds' corner; the tile is column * 0x100 + row * 0x10
    const cIntRule::cTileDef* plus = base.GetTileInfo(1, -1);
    CHECK(plus && plus->tile == 0x200 && plus->newCell == 1 && plus->orientation == 0);
    const cIntRule::cTileDef* crossing = base.GetTileInfo(0, 0);
    CHECK(crossing && crossing->tile == 0x110 && crossing->newCell == 0);
    CHECK(base.GetTileInfo(0, -1) == nullptr);   // 'A' only checks: no tile
    CHECK(base.GetTileInfo(2, 0) == nullptr);    // outside the bounds
    CHECK(base.typeMask == 0x5 && base.GetTypeAtOrigin() == kNetworkRoad);
    CHECK(base.autoTileBase == 0x0c00a000u && base.autoPathBase == 0x0d000000u && base.placeQueryId == 0xa0000004u);
    CHECK(base.handleX == 1 && base.handleZ == 0 && base.cost == 1500 && base.autoPlace && base.oneWayDir == 1);
    CHECK(base.stepOffsetA == 2 && base.stepOffsetB == 3);
    CHECK(base.hasReplacement && base.replacementX == 4 && base.replacementZ == -1);
    CHECK(base.previewEffect == "preview_test");
    CHECK(base.pieces.size() == 1 && base.pieces[0].id == 0x0a000001u && base.pieces[0].rotation == 1 && base.pieces[0].x == 8.0f &&
          base.pieces[0].z == -16.0f);
    // the constraint grid: as wide as the longest line, centre (1, 1); '-' 1 '|' 2 '/' 3 '#' 7 'X' 8
    CHECK(base.constraintWidth == 4 && base.constraintCenterX == 1 && base.constraintCenterZ == 1);
    CHECK(base.GetConstraintAtCell(0, -1) == 1 && base.GetConstraintAtCell(1, -1) == 2 && base.GetConstraintAtCell(-1, 0) == 3 &&
          base.GetConstraintAtCell(0, 0) == 7 && base.GetConstraintAtCell(1, 0) == 8);
    CHECK(base.IsWithinBounds(1, 1, 64, 64) && !base.IsWithinBounds(0, 1, 64, 64) && !base.IsWithinBounds(63, 5, 64, 64) &&
          !base.IsWithinBounds(5, 0, 64, 64));

    // ---- CopyFrom + Rotate 1: (x, z) -> (-z, x), the edge words turn left a byte, the bounds keep the original's slip ----
    const cIntRule& turned = rules[0x11];
    CHECK(turned.id == 0x11);
    CHECK(HasPoint(turned.newCells, 1, 1) && HasPoint(turned.occupiedCells, 0, -1) && HasPoint(turned.occupiedCells, 0, 1));
    const cIntRule::CheckType* turnedB = turned.FindCheckType('B');
    CHECK(turnedB && turnedB->checks[0].edges == 0x00020002u && turnedB->checks[1].edges == 0x02000200u);
    const cIntRule::CheckType* turnedA = turned.FindCheckType('A');
    CHECK(turnedA && turnedA->checks[0].mask == 0xff000000u);
    const cIntRule::CheckType* turnedRoad = turned.FindCheckType('a');   // road 0x00000200 (the east side) turns to the south side
    CHECK(turnedRoad && turnedRoad->checks[0].edges == 0x00020000u && turnedRoad->checks[0].mask == 0x00ff0000u);
    // minZ := minX (-1), minX := -maxZ (0), maxZ := maxX (1), maxX := -(old minX) = 1 (the quirk; -(old minZ) would be 1 too here)
    CHECK(turned.minX == 0 && turned.minZ == -1 && turned.maxZ == 1 && turned.maxX == 1);
    CHECK(turned.oneWayDir == 3);
    CHECK(turned.pieces[0].rotation == 2 && turned.pieces[0].x == 16.0f && turned.pieces[0].z == 8.0f);
    CHECK(turned.handleX == 0 && turned.handleZ == 1);
    const cIntRule::cTileDef* turnedPlus = turned.GetTileInfo(1, 1);
    CHECK(turnedPlus && turnedPlus->tile == 0x200 && turnedPlus->orientation == 1);
    // the constraint grid turns: the '-' above the centre becomes a '|' right of it
    CHECK(turned.GetConstraintAtCell(1, 0) == 2);

    // ---- CopyFrom + Transpose + Translate: x and z swap, edge bytes W<->N and E<->S with 1..3 -> 4 - value, the flip toggles ----
    const cIntRule& mirrored = rules[0x12];
    CHECK(HasPoint(mirrored.newCells, 0, 3));   // (1, -1) transposed (-1, 1), translated (+1, +2)
    const cIntRule::CheckType* mirroredB = mirrored.FindCheckType('B');
    CHECK(mirroredB && mirroredB->checks[0].edges == 0x00020002u && mirroredB->checks[1].edges == 0x02000200u);
    CHECK(mirrored.pieces[0].flip == 1 && mirrored.pieces[0].rotation == 2);
    CHECK(mirrored.oneWayDir == 5);
    CHECK(mirrored.minX == 0 && mirrored.maxX == 1 && mirrored.minZ == 1 && mirrored.maxZ == 3);

    // ---- the ordering rings: RotationRing 10, 11 (99 unknown, left out); AddTypes 12, 20 joins 12 behind 10 and 20 behind 11 ----
    CHECK(rules[0x10].rotationNext == 0x11 && rules[0x11].rotationNext == 0x13 && rules[0x13].rotationNext == 0x10 && rules[0x10].rotationPrev == 0x13);
    CHECK(rules[0x12].rotationNext == 0x20 && rules[0x20].rotationNext == 0x21 && rules[0x21].rotationNext == 0x12);
    CHECK(rules[0x10].typeNext == 0x12 && rules[0x12].typeNext == 0x10 && rules[0x12].typePrev == 0x10);
    CHECK(rules[0x11].typeNext == 0x20 && rules[0x20].typePrev == 0x11 && rules[0x13].typeNext == 0x21 && rules[0x21].typePrev == 0x13);

    // ---- Test and DoAutoComplete at (10, 10) over a world cache ----
    {
        auto* tool = new cSC4NetworkTool(kNetworkHighway);
        tool->AddRef();
        cSC4NetworkWorldCache cache;
        NetworkToolTestAccess::SetWorldCache(*tool, &cache);
        NetworkToolTestAccess::SetCity(*tool, 64);
        // A (10, 9) highway N-S edge; B (10, 10) road E-W + highway N-S; a (9, 10) road E; b (11, 10) road W.
        cSC4NetworkCellInfo* a = MakeCell(cache, 9, 10);
        a->networkFlags = 0x1;
        a->tileNeighborhoods[kNetworkRoad] = 0x00000200;
        cSC4NetworkCellInfo* checkA = MakeCell(cache, 10, 9);
        checkA->networkFlags = 0x4;
        checkA->tileNeighborhoods[kNetworkHighway] = 0x00020000;
        cSC4NetworkCellInfo* b = MakeCell(cache, 11, 10);
        b->networkFlags = 0x1;
        b->tileNeighborhoods[kNetworkRoad] = 0x02000000;
        cSC4NetworkCellInfo* crossing = MakeCell(cache, 10, 10);
        crossing->networkFlags = 0x5;
        crossing->tileNeighborhoods[kNetworkRoad] = 0x02000200;
        crossing->tileNeighborhoods[kNetworkHighway] = 0x00020002;
        CHECK(base.Test(10, 10, *tool) == cIntRule::kMatch);
        CHECK(host.demolished.size() == 1 && host.demolished[0].x == 11 && host.demolished[0].z == 9);
        // 'a' is multiok: another network type there is allowed; on 'b' it is not
        a->networkFlags = 0x41;
        CHECK(base.Test(10, 10, *tool) == cIntRule::kMatch);
        b->networkFlags = 0x41;
        CHECK(base.Test(10, 10, *tool) == cIntRule::kNoMatch);
        b->networkFlags = 0x1;
        // an edge outside the mask does not matter, one inside it fails hard
        checkA->tileNeighborhoods[kNetworkHighway] = 0x00020003;
        CHECK(base.Test(10, 10, *tool) == cIntRule::kMatch);
        checkA->tileNeighborhoods[kNetworkHighway] = 0x00030000;
        CHECK(base.Test(10, 10, *tool) == cIntRule::kFail);
        checkA->tileNeighborhoods[kNetworkHighway] = 0x00020000;
        // the crossing lacks the highway: no match
        crossing->networkFlags = 0x1;
        CHECK(base.Test(10, 10, *tool) == cIntRule::kNoMatch);
        crossing->networkFlags = 0x5;
        // a cell already part of an intersection: this rule replaces one, but only one marked in the cell info passes
        crossing->unknown5a = 1;
        CHECK(base.Test(10, 10, *tool) == cIntRule::kMatch);
        crossing->solvedOrImmovable = true;
        CHECK(base.Test(10, 10, *tool) == cIntRule::kMatch);   // fixed is fine while marked
        crossing->unknown5a = 0;
        CHECK(base.Test(10, 10, *tool) == cIntRule::kNoMatch);   // fixed and not marked
        crossing->solvedOrImmovable = false;
        // the new cell: water or a refused demolition reject it
        host.waterX = 11;
        host.waterZ = 9;
        CHECK(base.Test(10, 10, *tool) == cIntRule::kNoMatch);
        host.waterX = 1000;
        host.demolishAllowed = false;
        CHECK(base.Test(10, 10, *tool) == cIntRule::kNoMatch);
        host.demolishAllowed = true;
        // a tagged cell without a cell info fails hard; the optional 'C' (10, 11) is never looked at
        CHECK(base.Test(10, 10, *tool) == cIntRule::kMatch);
        CHECK(base.Test(20, 20, *tool) == cIntRule::kFail);

        // DoAutoComplete: the matching cells add nothing; the missing optional 'C' and a wrong edge on 'b' become draws
        NetworkToolTestAccess::Draws(*tool).clear();
        base.DoAutoComplete(10, 10, *tool);
        CHECK(NetworkToolTestAccess::Draws(*tool).size() == 1);
        if (NetworkToolTestAccess::Draws(*tool).size() == 1) {
            const tReservedCell& draw = NetworkToolTestAccess::Draws(*tool)[0];
            CHECK(draw.x == 10 && draw.z == 11 && draw.network == kNetworkHighway && draw.compact == 0x00000002u);
        }
        b->tileNeighborhoods[kNetworkRoad] = 0x03000000;
        NetworkToolTestAccess::Draws(*tool).clear();
        base.DoAutoComplete(10, 10, *tool);
        CHECK(NetworkToolTestAccess::Draws(*tool).size() == 2);
        if (NetworkToolTestAccess::Draws(*tool).size() == 2) {
            const tReservedCell& draw = NetworkToolTestAccess::Draws(*tool)[0];
            CHECK(draw.x == 11 && draw.z == 10 && draw.network == kNetworkRoad && draw.compact == 0x02000000u);
        }
        tool->AddEdgesToTile(1, 2, 0x03000000, kNetworkOneWayRoad, 0x40, 0xc0);
        CHECK(NetworkToolTestAccess::Draws(*tool).back().oneWayBits == 0x40 && NetworkToolTestAccess::Draws(*tool).back().oneWayMask == 0xc0);

        NetworkToolTestAccess::SetWorldCache(*tool, nullptr);
        tool->Release();
    }

    // ---- InsertIntersectionConstraints: the codes of the rule's occupied cells ('#' at the crossing, 'X' at 'b') ----
    {
        nSC4Networks::IntersectionRuleMap& live = const_cast<nSC4Networks::IntersectionRuleMap&>(cSC4NetworkTool::IntersectionRules());
        live = rules;
        auto* tool = new cSC4NetworkTool(kNetworkHighway);
        tool->AddRef();
        cSC4NetworkWorldCache cache;
        NetworkToolTestAccess::SetWorldCache(*tool, &cache);
        NetworkToolTestAccess::SetCity(*tool, 64);
        for (std::uint32_t x = 8; x <= 12; ++x)
            for (std::uint32_t z = 8; z <= 12; ++z) MakeCell(cache, x, z);
        tIntersection record;
        record.ruleId = 0x10;
        record.x = 10;
        record.z = 10;
        NetworkToolTestAccess::Intersections(*tool).push_back(record);
        RecordingSatisfier satisfier;
        tool->InsertIntersectionConstraints(satisfier);
        // occupied cells in list order: '+' (11, 9) '|'; 'a' (9, 10) '/'; 'B' (10, 10) '#'; 'b' (11, 10) 'X'. Vertex ids z * 65 + x.
        const std::vector<std::string> expected = {
            "eq 596 597", "eq 661 662", "slope 596 661 1000", "slope 597 662 1000",      // '|' at (11, 9): v0 = 596
            "eq 659 725", "slope 724 660 707",                                          // '/' at (9, 10): v0 = 659
            "eq 660 725", "eq 660 726", "eq 660 661",                                    // '#' at (10, 10): v0 = 660
            "slope 661 726 1000", "slope 661 662 1000", "slope 726 727 1000", "slope 662 727 1000",   // 'X' at (11, 10)
        };
        CHECK(satisfier.calls == expected);
        if (satisfier.calls != expected)
            for (const std::string& call : satisfier.calls) std::printf("  call: %s\n", call.c_str());
        // the marks and the solve-set removal walk the records inside the valid steps
        NetworkToolTestAccess::ValidSteps(*tool) = 1;
        tool->MarkHighwayIntersectionPieces(true);
        CHECK(cache.GetCell(10 + (10 << 16))->unknown5a == 1 && cache.GetCell(11 + (9 << 16))->unknown5a == 1 &&
              cache.GetCell(10 + (9 << 16))->unknown5a == 0);
        std::unordered_set<std::uint32_t> solve = {10 + (10 << 16), 10 + (9 << 16)};
        tool->RemoveHighwayIntersectionPiecesFromSetOfCellsToBeSolved(solve);
        CHECK(solve.size() == 1 && solve.count(10 + (9 << 16)) == 1);
        CHECK(NetworkToolTestAccess::Intersections(*tool)[0].cells.size() == 4);
        live.clear();
        NetworkToolTestAccess::SetWorldCache(*tool, nullptr);
        tool->Release();
    }

    std::remove(path.c_str());
    if (failures == 0) std::printf("sc4_networkintersections: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
