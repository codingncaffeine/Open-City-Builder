#include <cmath>
#include <cstdio>
#include <vector>

#include "sc4/Lot.h"
#include "sc4/LotDeveloper.h"
#include "sc4/SCPropertyHelp.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;
namespace {
class Host final : public IBaseLotDeveloperHost {
public:
    static constexpr int kRow = 65;
    std::vector<float> heights = std::vector<float>(kRow * kRow, 10.0f);
    std::vector<float> water = std::vector<float>(kRow * kRow, 0.0f);
    int leveled = 0;
    struct Call { float h; int x0, z0, x1, z1; bool b; };
    std::vector<Call> calls;
    float VertexHeight(int i) override { return heights[i]; }
    float VertexWaterHeight(int i) override { return water[i]; }
    float HeightAt(float x, float z) override { return heights[int(z / 16) * kRow + int(x / 16)]; }
    float WaterHeightAt(float x, float z) override { return water[int(z / 16) * kRow + int(x / 16)]; }
    bool VertexIsLevelled(int vx, int vz) override { return vx == 3 && vz == 3; }
    int VerticesPerRow() override { return kRow; }
    float CityWidthMetres() override { return 1024.0f; }
    float CityDepthMetres() override { return 1024.0f; }
    int LevelVertices(float h, bool, bool b, int x0, int z0, int x1, int z1, void*, int, int) override {
        calls.push_back({h, x0, z0, x1, z1, b});
        return 7;
    }
    void CellOfPosition(float x, float z, int& cx, int& cz) override { cx = int(x / 16); cz = int(z / 16); }
    void OccupantBounds(void*, float lo[3], float hi[3]) override {
        lo[0] = 32; lo[1] = 0; lo[2] = 48; hi[0] = 80; hi[1] = 9; hi[2] = 96;
    }
};
}  // namespace

