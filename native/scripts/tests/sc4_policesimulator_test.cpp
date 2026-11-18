#include <cmath>
#include <cstdio>
#include <map>

#include "sc4/PoliceSimulator.h"
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

struct Building : IPoliceOccupant {
    cSCPropertyHolder props;
    float x, z;
    bool budget = false;
    Building(float px, float pz) : x(px), z(pz) {}
    std::uint32_t OccupantType() override { return kProtectionBuildingOccupantType; }
    void Position(float p[3]) override { p[0] = x; p[1] = 0; p[2] = z; }
    cISCPropertyHolder* PropertyHolder() override { return &props; }
    bool IsPropOccupant() override { return budget; }
};

struct Host : IPoliceHost {
    int day = 100;
    std::map<std::uint32_t, int> messages;
    std::map<std::uint32_t, std::int64_t> history;
    float funding = 1.0f;
    bool hasRecord = true;
    int strikerAdds = 0, strikerRemoves = 0, jailbreakAdds = 0, jailbreakRemoves = 0;
    int CurrentDay() override { return day; }
    void PostMessage(std::uint32_t m, IPoliceOccupant*) override { ++messages[m]; }
    void AddHistory(std::uint32_t id, std::int64_t v) override { history[id] = v; }
    bool FundingFraction(IPoliceOccupant*, std::uint32_t, float& f) override { f = funding; return hasRecord; }
    std::uint32_t fundingInfoProperty = 0;
    bool FundingInfo(IPoliceOccupant*, std::uint32_t property, int& cost, float& f) override {
        fundingInfoProperty = property; cost = 4321; f = funding; return hasRecord;
    }
    float DepartmentBudgetFraction() override { return funding; }
    void SetStrikerGroup(IPoliceOccupant*, bool add) override { (add ? strikerAdds : strikerRemoves)++; }
    void SetJailbreakGroup(IPoliceOccupant*, bool add) override { (add ? jailbreakAdds : jailbreakRemoves)++; }
    void RegisterFundedStation(IPoliceOccupant*, std::uint32_t, bool) override {}
    int AvailableDispatchCount(std::uint32_t) override { return 5; }
    std::vector<std::uint32_t> ActiveDispatches(std::uint32_t) override { return {}; }
    bool GetDispatch(std::uint32_t, ProtectionDispatchInfo&) override { return false; }
    bool LotAllowsDispatch(IProtectionStation*) override { return true; }
    bool IsAboveWater(float, float) override { return true; }
    bool IsZonedAt(int, int) override { return true; }
    void TakeStationDispatch(IProtectionStation*) override {}
};

}  // namespace

