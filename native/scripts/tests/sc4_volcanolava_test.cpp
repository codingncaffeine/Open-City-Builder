// cSTEVolcanoLava against a fake terrain and city: the flow (start rect, production, flow to the lowest neighbor, rim
// loss, deposit / evaporation, the terrain refresh cadence), the surface it prepares for display, the steam, the scorch
// decals, the fires, and what the drawing asks of it.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <set>
#include <utility>

#include "sc4/VolcanoLava.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {
struct Host final : ILavaHost {
    explicit Host(int vertsPerRow = 8) : n(vertsPerRow), ground(static_cast<std::size_t>(vertsPerRow * vertsPerRow), 0.0f),
                                        waterLevel(ground.size(), 0.0f), waterVertex(ground.size(), false) {
        const std::int32_t o[8] = {1, n + 1, n, n - 1, -1, -n - 1, -n, -n + 1};   // E, SE, S, SW, W, NW, N, NE
        std::copy(o, o + 8, offsets);
    }
    int n;
    std::uint8_t neighbors = 0xff;
    std::int32_t offsets[8];
    std::vector<float> ground, waterLevel;
    std::vector<bool> waterVertex;
    bool water = false;                       // every vertex is a water vertex
    bool raiseGround = false;                 // RaiseVertex moves the ground, as the terrain does
    int raised = 0, padded = 0, changed = 0, periodic = 0, refreshed = 0;
    float raisedAmount = 0;
    std::uint32_t raisedIndex = 0;
    LavaRect lastChanged;
    cSCPropertyHolder tuning;

    struct Steam { std::int32_t key = 0; float xyz[3] = {0, 0, 0}; bool stopped = false, hard = false, released = false; };
    std::deque<Steam> steams;
    bool effects = true;
    struct Decal { float xyz[3] = {0, 0, 0}; float size = 0, fadeIn = 0, fadeOut = 0; int action = -1; };
    std::deque<Decal> decals;
    std::set<std::pair<int, int>> fireproof;
    std::vector<std::pair<int, int>> asked, ignited, quickBurned;
    int causes = 0;
    std::uint32_t lastCause = 0;
    std::int32_t gradientLastRow = 63;
    std::uint32_t frames[kLavaZoomCount] = {4, 4, 4, 4, 4};
    std::int32_t zoom = 2;
    bool rendererOk = true, ticksOk = true;
    int registered = 0, unregistered = 0, tickStarts = 0, tickStops = 0, listens = 0, unlistens = 0;

    std::int32_t CityCellCountX() override { return n - 1; }
    std::int32_t CityCellCountZ() override { return n - 1; }
    float CellWidth() override { return 16.0f; }
    std::uint8_t GetNeighbors(std::int32_t, std::int32_t) override { return neighbors; }
    std::int32_t NeighborOffset(int d) override { return offsets[d]; }
    std::int32_t TerrainStride() override { return n; }
    std::int32_t TerrainMaxX() override { return n - 1; }
    std::int32_t TerrainMaxZ() override { return n - 1; }
    float TerrainAltitude(std::int32_t i) override { return ground[static_cast<std::size_t>(i)]; }
    float WaterLevel(std::int32_t i) override { return waterLevel[static_cast<std::size_t>(i)]; }
    bool IsWaterVertex(std::int32_t i) override { return water || waterVertex[static_cast<std::size_t>(i)]; }
    void RaiseVertex(std::uint32_t i, float a) override {
        ++raised; raisedIndex = i; raisedAmount = a;
        if (raiseGround) ground[i] += a;
    }
    void PadRectangle(LavaRect& r, int by) override {
        ++padded;                                                          // the real terrain clamps the padded rectangle to its grid
        r.left = std::max(0, r.left - by); r.top = std::max(0, r.top - by); r.right = std::min(n - 1, r.right + by); r.bottom = std::min(n - 1, r.bottom + by);
    }
    void CreateVertRectAroundVertex(LavaRect& r, int x, int z, int radius) override {   // cut to the grid, as the terrain does
        r = {std::max(0, x - radius), std::max(0, z - radius), std::min(n - 1, x + radius), std::min(n - 1, z + radius)};
    }
    void TerrainRegionChanged(const LavaRect& r) override { ++changed; lastChanged = r; }
    void TerrainPeriodicRefresh() override { ++periodic; }
    void TerrainRefresh() override { ++refreshed; }
    const cISCPropertyHolder* LoadTuningExemplar() override { return &tuning; }
    void* StartSteam(std::int32_t key, const float xyz[3]) override {
        if (!effects) return nullptr;
        steams.push_back(Steam{key, {xyz[0], xyz[1], xyz[2]}});
        return &steams.back();
    }
    void StopSteam(void* steam, bool hard) override { static_cast<Steam*>(steam)->stopped = true; static_cast<Steam*>(steam)->hard = hard; }
    void ReleaseSteam(void* steam) override { static_cast<Steam*>(steam)->released = true; }
    void* AddScorchDecal(const float xyz[3], float size, float fadeIn) override {
        decals.push_back(Decal{{xyz[0], xyz[1], xyz[2]}, size, fadeIn});
        return &decals.back();
    }
    void RetireScorchDecal(void* decal, float fadeOut, int action) override { static_cast<Decal*>(decal)->fadeOut = fadeOut; static_cast<Decal*>(decal)->action = action; }
    bool IsCellFlammable(std::int32_t x, std::int32_t z) override { asked.push_back({x, z}); return fireproof.count({x, z}) == 0; }
    void IgniteCell(std::int32_t x, std::int32_t z) override { ignited.push_back({x, z}); }
    void QuickBurnCell(std::int32_t x, std::int32_t z) override { quickBurned.push_back({x, z}); }
    void SetFireCause(std::uint32_t cause) override { ++causes; lastCause = cause; }
    std::int32_t ColorGradientLastRow() override { return gradientLastRow; }
    std::uint32_t ColorGradientPixel(std::int32_t row) override { return 0xff102030u + static_cast<std::uint32_t>(row); }
    std::uint32_t SurfaceFrameCount(std::int32_t z) override { return z >= 0 && z < kLavaZoomCount ? frames[z] : 0; }
    std::int32_t CurrentZoom() override { return zoom; }
    bool RegisterWithRenderer() override { ++registered; return rendererOk; }
    void UnregisterFromRenderer() override { ++unregistered; }
    bool StartTicks() override { ++tickStarts; return ticksOk; }
    void StopTicks() override { ++tickStops; }
    void ListenForMessages(bool listen) override { ++(listen ? listens : unlistens); }
    void ClearFires() { asked.clear(); ignited.clear(); quickBurned.clear(); causes = 0; lastCause = 0; }
};