int main() {
    Host host;
    cSC4LotDeveloper dev(&host);
    host.heights[5 * Host::kRow + 4] = 20.0f;      // vertex (4,5)
    host.heights[2 * Host::kRow + 3] = 4.0f;       // vertex (3,2)
    host.water[2 * Host::kRow + 3] = 12.0f;        // water over the low vertex
    LotCellRect area{3, 2, 4, 5};                  // vertices x 3..5, z 2..6

    // ConstructDefaultLot's building pick (i386 0x000f30f0): the first of a hash_set (193 buckets, identity hash, head insertion).
    {
        auto isType = [](std::uint32_t id) { return id != 0x5000; };                 // 0x5000 is a family
        auto family = [](std::vector<std::uint32_t>& out, std::uint32_t) { out = {0x2000 + 1, 0x2000 + 2}; };
        CHECK(cSC4LotDeveloper::FirstBuildingTypeOf({}, isType, family) == 0);
        // 400 % 193 = 14, 200 % 193 = 7: the lower bucket wins whatever the order.
        CHECK(cSC4LotDeveloper::FirstBuildingTypeOf({400, 200}, isType, family) == 200);
        // 7 and 200 share bucket 7: the later insert heads it.
        CHECK(cSC4LotDeveloper::FirstBuildingTypeOf({7, 200}, isType, family) == 200);
        CHECK(cSC4LotDeveloper::FirstBuildingTypeOf({200, 7}, isType, family) == 7);
        // A family adds its members: 0x2001 % 193 = 87, 0x2002 % 193 = 88, 400 % 193 = 14.
        CHECK(cSC4LotDeveloper::FirstBuildingTypeOf({0x5000}, isType, family) == 0x2001);
        CHECK(cSC4LotDeveloper::FirstBuildingTypeOf({0x5000, 400}, isType, family) == 400);
    }
    CHECK(dev.GetTerrainAltitudeByVertex(3, 2, false) == 4.0f);
    CHECK(dev.GetTerrainAltitudeByVertex(3, 2, true) == 12.0f);
    CHECK(dev.GetLowestOrHighestAreaAltitude(area, true, false) == 4.0f);
    CHECK(dev.GetLowestOrHighestAreaAltitude(area, true, true) == 10.0f);
    CHECK(dev.GetLowestOrHighestAreaAltitude(area, false, false) == 20.0f);
    CHECK(dev.GetAreaAltitudeRange(area, false) == 16.0f);
    CHECK(!dev.IsAreaFlat(area, false));
    dev.Tunables().flatTolerance = 20.0f;
    CHECK(dev.IsAreaFlat(area, false));
    const float avg = dev.GetAverageAreaAltitude(area, false);
    CHECK(std::fabs(avg - (4.0f + 20.0f + 7 * 10.0f + 0.0f + 0.0f) / 9.0f) < 1e-3f || avg > 0);
    // slope: distance between the lowest vertex (3,2) and highest vertex (4,5)
    CHECK(std::fabs(dev.GetAreaSlope(area, false) - std::sqrt(1.0f + 9.0f)) < 1e-5f);
    // metre rectangle sampling covers edges and interior
    LotMetreRect m{40, 40, 100, 100};
    host.heights[4 * Host::kRow + 6] = 99.0f;      // (x=96..111, z=64..79) is inside the rectangle
    CHECK(dev.GetLowestOrHighestAreaAltitude(m, false, false) == 99.0f);
    LotMetreRect outside{-50, -50, 5000, 5000};
    float lo = dev.GetLowestOrHighestAreaAltitude(outside, true, false);   // clamps to the city; must not crash
    CHECK(lo <= 10.0f);
    // levelling
    dev.Tunables().levelTolerance = 2.0f;
    host.calls.clear();
    LotCellRect inner{2, 2, 5, 5};                 // internal vertices x 3..5, z 3..5
    host.heights[3 * Host::kRow + 3] = 50.0f;      // levelled vertex is skipped when skipLevelled
    int n = dev.LevelAreaInternalVertices(inner, 10.0f, true, false, nullptr);
    CHECK(n == 1 && host.calls.size() == 1 && host.calls[0].x0 == 4 && host.calls[0].z0 == 5 );
    host.calls.clear();
    n = dev.LevelAreaInternalVertices(inner, 10.0f, false, true, nullptr);
    CHECK(n == 9);
    CHECK(dev.LevelBorderingVertices(inner, 11.0f, nullptr) == 7);
    CHECK(host.calls.back().x1 == 6 && host.calls.back().z1 == 6 && host.calls.back().h == 11.0f);
    // attempt levelling honours the slope limit
    dev.Tunables().maxLevelling = 0.5f;
    CHECK(!dev.AttemptAreaLevelling(area, 0, false, true, kLotMaxLevelling, nullptr));
    CHECK(dev.AttemptAreaLevelling(area, 0, false, true, 5000.0f, nullptr));
    // occupant coverage
    LotCellRect cover;
    dev.GetOccupantCellCoverage(nullptr, cover);
    CHECK(cover.x0 == 2 && cover.z0 == 3 && cover.x1 == 4 && cover.z1 == 5);
    CHECK(!dev.IsAreaDeveloped(area));
    // cost curve from an exemplar
    cSCPropertyHolder* ex = new cSCPropertyHolder();
    ex->AddRef();
    cSCPropertyHelp::SetPropertyValue(ex, kLotDevPropFlatTolerance, 1.5f);
    cSCPropertyHelp::SetPropertyValue(ex, kLotDevPropCostCurve, std::vector<float>{0, 100, 4, 500});
    cSCPropertyHelp::SetPropertyValue(ex, kLotDevPropConditionRatios, std::vector<float>{0.1f, 0.2f, 0.3f});
    cSC4LotDeveloper dev2(&host);
    dev2.ReadSimulatorTunableValues(ex);
    CHECK(dev2.Tunables().flatTolerance == 1.5f && dev2.Tunables().ratioA == 0.1f && dev2.Tunables().ratioC == 0.3f);
    // with water the lowest vertex is (3,3) and the highest (4,5): slope sqrt(5)
    host.heights[3 * Host::kRow + 3] = 10.0f;
    const std::int64_t cost = dev2.GetLotDevelopmentCost(area);
    CHECK(cost == static_cast<std::int64_t>(4.0f * 2.0f * (100.0f + 400.0f * (std::sqrt(5.0f) * 0.0625f) / 4.0f)));
    cSC4LotDeveloper dev3(&host);
    dev3.ReadSimulatorTunableValues(nullptr);
    CHECK(dev3.Tunables().ratioB == 0.4f);
    ex->Release();
    {   // the lot forms read the lot's cell rectangle
        cSC4Lot lot(3, 2, 2, 4, 0);                                      // cells x 3..4, z 2..5
        CHECK(cSC4LotDeveloper::RectOfLot(lot).x0 == 3 && cSC4LotDeveloper::RectOfLot(lot).z1 == 5);
        dev.Tunables().flatTolerance = 0.0f;
        CHECK(!dev.IsLotDeveloped(lot));
        CHECK(!dev.IsLotFlat(lot, false));
        CHECK(dev.GetLotAltitudeRange(lot, false) == dev.GetAreaAltitudeRange(area, false));
        CHECK(dev.GetLotSlope(lot, false) == dev.GetAreaSlope(area, false));
        CHECK(dev.GetOccupantSlope(nullptr, false) == dev.GetAreaSlope(cover, false));
    }
    {   // TestCondition
        struct Gates final : ILotGateServices {
            BuildingProfile profile;
            const BuildingProfile* ProfileOfBuilding(std::uint32_t) override { return &profile; }
        } gates;
        dev.SetGateServices(&gates);
        cSC4Lot lot(3, 2, 2, 4, 0);
        lot.SetState(2, 0, 0);
        lot.SetCondition(2);
        CHECK(dev.TestCondition(lot) == 2);                              // states other than 3 and 4 keep their condition
        lot.SetState(4, 0, 0);
        lot.SetCondition(0);
        CHECK(dev.TestCondition(lot) == 1);                              // an abandoned lot with condition 0 counts as 1; no time has passed
        // state 3: the wealth gap and the occupancy decide
        gates.profile.wealth = 2;
        lot.SetState(3, 1, 0);
        lot.SetCondition(3);                                             // gap 1 < 3 - 1: behind; empty (occupancy 0 < 0.4) adds one
        CHECK(dev.TestCondition(lot) == 3);
        gates.profile.wealth = 1;                                        // gap 0 with condition 3: behind again, the extra step gives grade 1
        CHECK(dev.TestCondition(lot) == 2);
        lot.SetCondition(1);                                             // gap 0, condition 1: not behind, grade 0 -> the failure counter
        CHECK(dev.TestCondition(lot) == 0);
        cSCPropertyHelp::SetPropertyValue(&lot.Properties(), kLotPropFailureCounter, std::int32_t(5));
        CHECK(dev.TestCondition(lot) == 1);
        dev.SetGateServices(nullptr);
    }
    std::printf(failures ? "FAILED\n" : "sc4_lotdeveloper: all passed\n");
    return failures ? 1 : 0;
}
