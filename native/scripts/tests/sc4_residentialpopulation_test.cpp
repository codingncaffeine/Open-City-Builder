// The residential simulator's population half (residential.md, OpenSC4 specs): the monthly cycle, the proximity seed with its
// edge defect (5.3 worked example), DoPopulationChange (5.5), PostPopulationChange's history (5.6) and the month message (4.3).
#include <cmath>
#include <cstdio>
#include <map>

#include "sc4/ResidentialSimulator.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/SCProperty.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace sc4;

namespace {

class FakeCity : public ISimGridCity {
public:
    std::int32_t cells = 16;
    std::int32_t CitySizeXInCells() const override { return cells; }
    std::int32_t CitySizeZInCells() const override { return cells; }
    float CellSizeXMeters() const override { return 16.0f; }
    float CellSizeZMeters() const override { return 16.0f; }
};

struct Host : IResidentialHost {
    // the education/health seams (unused here)
    std::uint32_t CurrentDay() override { return 0; }
    void PostStrikeMessage(std::uint32_t, IResidentialOccupant*, bool, std::uint32_t) override {}
    void SetStrikerGroup(bool, IResidentialOccupant*, bool) override {}
    bool CityHasOccupantManager() override { return true; }
    bool IsBudgeted(IResidentialOccupant*, std::uint32_t) override { return false; }
    IBudgetRecord* GetBudgetRecord(IResidentialOccupant*, std::uint32_t) override { return nullptr; }
    void GetSchoolQueryData(IResidentialOccupant*, ResidentialQueryData&) override {}
    void GetHospitalQueryData(IResidentialOccupant*, ResidentialQueryData&) override {}
    void GetOccupantEfficiencyRatings(IResidentialOccupant*, float&, bool) override {}
    std::uint8_t CellWorkforce(unsigned, unsigned) override { return 0; }
    std::uint8_t GlobalWorkforce() override { return 0; }
    float CellLifeExpectancyBase(unsigned, unsigned) override { return 0.0f; }

