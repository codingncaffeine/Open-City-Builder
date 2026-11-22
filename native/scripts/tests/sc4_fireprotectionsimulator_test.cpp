#include <cmath>
#include <cstdio>
#include <map>

#include "sc4/AircraftDispatchSource.h"
#include "sc4/FireProtectionSimulator.h"
#include "sc4/SCProperty.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace sc4;

namespace {

class FakeCity : public ISimGridCity {
public:
    std::int32_t CitySizeXInCells() const override { return 16; }
    std::int32_t CitySizeZInCells() const override { return 16; }
    float CellSizeXMeters() const override { return 16.0f; }
    float CellSizeZMeters() const override { return 16.0f; }
};

struct Building : IFireOccupant {
    cSCPropertyHolder props;
    float x, z;
    bool budget = false;
    Building(float px, float pz) : x(px), z(pz) {}
    std::uint32_t OccupantType() override { return kProtectionBuildingOccupantType; }
    void Position(float p[3]) override { p[0] = x; p[1] = 0; p[2] = z; }
    cISCPropertyHolder* PropertyHolder() override { return &props; }
    bool IsPropOccupant() override { return budget; }
};

struct Host : IFireHost {
    int day = 100;
    std::map<std::uint32_t, int> messages;
    std::map<std::uint32_t, std::int64_t> history;
    float funding = 1.0f, police = 0.5f;
    bool hasRecord = true, lotAllowed = true;
    int strikerAdds = 0, strikerRemoves = 0, registered = 0, unregistered = 0;
    float setFraction = -1;
    int CurrentDay() override { return day; }
    void PostMessage(std::uint32_t m, IFireOccupant*) override { ++messages[m]; }
    void AddHistory(std::uint32_t id, std::int64_t v) override { history[id] = v; }
    bool FundingFraction(IFireOccupant*, std::uint32_t, float& f) override { f = funding; return hasRecord; }
    bool FundingInfo(IFireOccupant*, std::uint32_t, int& cost, float& f) override { cost = 1234; f = funding; return hasRecord; }
    bool SetFundingFraction(IFireOccupant*, std::uint32_t, const SC4Percentage& p) override { setFraction = p.value; return hasRecord; }
    float DepartmentBudgetFraction() override { return funding; }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> departments;
    float departmentCap = 0.0f;
    void EnsureDepartment(std::uint32_t id, std::uint32_t name, float maxFunding) override { departments.push_back({id, name}); departmentCap = maxFunding; }
    void* DepartmentBudget() override { return this; }
    void SetStrikerGroup(IFireOccupant*, bool add) override { (add ? strikerAdds : strikerRemoves)++; }
    void RegisterFundedStation(IFireOccupant*, std::uint32_t, bool add) override { (add ? registered : unregistered)++; }
    float PoliceCoverageShare() override { return police; }
    int AvailableDispatchCount(std::uint32_t) override { return 5; }
    std::vector<std::uint32_t> ActiveDispatches(std::uint32_t) override { return {}; }
    bool GetDispatch(std::uint32_t, ProtectionDispatchInfo&) override { return false; }
    bool LotAllowsDispatch(IProtectionStation*) override { return lotAllowed; }
    bool IsAboveWater(float, float) override { return true; }
    bool IsZonedAt(int, int) override { return true; }
    void TakeStationDispatch(IProtectionStation*) override {}
};

}  // namespace

