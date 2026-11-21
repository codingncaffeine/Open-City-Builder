// The parts of a bridge beyond its deck pieces: the suspension wire heights (PlotBridgeSuspensionWires), the cable stretches
// (PlotBridgeCable / PlotBridgeCables), the support column and wire tiles added to an occupant (AddSupportColumnToOccupant,
// AddSuspendedBridgeStructures), the specialization script (ParseNetworkBridgeSpecializations) and a specialized layout
// (ConstructSpecializedBridge).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "sc4/NetworkOccupant.h"
#include "sc4/NetworkTool.h"
#include "sc4/ResourceAsFile.h"

static int failures = 0;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);           \
            ++failures;                                                \
        }                                                              \
    } while (0)

class cIGZPersistResourceManager;
namespace SL {
cIGZPersistResourceManager* spRM = nullptr;   // the bridge type loader's resource manager (none here)
}
namespace sc4 {
bool SC4GetResourceAsFile(const cGZPersistResourceKey&, cIGZFile** out, bool) {
    *out = nullptr;
    return false;
}
}  // namespace sc4

using namespace sc4;

namespace sc4 {
struct NetworkToolTestAccess {
    // A straight drag along +x on row z, one cell per step (the pen direction of a +x drag is 4).
    static void SetDrag(cSC4NetworkTool& t, std::uint32_t z, std::uint32_t x0, int count) {
        t.cells_.clear();
        t.steps_.clear();
        for (int i = 0; i < count; ++i) {
            t.cells_.push_back(SC4PointU{x0 + static_cast<std::uint32_t>(i), z});
            tDraggedStep step;
            step.firstCell = step.lastCell = static_cast<std::uint32_t>(i);
            t.steps_.push_back(step);
        }
        t.totalSteps_ = t.validSteps_ = t.straightSteps_ = count;
        t.directionA_ = 4;
        t.widthPlusOne_ = 65;
        t.cellSize_ = 16.0f;
    }
};
}  // namespace sc4

