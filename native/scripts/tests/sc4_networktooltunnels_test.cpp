// cSC4NetworkTool's tunnels: IdentifyTunnelCells / FindTunnelEnd over a drag across a hill (the trigger rise, the end within
// the clearance, the end slope, water, the straight part), RemoveTunnelPiecesFromSetOfCellsToBeSolved and
// CopyTunnelElevationConstraintsIntoCellInfo. The terrain is a fake host whose vertex heights depend on the vertex column.
#include <cstdio>
#include <cstring>
#include <functional>
#include <unordered_set>
#include <vector>

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

namespace sc4 {
// The private state the tunnel pass reads (the friend declared in NetworkTool.h).
struct NetworkToolTestAccess {
    // A straight drag along +x on row z: one cell per step, cells x0 .. x0 + count - 1. The pen direction of a +x drag is 4
    // (the step's trailing edge is its -x side), the vertex offset tables as Init writes them for a city `width` cells wide.
    static void SetDrag(cSC4NetworkTool& t, int width, std::uint32_t z, std::uint32_t x0, int count, int straight) {
        t.cells_.clear();
        t.steps_.clear();
        for (int i = 0; i < count; ++i) {
            t.cells_.push_back(SC4PointU{x0 + static_cast<std::uint32_t>(i), z});
            tDraggedStep step;
            step.firstCell = step.lastCell = static_cast<std::uint32_t>(i);
            t.steps_.push_back(step);
        }
        t.totalSteps_ = t.validSteps_ = count;
        t.straightSteps_ = straight;
        t.directionA_ = 4;
        t.cityWidth_ = t.cityHeight_ = width;
        t.widthPlusOne_ = t.heightPlusOne_ = width + 1;
        const std::int32_t w = width;
        const std::int32_t offsets[16] = {w + 2, w + 1, w + 1, 0, 0, 1, 1, w + 2, 1, 1, w + 2, w + 2, w + 1, w + 1, 0, 0};
        std::memcpy(t.initScratch_, offsets, sizeof offsets);
        t.cellSize_ = 16.0f;
    }
    static std::vector<tDraggedStep>& Steps(cSC4NetworkTool& t) { return t.steps_; }
    static std::vector<tTunnel>& Tunnels(cSC4NetworkTool& t) { return t.tunnels_; }
    static cSC4NetworkPlacementParams& Placement(cSC4NetworkTool& t) { return t.placement_; }
    static float& SlopeRise(cSC4NetworkTool& t) { return t.slopeRise_; }
    static void SetWorldCache(cSC4NetworkTool& t, INetworkWorldCache* cache) { t.worldCache_ = cache; }
};
}  // namespace sc4

namespace {

constexpr int kWidth = 64;
constexpr float kPi = 3.14159265f;

// Every pure host call the tunnel pass does not make answers neutrally.
struct TerrainHost : INetworkToolHost {
    std::function<float(int)> heightAtColumn;   // vertex column -> height
    std::unordered_set<std::uint32_t> water;    // cells (x | z << 16) with water
    void RegisterShutdownCallback(bool (*)(void*), void*) override {}
    cIGZUnknown* AcquireTerrain() override { return nullptr; }
    float TerrainCellSize() override { return 16.0f; }
    float TerrainVertexHeight(std::int32_t vertex) override { return heightAtColumn(vertex % (kWidth + 1)); }
    bool TerrainAllowsNetwork(std::uint32_t x, std::uint32_t z) override { return water.count(x | (z << 16)) != 0; }
    int CityWidth() override { return kWidth; }
    int CityHeight() override { return kWidth; }
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
};

// 100 m up to column 10, a 20 m per cell climb to 300 m at 20, flat to 30, a 20 m per cell descent back to 100 m at 40.
float Hill(int column, float climbPerCell) {
    if (column <= 10) return 100.0f;
    if (column <= 20) return 100.0f + climbPerCell * static_cast<float>(column - 10);
    if (column <= 30) return 100.0f + climbPerCell * 10.0f;
    if (column <= 40) return 100.0f + climbPerCell * static_cast<float>(40 - column);
    return 100.0f;
}

// A road-like tool: trigger 35 degrees (11.2 m per 16 m cell), end slope 40 degrees, clearance 13.8 m.
cSC4NetworkTool* MakeTool(int straight, int count = 50) {
    auto* tool = new cSC4NetworkTool(0);
    tool->AddRef();
    NetworkToolTestAccess::SetDrag(*tool, kWidth, 5, 0, count, straight);
    cSC4NetworkPlacementParams& p = NetworkToolTestAccess::Placement(*tool);
    p.maxSlope = 35.0f * kPi / 180.0f;
    const float endSlope = 40.0f * kPi / 180.0f;
    std::memcpy(&p.unknown1c, &endSlope, sizeof endSlope);
    p.list60 = {0x0ad00000};
    p.list78 = {0};
    p.list90 = {13.8f};
    NetworkToolTestAccess::SlopeRise(*tool) = 11.203f;
    return tool;
}

int KindAt(cSC4NetworkTool& tool, int step) { return static_cast<int>(NetworkToolTestAccess::Steps(tool)[static_cast<std::size_t>(step)].direction); }

}  // namespace