    // the population half
    cSC4SimGrid<std::int16_t>* developer[3] = {};
    cSC4SimGrid<std::uint8_t>* buildingMap = nullptr;
    cSCPropertyHolder* exemplar = nullptr;
    int month = 1;
    int agents = 0;
    std::map<std::uint32_t, std::int32_t> history;
    bool AcquireServices() override { return true; }
    void AddAgent(cIGZMessageTarget2*, int, const char*, std::uint32_t) override { ++agents; }
    void RecordHistory(std::uint32_t id, std::int32_t value) override { history[id] = value; }
    const cISCPropertyHolder* SimulatorExemplar() override { return exemplar; }
    cSC4SimGrid<std::int16_t>* DeveloperPopulationMap(int wealth) override { return developer[wealth - 1]; }
    const cSC4SimGrid<std::uint8_t>* BuildingMap() override { return buildingMap; }
    int CurrentMonth() override { return month; }
};

// runs the cycle until it waits for the month (state 10); the number of calls it took
int RunCycle(cSC4ResidentialSimulator& sim) {
    int calls = 0;
    while (sim.State() != 10 && calls < 100) {
        sim.DoDailySimulation(1000);
        ++calls;
    }
    return calls;
}

void TestProximitySeed() {
    // 5.3 worked example: 4 x 4 city; building map key 6 at (1,0), (1,1), (2,3), key 7 at (3,0), no key 8.
    FakeCity city;
    city.cells = 4;
    SetSimGridCity(&city);
    cSC4SimGrid<std::uint8_t> map;
    map.AddRef();
    map.SetTractSize(1);
    map.SetTractValue(1, 0, 6);
    map.SetTractValue(1, 1, 6);
    map.SetTractValue(2, 3, 6);
    map.SetTractValue(3, 0, 7);
    Host host;
    host.buildingMap = &map;
    cRZRandom random(1);
    cSC4ResidentialSimulator sim(&host, &random);
    sim.AddRef();
    CHECK(sim.Init());
    CHECK(host.agents == 4);   // levels 0, 10, 6, 4
    RunCycle(sim);
    const std::uint8_t expected[3][4][4] = {
        {{1, 0, 0, 0}, {0, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}},   // R$ (key 6)
        {{1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}, {0, 0, 0, 0}},   // R$$ (key 7)
        {{1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}},   // R$$$ (no key 8)
    };
    for (int w = 0; w < 3; ++w)
        for (int x = 0; x < 4; ++x)
            for (int z = 0; z < 4; ++z) CHECK(sim.GetProximityMap(w + 1)->GetTractValue(x, z) == expected[w][x][z]);
    // no building map: every proximity grid is 0 everywhere
    Host bare;
    cSC4ResidentialSimulator plain(&bare, &random);
    plain.AddRef();
    plain.Init();
    RunCycle(plain);
    int nonzero = 0;
    for (int w = 1; w <= 3; ++w)
        for (const auto& column : plain.GetProximityMap(w)->GetGridData()) for (std::uint8_t v : column) nonzero += v != 0;
    CHECK(nonzero == 0);
}

void TestPopulation() {
    FakeCity city;   // 16 x 16 cells: 4 x 4 tracts of 4 cells
    SetSimGridCity(&city);
    cSC4SimGrid<std::int16_t> maps[3];
    for (auto& m : maps) {
        m.AddRef();
        m.SetTractSize(4);
    }
    cSCPropertyHolder ex;
    cSCPropertyHelp::SetPropertyValue(&ex, 0xc929c433, 0.2f);                                     // EQ decay
    cSCPropertyHelp::SetPropertyValue(&ex, 0x0929c439, 0.2f);                                     // HQ decay
    cSCPropertyHelp::SetPropertyValue(&ex, 0xc929cc58, 20.0f);                                    // R$ newcomer EQ
    cSCPropertyHelp::SetPropertyValue(&ex, 0xc929cc8d, 20.0f);                                    // R$ newcomer HQ
    cSCPropertyHelp::SetPropertyValue(&ex, 0x6941a67f, std::vector<float>{0.0f, 60.0f, 200.0f, 90.0f});   // HQ -> life expectancy
    Host host;
    for (int w = 0; w < 3; ++w) host.developer[w] = &maps[w];
    host.exemplar = &ex;
    cRZRandom random(1);
    cSC4ResidentialSimulator sim(&host, &random);
    sim.AddRef();
    CHECK(sim.Init());
    CHECK(sim.GetPopulationGrid(0)->GetTractSize() == 4 && sim.GetPopulationGrid(0)->GetTractCountX() == 4);

    // one R$ tract of 100: a new city's first cycle
    maps[0].SetTractValue(1, 2, 100);
    CHECK(RunCycle(sim) == 15);   // 0 13 1 14 2 15 3 11 4 12 5 6 7 8 9 -> 10
    CHECK(sim.GetPopulation() == 100);
    CHECK(sim.GetPopulationGrid(0)->GetTractValue(1, 2) == 100);
    // EQ: decayed 0 -> 0; m = 1; ((0 + 20) / 1 * 100 + 0 * 0) / 100 = 20
    CHECK(sim.GetEQGrid(0)->GetTractValue(1, 2) == 20.0f && sim.GetHQGrid(0)->GetTractValue(1, 2) == 20.0f);
    // age: (0 * 0 + 100 * 25) / 100 = 25; LE = curve(HQ 20) = 63 > 25, no reset
    CHECK(sim.GetAgeGrid()->GetTractValue(1, 2) == 25);
    CHECK(host.history[0x2a5e7009] == 63);                          // leSum 6300 / 100
    // age bucket (25 - 20) / 3 = 1 -> curve(24) = 24 (default) -> n = 24 of 100; average EQ 20
    CHECK(host.history[0xca545cab] == 24 && host.history[0xca545cca] == 20);
    CHECK(host.history[0xaa1a2cca] == 100 && host.history[0xaa1a2864] == 20 && host.history[0xaa1a2866] == 20);
    CHECK(sim.GetCellEQ(4, 8) == 20.0f && sim.GetCellEQ(0, 0) == 0.0f);

    // the month restarts the cycle only from state 10
    cRZMessage2Standard month;
    month.AddRef();
    month.SetType(0x66956816);
    sim.DoMessage(&month);
    CHECK(sim.State() == 0);
    sim.DoMessage(&month);   // not idle: the cycle continues
    CHECK(sim.State() == 0);

    // second month, December: the residents age a year; EQ decays (no newcomers)
    host.month = 12;
    RunCycle(sim);
    CHECK(sim.GetAgeGrid()->GetTractValue(1, 2) == 26);
    CHECK(std::fabs(sim.GetEQGrid(0)->GetTractValue(1, 2) - 19.8f) < 1e-5f);

    // newcomers blend: 100 -> 200, EQ_old 19.6 after decay, m = 2: (((19.6 + 20) / 2) * 100 + 100 * 19.6) / 200 = 19.7
    host.month = 1;
    sim.DoMessage(&month);
    maps[0].SetTractValue(1, 2, 200);
    RunCycle(sim);
    CHECK(sim.GetPopulation() == 200);
    CHECK(std::fabs(sim.GetEQGrid(0)->GetTractValue(1, 2) - 19.7f) < 1e-4f);
    // age (100 * 26 + 100 * 25) / 200 = 25
    CHECK(sim.GetAgeGrid()->GetTractValue(1, 2) == 25);

    // everyone leaves: population, EQ, HQ and age go to 0
    sim.DoMessage(&month);
    maps[0].SetTractValue(1, 2, 0);
    RunCycle(sim);
    CHECK(sim.GetPopulation() == 0 && sim.GetEQGrid(0)->GetTractValue(1, 2) == 0.0f && sim.GetAgeGrid()->GetTractValue(1, 2) == 0);
    CHECK(host.history[0x2a5e7009] == 0 && host.history[0xaa1a2cca] == 0);

    // generational reset: LE below the age resets it to 40 and scales EQ / HQ by 0.79999995 (80 x 0.01f)
    cSCPropertyHelp::SetPropertyValue(&ex, 0x6941a67f, std::vector<float>{0.0f, 10.0f, 200.0f, 10.0f});
    cSCPropertyHelp::SetPropertyValue(&ex, 0xca027325, 80.0f);
    sim.DoMessage(&month);
    maps[0].SetTractValue(1, 2, 100);
    RunCycle(sim);
    CHECK(sim.GetAgeGrid()->GetTractValue(1, 2) == 40);
    CHECK(sim.GetEQGrid(0)->GetTractValue(1, 2) == 20.0f * (80.0f * 0.01f));
    SetSimGridCity(nullptr);
}

}  // namespace

int main() {
    TestProximitySeed();
    TestPopulation();
    if (failures) return 1;
    std::printf("sc4_residentialpopulation: all passed\n");
    return 0;
}