namespace {

bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

// The catenary the wires follow: sag at the low point, 1 at the tower.
float Catenary(float t, float sag) {
    return static_cast<float>((std::exp(static_cast<double>(t)) + std::exp(static_cast<double>(-t)) - 2.0) / 1.0861612696304874) *
               (1.0f - sag) + sag;
}

std::vector<tBridgePiece> Row(std::uint32_t x0, std::uint32_t z, int count) {
    std::vector<tBridgePiece> pieces(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        pieces[static_cast<std::size_t>(i)].x = x0 + static_cast<std::uint32_t>(i);
        pieces[static_cast<std::size_t>(i)].z = z;
        for (float& w : pieces[static_cast<std::size_t>(i)].wire) w = 1.0f;
    }
    return pieces;
}

void TestWires() {
    // Between two towers at cells 10 and 20 (+x): the low point is in the middle of the span, the towers half a cell in.
    std::vector<tBridgePiece> pieces = Row(10, 5, 11);
    cSC4NetworkTool::PlotBridgeSuspensionWires(pieces.data(), 11, 1, 0.25f, 1);
    // low = (10 + 21) / 2 = 15.5, tower = 21 - 0.5 = 20.5; corner 0 of cell 15 is x = 15 -> t = -0.1
    CHECK(Near(pieces[5].wire[0], Catenary(-0.1f, 0.25f)));
    CHECK(Near(pieces[5].wire[2], Catenary(0.1f, 0.25f)));       // corner 2 at x = 16
    CHECK(Near(pieces[1].wire[0], Catenary(-0.9f, 0.25f)));
    CHECK(Near(pieces[0].wire[0], 1.0f));                        // past the tower the catenary is above 1: kept
    CHECK(pieces[5].wire[0] < 0.27f && pieces[0].wire[0] > 0.99f);
    // Mode 0, the start end span: the low point at the start.
    std::vector<tBridgePiece> start = Row(0, 5, 6);
    cSC4NetworkTool::PlotBridgeSuspensionWires(start.data(), 6, 1, 0.5f, 0);
    CHECK(Near(start[0].wire[0], 0.5f));                         // t = 0
    CHECK(Near(start[4].wire[2], Catenary(5.0f / 5.5f, 0.5f)));  // the tower at 6 - 0.5
    // Mode 2, the far end span: the low point at the far end, the tower half a cell past the first cell.
    std::vector<tBridgePiece> end = Row(0, 5, 6);
    cSC4NetworkTool::PlotBridgeSuspensionWires(end.data(), 6, 1, 0.5f, 2);
    CHECK(Near(end[5].wire[2], 0.5f));                           // x = 6, the far end
    CHECK(Near(end[1].wire[0], Catenary(-5.0f / -5.5f, 0.5f))); // the tower at 0.5
    // Wires only ever go down.
    std::vector<tBridgePiece> low = Row(10, 5, 11);
    for (tBridgePiece& p : low) for (float& w : p.wire) w = 0.1f;
    cSC4NetworkTool::PlotBridgeSuspensionWires(low.data(), 11, 1, 0.25f, 1);
    CHECK(Near(low[0].wire[0], 0.1f) && Near(low[5].wire[0], 0.1f));
}

void TestCables() {
    // A cable along +x from local x 8 to 40 (cells 0..2 of a bridge starting at cell 10, row 5): cut at x 176 and 192.
    std::vector<tBridgePiece> pieces = Row(10, 5, 4);
    const float from[3] = {8.0f, 100.0f, 8.0f}, to[3] = {40.0f, 132.0f, 8.0f};
    cSC4NetworkTool::PlotBridgeCable(pieces.data(), 4, 1, from, to, 0x1234u, 0.5f);
    CHECK(pieces[0].cables.size() == 1 && pieces[1].cables.size() == 1 && pieces[2].cables.size() == 1);
    CHECK(pieces[3].cables.empty());
    if (pieces[1].cables.size() == 1) {
        const tBridgeCablePiece& c = pieces[1].cables[0];
        CHECK(Near(c.from[0], 176.0f) && Near(c.to[0], 192.0f));
        CHECK(Near(c.from[1], 108.0f) && Near(c.to[1], 124.0f));
        CHECK(Near(c.from[2], 88.0f));                            // z = 5 * 16 + 8
        CHECK(c.texture == 0x1234u);
        CHECK(Near(c.halfWidth, std::sqrt(2.0f) * 0.5f));         // rises 1 in 1: sqrt(2) times thicker
    }
    // A vertical cable (no ground length) is not drawn.
    std::vector<tBridgePiece> none = Row(10, 5, 2);
    const float up[3] = {8.0f, 200.0f, 8.0f};
    cSC4NetworkTool::PlotBridgeCable(none.data(), 2, 1, from, up, 1u, 0.5f);
    CHECK(none[0].cables.empty());
    // A fan of three: cable i from a + i (b - a) / 2 to c + i (d - c) / 2.
    std::vector<tBridgePiece> fan = Row(10, 5, 4);
    const float a[3] = {8, 100, 8}, b[3] = {24, 100, 8}, c[3] = {56, 140, 8}, d[3] = {56, 120, 8};
    cSC4NetworkTool::PlotBridgeCables(fan.data(), 4, 1, a, b, c, d, 3, 7u, 0.25f);
    std::size_t stretches = 0;
    for (const tBridgePiece& p : fan) stretches += p.cables.size();
    // world x 168 / 176 / 184 to 216: four, four (a zero-length cut where it starts on a cell edge) and three stretches
    CHECK(stretches == 11);
    // One cable divides by zero in the original and lands nowhere.
    std::vector<tBridgePiece> one = Row(10, 5, 4);
    cSC4NetworkTool::PlotBridgeCables(one.data(), 4, 1, a, b, c, d, 1, 7u, 0.25f);
    for (const tBridgePiece& p : one) CHECK(p.cables.empty());
}

void TestColumnAndWireTiles() {
    cSC4NetworkOccupant occupant;
    float top[4][3], bottom[4][3];
    const float fp[4][2] = {{-4, -4}, {-4, 4}, {4, 4}, {4, -4}};
    for (int k = 0; k != 4; ++k) {
        top[k][0] = bottom[k][0] = 100.0f + fp[k][0];
        top[k][2] = bottom[k][2] = 200.0f + fp[k][1];
        top[k][1] = 64.0f;
        bottom[k][1] = 16.0f;
    }
    cSC4NetworkTool::AddSupportColumnToOccupant(&occupant, top, bottom, false);
    const std::vector<SC4AddOnPart>& parts = occupant.AddOnParts();
    CHECK(parts.size() == 10);
    int sides = 0, caps = 0;
    for (const SC4AddOnPart& p : parts) {
        if (p.id == 0x8080000u) ++sides;
        if (p.id == 0x912220eu) ++caps;
    }
    CHECK(sides == 8 && caps == 2);
    if (parts.size() == 10) {
        // the first side triangle: top 0, bottom 0, bottom 1; v = height / 16
        CHECK(Near(parts[0].corner[0].y, 64.0f) && Near(parts[0].corner[1].y, 16.0f) && Near(parts[0].corner[2].y, 16.0f));
        CHECK(Near(parts[0].corner[0].v, 4.0f) && Near(parts[0].corner[1].v, 1.0f) && Near(parts[0].corner[2].u, 1.0f));
        CHECK(Near(parts[0].corner[2].x, 96.0f) && Near(parts[0].corner[2].z, 204.0f));
        // the cap lies on the top ring
        CHECK(Near(parts[8].corner[0].y, 64.0f) && Near(parts[9].corner[2].y, 64.0f));
    }

    // The wire tiles of a span piece: both edges (flags 1), each a top tile of the wire texture and hanger tiles down to the
    // deck; four faces (two each side) per tile.
    cSC4NetworkTool tool(0);
    NetworkToolTestAccess::SetDrag(tool, 5, 10, 4);
    cSC4NetworkCellInfo cell;
    const std::int32_t row = 65;
    cell.vertexBaseHeights[0] = 5 * row + 12;      // corners (12,5) (12,6) (13,6) (13,5)
    cell.vertexBaseHeights[1] = 6 * row + 12;
    cell.vertexBaseHeights[2] = 6 * row + 13;
    cell.vertexBaseHeights[3] = 5 * row + 13;
    tBridgePiece piece;
    piece.suspension = 1;
    piece.wireValue = 40.0f;
    for (float& w : piece.wire) w = 1.0f;
    piece.wire[0] = piece.wire[1] = 0.5f;           // 20 above the deck on corners 0 and 1
    for (float& h : piece.corners) h = 100.0f;
    cSC4NetworkBridgeTypeInfo type;
    type.suspensionTopTexture = 0xaaa;
    type.suspensionHangerTexture = 0xbbb;
    type.suspensionTileHeight = 16.0f;
    type.suspensionDeckOffset = 2.0f;
    type.suspensionInset = 1.0f;
    cSC4NetworkOccupant wires;
    tool.AddSuspendedBridgeStructures(&wires, piece, &cell, type);
    std::map<std::uint32_t, int> counts;
    for (const SC4AddOnPart& p : wires.AddOnParts()) ++counts[p.id];
    // edge 0-1 (heights 20 / 20): a top tile and one hanger tile (16 + 4); edge 2-3 (40 / 40): a top tile and two (16 + 16 + 8)
    CHECK(counts[0xaaa] == 8);
    CHECK(counts[0xbbb] == 4 + 8);
    if (!wires.AddOnParts().empty()) {
        const SC4AddOnPart& first = wires.AddOnParts().front();
        // corner 0's edge moves 1 towards corner 3 (+x): x = 12 * 16 + 1; the top at deck + offset + wire
        CHECK(Near(first.corner[0].x, 193.0f) && Near(first.corner[0].z, 80.0f));
        CHECK(Near(first.corner[0].y, 122.0f));
        CHECK(Near(first.corner[2].y, 106.0f) && Near(first.corner[2].v, 1.0f));
    }
    // A wide deck's inner edge (flag 0x10) is left out.
    cSC4NetworkOccupant outer;
    piece.suspension = 1 | 0x10;
    tool.AddSuspendedBridgeStructures(&outer, piece, &cell, type);
    std::map<std::uint32_t, int> outerCounts;
    for (const SC4AddOnPart& p : outer.AddOnParts()) ++outerCounts[p.id];
    CHECK(outerCounts[0xaaa] == 4 && outerCounts[0xbbb] == 8);
}

const char kScript[] =
    "# a test script\n"
    "section arch\n"
    "  height 2\n"
    "  piece <0x100, 1, 3> <0x101, 0, -3.0, 1.5>: 7.5, pylon\n"
    "end\n"
    "bridge 5\n"
    "  section ramp\n"
    "    piece 2 * <0x200, 0, 4>: end\n"
    "  end\n"
    "  length 6\n"
    "    piece ramp arch rev(ramp)\n"
    "  end\n"
    "  length 7\n"
    "    piece length(6) <0x300, 2>\n"
    "  end\n"
    "  length 2\n"
    "    piece ramp\n"
    "  end\n"
    "end\n"
    "bridge 9\n"
    "  length 3\n"
    "    piece 3 * <1, 0>\n"
    "  end\n"
    "end\n"
    "stop\n"
    "bridge 5\n";

void TestScript() {
    std::vector<cSC4NetworkBridgeTypeInfo> types(1);
    types[0].id = 5;
    types[0].minSpan = 4;
    types[0].maxSpan = 10;
    const bool ok = ParseNetworkBridgeSpecializations(kScript, static_cast<long>(std::strlen(kScript)), types, 1);
    // "length 2" is below the type's minimum span: the parse stops there and keeps what it read.
    CHECK(!ok);
    const auto& specs = types[0].specializations;
    CHECK(specs.size() == 2 && specs.count(6) == 1 && specs.count(7) == 1);
    if (specs.count(6) == 0) return;
    const cSC4NetworkBridgeSpecialization& six = specs.at(6);
    CHECK(six.pieces.size() == 6 && six.heights.size() == 7);
    if (six.pieces.size() != 6 || six.heights.size() != 7) return;
    // ramp: two pieces rising 4 each; arch (its height 2 is relative to its own start): up 3, down 3; rev(ramp): down 4 twice
    const float heights[7] = {0, 4, 8, 11, 8, 4, 0};
    for (int i = 0; i < 7; ++i) CHECK(Near(six.heights[static_cast<std::size_t>(i)], heights[i]));
    CHECK(six.pieces[0].model == 0x200 && six.pieces[0].attribute == 2 && Near(six.pieces[0].height, 0.0f));
    CHECK(six.pieces[2].model == 0x100 && six.pieces[2].rotation == 1 && Near(six.pieces[2].height, 8.0f));
    CHECK(six.pieces[3].model == 0x101 && six.pieces[3].attribute == 1 && Near(six.pieces[3].supportHeight, 7.5f));
    CHECK(Near(six.pieces[3].height, 12.5f));
    CHECK(six.pieces[4].model == 0x200 && six.pieces[4].rotation == 2 && Near(six.pieces[4].height, 4.0f));
    CHECK(six.pieces[5].rotation == 2 && Near(six.pieces[5].height, 0.0f));
    CHECK(Near(six.pieces[0].supportHeight, -1.0f));
    const cSC4NetworkBridgeSpecialization& seven = specs.at(7);
    CHECK(seven.pieces.size() == 7 && seven.heights.size() == 8);
    if (seven.pieces.size() == 7) CHECK(seven.pieces[6].model == 0x300 && seven.pieces[6].rotation == 2 && Near(seven.pieces[6].height, 0.0f));

    // Two cells per step: a step closes every second piece.
    std::vector<cSC4NetworkBridgeTypeInfo> wide(1);
    wide[0].id = 1;
    wide[0].minSpan = 1;
    wide[0].maxSpan = 10;
    const char kWide[] = "bridge 1\nlength 2\npiece 4 * <7, 0, 2.5>\nend\nend\n";
    CHECK(ParseNetworkBridgeSpecializations(kWide, static_cast<long>(std::strlen(kWide)), wide, 2));
    if (wide[0].specializations.count(2)) {
        const cSC4NetworkBridgeSpecialization& w = wide[0].specializations.at(2);
        CHECK(w.pieces.size() == 4 && w.heights.size() == 3);
        if (w.heights.size() == 3) CHECK(Near(w.heights[2], 5.0f));
    }
}

}  // namespace

int main() {
    TestWires();
    TestCables();
    TestColumnAndWireTiles();
    TestScript();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("sc4_networkbridgeparts: all passed\n");
    return 0;
}