int main() {
    FakeCity city;
    SetSimGridCity(&city);
    Host host;
    cSC4PoliceSimulator sim(&host);
    sim.Protection().CreateGrids(16, 16);
    sim.Protection().zoneColumns.assign(16, std::vector<std::int8_t>(16, 1));

    // efficiency: curve (0,0)(100,100)(120,110)
    Building station(32, 32);
    cSCPropertyHelp::SetPropertyValue(&station.props, kPoliceStationEffectPeak, std::int32_t(100));
    cSCPropertyHelp::SetPropertyValue(&station.props, kPoliceStationEffectEdge, std::int32_t(10));
    cSCPropertyHelp::SetPropertyValue(&station.props, kPoliceStationRadius, 64.0f);
    cSCPropertyHelp::SetPropertyValue(&station.props, kPoliceStationDispatchCount, std::uint32_t(3));
    CHECK(sim.IsPoliceStation(&station) && !sim.IsJail(&station));
    host.funding = 0.5f;
    CHECK(std::fabs(sim.GetStationEfficiency(&station) - 0.5f) < 1e-5f);
    host.funding = 1.1f;
    CHECK(std::fabs(sim.GetStationEfficiency(&station) - 1.05f) < 1e-5f);
    host.hasRecord = false;
    CHECK(sim.GetStationEfficiency(&station) == 0.0f);
    host.hasRecord = true;
    host.funding = 1.0f;

    ProtectionEffect effect;
    CHECK(sim.ProtectionGetStationEffect(&station, effect));
    CHECK(effect.peak == 100 && effect.radius == 64.0f && effect.x == 32.0f && effect.capacity == 100);
    float covered = -1, strength = -1;
    SC4Percentage half{0.5f};
    CHECK(sim.GetOccupantCoverage(&station, half, covered, strength));
    CHECK(strength == 64.0f && std::fabs(covered - 50.0f * 64.0f / 90.0f) < 1e-3f);   // used 50 lies between edge 10 and peak 100
    SC4Percentage over{2.0f};
    CHECK(sim.GetOccupantCoverage(&station, over, covered, strength) && covered == 0.0f);

    sim.DoOccupantInserted(&station);
    CHECK(sim.GetPoliceStationCount() == 1 && host.messages[kPoliceMessageStationsChanged] == 1);
    CHECK(sim.GetTotalDispatchCount() == 3);
    std::uint32_t mask = 0, base = 0, count = 0;
    CHECK(sim.GetDispatchesAtStation(&station, &mask, &base, &count) && count == 3 && base == 1 && mask == 7);
    CHECK(sim.GetNearestStation(200, 200) == &station);

    // jails and criminals
    Building jail(100, 100);
    cSCPropertyHelp::SetPropertyValue(&jail.props, kPoliceJailCapacity, std::uint32_t(100));
    CHECK(sim.IsJail(&jail));
    sim.DoOccupantInserted(&jail);
    CHECK(sim.GetJailCount() == 1 && sim.GetJailCapacity() == 100);
    CHECK(host.messages[kPoliceMessageStationsChanged] == 1);   // a jail is not a station: no 0xa949569a
    sim.DoOccupantChanged(&jail);
    CHECK(host.messages[kPoliceMessageStationsChanged] == 1);
    sim.DoOccupantChanged(&station);
    CHECK(host.messages[kPoliceMessageStationsChanged] == 2);
    int cost = 0;
    CHECK(sim.ProtectionGetStationFunding(&station, nullptr));
    CHECK(sim.ProtectionGetStationFunding(&station, &cost) && cost == 4321 &&
          host.fundingInfoProperty == kPoliceStationFundingProperty);
    sim.SetCriminalCount(50);
    sim.RecalculateCriminalStatistics();
    CHECK(sim.GetJailPopulation() == 50 && sim.GetJailedPercentage() == 0.5f);
    sim.SetCriminalCount(250);
    sim.RecalculateCriminalStatistics();
    CHECK(sim.GetJailPopulation() == 100 && sim.GetJailedPercentage() == 1.0f);
    CHECK(sim.ResetUnjailedCriminals() == 250 - 110);        // 100 * 1.1 stay free-roaming allowance
    CHECK(sim.GetCriminalCount() == 110);
    sim.DoMonthlyCriminalRehabilitation();                   // 110 * 0.01 rounds to 1
    CHECK(sim.GetCriminalCount() == 109);

    // monthly crime statistics
    sim.AddCrimes(7);
    sim.AddArrests(3);
    sim.ProcessMonthlyCrimeStats();
    CHECK(sim.GetCriminalCount() == 112 && sim.GetArrestCount() == 3 && sim.GetCrimeCount() == 7);
    CHECK(host.history[kPoliceHistoryCrimes] == 7 && host.history[kPoliceHistoryArrests] == 3);
    sim.UpdatePoliceStationCrimeStats(&station, cSC4PoliceSimulator::kArrest);
    sim.UpdatePoliceStationCrimeStats(&station, cSC4PoliceSimulator::kCrime);
    CHECK(sim.GetPoliceStationCrimeCount(&station) == 0 && sim.GetPoliceStationArrestCount(&station) == 0);   // rolled stats only
    sim.UpdatePoliceStationCrimeStats(&station, cSC4PoliceSimulator::kRollover);
    CHECK(sim.GetPoliceStationCrimeCount(&station) == 2 && sim.GetPoliceStationArrestCount(&station) == 1);

    // jailbreak
    CHECK(sim.GetChanceOfJailbreak(&jail) > 0.0f);           // jail is full (100%): curve(100) = 100%
    CHECK(sim.BeginJailbreak(&jail) && sim.IsInJailbreak(&jail) && !sim.BeginJailbreak(&jail));
    CHECK(sim.GetChanceOfJailbreak(&jail) == 0.0f);
    CHECK(sim.GetCriminalCount() < 112);                     // inmates escaped
    sim.SetCriminalCount(0);
    sim.RecalculateCriminalStatistics();
    sim.CheckForJailbreakEnd();                              // 0% jailed < 50% threshold
    CHECK(!sim.IsInJailbreak(&jail) && host.messages[kPoliceMessageJailbreakEnd] == 1);

    // strikes: very low funding makes the curve (20% at 0) fire eventually; with funding 0 chance is 20%
    host.funding = 0.0f;
    CHECK(std::fabs(sim.GetChanceOfStrike() - 0.2f) < 1e-5f);
    bool started = false;
    for (int i = 0; i < 100 && !started; ++i) started = sim.UpdateStrikingState(&station);
    CHECK(started && cSC4PoliceSimulator::IsStationOnStrike(&station) && host.strikerAdds == 1);
    CHECK(sim.BeginStrike(&station) && sim.IsOnStrike() && !sim.BeginStrike(&station));
    CHECK(sim.GetStrikeLength() == 0 && sim.GetChanceOfStrike() == 1.0f);
    host.funding = 1.0f;
    CHECK(sim.GetStationEfficiency(&station) == 1.0f * 0.5f);   // striking stations work at half efficiency
    host.day += 5;
    CHECK(sim.UpdateStrikingState(&station) && !cSC4PoliceSimulator::IsStationOnStrike(&station));   // funding back above 0.9 ends it
    CHECK(host.strikerRemoves == 1 && sim.EndStrike() && !sim.IsOnStrike() && sim.GetStrikeLength() == -1);
    {   // list copies and raw values
        std::list<IProtectionStation*> stations{nullptr};
        std::list<IPoliceOccupant*> jails;
        const int stationCount = sim.GetPoliceStations(stations);
        CHECK(stationCount == int(sim.Protection().Stations().size()) && int(stations.size()) == stationCount);   // replaced, not appended
        CHECK(sim.GetJails(jails) == int(sim.Jails().size()) && jails.size() == sim.Jails().size());
        sim.SetPoliceRawValues(0x1122334455667788ull, 0x99);
        CHECK(sim.GetJailDepartmentBudget() == 0x1122334455667788ull && sim.GetPolicePowerGrid() == 0x99);
    }
    std::printf(failures ? "FAILED\n" : "sc4_policesimulator: all passed\n");
    return failures ? 1 : 0;
}