int main() {
    TerrainHost host;
    SetNetworkToolHost(&host);

    // ---- a steep hill: the tunnel runs from the step before the first steep rise to the step where the terrain is back ----
    {
        host.heightAtColumn = [](int c) { return Hill(c, 20.0f); };
        cSC4NetworkTool* tool = MakeTool(50);
        int steps = 50;
        tool->IdentifyTunnelCells(steps);
        CHECK(steps == 50);   // the count is never cut
        const std::vector<tTunnel>& tunnels = NetworkToolTestAccess::Tunnels(*tool);
        CHECK(tunnels.size() == 1);
        if (tunnels.size() == 1) {
            // the rise from step 10 (trailing edge column 10, 100 m) to step 11 (column 11, 120 m) is 20 m > 11.2 m
            CHECK(tunnels[0].startStep == 10);
            CHECK(tunnels[0].startHeight == 100.0f);
            // far edges: step 39's +x edge is column 40, 100 m, the height nearest the start; the walk stops there
            CHECK(tunnels[0].endStep == 39);
            CHECK(tunnels[0].endHeight == 100.0f);
        }
        CHECK(KindAt(*tool, 9) == 0 && KindAt(*tool, 10) == 2 && KindAt(*tool, 39) == 2 && KindAt(*tool, 40) == 0);
        bool interior = true;
        for (int s = 11; s < 39; ++s) interior = interior && KindAt(*tool, s) == 3;
        CHECK(interior);

        // the set to solve loses every cell of the tunnel's steps, portals included
        std::unordered_set<std::uint32_t> set;
        for (std::uint32_t x = 0; x < 50; ++x) set.insert(x | (5u << 16));
        tool->RemoveTunnelPiecesFromSetOfCellsToBeSolved(set);
        CHECK(set.size() == 20);
        CHECK(set.count(9u | (5u << 16)) == 1 && set.count(10u | (5u << 16)) == 0 && set.count(39u | (5u << 16)) == 0 &&
              set.count(40u | (5u << 16)) == 1);

        // the portal cells are pinned at the portal heights, windows collapsed; one portion per tunnel
        cSC4NetworkWorldCache cache;
        NetworkToolTestAccess::SetWorldCache(*tool, &cache);
        for (std::uint32_t x : {10u, 20u, 39u}) {
            cSC4NetworkCellInfo* cell = cache.CreateCell(x | (5u << 16));
            for (int i = 0; i < 4; ++i) {
                cell->vertexHeights[i] = 250.0f;
                cell->range70[i] = {200.0f, 300.0f};
            }
        }
        std::vector<tImmovablePortion> portions;
        tool->CopyTunnelElevationConstraintsIntoCellInfo(portions);
        CHECK(portions.size() == 1);
        if (portions.size() == 1) CHECK(portions[0].x == 10 && portions[0].step == 39 && portions[0].valueA == 100.0f && portions[0].valueB == 100.0f);
        const cSC4NetworkCellInfo* start = cache.GetCell(10u | (5u << 16));
        const cSC4NetworkCellInfo* middle = cache.GetCell(20u | (5u << 16));
        CHECK(start && start->vertexHeights[0] == 100.0f && start->vertexHeights[3] == 100.0f && start->range70[2].low == 100.0f &&
              start->range70[2].high == 100.0f);
        CHECK(middle && middle->vertexHeights[0] == 250.0f && middle->range70[0].low == 200.0f);   // the interior is left alone
        NetworkToolTestAccess::SetWorldCache(*tool, nullptr);
        tool->Release();
    }

    // ---- control: a climb of 10 m per cell stays under the 11.2 m trigger, so no tunnel --------------------------------
    {
        host.heightAtColumn = [](int c) { return Hill(c, 10.0f); };
        cSC4NetworkTool* tool = MakeTool(50);
        int steps = 50;
        tool->IdentifyTunnelCells(steps);
        CHECK(NetworkToolTestAccess::Tunnels(*tool).empty());
        CHECK(KindAt(*tool, 10) == 0 && KindAt(*tool, 20) == 0);
        tool->Release();
    }

    // ---- water under the hill ends the search: no tunnel -------------------------------------------------------------
    {
        host.heightAtColumn = [](int c) { return Hill(c, 20.0f); };
        host.water = {25u | (5u << 16)};
        cSC4NetworkTool* tool = MakeTool(50);
        int steps = 50;
        tool->IdentifyTunnelCells(steps);
        CHECK(NetworkToolTestAccess::Tunnels(*tool).empty());
        host.water.clear();
        tool->Release();
    }

    // ---- the end must lie on the straight part of the drag ----------------------------------------------------------------
    {
        cSC4NetworkTool* tool = MakeTool(30);
        int steps = 50;
        tool->IdentifyTunnelCells(steps);
        CHECK(NetworkToolTestAccess::Tunnels(*tool).empty());
        tool->Release();
    }

    // ---- the far side must come back within the end slope: a far side 300 m lower over 29 cells (above tan 40 x 16 x 29)
    // still ends the walk (below the clearance) but its nearest height is too far from the start -------------------------------
    {
        host.heightAtColumn = [](int c) { return c <= 10 ? 100.0f : (c <= 20 ? 100.0f + 20.0f * static_cast<float>(c - 10) : (c <= 30 ? 300.0f : -400.0f)); };
        cSC4NetworkTool* tool = MakeTool(50);
        int steps = 50;
        tool->IdentifyTunnelCells(steps);
        CHECK(NetworkToolTestAccess::Tunnels(*tool).empty());
        tool->Release();
    }

    // ---- a drag of three steps or fewer is never searched -------------------------------------------------------------------
    {
        host.heightAtColumn = [](int c) { return 100.0f + 50.0f * static_cast<float>(c); };
        cSC4NetworkTool* tool = MakeTool(3, 3);
        int steps = 3;
        tool->IdentifyTunnelCells(steps);
        CHECK(NetworkToolTestAccess::Tunnels(*tool).empty());
        tool->Release();
    }

    SetNetworkToolHost(nullptr);
    if (failures == 0) std::printf("sc4_networktooltunnels: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