int main() {
    FakeCity city;
    SetSimGridCity(&city);
    Host host;
    cSC4FireProtectionSimulator sim(&host);
    sim.Protection().CreateGrids(16, 16);
    sim.Protection().zoneColumns.assign(16, std::vector<std::int8_t>(16, 1));
    CHECK(sim.GetGZCLSID() == 0x2990c05a && sim.GetFireProtectionGrid() == sim.Protection().AdjustedGrid());
    CHECK(sim.CanHandleDispatchType(1) && !sim.CanHandleDispatchType(0) && sim.ProtectionGetDispatchType() == 1);

    Building station(32, 32);
    cSCPropertyHelp::SetPropertyValue(&station.props, kFireStationEffectPeak, std::int32_t(100));
    cSCPropertyHelp::SetPropertyValue(&station.props, kFireStationEffectEdge, std::int32_t(10));
    cSCPropertyHelp::SetPropertyValue(&station.props, kFireStationRadius, 64.0f);
    cSCPropertyHelp::SetPropertyValue(&station.props, kFireStationDispatchCount, std::uint32_t(3));
    CHECK(sim.IsFireStation(&station));

    // efficiency: curve (0,0)(100,100)(120,110), x0.8 without road access, x0.5 on strike
    host.funding = 0.5f;
    CHECK(std::fabs(sim.GetStationEfficiency(&station) - 0.5f) < 1e-5f);
    host.funding = 1.1f;
    CHECK(std::fabs(sim.GetStationEfficiency(&station) - 1.05f) < 1e-5f);
    host.hasRecord = false;
    CHECK(sim.GetStationEfficiency(&station) == 0.0f);
    host.hasRecord = true;
    host.funding = 1.0f;
    host.lotAllowed = false;
    CHECK(std::fabs(sim.GetStationEfficiency(&station) - 0.8f) < 1e-6f);
    host.lotAllowed = true;

    // funding access
    int cost = 0; SC4Percentage pct;
    CHECK(sim.GetStationFunding(&station, &cost, &pct) && cost == 1234 && pct.value == 1.0f);
    CHECK(sim.SetStationFunding(&station, SC4Percentage{0.25f}) && host.setFraction == 0.25f);
    int c2 = 0;
    CHECK(sim.ProtectionGetStationFunding(&station, &c2) && c2 == 1234 && sim.ProtectionGetStationFunding(&station, nullptr));
    host.hasRecord = false;
    CHECK(!sim.GetStationFunding(&station, &cost, &pct) && !sim.ProtectionGetStationFunding(&station, &c2));
    host.hasRecord = true;

    ProtectionEffect effect;
    CHECK(sim.ProtectionGetStationEffect(&station, effect));
    CHECK(effect.peak == 100 && effect.radius == 64.0f && effect.x == 32.0f && effect.capacity == 100);
    station.budget = true; host.funding = 0.5f;
    CHECK(sim.ProtectionGetStationEffect(&station, effect) && effect.radius == 32.0f);   // department budget path: curve(50)/100
    station.budget = false; host.funding = 1.0f;
    float covered = -1, strength = -1;
    CHECK(sim.GetOccupantCoverage(&station, SC4Percentage{0.5f}, covered, strength));
    CHECK(strength == 64.0f && std::fabs(covered - 50.0f * 64.0f / 90.0f) < 1e-3f);
    CHECK(sim.GetOccupantCoverage(&station, SC4Percentage{2.0f}, covered, strength) && covered == 0.0f);

    // dispatch effect (no properties yet -> false), radius 0
    CHECK(!sim.ProtectionGetDispatchEffect(&station, effect) && sim.GetStationDispatchRadius(&station) == 0.0f);
    cSCPropertyHelp::SetPropertyValue(&station.props, kFireDispatchPeak, std::int32_t(40));
    cSCPropertyHelp::SetPropertyValue(&station.props, kFireDispatchEdge, std::int32_t(5));
    cSCPropertyHelp::SetPropertyValue(&station.props, kFireDispatchRadius, 20.0f);
    CHECK(sim.ProtectionGetDispatchEffect(&station, effect) && effect.peak == 40 && effect.capacity == 40);
    CHECK(sim.GetStationDispatchRadius(&station) == 20.0f);

    sim.DoOccupantInserted(&station);
    CHECK(sim.GetFireStationCount() == 1 && host.registered == 1);
    CHECK(sim.GetTotalDispatchCount() == 3);
    sim.ProtectionSetStationBaseCallNumber(&station, 5);
    sim.ProtectionSetStationAvailabilityFlags(&station, 0xff);
    std::uint32_t mask = 0, base = 0, count = 0;
    CHECK(sim.GetDispatchesAtStation(&station, &mask, &base, &count) && count == 3 && base == 5 && mask == 7);
    std::list<IFireOccupant*> stations;
    CHECK(sim.GetFireStations(stations) == 1 && stations.front() == &station);

    std::uint32_t type = 0; FireResourceKey model{1, 2, 3}, second{9, 8, 7};
    CHECK(sim.GetDispatchAutomataProperties(nullptr, type, model, second));
    CHECK(type == 0x4202 && model.type == 0 && model.group == 0 && model.instance == 0 && second.type == 0x11882 && second.group == 8);

    // coverage history
    sim.ProtectionEndOfCycle();
    CHECK(host.messages[kFireMessageCoverage] == 1 && host.history.count(kFireHistoryCombinedCoverage) == 1);
    CHECK(host.history[kFireHistoryCombinedCoverage] == static_cast<int>((0.5f * 100.0f + sim.GetPercentCoverage() * 100.0f) * 0.5f));

    // strikes: funding 0 -> 20% chance per check from the curve (percent scale)
    host.funding = 0.0f;
    CHECK(std::fabs(sim.GetChanceOfStrike() - 0.2f) < 1e-5f);
    bool started = false;
    for (int i = 0; i < 200 && !started; ++i) started = sim.UpdateStrikingState(&station);
    CHECK(started && cSC4FireProtectionSimulator::IsStationOnStrike(&station) && host.strikerAdds == 1);
    CHECK(sim.BeginStrike(&station) && sim.IsOnStrike() && !sim.BeginStrike(&station));
    CHECK(sim.GetStrikeStartDate() == 100 && sim.GetStrikeLength() == 0 && sim.GetChanceOfStrike() == 1.0f);
    std::list<IFireOccupant*> strikers;
    CHECK(sim.GetStrikingStations(strikers) == 1);
    host.funding = 1.0f;
    CHECK(sim.GetStationEfficiency(&station) == 1.0f * 0.5f);
    host.day += 5;
    CHECK(sim.UpdateStrikingState(&station) && !cSC4FireProtectionSimulator::IsStationOnStrike(&station));   // funding above 0.9 ends it
    CHECK(host.strikerRemoves == 1 && sim.EndStrike() && !sim.IsOnStrike() && sim.GetStrikeLength() == -1);
    CHECK(host.messages[kFireMessageStrikeBegin] == 1 && host.messages[kFireMessageStrikeEnd] == 1);
    // strike expires by itself after 60 days even with low funding
    host.funding = 0.0f;
    cSCPropertyHelp::SetPropertyValue(&station.props, kFireStrikeStartDate, std::uint32_t(1));
    host.day = 1 + 61;
    CHECK(sim.UpdateStrikingState(&station) && !cSC4FireProtectionSimulator::IsStationOnStrike(&station));

    // monthly: a state change starts the city strike, later all-clear ends it
    cSCPropertyHelp::SetPropertyValue(&station.props, kFireStrikeStartDate, std::uint32_t(0));
    host.funding = 0.0f;
    for (int i = 0; i < 200 && !sim.IsOnStrike(); ++i) {
        cSCPropertyHelp::SetPropertyValue(&station.props, kFireStrikeStartDate, std::uint32_t(0));
        sim.DoMonthlySimulation();
    }
    CHECK(sim.IsOnStrike());
    host.funding = 1.0f; host.day += 5;
    sim.DoMonthlySimulation();                               // station returns to work -> city strike ends
    CHECK(!sim.IsOnStrike());

    sim.DoOccupantRemoved(&station);
    CHECK(sim.GetFireStationCount() == 0 && host.unregistered == 1);
    // Init makes the fire plane dispatch source (class 0x4c0ae114) and initializes it; Shutdown shuts it down and lets it go.
    CHECK(sim.PlaneDispatchSource() == nullptr);
    sim.Init();
    CHECK(host.departments.size() == 1 && host.departments[0].first == 0x28f55a9f && host.departments[0].second == 0xaa4935fe && host.departmentCap == 1.2f);
    CHECK(sim.PlaneDispatchSource() && sim.PlaneDispatchSource()->IsInitialized() && sim.PlaneDispatchSource()->GetGZCLSID() == 0x4c0ae114);
    CHECK(sim.PlaneDispatchSource() && sim.PlaneDispatchSource()->CanHandleDispatchType(5));
    sim.Shutdown();
    CHECK(sim.PlaneDispatchSource() == nullptr);
    std::printf(failures ? "FAILED\n" : "sc4_fireprotectionsimulator: all passed\n");
    return failures ? 1 : 0;
}
