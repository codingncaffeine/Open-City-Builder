// cSC4WaterPathFinder — unit test with a fake city: water cells with a depth and a body id. Checks the depth bytes
// (ceil of depth / 0.2, capped at 40 m), the compressed route bytes, routes around land, the pull of deep water,
// routes only within one body of water, runs of at most 255 steps, and the dirty rect the messages grow.
#include <cmath>
#include <cstdio>
#include <vector>

#include "rz/cRZMessage2Standard.h"
#include "sc4/WaterPathFinder.h"

using namespace sc4;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {

struct FakeGridCity final : ISimGridCity {
    int sx, sz;
    FakeGridCity(int x, int z) : sx(x), sz(z) {}
    std::int32_t CitySizeXInCells() const override { return sx; }
    std::int32_t CitySizeZInCells() const override { return sz; }
    float CellSizeXMeters() const override { return 16.0f; }
    float CellSizeZMeters() const override { return 16.0f; }
};

struct FakeHost final : IWaterPathFinderHost {
    int sx, sz;
    std::vector<float> depth;            // <= 0 = land
    std::vector<std::uint16_t> body;
    std::vector<std::uint32_t> subscribed;
    int rectX0 = 0, rectZ0 = 0, rectX1 = 0, rectZ1 = 0;
    FakeHost(int x, int z, float d, std::uint16_t b) : sx(x), sz(z), depth(std::size_t(x * z), d), body(std::size_t(x * z), b) {}
    std::size_t At(int x, int z) const { return std::size_t(z * sx + x); }
    void Land(int x, int z) { depth[At(x, z)] = 0.0f; body[At(x, z)] = 0; }
    void CityCellCount(int& x, int& z) override { x = sx; z = sz; }
    bool IsNavigable(int x, int z) override { return depth[At(x, z)] > 0.0f; }
    float WaterDepthForCell(int x, int z) override { return depth[At(x, z)]; }
    std::uint16_t BodyOfWater(int x, int z) override { return body[At(x, z)]; }
    bool CellHasWater(int x, int z) override { return depth[At(x, z)] > 0.0f; }
    bool OccupantCells(void*, int& x0, int& z0, int& x1, int& z1) override {
        x0 = rectX0; z0 = rectZ0; x1 = rectX1; z1 = rectZ1;
        return true;
    }
    void AddNotification(cIGZMessageTarget2*, std::uint32_t m) override { subscribed.push_back(m); }
    void RemoveNotification(cIGZMessageTarget2*, std::uint32_t m) override {
        for (auto it = subscribed.begin(); it != subscribed.end(); ++it) if (*it == m) { subscribed.erase(it); break; }
    }
};

// Decodes a compressed route; false when it is malformed. Fills the visited cells.
bool Decode(const std::vector<std::uint8_t>& r, std::vector<std::pair<int, int>>& cells) {
    cells.clear();
    if (r.size() < 3 || r[0] != 0) return false;
    int x = r[1], z = r[2];
    cells.push_back({x, z});
    std::size_t i = 3;
    while (i < r.size()) {
        if (r[i] != 0x40 || i + 1 >= r.size()) return false;
        const int n = r[i + 1];
        const std::size_t bytes = std::size_t(n / 4 + (n % 4 ? 1 : 0));
        if (i + 2 + bytes > r.size()) return false;
        for (int s = 0; s < n; ++s) {
            const int dir = (r[i + 2 + std::size_t(s / 4)] >> ((s % 4) * 2)) & 3;
            static const int dx[4] = {-1, 0, 1, 0}, dz[4] = {0, -1, 0, 1};
            x += dx[dir];
            z += dz[dir];
            cells.push_back({x, z});
        }
        i += 2 + bytes;
    }
    return true;
}

cRZMessage2Standard Message(std::uint32_t type, std::uint64_t d1 = 0, std::uint64_t d2 = 0, std::uint64_t d3 = 0) {
    cRZMessage2Standard m;
    m.SetType(type);
    m.SetData1(d1);
    m.SetData2(d2);
    m.SetData3(d3);
    return m;
}

}  // namespace