bool Near(float a, float b, float tolerance = 1e-5f) { return std::fabs(a - b) < tolerance; }
void DefaultTuning() { cSTEVolcanoLava::TuningValues() = cSTEVolcanoLava::Tuning{}; }
}  // namespace

int main() {
    // ============================================================================================ the flow core
    // (no gradient image: the surface is not prepared, so the active rectangle stays as the flow left it)
    Host host;
    host.gradientLastRow = -1;
    cSTEVolcanoLava lava(&host);
    lava.SetGrid(8, 8, 16.0f);
    CHECK(lava.GetLavaHeightByCityCell(1, 1) == 0.0f && lava.GetLavaHeightByCityCell(-1, 0) == 0.0f && lava.GetLavaHeightByCityCell(0, 9) == 0.0f);

    // start: the cell is the floor of the position / 16, the radius is in cells, the flags latch once
    lava.StartLavaFlow(55.0f, 71.0f, 20.0f, true);                       // cell (3, 4), radius 1
    CHECK(lava.StartRect().left == 2 && lava.StartRect().top == 3 && lava.StartRect().right == 4 && lava.StartRect().bottom == 5);
    CHECK(lava.Bounds().left == 2 && lava.Bounds().bottom == 5);
    CHECK(lava.Registered() && lava.Ticking() && host.registered == 1 && host.tickStarts == 1);
    lava.StartLavaFlow(48.0f, 64.0f, 0.0f, false);                       // exactly on a cell boundary: still cell (3, 4)
    CHECK(lava.StartRect().left == 3 && lava.StartRect().top == 4);
    CHECK(host.registered == 1 && host.tickStarts == 1);                 // asked once

    // nothing happens while the lava is not initialized
    lava.SetActive(false);
    lava.DoLavaFlow(1000);
    CHECK(host.padded == 0 && lava.GetLavaHeightByCityCell(3, 4) == 0.0f);

    // production only: no solidification, a flat base, no neighbor lower than the cell it came from
    lava.SetActive(true);
    lava.SetLavaProductionRate(0.5f);
    lava.SetSolidificationRate(0.0f);
    lava.StartLavaFlow(55.0f, 71.0f, 0.0f, false);                       // single cell (3, 4)
    host.neighbors = 0;                                                   // nothing to flow to
    lava.DoLavaFlow(10);
    CHECK(lava.GetLavaHeightByCityCell(3, 4) == 5.0f);
    CHECK(host.padded == 1 && host.changed == 0);                         // no deposit: no terrain change
    CHECK(lava.Bounds().left == 2 && lava.Bounds().top == 3 && lava.Bounds().right == 4 && lava.Bounds().bottom == 5);   // padded by one

    // flow: a lower neighbor takes lava at maxFlowRate * movable * dt, capped by what the cell holds; lava arriving
    // on bare ground sets the cells before that vertex on fire
    auto& t = cSTEVolcanoLava::TuningValues();
    t.maxFlowRate = 0.01f;
    t.heightDiffForMaxFlowRate = 4.0f;
    host.neighbors = 0x01;                                                // only direction 0 (east, index + 1)
    lava.SetLavaProductionRate(0.0f);
    host.ground[4 * 8 + 3] = 10.0f;                                        // the cell stands 10 above its neighbor
    host.ClearFires();
    lava.DoLavaFlow(2);
    // drop = ((10 + 5) - (0 + 0)) * 0.5 = 7.5 >= 4, so rate = 0.01; movable = min(5, 7.5) = 5; wanted = 0.01 * 5 * 2 = 0.1
    // (the scan then reaches the neighbor, which passes a little on in turn: too little to burn anything)
    CHECK(std::fabs(lava.GetLavaHeightByCityCell(4, 4) - 0.0999875f) < 1e-7f);
    CHECK(std::fabs(lava.GetLavaHeightByCityCell(3, 4) - 4.9f) < 1e-6f);
    CHECK(host.ignited.size() == 4 && host.ignited[0] == std::make_pair(3, 3) && host.ignited[3] == std::make_pair(4, 4) && host.quickBurned.empty());
    CHECK(host.causes == 4 && host.lastCause == kLavaFireCause);

    // below the max-flow height difference the rate scales: drop 1.5 -> rate = 0.01 * 1.5 / 4
    t.heightDiffForMaxFlowRate = 40.0f;
    host.ground[4 * 8 + 3] = 0.0f;
    {
        cSTEVolcanoLava s(&host);
        s.SetGrid(8, 8, 16.0f);
        s.SetActive(true);
        s.SetLavaProductionRate(0.0f);
        s.SetSolidificationRate(0.0f);
        s.StartLavaFlow(55.0f, 71.0f, 0.0f, false);
        s.LavaDepth()[4 * 8 + 3] = 3.0f;
        s.DoLavaFlow(100);
        // drop = 3 * 0.5 = 1.5; rate = 0.01 * 1.5 / 40 = 0.000375; movable 1.5; wanted = 0.000375 * 1.5 * 100 = 0.05625
        CHECK(std::fabs(s.GetLavaHeightByCityCell(3, 4) - 2.94375f) < 1e-6f);
    }

    // the flow reads the ground as it is now: a neighbor that was raised meanwhile is no longer the lowest
    {
        cSTEVolcanoLava s(&host);
        s.SetGrid(8, 8, 16.0f);
        s.SetActive(true);
        s.SetLavaProductionRate(0.0f);
        s.SetSolidificationRate(0.0f);
        s.StartLavaFlow(55.0f, 71.0f, 0.0f, false);
        s.LavaDepth()[4 * 8 + 3] = 3.0f;
        host.ground[4 * 8 + 4] = 50.0f;
        s.DoLavaFlow(100);
        CHECK(s.GetLavaHeightByCityCell(3, 4) == 3.0f);
        host.ground[4 * 8 + 4] = 0.0f;
    }

    // the rim of the terrain (column 0 / last column / first row / last row) only loses lava
    {
        cSTEVolcanoLava s(&host);
        s.SetGrid(8, 8, 16.0f);
        s.SetActive(true);
        s.SetLavaProductionRate(0.0f);
        s.SetSolidificationRate(0.0f);
        s.LavaDepth()[3 * 8 + 0] = 1000.0f;
        s.StartLavaFlow(0.0f, 48.0f, 0.0f, false);                         // cell (0, 3)
        host.ClearFires();
        s.DoLavaFlow(10);
        // 1000 * (1e-7 * 10) = 0.001
        CHECK(host.ignited.empty() && s.GetLavaHeightByCityCell(0, 3) < 999.9995f && s.GetLavaHeightByCityCell(0, 3) > 999.9985f);
        s.LavaDepth()[3 * 8 + 0] = 0.0f;
        s.LavaDepth()[3 * 8 + 7] = 1000.0f;                                // the last column
        s.StartLavaFlow(7.0f * 16.0f, 48.0f, 0.0f, false);
        s.DoLavaFlow(10);
        CHECK(s.GetLavaHeightByCityCell(7, 3) < 999.9995f && s.GetLavaHeightByCityCell(7, 3) > 999.9985f && s.GetLavaHeightByCityCell(6, 3) == 0.0f);
    }

    // solidification: enough lava deposits (land or water multiplier), thin lava evaporates
    {
        cSTEVolcanoLava s(&host);
        s.SetGrid(8, 8, 16.0f);
        s.SetActive(true);
        s.SetLavaProductionRate(0.0f);
        s.SetSolidificationRate(0.01f);
        s.StartLavaFlow(55.0f, 71.0f, 0.0f, false);
        host.neighbors = 0;
        t.minAmountForDepositToHappen = 2.0f;
        t.liquidToSolidConversionFactor = 0.8f;
        t.underWaterSolidificationMultiple = 5.0f;
        s.LavaDepth()[4 * 8 + 3] = 10.0f;
        host.raised = host.changed = host.periodic = host.refreshed = 0;
        host.decals.clear();
        s.DoLavaFlow(10);
        // land: min(1, 0.01 * 10) = 0.1 -> 1.0 settles, raising the terrain by 1.0 * 0.8
        CHECK(std::fabs(s.GetLavaHeightByCityCell(3, 4) - 9.0f) < 1e-5f);
        CHECK(host.raised == 1 && host.raisedIndex == 35 && std::fabs(host.raisedAmount - 0.8f) < 1e-6f && host.decals.size() == 1);
        CHECK(host.changed == 1 && host.lastChanged.left == 3 && host.lastChanged.right == 3 && host.lastChanged.top == 4 &&
              host.lastChanged.bottom == 4 && host.refreshed == 1);
        host.water = true;                                                 // under water: min(1, 5 * 0.1) = 0.5
        s.DoLavaFlow(10);
        CHECK(std::fabs(s.GetLavaHeightByCityCell(3, 4) - 4.5f) < 1e-4f);
        host.water = false;
        s.LavaDepth()[4 * 8 + 3] = 1.5f;                                   // below the deposit minimum: x0.9
        s.DoLavaFlow(10);
        CHECK(std::fabs(s.GetLavaHeightByCityCell(3, 4) - 1.35f) < 1e-5f);
        s.LavaDepth()[4 * 8 + 3] = 0.05f;                                   // ... or nothing at all below 0.1
        s.DoLavaFlow(10);
        CHECK(s.GetLavaHeightByCityCell(3, 4) == 0.0f);
    }

    // the periodic terrain refresh fires on the hundredth deposit frame and resets the counter
    {
        cSTEVolcanoLava s(&host);
        s.SetGrid(8, 8, 16.0f);
        s.SetActive(true);
        s.SetLavaProductionRate(0.0f);
        s.SetSolidificationRate(0.01f);
        s.StartLavaFlow(55.0f, 71.0f, 0.0f, false);
        host.neighbors = 0;
        host.periodic = 0;
        for (int i = 0; i < 100; ++i) {
            s.LavaDepth()[4 * 8 + 3] = 10.0f;
            s.DoLavaFlow(10);
        }
        CHECK(host.periodic == 1 && s.FrameCounter() == 99);                // started at 100: the first frame fires and resets, 99 more frames count up
    }

    // neighbor choice: straight neighbors first, a diagonal must beat the best straight drop times sqrt(2)
    {
        cSTEVolcanoLava s(&host);
        s.SetGrid(8, 8, 16.0f);
        const int center = 3 * 8 + 3;
        for (float& depth : s.LavaDepth()) depth = 10.0f;                  // level everywhere, so only the chosen neighbors drop
        host.neighbors = 0xff;
        s.LavaDepth()[center + 1] = 6.0f;                                   // east: drop 4
        s.LavaDepth()[center + 9] = 4.5f;                                   // south-east: drop 5.5 < 4 * 1.4142135 = 5.657
        CHECK(s.GetNeighborWithLowestLavaLevel(center) == 0);
        s.LavaDepth()[center + 9] = 4.0f;                                   // drop 6 > 5.657
        CHECK(s.GetNeighborWithLowestLavaLevel(center) == 1);
        host.ground[static_cast<std::size_t>(center + 9)] = 3.0f;           // the ground under it counts: drop 3
        CHECK(s.GetNeighborWithLowestLavaLevel(center) == 0);
        host.ground[static_cast<std::size_t>(center + 9)] = 0.0f;
        host.neighbors = 0x00;
        CHECK(s.GetNeighborWithLowestLavaLevel(center) == -1);
        host.neighbors = 0x02;                                             // only the diagonal is open
        CHECK(s.GetNeighborWithLowestLavaLevel(center) == 1);
    }

    // tuning load: max flow rate is divided by a million; the smallest alpha is a byte, the two times are 32-bit
    cSCPropertyHelp::SetPropertyValue(&host.tuning, 0x69987e61, 2.0f);
    cSCPropertyHelp::SetPropertyValue(&host.tuning, 0x69987e62, 30.0f);
    cSCPropertyHelp::SetPropertyValue(&host.tuning, 0x69987e64, std::uint8_t{7});
    cSCPropertyHelp::SetPropertyValue(&host.tuning, 0x69987e6b, std::uint32_t{200000});
    cSCPropertyHelp::SetPropertyValue(&host.tuning, 0x69987e6d, std::uint32_t{500000});
    lava.ReadTuningParameters();
    CHECK(std::fabs(t.maxFlowRate - 2e-6f) < 1e-12f && t.heightDiffForMaxFlowRate == 30.0f);
    CHECK(t.minAlpha == 7 && t.animationFrameDuration == 200000 && t.scorchDecalLifeTime == 500000);

    // ============================================================================================ Init and Shutdown
    {
        DefaultTuning();
        cSTEVolcanoLava::ResetStatics();
        Host h(16);
        cSCPropertyHelp::SetPropertyValue(&h.tuning, 0x69987e67, 44.0f);
        cSTEVolcanoLava s(&h);
        CHECK(!s.IsInitialized());
        s.DoMessage(kLavaMessageAnimationTick, 0, 1000);                   // not initialized: nothing, not even a fire around the vent
        CHECK(h.padded == 0 && h.asked.empty());
        CHECK(s.Init() && s.IsInitialized() && h.listens == 1);
        CHECK(t.scorchedTerrainDecalSize == 44.0f);                        // the tuning is read by the first Init ...
        t.scorchedTerrainDecalSize = 45.0f;
        CHECK(s.Init() && h.listens == 1 && t.scorchedTerrainDecalSize == 45.0f);   // ... once; a second Init does nothing
        CHECK(s.VertsX() == 16 && s.VertsZ() == 16 && s.Vertices().size() == 256);
        const LavaVertex& v = s.Vertices()[2 * 16 + 3];
        CHECK(v.x == 48.0f && v.z == 32.0f && v.color[3] == 0 && v.u2 == 0.0f && v.v2 == 0.0f);
        CHECK(s.TextureZoom() == 2 && Near(v.u, 0.00109375f * 4.0f * 48.0f, 1e-7f) && Near(v.v, 0.00109375f * 4.0f * 32.0f, 1e-7f));
        s.SetDrawZoom(4);                                                  // the drawing's zoom rescales the texture
        CHECK(s.TextureZoom() == 4 && Near(s.Vertices()[2 * 16 + 3].u, 0.00109375f * 16.0f * 48.0f, 1e-7f));
        s.StartLavaFlow(88.0f, 88.0f, 16.0f, true);
        CHECK(s.Registered() && s.Ticking() && h.registered == 1 && h.tickStarts == 1);
        s.LavaDepth()[5 * 16 + 5] = 4.0f;
        CHECK(s.GetLavaHeightByCityCell(5, 5) == 4.0f);
        h.periodic = 0;
        CHECK(s.Shutdown() && !s.IsInitialized());
        CHECK(h.periodic == 1 && h.unlistens == 1 && h.unregistered == 1 && h.tickStops == 1);
        CHECK(!s.Registered() && !s.Ticking() && s.GetLavaHeightByCityCell(5, 5) == 0.0f);
        CHECK(s.Shutdown() && h.unlistens == 1 && h.unregistered == 1);     // once
        s.DoLavaFlow(1000);                                                // and nothing flows afterwards
        // a renderer or a tick manager that refuses is asked again by the next start
        Host shy(16);
        shy.rendererOk = shy.ticksOk = false;
        cSTEVolcanoLava r(&shy);
        r.Init();
        r.StartLavaFlow(88.0f, 88.0f, 0.0f, true);
        r.StartLavaFlow(88.0f, 88.0f, 0.0f, false);
        CHECK(!r.Registered() && !r.Ticking() && shy.registered == 2 && shy.tickStarts == 1);
        r.Shutdown();
        CHECK(shy.unregistered == 0 && shy.tickStops == 0);
        // the city's shutdown message shuts the lava down
        Host m(16);
        cSTEVolcanoLava q(&m);
        q.Init();
        q.DoMessage(kLavaMessageShutdown, 0, 0);
        CHECK(!q.IsInitialized() && m.unlistens == 1);
    }

    // ============================================================================================ the surface
    {
        DefaultTuning();
        Host h(16);
        cSTEVolcanoLava s(&h);
        s.Init();
        s.SetLavaProductionRate(0.0f);
        s.SetSolidificationRate(0.0f);
        h.neighbors = 0;
        auto at = [](int x, int z) { return static_cast<std::size_t>(z * 16 + x); };
        h.ground[at(3, 4)] = 10.0f;
        s.LavaDepth()[at(3, 4)] = 1.3f;                                    // between the two alpha depths
        s.LavaDepth()[at(4, 4)] = 0.3f;                                    // thin
        s.LavaDepth()[at(5, 4)] = 5.0f;                                    // deep
        s.LavaDepth()[at(4, 5)] = 100.0f;                                  // off the gradient
        s.StartLavaFlow(56.0f, 72.0f, 0.0f, false);
        s.DoLavaFlow(1000);
        const auto& v = s.Vertices();
        CHECK(Near(v[at(3, 4)].y, 11.3f) && v[at(3, 4)].color[3] == 137);  // (1.3 - 0.6) * (255 - 20) / 1.4 = 117, + 20
        CHECK(v[at(3, 4)].color[0] == 0x10 && v[at(3, 4)].color[1] == 0x20 && v[at(3, 4)].color[2] == 0x30 + 8);   // row 1.3 * 6.4
        CHECK(Near(v[at(4, 4)].y, 0.3f) && v[at(4, 4)].color[3] == 20 && v[at(4, 4)].color[2] == 0x30 + 1);
        CHECK(v[at(5, 4)].color[3] == 0xff && v[at(5, 4)].color[2] == 0x30 + 32);
        CHECK(v[at(4, 5)].color[2] == 0x30 + 63);                          // the last row
        CHECK(v[at(2, 4)].color[3] == 0 && v[at(2, 4)].color[2] == 0x30 && v[at(2, 4)].y == 0.0f);   // no lava: clear
        CHECK(v[at(8, 8)].color[3] == 0 && v[at(8, 8)].color[0] == 0);     // outside the rectangle: untouched
        // the active rectangle is now the vertices that hold lava
        CHECK(s.Bounds().left == 3 && s.Bounds().top == 4 && s.Bounds().right == 5 && s.Bounds().bottom == 5 && s.HasLava());
        // lava that is gone clears its vertex and leaves the rectangle
        s.LavaDepth()[at(5, 4)] = 0.0f;
        s.DoLavaFlow(1000);
        CHECK(v[at(5, 4)].color[3] == 0 && s.Bounds().right == 4);
        // the ground rising under the lava lifts the surface
        h.ground[at(3, 4)] = 12.0f;
        s.DoLavaFlow(1000);
        CHECK(Near(v[at(3, 4)].y, 13.3f));
        // without the gradient image nothing is prepared
        h.gradientLastRow = -1;
        s.LavaDepth()[at(3, 4)] = 9.0f;
        s.DoLavaFlow(1000);
        CHECK(v[at(3, 4)].color[3] == 137 && Near(v[at(3, 4)].y, 13.3f));
        // the smallest alpha of the tuning: the span above it shrinks
        h.gradientLastRow = 63;
        t.minAlpha = 100;
        s.LavaDepth()[at(3, 4)] = 1.3f;
        s.DoLavaFlow(1000);
        CHECK(v[at(3, 4)].color[3] == 100 + 77 && v[at(4, 4)].color[3] == 100);   // 0.7 * 155 / 1.4 = 77.5
    }

    // ============================================================================================ steam
    {
        DefaultTuning();
        Host h(16);
        cSTEVolcanoLava s(&h);
        s.Init();
        s.SetLavaProductionRate(0.0f);
        s.SetSolidificationRate(0.0f);
        h.neighbors = 0;
        auto at = [](int x, int z) { return static_cast<std::size_t>(z * 16 + x); };
        s.LavaDepth()[at(5, 4)] = 5.0f;                                    // deep lava standing in water one unit deeper
        s.LavaDepth()[at(6, 5)] = 5.0f;                                    // in the same block of four by four cells
        s.LavaDepth()[at(13, 14)] = 5.0f;                                  // in the last block (cells 12.. belong to block 2)
        s.LavaDepth()[at(9, 4)] = 5.0f;                                    // on land
        s.LavaDepth()[at(1, 1)] = 2.0f;                                    // in water, but not more than the deposit minimum
        for (std::size_t i : {at(5, 4), at(6, 5), at(13, 14), at(1, 1)}) {
            h.waterVertex[i] = true;
            h.waterLevel[i] = 6.0f;
        }
        h.waterLevel[at(1, 1)] = 3.0f;                                     // one unit over that lava: only its depth rules it out
        h.waterLevel[at(9, 4)] = 6.0f;                                     // the level is right there, but it is not a water vertex
        s.StartLavaFlow(8.0f, 8.0f, 20.0f * 16.0f, false);                 // the whole grid is the vent's rectangle
        s.DoLavaFlow(1000);
        CHECK(h.steams.size() == 2 && s.SteamCount() == 2);
        CHECK(h.steams[0].key == 1 + 1 * 3 && h.steams[0].xyz[0] == 80.0f && h.steams[0].xyz[1] == 6.0f && h.steams[0].xyz[2] == 64.0f);
        CHECK(h.steams[1].key == 2 + 2 * 3 && h.steams[1].xyz[0] == 13.0f * 16.0f);
        s.DoLavaFlow(1000);
        CHECK(h.steams.size() == 2 && !h.steams[0].stopped);               // they go on
        s.LavaDepth()[at(6, 5)] = 0.0f;
        // the water sinks under the lava surface: the steam stops softly; back above it: a new one
        h.waterLevel[at(5, 4)] = 4.9f;
        s.DoLavaFlow(1000);
        CHECK(h.steams[0].stopped && !h.steams[0].hard && s.SteamCount() == 1 && h.steams.size() == 2);
        h.waterLevel[at(5, 4)] = 6.0f;
        s.DoLavaFlow(1000);
        CHECK(h.steams.size() == 3 && h.steams[2].key == 4 && s.SteamCount() == 2);
        // two units of water over the lava are too many
        h.waterLevel[at(5, 4)] = 7.0f;
        s.DoLavaFlow(1000);
        CHECK(h.steams[2].stopped && s.SteamCount() == 1);
        // an effect that ended by itself is forgotten (and released), and comes back while the place is still good
        h.waterLevel[at(5, 4)] = 6.0f;
        s.DoLavaFlow(1000);
        CHECK(h.steams.size() == 4 && s.SteamCount() == 2);
        s.DoMessage(kLavaMessageSteamEnded, 4, 0);
        CHECK(s.SteamCount() == 1 && h.steams[3].released && !h.steams[3].stopped);
        s.DoMessage(kLavaMessageSteamEnded, 77, 0);                        // an unknown key
        CHECK(s.SteamCount() == 1);
        // shutting down stops what is left at once
        s.Shutdown();
        CHECK(h.steams[1].stopped && h.steams[1].hard && s.SteamCount() == 0);
        // a city without effects has no steam
        Host quiet(16);
        quiet.effects = false;
        quiet.neighbors = 0;
        cSTEVolcanoLava d(&quiet);
        d.Init();
        d.SetLavaProductionRate(0.0f);
        d.LavaDepth()[at(5, 4)] = 5.0f;
        quiet.waterVertex[at(5, 4)] = true;
        quiet.waterLevel[at(5, 4)] = 6.0f;
        d.StartLavaFlow(88.0f, 72.0f, 32.0f, false);
        d.DoLavaFlow(1000);
        CHECK(d.SteamCount() == 0);
    }

    // ============================================================================================ scorch decals
    {
        DefaultTuning();
        t.scorchDecalLifeTime = 25;
        t.scorchedTerrainDecalSize = 50.0f;
        t.scorchDecalFadeInTime = 1.0f;
        t.scorchDecalFadeOutTime = 2.0f;
        Host h(16);
        h.neighbors = 0;
        h.frames[2] = 0;
        cSTEVolcanoLava s(&h);
        s.Init();
        s.SetLavaProductionRate(0.0f);
        s.SetSolidificationRate(0.001f);
        auto at = [](int x, int z) { return static_cast<std::size_t>(z * 16 + x); };
        h.ground[at(5, 6)] = 30.0f;
        s.LavaDepth()[at(5, 6)] = 10.0f;
        s.LavaDepth()[at(6, 6)] = 10.0f;                                   // the same block: one decal
        s.LavaDepth()[at(9, 6)] = 10.0f;                                   // the next block
        s.StartLavaFlow(88.0f, 104.0f, 64.0f, false);
        s.DoAnimationTick(10);
        CHECK(h.decals.size() == 2 && s.DecalCount() == 2);
        CHECK(h.decals[0].xyz[0] == 80.0f && h.decals[0].xyz[1] == 30.0f && h.decals[0].xyz[2] == 96.0f && h.decals[0].size == 50.0f && h.decals[0].fadeIn == 1.0f);
        CHECK(h.decals[1].xyz[0] == 144.0f && h.decals[0].action == -1);
        s.DoAnimationTick(10);                                             // 20 of the 25 microseconds
        CHECK(h.decals.size() == 2 && h.decals[0].action == -1);
        s.DoAnimationTick(10);                                             // their time is up: they fade out and are forgotten
        CHECK(h.decals[0].action == kLavaDecalFadedOut && h.decals[0].fadeOut == 2.0f && h.decals[1].action == kLavaDecalFadedOut && s.DecalCount() == 0);
        s.DoAnimationTick(10);                                             // lava still settles there: new decals
        CHECK(h.decals.size() == 4 && s.DecalCount() == 2);
        s.Shutdown();
        CHECK(h.decals[2].action == kLavaDecalRemoved && h.decals[3].action == kLavaDecalRemoved && s.DecalCount() == 0);
    }

    // ============================================================================================ fires
    {
        DefaultTuning();                                                   // burn from 0.01, deep fry from 2
        Host h(16);
        cSTEVolcanoLava s(&h);
        s.Init();
        const int vertex = 6 * 16 + 5;                                     // (5, 6)
        s.SetThingsOnFire(vertex, 0.0f, 0.02f);                            // crosses the burn threshold
        CHECK(h.ignited.size() == 4 && h.ignited[0] == std::make_pair(4, 5) && h.ignited[1] == std::make_pair(4, 6) &&
              h.ignited[2] == std::make_pair(5, 5) && h.ignited[3] == std::make_pair(5, 6));
        CHECK(h.quickBurned.empty() && h.causes == 4 && h.lastCause == 3);
        h.ClearFires();
        s.SetThingsOnFire(vertex, 0.5f, 1.0f);                             // already burning, not deep yet
        s.SetThingsOnFire(vertex, 0.0f, 0.001f);                           // too thin
        s.SetThingsOnFire(vertex, 2.5f, 1.0f);                             // already deep
        CHECK(h.asked.empty());
        s.SetThingsOnFire(vertex, 1.5f, 0.6f);                             // crosses the deep-fry threshold
        CHECK(h.quickBurned.size() == 4 && h.ignited.empty() && h.causes == 4);
        h.ClearFires();
        s.SetThingsOnFire(vertex, 0.0f, 3.0f);                             // crosses both at once: it only ignites
        CHECK(h.ignited.size() == 4 && h.quickBurned.empty());
        h.ClearFires();
        s.SetThingsOnFire(vertex, 0.01f, 5.0f);                            // exactly at the burn threshold counts as burning: deep fry
        CHECK(h.quickBurned.size() == 4 && h.ignited.empty());
        h.ClearFires();
        h.fireproof = {{4, 5}, {5, 6}};
        s.SetThingsOnFire(vertex, 0.0f, 0.02f);
        CHECK(h.asked.size() == 4 && h.ignited.size() == 2 && h.causes == 2);
        h.ClearFires();
        h.fireproof.clear();
        s.SetThingsOnFire(3 * 16 + 0, 0.0f, 0.02f);                        // the first column asks for the cells before it too
        CHECK(h.asked.size() == 4 && h.asked[0] == std::make_pair(-1, 2));

        // the cells around the vent burn while it produces: eight random cells a tick out of the 15 x 15 around it,
        // cut to the city; every cell once before any comes again
        h.ClearFires();
        s.SetLavaProductionRate(0.0f);
        s.SetSolidificationRate(0.0f);
        s.StartLavaFlow(8.5f * 16.0f, 8.5f * 16.0f, 0.0f, false);          // cell (8, 8): cells 1..14
        s.DoAnimationTick(10);
        CHECK(h.asked.empty());                                            // no production: nothing burns
        s.SetLavaProductionRate(1e-9f);
        h.neighbors = 0;
        for (int i = 0; i < 24; ++i) s.DoAnimationTick(10);
        std::set<std::pair<int, int>> seen(h.asked.begin(), h.asked.end());
        bool inside = true;
        for (const auto& cell : h.asked) inside = inside && cell.first >= 1 && cell.first <= 14 && cell.second >= 1 && cell.second <= 14;
        CHECK(h.asked.size() == 192 && seen.size() == 192 && inside && h.ignited.size() == 192);
        s.DoAnimationTick(10);                                             // the last four, one pick lost to the rewind, three again
        seen.insert(h.asked.begin(), h.asked.end());
        CHECK(h.asked.size() == 199 && seen.size() == 196);
        h.ClearFires();
        h.fireproof = {{0, 0}};
        s.StartLavaFlow(-400.0f, 80.0f, 0.0f, false);                      // a vent whose surroundings are outside the city: cell (0, 0) only
        s.BurnSurroundingStuff();
        bool origin = !h.asked.empty();
        for (const auto& cell : h.asked) origin = origin && cell == std::make_pair(0, 0);
        CHECK(origin && h.ignited.empty());
        CHECK(s.BurnRadius() == 7);
    }

    // ============================================================================================ the surface's frames
    {
        DefaultTuning();                                                   // 80000 microseconds a frame
        Host h(16);
        cSTEVolcanoLava s(&h);
        s.Init();
        s.SetLavaProductionRate(0.0f);
        s.DoMessage(kLavaMessageAnimationTick, 0, 50000);
        CHECK(s.Frame() == 0);
        s.DoMessage(kLavaMessageAnimationTick, 0, 30000);                  // exactly the duration: not yet
        CHECK(s.Frame() == 0);
        s.DoMessage(kLavaMessageAnimationTick, 0, 1);
        CHECK(s.Frame() == 1);
        for (int i = 0; i < 3; ++i) s.DoMessage(kLavaMessageAnimationTick, 0, 80001);
        CHECK(s.Frame() == 0);                                             // four frames: around
        h.frames[2] = 0;                                                   // no texture at this zoom: the frame stands
        s.DoMessage(kLavaMessageAnimationTick, 0, 500000);
        CHECK(s.Frame() == 0);
    }

    // ============================================================================================ what is drawn
    {
        std::vector<std::uint16_t> indices;
        cSTEVolcanoLava::TriangleIndices(3, indices);
        const std::uint16_t expected[18] = {0, 4, 5, 0, 5, 1, 1, 5, 6, 1, 6, 2, 2, 6, 7, 2, 7, 3};
        CHECK(indices.size() == 18 && std::equal(indices.begin(), indices.end(), expected));

        DefaultTuning();
        Host h(16);
        h.neighbors = 0;
        cSTEVolcanoLava s(&h);
        s.Init();
        s.SetLavaProductionRate(0.0f);
        s.SetSolidificationRate(0.0f);
        auto at = [](int x, int z) { return static_cast<std::size_t>(z * 16 + x); };
        for (std::size_t i : {at(3, 4), at(4, 4), at(8, 4), at(3, 5), at(6, 6)}) s.LavaDepth()[i] = 5.0f;
        s.StartLavaFlow(8.0f, 8.0f, 20.0f * 16.0f, false);
        s.DoLavaFlow(1000);
        CHECK(s.Bounds().left == 3 && s.Bounds().top == 4 && s.Bounds().right == 8 && s.Bounds().bottom == 6);
        const LavaRect all{0, 0, 15, 15}, wide{-12, -12, 27, 27};
        CHECK(s.ComputeDrawRect(all, wide));
        CHECK(s.DrawRect().left == 3 && s.DrawRect().top == 4 && s.DrawRect().right == 8 && s.DrawRect().bottom == 6);
        std::vector<LavaRun> runs;
        s.CollectSurfaceRuns(runs);
        // a run starts one cell before its first lava vertex and ends with the cell after the first vertex without
        // row 4 (its vertices and row 5's): lava at 3 and 4 -> cells 2..5; lava at 8, the rectangle's edge -> cells 7 and 8
        // row 5 (and row 6's): lava at 3 -> cells 2..4; lava at 6 in row 6 -> cells 5..7
        CHECK(runs.size() == 4);
        CHECK(runs.size() == 4 && runs[0].row == 4 && runs[0].firstCell == 2 && runs[0].cellCount == 4);
        CHECK(runs.size() == 4 && runs[1].row == 4 && runs[1].firstCell == 7 && runs[1].cellCount == 2);
        CHECK(runs.size() == 4 && runs[2].row == 5 && runs[2].firstCell == 2 && runs[2].cellCount == 3);
        CHECK(runs.size() == 4 && runs[3].row == 5 && runs[3].firstCell == 5 && runs[3].cellCount == 3);
        // the screen ends at cell 8: the run at the edge is cut there; the terrain's visible rectangle cuts too
        const LavaRect narrow{0, 0, 8, 15};
        s.ComputeDrawRect(narrow, wide);
        s.CollectSurfaceRuns(runs);
        CHECK(runs.size() == 4 && runs[1].firstCell == 7 && runs[1].cellCount == 1);
        const LavaRect ground{4, 5, 27, 27};
        s.ComputeDrawRect(all, ground);
        CHECK(s.DrawRect().left == 4 && s.DrawRect().top == 5 && s.DrawRect().right == 8 && s.DrawRect().bottom == 6);
        s.CollectSurfaceRuns(runs);
        CHECK(runs.size() == 1 && runs[0].row == 5 && runs[0].firstCell == 5 && runs[0].cellCount == 3);
        // lava off the screen: the empty rectangle at the origin, nothing drawn
        const LavaRect away{10, 10, 15, 15};
        s.ComputeDrawRect(away, wide);
        s.CollectSurfaceRuns(runs);
        CHECK(s.DrawRect().left == 0 && s.DrawRect().right == 0 && s.DrawRect().top == 0 && s.DrawRect().bottom == 0 && runs.empty());
        // away from the city's edge there are no walls
        std::vector<LavaSideQuad> quads;
        s.ComputeDrawRect(all, wide);
        s.CollectSideQuads(quads);
        CHECK(quads.empty());
    }
    {
        // lava at the city's edge: walls from the lava surface down to the ground
        DefaultTuning();
        Host h(16);
        h.neighbors = 0;
        cSTEVolcanoLava s(&h);
        s.Init();
        s.SetLavaProductionRate(0.0f);
        s.SetSolidificationRate(0.0f);
        auto at = [](int x, int z) { return static_cast<std::size_t>(z * 16 + x); };
        h.ground[at(0, 4)] = 20.0f;
        h.ground[at(0, 5)] = 22.0f;
        s.LavaDepth()[at(0, 4)] = 5.0f;
        s.LavaDepth()[at(0, 5)] = 6.0f;
        s.StartLavaFlow(8.0f, 8.0f, 20.0f * 16.0f, false);
        s.DoLavaFlow(1000);
        const LavaRect all{0, 0, 15, 15}, wide{-12, -12, 27, 27};
        s.ComputeDrawRect(all, wide);
        std::vector<LavaSideQuad> quads;
        s.CollectSideQuads(quads);
        CHECK(quads.size() == 1);
        if (quads.size() == 1) {
            const LavaSideVertex* q = quads[0].vertices;
            // (the lava at the rim lost a little: 25 and 28 less half a thousandth)
            CHECK(q[0].x == 0.0f && Near(q[0].y, 25.0f, 1e-2f) && q[0].z == 64.0f && q[1].x == 0.0f && q[1].y == 20.0f && q[1].z == 64.0f);
            CHECK(q[2].y == 22.0f && q[2].z == 80.0f && Near(q[3].y, 28.0f, 1e-2f) && q[3].z == 80.0f && q[0].y < 25.0f);
            CHECK(Near(q[0].u, 64.0f * 0.00109375f, 1e-7f) && Near(q[0].v, 25.0f * 0.00109375f, 1e-5f) && Near(q[2].u, 80.0f * 0.00109375f, 1e-7f) &&
                  Near(q[2].v, 22.0f * 0.00109375f, 1e-7f) && Near(q[1].v, 20.0f * 0.00109375f, 1e-7f));
            CHECK(q[0].color[0] == 0xff && q[0].color[1] == 0 && q[0].color[2] == 0 && q[0].color[3] == 0xff && q[3].color[0] == 0xff);
        }
        // the first row's wall runs against x, the last column's against z
        s.LavaDepth()[at(0, 4)] = s.LavaDepth()[at(0, 5)] = 0.0f;
        s.LavaDepth()[at(4, 0)] = 5.0f;
        s.LavaDepth()[at(5, 0)] = 5.0f;
        s.LavaDepth()[at(15, 9)] = 5.0f;
        s.LavaDepth()[at(15, 10)] = 5.0f;
        s.StartLavaFlow(8.0f, 8.0f, 20.0f * 16.0f, false);
        s.DoLavaFlow(1000);
        s.ComputeDrawRect(all, wide);
        s.CollectSideQuads(quads);
        // the rectangle is (4, 0)..(15, 10): the first row is walked from x 15 down to 4 (the steps 6-5 and 5-4 touch
        // lava), the last column from z 10 up to 0 (the steps 10-9 and 9-8)
        int firstRow = 0, lastColumn = 0;
        for (const LavaSideQuad& quad : quads) {
            if (quad.vertices[0].z == 0.0f && quad.vertices[3].z == 0.0f) ++firstRow;
            if (quad.vertices[0].x == 240.0f && quad.vertices[3].x == 240.0f) ++lastColumn;
        }
        CHECK(firstRow == 2 && lastColumn == 2 && quads.size() == 4);
        if (quads.size() == 4) {
            CHECK(quads[0].vertices[0].x == 96.0f && quads[0].vertices[3].x == 80.0f);             // against x: 6 -> 5
            CHECK(Near(quads[0].vertices[3].u, 80.0f * 0.00109375f, 1e-7f));                        // u follows x there
            CHECK(quads[2].vertices[0].z == 160.0f && quads[2].vertices[3].z == 144.0f);           // against z: 10 -> 9
            CHECK(Near(quads[2].vertices[3].u, 144.0f * 0.00109375f, 1e-7f));                       // ... and z here
        }
    }

    std::puts(failures ? "sc4_volcanolava: FAILED" : "sc4_volcanolava ok");
    return failures != 0;
}