int main() {
    // ---- straight route, both directions, and the subscriptions ----
    {
        FakeGridCity city(16, 16);
        SetSimGridCity(&city);
        FakeHost host(16, 16, 10.0f, 1);
        auto* finder = new cSC4WaterPathFinder(&host);
        finder->AddRef();
        CHECK(finder->Init());
        CHECK(!finder->Init());
        CHECK(host.subscribed.size() == 6);
        auto init = Message(kWaterPathMsgPostCityInit);
        finder->DoMessage(&init);
        CHECK(finder->Dirty());
        std::vector<std::uint8_t> fwd, back;
        std::int32_t cells = 0;
        CHECK(finder->FindPath(2, 2, 6, 2, cells, &fwd, &back));
        CHECK(!finder->Dirty());
        CHECK(cells == 5);
        CHECK((fwd == std::vector<std::uint8_t>{0x00, 2, 2, 0x40, 4, 0xaa}));   // four steps of x+1 (2) packed 2 bits each
        CHECK((back == std::vector<std::uint8_t>{0x00, 6, 2, 0x40, 4, 0x00}));  // four steps of x-1 (0)
        CHECK(finder->DepthMap()->GetTractValue(3, 3) == 50);                   // 10 m / 0.2
        // a route of a single cell: only the start
        CHECK(finder->FindPath(4, 4, 4, 4, cells, &fwd, nullptr));
        CHECK(cells == 1);
        CHECK((fwd == std::vector<std::uint8_t>{0x00, 4, 4}));
        CHECK(finder->Shutdown());
        CHECK(host.subscribed.empty());
        CHECK(finder->DepthMap() == nullptr);
        finder->Release();
    }
    // ---- depth bytes: ceil(depth / 0.2), capped at 40 m, 0 on land ----
    {
        FakeGridCity city(8, 8);
        SetSimGridCity(&city);
        FakeHost host(8, 8, 2.0f, 1);
        host.depth[host.At(1, 0)] = 2.1f;
        host.depth[host.At(2, 0)] = 55.0f;
        host.depth[host.At(3, 0)] = 0.01f;
        host.Land(4, 0);
        cSC4WaterPathFinder finder(&host);
        finder.AddRef();
        finder.Init();
        auto init = Message(kWaterPathMsgPostCityInit);
        finder.DoMessage(&init);
        auto end = Message(kWaterPathMsgTerrainOperationEnd);
        finder.DoMessage(&end);
        CHECK(!finder.Dirty());
        CHECK(finder.DepthMap()->GetTractValue(0, 0) == 10);
        CHECK(finder.DepthMap()->GetTractValue(1, 0) == 11);
        CHECK(finder.DepthMap()->GetTractValue(2, 0) == 200);
        CHECK(finder.DepthMap()->GetTractValue(3, 0) == 1);
        CHECK(finder.DepthMap()->GetTractValue(4, 0) == 0);
        // a terrain change: the rect grows one cell on the low side and is clamped to the city; the depth updates on the next search
        host.depth[host.At(4, 4)] = 30.0f;      // on the low edge: the message says x 5..9, z 5..5
        host.depth[host.At(4, 3)] = 30.0f;      // outside it
        auto changed = Message(kWaterPathMsgTerrainChanged, (5u << 16) | 9u, (5u << 16) | 5u);
        finder.DoMessage(&changed);
        CHECK(finder.Dirty());
        CHECK(finder.DepthMap()->GetTractValue(4, 4) == 10);
        std::int32_t cells = 0;
        std::vector<std::uint8_t> fwd;
        CHECK(finder.FindPath(0, 1, 1, 1, cells, &fwd, nullptr));
        CHECK(finder.DepthMap()->GetTractValue(4, 4) == 150);
        CHECK(finder.DepthMap()->GetTractValue(4, 3) == 10);
        // a bridge piece over water dirties its cells; one on land or of another type does not
        host.rectX0 = 6; host.rectZ0 = 6; host.rectX1 = 7; host.rectZ1 = 7;
        auto other = Message(kWaterPathMsgOccupantInserted, 0, 0, 0x12345678);
        finder.DoMessage(&other);
        CHECK(!finder.Dirty());
        host.rectX0 = 4; host.rectZ0 = 0;
        auto onLand = Message(kWaterPathMsgOccupantInserted, 0, 0, kWaterPathNetworkOccupantType);
        finder.DoMessage(&onLand);
        CHECK(!finder.Dirty());
        host.rectX0 = 6; host.rectZ0 = 6;
        host.depth[host.At(7, 7)] = 4.0f;
        auto bridge = Message(kWaterPathMsgOccupantRemoved, 0, 0, kWaterPathNetworkOccupantType);
        finder.DoMessage(&bridge);
        CHECK(finder.Dirty());
        finder.DoMessage(&end);
        CHECK(finder.DepthMap()->GetTractValue(7, 7) == 20);
        finder.Shutdown();
    }
    // ---- around a wall of land; another body of water is unreachable ----
    {
        FakeGridCity city(16, 16);
        SetSimGridCity(&city);
        FakeHost host(16, 16, 10.0f, 1);
        for (int z = 0; z <= 12; ++z) host.Land(8, z);
        for (int x = 0; x < 16; ++x) host.body[host.At(x, 15)] = 2;   // the last row is another lake (kept navigable)
        cSC4WaterPathFinder finder(&host);
        finder.AddRef();
        finder.Init();
        auto init = Message(kWaterPathMsgPostCityInit);
        finder.DoMessage(&init);
        std::vector<std::uint8_t> fwd;
        std::int32_t cells = 0;
        CHECK(finder.FindPath(4, 4, 12, 4, cells, &fwd, nullptr));
        std::vector<std::pair<int, int>> path;
        CHECK(Decode(fwd, path));
        CHECK(int(path.size()) == cells);
        CHECK(path.back() == std::make_pair(12, 4));
        bool dry = false, under = false;
        for (auto [x, z] : path) { if (host.depth[host.At(x, z)] <= 0.0f) dry = true; if (z >= 13) under = true; }
        CHECK(!dry);
        CHECK(under);
        CHECK(cells == 1 + 9 + 8 + 9);       // the wall: down 9 rows to z=13, 8 across, up 9
        CHECK(!finder.FindPath(4, 4, 4, 15, cells, &fwd, nullptr));   // another body
        CHECK(!finder.FindPath(8, 4, 4, 4, cells, &fwd, nullptr));    // the start is on land (body 0)
        finder.Shutdown();
    }
    // ---- deep water pulls the route: a 40 m channel five rows away beats the shallow straight line ----
    {
        FakeGridCity city(16, 16);
        SetSimGridCity(&city);
        FakeHost host(16, 16, 2.0f, 1);
        for (int x = 0; x < 16; ++x) host.depth[host.At(x, 10)] = 40.0f;
        cSC4WaterPathFinder finder(&host);
        finder.AddRef();
        finder.Init();
        auto init = Message(kWaterPathMsgPostCityInit);
        finder.DoMessage(&init);
        std::vector<std::uint8_t> fwd;
        std::int32_t cells = 0;
        CHECK(finder.FindPath(0, 5, 15, 5, cells, &fwd, nullptr));
        std::vector<std::pair<int, int>> path;
        CHECK(Decode(fwd, path));
        int deep = 0;
        for (auto [x, z] : path) if (z == 10) ++deep;
        CHECK(deep == 16);
        CHECK(cells == 1 + 5 + 15 + 5);
        finder.Shutdown();
    }
    // ---- a long route: runs of at most 255 steps ----
    {
        FakeGridCity city(256, 4);
        SetSimGridCity(&city);
        FakeHost host(256, 4, 10.0f, 1);
        cSC4WaterPathFinder finder(&host);
        finder.AddRef();
        finder.Init();
        auto init = Message(kWaterPathMsgPostCityInit);
        finder.DoMessage(&init);
        std::vector<std::uint8_t> fwd;
        std::int32_t cells = 0;
        CHECK(finder.FindPath(0, 0, 255, 3, cells, &fwd, nullptr));
        CHECK(cells == 259);
        CHECK(fwd.size() == 3 + 2 + 64 + 2 + 1);
        CHECK(fwd[3] == 0x40 && fwd[4] == 255 && fwd[69] == 0x40 && fwd[70] == 3);
        std::vector<std::pair<int, int>> path;
        CHECK(Decode(fwd, path));
        CHECK(path.back() == std::make_pair(255, 3));
        finder.Shutdown();
    }
    SetSimGridCity(nullptr);
    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::printf("sc4_waterpathfinder: ok\n");
    return 0;
}
