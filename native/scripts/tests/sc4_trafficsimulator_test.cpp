#include <cfloat>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>

#include "sc4/Lot.h"
#include "sc4/PathFinder.h"
#include "sc4/SCProperty.h"
#include "sc4/TrafficSimulator.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace sc4;

namespace {

// One cISC4Lot as the traffic simulator sees it: the property holder that carries the trip capacities and the
// occupied rectangle.
struct FakeLot : ITrafficLot {
    cSCPropertyHolder props;
    SC4RectInt32 rect{};
    std::list<cSC4TripData> trips;
    int refreshes = 0;
    sc4::cISCPropertyHolder* Properties() override { return &props; }
    void OccupiedRect(SC4RectInt32& out) override { out = rect; }
    void* TrafficSource() override { return this; }
    std::list<cSC4TripData>* TripList(void*) override { return &trips; }
    void RefreshTrafficSource(void*) override { ++refreshes; }
};

// One cISC4NeighborConnection.
struct FakeConnection {
    cSCPropertyHolder props;
    void* city = nullptr;
    int x = 0, z = 0;
};

// One cISC4RegionalCity.
struct FakeCity {
    bool established = true;
    int regionX = 0, regionZ = 0;
    std::map<std::uint32_t, int> populations;
};

// cISC4PathFinder: records what SetupTrip configures.
struct FakePathFinder : cISC4PathFinder {
    struct Call {
        std::string name;
        float f = 0.0f;
        std::uint32_t u = 0;
        const void* p = nullptr;
        SC4RectInt32 rect{};
        bool b = false;
    };
    std::vector<Call> calls;
    void Add(std::string name) { calls.push_back({std::move(name)}); }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 0; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    void SetFlag(std::uint32_t flag) override { Call c; c.name = "SetFlag"; c.u = flag; calls.push_back(c); }
    void ClearFlag(std::uint32_t) override {}
    void ClearAllFlags() override { Add("ClearAllFlags"); }
    void SetRouteInfo(std::deque<RouteData>*) override {}
    void SetTripDestinationFilter(int filter) override {
        Call c; c.name = "SetTripDestinationFilter"; c.u = std::uint32_t(filter); calls.push_back(c);
    }
    void SetDestinationFlag(std::uint8_t flag) override { Call c; c.name = "SetDestinationFlag"; c.u = flag; calls.push_back(c); }
    void ClearDestinationFlag(std::uint8_t) override {}
    void ClearAllDestinationFlags() override { Add("ClearAllDestinationFlags"); }
    void SetTravelTypeFlag(std::uint16_t flag, bool alsoStart) override {
        Call c; c.name = "SetTravelTypeFlag"; c.u = flag; c.b = alsoStart; calls.push_back(c);
    }
    void ClearTravelTypeFlag(std::uint16_t) override {}
    void ClearAllTravelTypeFlags() override { Add("ClearAllTravelTypeFlags"); }
    void SetStartingCostArray(float* costs) override { Call c; c.name = "SetStartingCostArray"; c.p = costs; calls.push_back(c); }
    void SetMaxTripLength(float length) override { Call c; c.name = "SetMaxTripLength"; c.f = length; calls.push_back(c); }
    void SetTrafficDensity(std::uint32_t density) override { Call c; c.name = "SetTrafficDensity"; c.u = density; calls.push_back(c); }
    void SetStartRect(SC4RectInt32 rect) override { Call c; c.name = "SetStartRect"; c.rect = rect; calls.push_back(c); }
    void SetEndRect(SC4RectInt32 rect) override { Call c; c.name = "SetEndRect"; c.rect = rect; calls.push_back(c); }
    void SetMaxWorkTime(std::uint32_t milliseconds) override { Call c; c.name = "SetMaxWorkTime"; c.u = milliseconds; calls.push_back(c); }
    void Reset() override {}
    std::uint32_t GetTrafficArrived() override { return 0; }
    float GetTripLength() override { return 0.0f; }
    float GetTripLengthScaledForDisplay() override { return 0.0f; }
    void GetEndCell(int& x, int& z) const override { x = 0; z = 0; }
    std::uint8_t GetLastTravelType() const override { return 0; }
    int FindPath() override { return 0; }
    bool WasTripReversible() override { return true; }
    void GetCompressedRoute(std::vector<std::uint8_t>& out, std::uint32_t& types) override { out.clear(); types = 0; }
};

const FakePathFinder::Call* FindCall(const FakePathFinder& pf, const char* name) {
    for (const FakePathFinder::Call& c : pf.calls)
        if (c.name == name) return &c;
    return nullptr;
}

struct Host : ITrafficHost {
    std::map<std::uint32_t, int> messages;
    std::vector<std::array<int, 3>> potholes;
    std::map<int, int> road, transit;
    float funding = 1.0f;
    bool rightHand = true;
    std::map<std::uint32_t, short> grids;
    std::vector<std::vector<std::int32_t>> ordinances;

    // lots by cell, separately per lot-manager filter
    std::map<std::pair<int, int>, FakeLot*> lots[2];
    // neighbour connections: FindConnection keyed by (type, x, z)
    std::map<std::tuple<int, int, int>, FakeConnection*> connections;
    std::map<void*, FakeConnection*> otherSide;
    std::vector<void*> allConnections;
    std::vector<void*> connectedCities;
    std::map<void*, FakeCity*> cities;
    std::map<std::uint32_t, int> demandTotals;
    SC4RectInt32 sourceRect{};
    // GetTransitSwitchQueryData: the switch grid, the switch/lot property holders and the budget
    std::map<std::pair<int, int>, std::uint32_t> switchGrid;
    cSCPropertyHolder switchHolder, lotHolder;
    bool lotHasBuilding = true, itemOk = true, departmentOk = true;
    std::uint32_t itemDepartment = 4, lastPurpose = 0, lastDepartment = 0;
    std::int64_t itemCost = 0;
    float departmentFraction = 1.0f;
    const sc4::cISCPropertyHolder* lastHolder = nullptr;
    std::uint32_t TransitSwitchIdAtCell(int x, int z) override { auto it = switchGrid.find({x, z}); return it == switchGrid.end() ? 0 : it->second; }
    sc4::cISCPropertyHolder* TransitSwitchProperties(void*) override { return &switchHolder; }
    sc4::cISCPropertyHolder* LotBuildingProperties(ITrafficLot*) override { return lotHasBuilding ? &lotHolder : nullptr; }
    bool BudgetItemForPurpose(sc4::cISCPropertyHolder* holder, std::uint32_t purpose, std::uint32_t& department, std::int64_t& cost) override {
        lastHolder = holder; lastPurpose = purpose; department = itemDepartment; cost = itemCost; return itemOk;
    }
    bool DepartmentFundingFraction(std::uint32_t department, float& fraction) override { lastDepartment = department; fraction = departmentFraction; return departmentOk; }

    void PostMessage(std::uint32_t m) override { ++messages[m]; }
    void PostPotholeMessage(int x, int z, bool on) override { potholes.push_back({x, z, on}); }
    void SetRoadBudgetExpense(int line, int amount) override { road[line] = amount; }
    void SetMassTransitBudgetExpense(int line, int amount) override { transit[line] = amount; }
    float MassTransitFundingFraction() override { return funding; }
    float CityCellSize() override { return 16.0f; }
    bool IsRightHandTraffic() override { return rightHand; }
    short DemandGridValue(std::uint32_t id, int, int) override { auto it = grids.find(id); return it == grids.end() ? 0 : it->second; }
    void ActiveOrdinanceStrategyAdjustments(std::vector<std::vector<std::int32_t>>& out) override { out = ordinances; }
    ITrafficLot* LotAt(int x, int z, int filter) override {
        auto it = lots[filter != 0 ? 1 : 0].find({x, z});
        return it == lots[filter != 0 ? 1 : 0].end() ? nullptr : it->second;
    }
    void* NeighborFindConnection(int connectionType, int x, int z) override {
        auto it = connections.find({connectionType, x, z});
        return it == connections.end() ? nullptr : it->second;
    }
    void* NeighborFindConnectionOnOtherSide(void* connection) override {
        auto it = otherSide.find(connection);
        return it == otherSide.end() ? nullptr : it->second;
    }
    void NeighborGetAllConnections(std::vector<void*>& out) override { out = allConnections; }
    void NeighborGetConnectedCities(std::vector<void*>& out) override { out = connectedCities; }
    sc4::cISCPropertyHolder* NeighborConnectionProperties(void* connection) override {
        return connection ? &static_cast<FakeConnection*>(connection)->props : nullptr;
    }
    void NeighborConnectionCell(void* connection, int& x, int& z) override {
        x = static_cast<FakeConnection*>(connection)->x;
        z = static_cast<FakeConnection*>(connection)->z;
    }
    void* NeighborConnectionCity(void* connection) override { return static_cast<FakeConnection*>(connection)->city; }
    bool NeighborCityEstablished(void* city) override { return static_cast<FakeCity*>(city)->established; }
    void NeighborCityRegionCell(void* city, int& x, int& z) override {
        x = static_cast<FakeCity*>(city)->regionX;
        z = static_cast<FakeCity*>(city)->regionZ;
    }
    int NeighborCityExtrapolatedPopulation(void* city, std::uint32_t purpose) override {
        auto it = static_cast<FakeCity*>(city)->populations.find(purpose);
        return it == static_cast<FakeCity*>(city)->populations.end() ? 0 : it->second;
    }
    int RegionDemandTotal(std::uint32_t gridId) override {
        auto it = demandTotals.find(gridId);
        return it == demandTotals.end() ? 0 : it->second;
    }
    SC4RectInt32 TrafficSourceStartRect(void*) override { return sourceRect; }
};

}  // namespace

int main() {
    Host host;
    cRZRandom random(12345);
    cSC4TrafficSimulator sim(&host, &random);
    CHECK(sim.GetSimulatorType() == 0x88027f0d);
    CHECK(sim.GetGZCLSID() == 0x6990c1aa);

    // ---- cell set
    CellSet set;
    CHECK(set.Insert(5) && set.Insert(9) && set.Insert(7) && !set.Insert(5));
    CHECK(set.Erase(5) && !set.Erase(5) && set.Size() == 2 && set.Contains(9) && set.Contains(7));

    // ---- network info grid
    sim.InitGrid(8, 8);
    CHECK(sim.GetNetworkInfo(1, 2, 3) == nullptr);
    NetworkInfo* a = sim.AddNetworkInfo(1, 2, 3);
    a->inverseCapacity = 0.25f;
    a->connectionCount = 3;
    CHECK(sim.AddNetworkInfo(1, 2, 3) == a);
    CHECK(sim.GetCapacity(1, 2, 3) == 4.0f);
    CHECK(sim.GetConnectionCount(1, 2, 3) == 3);
    CHECK(sim.GetCapacity(2, 2, 3) == 0.0f && sim.GetConnectionCount(2, 2, 3) == 0);
    sim.AddNetworkInfo(2, 2, 3);
    sim.RemoveNetworkInfo(1, 2, 3);
    CHECK(sim.GetNetworkInfo(1, 2, 3) == nullptr && sim.GetNetworkInfo(2, 2, 3) != nullptr);
    CHECK(sim.GetNetworkInfo(1, -1, 0) == nullptr && sim.GetNetworkInfo(1, 99, 0) == nullptr);

    // ---- tunables from an exemplar
    cSCPropertyHolder exemplar;
    cSCPropertyHelp::SetPropertyValue(&exemplar, 0x491332e6, 2.5f);
    cSCPropertyHelp::SetPropertyValue(&exemplar, 0x491332e7, std::vector<float>{60.0f, 0.0f, 30.0f});   // travel type 0
    cSCPropertyHelp::SetPropertyValue(&exemplar, 0x6a84493e, std::vector<float>{1.0f, 2.0f});
    cSCPropertyHelp::SetPropertyValue(&exemplar, 0xa92356b9, std::vector<float>{0.0f, 1.0f, 2.0f, 4.0f});  // congestion curve points (0,1) (2,4)
    cSCPropertyHelp::SetPropertyValue(&exemplar, 0xa92356be, std::vector<float>{2.0f, 3.0f});
    cSCPropertyHelp::SetPropertyValue(&exemplar, 0x4953e8a3, std::vector<std::uint8_t>{10, 20, 70});
    cSCPropertyHelp::SetPropertyValue(&exemplar, 0x4953e8a4, std::vector<std::uint8_t>{30, 30, 40});
    CHECK(!sim.ReadSimulatorTunableValues(nullptr));
    CHECK(sim.ReadSimulatorTunableValues(&exemplar));
    const TrafficTunables& t = sim.Tunables();
    CHECK(t.value_bcc == 2.5f);
    // 60 km/h at 16 m cells: 60*1000/60/16 = 62.5 cells per minute
    CHECK(t.cellsPerMinute[0][0] == 62.5f && t.inverseMaximumSpeed[0][0] == 1.0f / 62.5f);
    CHECK(t.cellsPerMinute[0][1] == 0.0f && t.inverseMaximumSpeed[0][1] == FLT_MAX);
    CHECK(t.inverseMaximumSpeed[0][2] == 1.0f / (30.0f * 1000.0f / 60.0f / 16.0f));
    CHECK(t.inverseMaximumSpeed[0][3] == FLT_MAX && t.inverseMaximumSpeed[4][0] == FLT_MAX);
    CHECK(t.budgetScale[0] == 1.0f && t.budgetScale[1] == 2.0f && t.budgetScale[2] == 0.0f);
    CHECK(t.backgroundTrafficWeights[0] == 2.0f && t.backgroundTrafficWeights[1] == 3.0f && t.backgroundTrafficWeights[2] == 1.0f);
    std::vector<SC4Point> pts;
    sim.CongestionToInverseSpeedCurve().GetResponseCurveData(pts);
    CHECK(pts.size() == 2 && pts[0].y == 1.0f && pts[1].y == 0.25f);   // y values are stored reciprocal
    CHECK(t.value_eb0 == 255.0f / (0.25f - 1.0f));
    std::vector<SC4Percentage> pct;
    sim.GetTravelStrategyPercentages(0, pct);
    CHECK(pct.size() == 3 && std::fabs(pct[0].value - 0.10f) < 1e-6f && std::fabs(pct[2].value - 0.70f) < 1e-6f);
    sim.GetTravelStrategyPercentages(1, pct);
    CHECK(std::fabs(pct[2].value - 0.40f) < 1e-6f);
    // default funding curve (0, 0.02) (100, -0.01) (120, -0.04) since the exemplar has none
    CHECK(std::fabs(sim.FundingCurve().GetOutputValue(100.0f) + 0.01f) < 1e-6f);
    CHECK(t.value_ff8 == 10.0f && t.value_ffc == 60.0f && t.railAccidentChance == 1.0f);

    // ordinances adjust wealth classes 1..3 (clamped to 0..100)
    host.ordinances = {{5, 5, 5, 0, 0, 0, 0, 0, 0}, {-100, 200, 0, 0, 0, 0, 0, 0, 0}};
    sim.UpdateTravelStrategies(&exemplar);
    sim.GetTravelStrategyPercentages(1, pct);
    CHECK(std::fabs(pct[0].value - 0.0f) < 1e-6f);        // 30 + 5 - 100 -> 0
    CHECK(std::fabs(pct[1].value - 1.0f) < 1e-6f);        // 30 + 5 + 200 -> 100
    CHECK(std::fabs(pct[2].value - 0.45f) < 1e-6f);
    host.ordinances.clear();

    // ---- travel time ratio and edge densities
    CHECK(sim.GetTravelTimeRatio(2, 3, 1) == 1.0f);   // travel type not affected by traffic -> 1
    sim.MutableTunables().travelTypeEnabled.fill(false);
    sim.MutableTunables().travelTypeEnabled[0] = true;
    sim.EdgeDensityEntry(2, 3, 0, false).density = 6;
    sim.EdgeDensityEntry(2, 3, 0, true).density = 2;
    CHECK(sim.GetTrafficEdgeDensity(2, 3, 0, false).density == 6 && sim.GetTrafficEdgeDensity(2, 3, 0, true).density == 2);
    CHECK(sim.GetTrafficEdgeDensity(2, 3, 1, false).density == 0);
    CHECK(cSC4TrafficSimulator::EdgeDensityKey(2, 3, 5, true) == (0x203u | 5u << 16 | 1u << 24));

    // ---- transit switch cost
    {
        auto& sw = sim.TransitSwitchEntry(77);
        sw.traffic = 0; sw.capacity = 10; sw.cost = 3;
        sim.transitSwitchCostScale = 2.0f;
        CHECK(sim.TransitSwitchEntryCost(77) == sim.CongestionToInverseSpeedCurve().GetOutputValue(0.0f) * 3.0f * 2.0f);
        sw.traffic = 50;
        CHECK(sim.TransitSwitchEntryCost(77) == 1.7014117e+38f);
        sw.traffic = 20;
        CHECK(sim.TransitSwitchEntryCost(77) == sim.CongestionToInverseSpeedCurve().GetOutputValue(2.0f) * 3.0f * 2.0f);
    }


    // ---- GetTransitSwitchQueryData (called with the strike state of the block above: none)
    {
        TransitSwitchQueryData data;
        // a station: found through the lot at the cell, its record's cell inside the lot's rectangle
        FakeLot lot;
        lot.rect = SC4RectInt32{10, 20, 12, 22};
        host.lots[0][{11, 21}] = &lot;
        sim.TransitStations().push_back({7, 3, 3});    // outside the rectangle
        CHECK(!sim.GetTransitSwitchQueryData(11, 21, data));   // no record inside it yet
        sim.TransitStations().push_back({150, 12, 22});   // on the rectangle's far corner
        host.itemCost = 1000;
        host.itemDepartment = 9;
        host.departmentFraction = 0.755f;
        CHECK(sim.GetTransitSwitchQueryData(11, 21, data));
        CHECK(data.served == 150 && data.capacity == 150 && data.usage == 150 && data.percent == 1.0f);
        CHECK(data.maintenance == 755 && host.lastPurpose == 0xca550301 && host.lastDepartment == 9 && host.lastHolder == &host.lotHolder);
        // a lot with no building keeps the figures and skips the budget
        host.lotHasBuilding = false;
        host.lastPurpose = 0;
        data = TransitSwitchQueryData{};
        CHECK(sim.GetTransitSwitchQueryData(11, 21, data) && data.served == 150 && data.maintenance == 0 && host.lastPurpose == 0);
        host.lotHasBuilding = true;
        // no budget item or no department: the maintenance stays 0 but the call succeeds
        host.itemOk = false;
        CHECK(sim.GetTransitSwitchQueryData(11, 21, data) && data.maintenance == 0);
        host.itemOk = true;
        host.departmentOk = false;
        CHECK(sim.GetTransitSwitchQueryData(11, 21, data) && data.maintenance == 0);
        host.departmentOk = true;
        // a transit switch wins over the lot: capacity, usage and percent come from its entry; the holder is its own
        auto& entry = sim.TransitSwitchEntry(5);
        entry.capacity = 80.9f; entry.usage = 33.7f; entry.percent = 0.4f;
        host.switchGrid[{11, 21}] = 5;
        CHECK(sim.GetTransitSwitchQueryData(11, 21, data));
        CHECK(data.served == 80 && data.capacity == 80 && data.usage == 33 && data.percent == 0.4f);
        CHECK(host.lastHolder == &host.switchHolder && data.maintenance == 755);
        // an unknown cell without a lot answers false
        CHECK(!sim.GetTransitSwitchQueryData(0, 0, data));
    }

    // ---- background traffic
    host.grids[0x1010] = 3; host.grids[0x1030] = 4;       // group 0 = 7
    host.grids[0x3110] = 1; host.grids[0x3330] = 2;       // group 1 = 3
    host.grids[0x4400] = 10;                              // group 2 = 10 (weight 1)
    CHECK(sim.GetBackgroundTraffic(4, 4) == 7 * 2.0f + 3 * 3.0f + 10 * 1.0f);

    // ---- road funding, potholes
    sim.AddNetworkInfo(1, 1, 1);
    for (int i = 0; i < 6; ++i) sim.HealthyRoadCells().Insert(std::uint16_t((i << 8) | i));
    sim.SetRoadPotholeCapacity(6);
    host.potholes.clear();
    // curve(0%) = 0.02: accumulator 0.02 -> 0 potholes; push the accumulator with repeated calls
    SC4Percentage zero{0.0f};
    for (int i = 0; i < 25; ++i) sim.ApplyRoadFunding(zero);     // 25 * 0.02 = 0.5 -> 3 of 6
    CHECK(sim.RoadPotholeCount() == 3 && sim.DamagedRoadCells().Size() == 3 && sim.HealthyRoadCells().Size() == 3);
    CHECK(sim.StatRoadPotholes() == 3);
    {
        int damaged = 0;
        for (int i = 0; i < 6; ++i) damaged += sim.IsRoadDamaged(i, i) ? 1 : 0;
        CHECK(damaged == 3);
    }
    SC4Percentage full{1.0f};
    for (int i = 0; i < 100; ++i) sim.ApplyRoadFunding(full);    // curve(100%) = -0.01 -> accumulator back to 0
    CHECK(sim.RoadPotholeCount() == 0 && sim.DamagedRoadCells().Size() == 0 && sim.HealthyRoadCells().Size() == 6);
    // a pothole on a cell that has road network info penalises it and posts a message
    {
        CellSet healthy, damaged;
        healthy.Insert((1 << 8) | 1);
        host.potholes.clear();
        sim.ApplyPotholeChanges(healthy, damaged, 1, 1);
        CHECK(damaged.Contains((1 << 8) | 1) && healthy.Size() == 0);
        CHECK(sim.GetNetworkInfo(1, 1, 1)->potholePenalty == 32.0f);
        CHECK(host.potholes.size() == 1 && host.potholes[0][0] == 1 && host.potholes[0][1] == 1 && host.potholes[0][2] == 1);
        sim.ApplyPotholeChanges(healthy, damaged, -3, 1);
        CHECK(healthy.Contains((1 << 8) | 1) && damaged.Size() == 0 && sim.GetNetworkInfo(1, 1, 1)->potholePenalty == 0.0f);
        CHECK(host.potholes.size() == 2 && host.potholes[1][2] == 0);
    }
    // rail
    sim.HealthyRailCells().Insert(0x0102);
    sim.SetRailPotholeCapacity(1);
    for (int i = 0; i < 60; ++i) sim.ApplyMassTransitFunding(zero);   // 60 * 0.02 clamps to 1.0
    CHECK(sim.RailPotholeCount() == 1 && sim.DamagedRailCells().Contains(0x0102));
    CHECK(sim.CheckRailAccident(1, 2) && !sim.CheckRailAccident(0, 0));   // chance 1.0
    sim.MutableTunables().railAccidentChance = 0.0f;
    CHECK(!sim.CheckRailAccident(1, 2));

    // ---- strikes
    {
        sim.MutableTunables().strikeChanceScale = 100.0f;
        sim.SetStrikeEligible(5);
        host.funding = 0.9f;
        host.messages.clear();
        sim.UpdateStrikes();                                   // funding above the threat level: nothing
        CHECK(sim.GetStrikeState() == cSC4TrafficSimulator::kStrikeNone && host.messages.empty());
        host.funding = 0.5f;
        sim.UpdateStrikes();                                   // threatened
        CHECK(sim.GetStrikeState() == cSC4TrafficSimulator::kStrikeThreatened && host.messages[kTrafficMessageStrikeThreatened] == 1);
        CHECK(std::fabs(sim.StrikeChance() - 20.0f) < 1e-4f);
        sim.UpdateStrikes();                                   // funding still low -> strike begins
        CHECK(sim.GetStrikeState() == cSC4TrafficSimulator::kStrikeOn && sim.StrikeActiveFlag() && host.messages[kTrafficMessageStrikeBegan] == 1);
        for (int i = 0; i < 24; ++i) sim.UpdateStrikes();       // 24 days of strike
        CHECK(sim.GetStrikeState() == cSC4TrafficSimulator::kStrikeOn && sim.StrikeDays() == 24);
        {   // while the strike is on, a transit switch reports nothing served, no usage and a zero percent (its capacity stays)
            auto& entry = sim.TransitSwitchEntry(6);
            entry.capacity = 40.0f; entry.usage = 10.0f; entry.percent = 0.5f;
            host.switchGrid[{30, 31}] = 6;
            TransitSwitchQueryData data;
            CHECK(sim.GetTransitSwitchQueryData(30, 31, data));
            CHECK(data.served == 0 && data.capacity == 40 && data.usage == 0 && data.percent == 0.0f);
            host.switchGrid.erase({30, 31});
        }
        sim.UpdateStrikes();                                   // day cap reached -> ends
        CHECK(sim.GetStrikeState() == cSC4TrafficSimulator::kStrikeNone && !sim.StrikeActiveFlag() && host.messages[kTrafficMessageStrikeEnded] == 1);
        // threat averted when funding recovers
        sim.UpdateStrikes();
        CHECK(sim.GetStrikeState() == cSC4TrafficSimulator::kStrikeThreatened);
        host.funding = 1.0f;
        sim.UpdateStrikes();
        CHECK(sim.GetStrikeState() == cSC4TrafficSimulator::kStrikeNone && host.messages[kTrafficMessageStrikeAverted] == 1);
        // no eligible service: no threat
        sim.SetStrikeEligible(0);
        host.funding = 0.0f;
        sim.UpdateStrikes();
        CHECK(sim.GetStrikeState() == cSC4TrafficSimulator::kStrikeNone);
    }

    // ---- budgets (round to nearest even)
    {
        auto& scale = sim.MutableTunables().budgetScale;
        scale.fill(0.0f);
        scale[0] = 0.5f; scale[10] = 1.0f; scale[3] = 1.0f; scale[12] = 1.0f; scale[2] = 0.5f; scale[6] = 0.5f;
        sim.Counters().c908 = 5;     // 2.5 -> 2
        sim.Counters().c930 = 3;
        sim.Counters().c914 = 7;
        sim.Counters().c938 = 1;
        sim.Counters().c910 = 3;     // 1.5 -> 2
        sim.Counters().c920 = 7;     // 3.5 -> 4
        sim.UpdateRoadsBudget();
        CHECK(host.road[2] == 3 + 2 && host.road[1] == 7 && host.road[3] == 1 + 2 && host.road[4] == 4);
        scale[1] = 0.5f; scale[7] = 2.0f; scale[8] = 0.25f; scale[9] = 1.0f;
        sim.Counters().c90c = 9;     // 4.5 -> 4
        sim.Counters().c924 = 2;
        sim.Counters().c928 = 10;    // 2.5 -> 2
        sim.Counters().c92c = 6;
        sim.UpdateMassTransitBudget();
        CHECK(host.transit[3] == 4 && host.transit[4] == 4 && host.transit[5] == 2 && host.transit[6] == 6);
    }

    // ---- trip capacity widening
    {
        cSCPropertyHolder lot;
        CHECK(cSC4TrafficSimulator::GetMaxTripCapacity(&lot, 2) == 0);
        cSC4TrafficSimulator::SetMaxTripCapacity(&lot, 2, 40);
        CHECK(cSC4TrafficSimulator::GetMaxTripCapacity(&lot, 2) == 40);
        CHECK(lot.GetProperty(kTripCapacityProperty)->GetPropertyValue()->GetType() == 0x8004);
        cSC4TrafficSimulator::SetTripCapacity(&lot, 6, 7);
        CHECK(cSC4TrafficSimulator::GetUsedTripCapacity(&lot, 2) == 7);
        cSC4TrafficSimulator::SetTripCapacity(&lot, 10, 9);
        CHECK(cSC4TrafficSimulator::GetTrafficArrived(&lot, 2) == 9);
        cSC4TrafficSimulator::SetMaxTripCapacity(&lot, 3, 70000);   // widens to 32 bit, keeping the rest
        CHECK(lot.GetProperty(kTripCapacityProperty)->GetPropertyValue()->GetType() == 0x8006);
        CHECK(cSC4TrafficSimulator::GetMaxTripCapacity(&lot, 3) == 70000 && cSC4TrafficSimulator::GetMaxTripCapacity(&lot, 2) == 40);
        CHECK(cSC4TrafficSimulator::GetUsedTripCapacity(&lot, 2) == 7 && cSC4TrafficSimulator::GetTrafficArrived(&lot, 2) == 9);
    }

    // ---- neighbour connection geometry (grid is 8x8: valid indices 0..7)
    {
        int ix = -9, iz = -9;
        CHECK(sim.LastCellX() == 7 && sim.LastCellZ() == 7);
        CHECK(sim.GetNeighborConnectionInnerCell(-1, 4, ix, iz) == 0 && ix == 0 && iz == 4);
        CHECK(sim.GetNeighborConnectionInnerCell(8, 4, ix, iz) == 2 && ix == 7 && iz == 4);
        CHECK(sim.GetNeighborConnectionInnerCell(3, -1, ix, iz) == 1 && ix == 3 && iz == 0);
        CHECK(sim.GetNeighborConnectionInnerCell(3, 8, ix, iz) == 3 && ix == 3 && iz == 7);
        CHECK(sim.GetNeighborConnectionInnerCell(3, 3, ix, iz) == -1);            // interior cell
        CHECK(sim.GetNeighborConnectionInnerCell(-1, -1, ix, iz) == -1);          // corner
        CHECK(sim.GetNeighborConnectionInnerCell(-1, 9, ix, iz) == -1);
        int x = -1, z = 4;
        sim.FindHighwayReturnTile(x, z, true);  CHECK(x == -1 && z == 3);          // right-hand, flag equal: west side steps z-1
        x = 8; z = 4; sim.FindHighwayReturnTile(x, z, true);  CHECK(x == 8 && z == 5);
        x = 3; z = -1; sim.FindHighwayReturnTile(x, z, true); CHECK(x == 4 && z == -1);
        x = 3; z = 8; sim.FindHighwayReturnTile(x, z, true);  CHECK(x == 2 && z == 8);
        x = 3; z = 3; sim.FindHighwayReturnTile(x, z, true);  CHECK(x == 3 && z == 3);   // interior: unchanged
        x = -1; z = 4; sim.FindHighwayReturnTile(x, z, false); CHECK(x == -1 && z == 5);
        x = 8; z = 4; sim.FindHighwayReturnTile(x, z, false);  CHECK(x == 8 && z == 3);
        x = 3; z = -1; sim.FindHighwayReturnTile(x, z, false); CHECK(x == 2 && z == -1);
        x = 3; z = 8; sim.FindHighwayReturnTile(x, z, false);  CHECK(x == 4 && z == 8);
        host.rightHand = false;
        x = -1; z = 4; sim.FindHighwayReturnTile(x, z, true);  CHECK(x == -1 && z == 5);   // left-hand traffic mirrors
    }

    // ---- intersection invalidation
    sim.InvalidateIntersectionData(4, 5, 6, 7);
    sim.InvalidateIntersectionData(2, 6, 9, 3);
    int x0, z0, x1, z1;
    sim.GetIntersectionDirtyRect(x0, z0, x1, z1);
    CHECK(sim.IntersectionDirty() && x0 == 2 && z0 == 5 && x1 == 9 && z1 == 7);

    // ---- subnetworks
    {
        cSC4TrafficSimulator net(&host, &random);
        net.InitGrid(8, 8);
        net.AddNetworkInfo(1, 1, 1)->subnetwork = {1, 0, 2, 0};
        net.AddNetworkInfo(1, 2, 1)->subnetwork = {2, 3, 0, 0};
        net.AddNetworkInfo(2, 3, 1)->subnetwork = {9, 0, 0, 0};   // not a road: ignored
        std::vector<std::uint32_t> ids;
        net.GetSubnetworksInRectangle({0, 0, 3, 3}, ids);
        CHECK(ids == (std::vector<std::uint32_t>{1, 2, 3}));
        net.GetSubnetworksInRectangle({4, 4, 2, 2}, ids);
        CHECK(ids.empty());
        TrafficCellRegion region;
        region.bounds = {1, 1, 2, 1};
        region.mask = {{true}, {false}};
        net.GetSubnetworksInRegion(region, ids);
        CHECK(ids == (std::vector<std::uint32_t>{1, 2}));

        net.Subnetworks().resize(4);
        net.Subnetworks()[2].occupantsByType = {{0x10, 7}, {0x20, 3}};
        net.Subnetworks()[3].occupantsByType = {{0x10, 11}};
        net.Subnetworks()[2].destinations = {5, 6, 7, 8};
        net.Subnetworks()[3].destinations = {9, 1, 1, 1};
        cSCPropertyHolder lotA, lotB, lotC, lotNone;
        cSCPropertyHelp::SetPropertyValue(&lotA, kLotSubnetworksProperty, std::vector<std::uint32_t>{2, 3, 99});
        cSCPropertyHelp::SetPropertyValue(&lotB, kLotSubnetworksProperty, std::vector<std::uint32_t>{3});
        cSCPropertyHelp::SetPropertyValue(&lotC, kLotSubnetworksProperty, std::vector<std::uint32_t>{1});
        CHECK(net.AreLotsConnected(&lotA, &lotB) && !net.AreLotsConnected(&lotA, &lotC) && !net.AreLotsConnected(&lotA, &lotNone));
        CHECK(net.GetConnectedOccupantCount(&lotA, 0x10) == 11 && net.GetConnectedOccupantCount(&lotA, 0x20) == 3);
        CHECK(net.GetConnectedOccupantCount(&lotA, 0x30) == 0 && net.GetConnectedOccupantCount(&lotNone, 0x10) == 0);
        CHECK(net.GetConnectedDestinationCount(&lotA, 0) == 9 && net.GetConnectedDestinationCount(&lotA, 3) == 8);
        CHECK(net.GetConnectedDestinationCount(&lotA, 4) == 0);
        net.GetSubnetworksForLot(&lotA, ids);
        CHECK(ids == (std::vector<std::uint32_t>{2, 3, 99}));
        net.GetOccupantCountForAllSubnetworks(0x10, ids);
        CHECK(ids == (std::vector<std::uint32_t>{0, 0, 7, 11}));
        net.ClearSubnetworks();
        net.GetSubnetworksInRectangle({0, 0, 7, 7}, ids);
        CHECK(ids.empty() && net.NextSubnetworkId() == 1);
    }

    // ==== neighbour connection tables (0x1000820ac, 0x100082358, 0x1000828a2, 0x100082ae2, 0x1000884e8,
    //      0x100089830, 0x10008a090) ====
    {
        cSC4TrafficSimulator net(&host, &random);
        net.InitGrid(8, 8);
        CHECK(net.NeighborConnections()[0].size() == 8 && net.NeighborConnections()[1].size() == 8);
        CHECK(net.CellTripFlags().size() == 10 && net.CellTripFlags()[0].size() == 10);
        CHECK(net.CellConnections(3, 3).masks[0] == 0);
        CHECK(net.DefaultNeighborPool() != nullptr);

        int side = -1, index = -1;
        CHECK(cSC4TrafficSimulator::NeighborSide(-1, 4, 7, 7, side, index) && side == 0 && index == 4);
        CHECK(cSC4TrafficSimulator::NeighborSide(8, 4, 7, 7, side, index) && side == 2 && index == 4);
        CHECK(cSC4TrafficSimulator::NeighborSide(4, -1, 7, 7, side, index) && side == 1 && index == 4);
        CHECK(cSC4TrafficSimulator::NeighborSide(4, 8, 7, 7, side, index) && side == 3 && index == 4);
        CHECK(!cSC4TrafficSimulator::NeighborSide(3, 3, 7, 7, side, index));
        CHECK(!cSC4TrafficSimulator::NeighborSide(-1, -1, 7, 7, side, index));
        CHECK(net.GetTrafficNeighborConnection(3, 3) == nullptr);
        CHECK(net.GetTrafficNeighborConnection(-1, 4) == &net.NeighborConnections()[0][4]);
        CHECK(net.GetTrafficNeighborConnection(4, 8) == &net.NeighborConnections()[3][4]);

        FakeCity cityA, cityB;
        cityA.regionX = 2; cityA.regionZ = 2;
        cityB.regionX = 3; cityB.regionZ = 3;
        FakeConnection roadWest, railWest, railWest2, east, farWest;
        roadWest.city = &cityA; roadWest.x = -1; roadWest.z = 4;
        railWest.city = &cityA; railWest.x = -1; railWest.z = 5;
        railWest2.city = &cityA; railWest2.x = -1; railWest2.z = 4;
        east.city = &cityB; east.x = 8; east.z = 3;
        // the cost is the far side's property 0xca947077 (connection type 2: valid for transit network 1)
        cSCPropertyHelp::SetPropertyValue(&farWest.props, 0xca947077, 55.0f);
        host.otherSide[&roadWest] = &farWest;
        host.connections[{2, -1, 4}] = &roadWest;
        host.connections[{2, -1, 5}] = &railWest;
        host.connections[{10, -1, 4}] = &railWest2;
        host.connections[{2, 8, 3}] = &east;

        net.AddNeighborConnection(2, -1, 4);
        TrafficNeighborConnection* entry = net.GetTrafficNeighborConnection(-1, 4);
        CHECK(entry != nullptr && entry->types == 4 && entry->cost == 55.0f && entry->pool != 0);
        sc4::cISCPropertyHolder* poolA = net.GetNeighborPoolPropertyHolder(1, -1, 4);
        CHECK(poolA != nullptr && poolA == &net.NeighborPools()[entry->pool]);
        std::int32_t px = 0, pz = 0;
        CHECK(cSCPropertyHelp::GetPropertyValue(poolA, 0x4a23576b, px) && px == 2);
        CHECK(cSCPropertyHelp::GetPropertyValue(poolA, 0x4a23576c, pz) && pz == 2);
        CHECK(net.GetNeighborPoolPropertyHolder(0, -1, 4) == nullptr);   // transit network 0 wants type 9
        CHECK(net.GetNeighborPoolPropertyHolder(-1, -1, 4) == poolA);    // -1 accepts any type
        CHECK(net.GetNeighborPoolPropertyHolder(1, 1, 1) == nullptr);    // inside the map: no pool

        net.AddNeighborConnection(2, -1, 5);                             // same city: same pool
        CHECK(net.GetTrafficNeighborConnection(-1, 5)->pool == entry->pool);
        net.AddNeighborConnection(2, 8, 3);                              // other city: new pool
        CHECK(net.GetTrafficNeighborConnection(8, 3)->pool != entry->pool && net.NeighborPools().size() == 2);
        net.AddNeighborConnection(2, 3, -1);                             // no such connection: ignored
        CHECK(net.GetTrafficNeighborConnection(3, -1)->types == 0);

        CHECK(net.GetNeighborConnection(1, -1, 4) == &roadWest);         // transit network 1 accepts type 2
        CHECK(net.GetNeighborConnection(0, -1, 4) == nullptr);           // transit network 0 wants type 9
        CHECK(net.GetNeighborConnection(1, 3, 3) == nullptr);            // inside the map
        CHECK(net.GetNeighborConnection(1, -1, 6) == nullptr);           // border cell without a record

        net.AddNeighborConnection(10, -1, 4);
        CHECK(net.GetTrafficNeighborConnection(-1, 4)->types == (4 | (1 << 10)));
        net.CellTripFlags()[0][5] = 0x0b;                                // border (-1, 4) is row 0, column 5
        net.ConnectionCounts() = {3, 3, 3, 3};
        net.RemoveNeighborConnection(2, -1, 4);                          // one type left: pool and flags stay
        CHECK(net.GetTrafficNeighborConnection(-1, 4)->types == (1 << 10));
        CHECK(net.GetTrafficNeighborConnection(-1, 4)->pool != 0);
        CHECK(net.CellTripFlags()[0][5] == 0x0b && net.ConnectionCounts()[0] == 3);
        net.RemoveNeighborConnection(10, -1, 4);
        CHECK(net.GetTrafficNeighborConnection(-1, 4)->types == 0);
        CHECK(net.GetTrafficNeighborConnection(-1, 4)->pool == 0);
        CHECK(net.CellTripFlags()[0][5] == 0);
        CHECK(net.ConnectionCounts() == (std::array<int, 4>{2, 2, 3, 2}));   // flags 0x0b: lanes 0, 1, 3
        net.RemoveNeighborConnection(10, -1, 4);                         // nothing left: no counter change
        CHECK(net.ConnectionCounts()[0] == 2);

        // RemoveUnusedPools: city A still has the (-1, 5) connection, city B's pool is still used
        net.RemoveUnusedPools();
        CHECK(net.NeighborPools().size() == 2);
        net.GetTrafficNeighborConnection(-1, 5)->pool = 0;
        net.RemoveUnusedPools();
        CHECK(net.NeighborPools().size() == 1);
        CHECK(net.NeighborPools().begin()->first == net.GetTrafficNeighborConnection(8, 3)->pool);

        // HasMapExit: the inner cell's connection masks first, then the border record's type 9 bit
        cSC4TrafficSimulator exits(&host, &random);
        exits.InitGrid(8, 8);
        CHECK(!exits.HasMapExit(-1, 4));
        exits.CellConnections(0, 4).masks[2] = std::uint16_t(1u << (0 + 4));   // west, lane 1, type 2
        CHECK(exits.HasMapExit(-1, 4));
        CHECK(!exits.HasMapExit(8, 4));
        exits.CellConnections(7, 4).masks[8] = std::uint16_t(1u << 2);          // east, lane 0, type 8
        CHECK(exits.HasMapExit(8, 4));
        CHECK(!exits.HasMapExit(4, -1));
        exits.GetTrafficNeighborConnection(4, -1)->types = 0x200;
        CHECK(exits.HasMapExit(4, -1));
        exits.GetTrafficNeighborConnection(4, -1)->types = 0x1;
        CHECK(!exits.HasMapExit(4, -1));
        CHECK(!exits.HasMapExit(4, 4));                                        // interior cell
    }

    // ==== congestion / commute accounting ====
    {
        cSC4TrafficSimulator dest(&host, &random);
        dest.InitGrid(8, 8);
        FakeLot lot;
        lot.rect = {2, 2, 2, 2};
        host.lots[0][{2, 2}] = &lot;
        cSC4TrafficSimulator::SetMaxTripCapacity(&lot.props, 1, 10);
        cSC4TrafficSimulator::SetTripCapacity(&lot.props, 5, 3);          // used trips of lane group 1
        dest.ApplyTrafficToDestination(4, 2, 2, 1, 1);
        CHECK(cSC4TrafficSimulator::GetUsedTripCapacity(&lot.props, 1) == 7);
        CHECK(dest.ConnectionCounts()[1] == 0);
        CHECK(dest.CellTripFlags()[3][3] == 0);
        cSC4TrafficSimulator::SetMaxTripCapacity(&lot.props, 1, 8);       // now 12 of 8: the destination is full
        dest.CellTripFlags()[3][3] = 0xff;
        dest.ApplyTrafficToDestination(5, 2, 2, 1, 1);
        CHECK(cSC4TrafficSimulator::GetUsedTripCapacity(&lot.props, 1) == 12);
        CHECK(dest.CellTripFlags()[3][3] == 0xfd);
        CHECK(dest.ConnectionCounts()[1] == -1);
        dest.ApplyTrafficToDestination(1, 6, 6, 1, 1);                    // no lot there: nothing happens
        CHECK(dest.ConnectionCounts()[1] == -1);

        // border destination: the pool holder, the connection's traffic volume and its used counter
        cSC4TrafficSimulator border(&host, &random);
        border.InitGrid(8, 8);
        FakeCity cityC;
        cityC.regionX = 5; cityC.regionZ = 6;
        FakeConnection highway;
        highway.city = &cityC; highway.x = -1; highway.z = 4;
        host.connections[{9, -1, 4}] = &highway;
        border.AddNeighborConnection(9, -1, 4);
        CHECK(border.GetTrafficNeighborConnection(-1, 4)->cost == border.GetTripScale());
        sc4::cISCPropertyHolder* poolC = border.GetNeighborPoolPropertyHolder(0, -1, 4);
        CHECK(poolC != nullptr);
        cSC4TrafficSimulator::SetMaxTripCapacity(poolC, 0, 2);
        border.CellTripFlags()[0][5] = 0x03;
        border.ConnectionCounts()[0] = 1;
        border.ApplyTrafficToDestination(3, -1, 4, 0, 0);
        CHECK(cSC4TrafficSimulator::GetUsedTripCapacity(poolC, 0) == 3);
        CHECK(cSC4TrafficSimulator::GetUsedTripCapacity(&highway.props, 0) == 3);
        CHECK(border.CellTripFlags()[0][5] == 0x02);
        CHECK(border.ConnectionCounts()[0] == 0);
        std::vector<std::uint32_t> volumes;
        CHECK(cSCPropertyHelp::GetPropertyValue(&highway.props, 0xaa9ea81a, volumes));
        CHECK(volumes.size() == 9 && volumes[0] == 3);

        // WorkOnRefreshCommuteMap: flag bytes from the lots' maximum capacities
        cSC4TrafficSimulator commute(&host, &random);
        commute.InitGrid(8, 8);
        FakeLot lotA, lotB;
        lotA.rect = {2, 2, 2, 2};
        lotB.rect = {3, 3, 3, 3};
        host.lots[1][{2, 2}] = &lotA;
        host.lots[0][{3, 3}] = &lotB;                                     // only the filter-0 lookup sees it
        cSC4TrafficSimulator::SetMaxTripCapacity(&lotA.props, 0, 5);
        cSC4TrafficSimulator::SetMaxTripCapacity(&lotB.props, 2, 7);
        commute.CellTripFlags()[3][3] = 0xff;
        commute.WorkOnRefreshCommuteMap();
        CHECK(commute.CellTripFlags()[3][3] == 0xf9);      // 0xf8 mask keeps bit 3, lane 0 set
        CHECK(commute.CellTripFlags()[4][4] == 0x04);      // lane 2 from the filter-0 lot, no counter
        CHECK(commute.ConnectionCounts()[0] == 1 && commute.ConnectionCounts()[2] == 0);

        // WorkOnClearTripDestinations: used 4..7 -> arrived 8..11, then the totals per lane group
        for (std::uint32_t lane = 0; lane < 4; ++lane) {
            cSC4TrafficSimulator::SetMaxTripCapacity(&lotA.props, lane, 4 + lane);
            cSC4TrafficSimulator::SetTripCapacity(&lotA.props, lane + 4, 1 + lane);
        }
        commute.WorkOnClearTripDestinations();
        CHECK(cSC4TrafficSimulator::GetTrafficArrived(&lotA.props, 0) == 1);
        CHECK(cSC4TrafficSimulator::GetTrafficArrived(&lotA.props, 3) == 4);
        CHECK(cSC4TrafficSimulator::GetUsedTripCapacity(&lotA.props, 0) == 0);
        CHECK(cSC4TrafficSimulator::GetUsedTripCapacity(&lotA.props, 3) == 0);
        CHECK(commute.TripCapacityTotals() == (std::array<int, 4>{4, 5, 6, 7}));
        CHECK(cSC4TrafficSimulator::GetMaxTripCapacity(&lotB.props, 2) == 7);   // untouched: not filter 1

        // WorkOnUpdateNeighborConnections: volumes roll over, used capacities become arrived counters
        cSC4TrafficSimulator roll(&host, &random);
        FakeConnection rolled;
        host.allConnections = {&rolled};
        cSCPropertyHelp::SetPropertyValue(&rolled.props, 0xaa9ea81a,
                                           std::vector<std::uint32_t>{1, 2, 3, 4, 5, 6, 7, 8, 9});
        for (std::uint32_t lane = 0; lane < 4; ++lane)
            cSC4TrafficSimulator::SetTripCapacity(&rolled.props, lane + 4, 10 + lane);
        roll.WorkOnUpdateNeighborConnections();
        std::vector<std::uint32_t> rolledOver;
        CHECK(cSCPropertyHelp::GetPropertyValue(&rolled.props, 0xaa9ea81d, rolledOver));
        CHECK(rolledOver == (std::vector<std::uint32_t>{1, 2, 3, 4, 5, 6, 7, 8, 9}));
        CHECK(cSCPropertyHelp::GetPropertyValue(&rolled.props, 0xaa9ea81a, rolledOver));
        CHECK(rolledOver == std::vector<std::uint32_t>(9, 0));
        CHECK(cSC4TrafficSimulator::GetTrafficArrived(&rolled.props, 1) == 11);
        CHECK(cSC4TrafficSimulator::GetUsedTripCapacity(&rolled.props, 1) == 0);
        host.allConnections.clear();

        // CapacityStillAvailable (0x100087c06)
        cSC4TrafficSimulator cap(&host, &random);
        cap.InitGrid(8, 8);
        FakeLot capLot;
        capLot.rect = {2, 2, 2, 2};
        host.lots[0][{2, 2}] = &capLot;
        cSC4TrafficSimulator::SetMaxTripCapacity(&capLot.props, 0, 10);
        cSC4TrafficSimulator::SetTripCapacity(&capLot.props, 4, 3);
        cSC4TripData trip;
        trip.sourceX = 2; trip.sourceZ = 2; trip.type = 0; trip.count = 7;
        CHECK(cap.CapacityStillAvailable(trip));
        trip.count = 8;
        CHECK(!cap.CapacityStillAvailable(trip));
        trip.sourceX = 6; trip.sourceZ = 6;                              // no lot: only an empty trip fits
        trip.count = 0;
        CHECK(cap.CapacityStillAvailable(trip));
        trip.count = 1;
        CHECK(!cap.CapacityStillAvailable(trip));
        // border source with a connection record but no pool: the default pool's lane group 3 is unlimited
        trip.sourceX = -1; trip.sourceZ = 4; trip.type = 3; trip.count = 1000;
        cap.GetTrafficNeighborConnection(-1, 4)->types = 0x200;
        CHECK(cap.GetNeighborPoolPropertyHolder(-1, -1, 4) == cap.DefaultNeighborPool());
        CHECK(cap.CapacityStillAvailable(trip));
    }

    // ==== 0x100087336 RecalculateNeighborConnectionCapacities ====
    {
        cSC4TrafficSimulator rec(&host, &random);
        rec.InitGrid(8, 8);
        host.connections.clear();
        host.otherSide.clear();
        host.cities.clear();
        host.allConnections.clear();
        FakeCity a, b;
        a.regionX = 1; a.regionZ = 1;
        a.populations = {{0x2010, 30}, {0x2020, 60}, {0x2030, 90}};
        b.regionX = 2; b.regionZ = 2;
        b.populations = {{0x2010, 10}, {0x2020, 20}, {0x2030, 30}};
        FakeConnection ca, cb;
        ca.city = &a; ca.x = -1; ca.z = 1;
        cb.city = &b; cb.x = -1; cb.z = 2;
        host.connections[{2, -1, 1}] = &ca;
        host.connections[{2, -1, 2}] = &cb;
        rec.AddNeighborConnection(2, -1, 1);
        rec.AddNeighborConnection(2, -1, 2);
        rec.GetTrafficNeighborConnection(-1, 1)->types |= 0x200;   // HasMapExit's border-record fallback
        rec.GetTrafficNeighborConnection(-1, 2)->types |= 0x200;
        host.demandTotals[0x2010] = 100;
        host.demandTotals[0x2020] = 200;
        host.demandTotals[0x2030] = 300;
        host.connectedCities = {&a, &b};
        rec.RecalculateNeighborConnectionCapacities();
        sc4::cISCPropertyHolder* poolA =
            &rec.NeighborPools()[rec.GetTrafficNeighborConnection(-1, 1)->pool];
        sc4::cISCPropertyHolder* poolB =
            &rec.NeighborPools()[rec.GetTrafficNeighborConnection(-1, 2)->pool];
        // city A: 30/40 of 100, 60/80 of 200, 90/120 of 300; city B gets the rest
        CHECK(cSC4TrafficSimulator::GetMaxTripCapacity(poolA, 0) == 75);
        CHECK(cSC4TrafficSimulator::GetMaxTripCapacity(poolA, 1) == 150);
        CHECK(cSC4TrafficSimulator::GetMaxTripCapacity(poolA, 2) == 225);
        CHECK(cSC4TrafficSimulator::GetMaxTripCapacity(poolA, 3) == 0xffffffffu);
        CHECK(cSC4TrafficSimulator::GetUsedTripCapacity(poolA, 3) == 0);
        CHECK(cSC4TrafficSimulator::GetMaxTripCapacity(poolB, 0) == 25);
        CHECK(cSC4TrafficSimulator::GetMaxTripCapacity(poolB, 1) == 50);
        CHECK(cSC4TrafficSimulator::GetMaxTripCapacity(poolB, 2) == 75);
        CHECK(rec.CellTripFlags()[0][2] == 0x0f && rec.CellTripFlags()[0][3] == 0x0f);
        CHECK(rec.ConnectionCounts() == (std::array<int, 4>{2, 2, 2, 2}));
        // every connected city empty: the x86 conversion of NaN lands as 0
        a.populations.clear();
        b.populations.clear();
        rec.RecalculateNeighborConnectionCapacities();
        CHECK(cSC4TrafficSimulator::GetMaxTripCapacity(poolA, 0) == 0);
        host.connectedCities.clear();
    }

    // ==== 0x100087cc4 SetupTrip ====
    {
        cSC4TrafficSimulator sim2(&host, &random);
        sim2.InitGrid(8, 8);
        host.sourceRect = {1, 1, 4, 4};
        FakePathFinder primary, secondary;
        StandardTripParams params;
        params.primary = &primary;
        params.secondary = &secondary;
        params.source = &host;
        params.workTime = 123;
        params.flag51 = 1;
        cSC4TripData data;
        data.count = 50; data.type = 1; data.index = 2; data.flags = 0; data.kind = 0;
        data.sourceX = 3; data.sourceZ = 4;
        sim2.SetupTrip(params, data);
        const auto call = [](const FakePathFinder& pf, const char* name) -> const FakePathFinder::Call* {
            for (const FakePathFinder::Call& c : pf.calls)
                if (c.name == name) return &c;
            return nullptr;
        };
        const FakePathFinder::Call* density = call(primary, "SetTrafficDensity");
        CHECK(density != nullptr && density->u == 50);
        const FakePathFinder::Call* filter = call(primary, "SetTripDestinationFilter");
        CHECK(filter != nullptr && filter->u == 0);
        CHECK(call(primary, "ClearAllDestinationFlags") != nullptr);
        const FakePathFinder::Call* destinationFlag = call(primary, "SetDestinationFlag");
        CHECK(destinationFlag != nullptr && destinationFlag->u == 2);            // 1 << trip.type
        const FakePathFinder::Call* workTime = call(primary, "SetMaxWorkTime");
        const FakePathFinder::Call* workTime2 = call(secondary, "SetMaxWorkTime");
        CHECK(workTime != nullptr && workTime->u == 123 && workTime2 != nullptr && workTime2->u == 123);
        const FakePathFinder::Call* startRect = call(primary, "SetStartRect");
        CHECK(startRect != nullptr && startRect->rect.x0 == 1 && startRect->rect.z1 == 4);
        CHECK(call(primary, "ClearAllFlags") != nullptr && call(primary, "ClearAllTravelTypeFlags") != nullptr);
        const FakePathFinder::Call* costs = call(primary, "SetStartingCostArray");
        CHECK(costs != nullptr && costs->p == nullptr);                          // cleared before the trip type
        const FakePathFinder::Call* tripLength = call(primary, "SetMaxTripLength");
        CHECK(tripLength != nullptr && tripLength->f == 90.0f);
        const FakePathFinder::Call* flag = call(primary, "SetFlag");
        CHECK(flag != nullptr && flag->u == 4);                                  // params.flag51
        CHECK(params.useSecondary == 0 && params.secondPhase == 1);
        CHECK(params.primary == &primary && params.secondary == &secondary);
        // travel types: 0x1cf (cleared) then 3, then the index-2 starting cost array (values_ee0)
        std::vector<std::uint32_t> travelTypeFlags;
        for (const FakePathFinder::Call& c : primary.calls)
            if (c.name == "SetTravelTypeFlag") travelTypeFlags.push_back(c.u);
        CHECK(travelTypeFlags.size() == 2 && travelTypeFlags[0] == 0x1cf && travelTypeFlags[1] == 3);
        CHECK(primary.calls.back().name == "SetStartingCostArray");
        CHECK(primary.calls.back().p == sim2.MutableTunables().values_ee0.data());

        // kind 1: the destination is the lot at the source cell, and only the destination flag differs
        FakePathFinder primary2;
        StandardTripParams params2;
        params2.primary = &primary2;
        params2.source = &host;
        params2.workTime = 7;
        FakeLot tripLot;
        tripLot.rect = {2, 3, 5, 6};
        host.lots[0][{3, 4}] = &tripLot;
        cSC4TripData data2;
        data2.count = 7; data2.type = 0; data2.kind = 1; data2.flags = 2;
        data2.sourceX = 3; data2.sourceZ = 4;
        sim2.SetupTrip(params2, data2);
        const FakePathFinder::Call* filter2 = call(primary2, "SetTripDestinationFilter");
        CHECK(filter2 != nullptr && filter2->u == 3);
        const FakePathFinder::Call* endRect = call(primary2, "SetEndRect");
        CHECK(endRect != nullptr && endRect->rect.x0 == 2 && endRect->rect.z0 == 3 && endRect->rect.x1 == 5 &&
              endRect->rect.z1 == 6);
        CHECK(call(primary2, "SetDestinationFlag") == nullptr);
        // kind 1 without a lot: the source cell is the end rectangle
        host.lots[0].erase({3, 4});
        FakePathFinder primary3;
        StandardTripParams params3;
        params3.primary = &primary3;
        params3.source = &host;
        sim2.SetupTrip(params3, data2);
        const FakePathFinder::Call* endRect3 = call(primary3, "SetEndRect");
        CHECK(endRect3 != nullptr && endRect3->rect.x0 == 3 && endRect3->rect.x1 == 3 &&
              endRect3->rect.z0 == 4 && endRect3->rect.z1 == 4);
        // type 3 drives the secondary path finder with travel types 0x30 and flag 1
        FakePathFinder primary4;
        StandardTripParams params4;
        params4.primary = &primary4;
        params4.source = &host;
        cSC4TripData data4;
        data4.type = 3; data4.kind = 0; data4.sourceX = 0; data4.sourceZ = 0;
        sim2.SetupTrip(params4, data4);
        CHECK(params4.useSecondary == 1 && params4.secondPhase == 0);
        const FakePathFinder::Call* flag4 = call(primary4, "SetFlag");
        CHECK(flag4 != nullptr && flag4->u == 1);
        bool saw30 = false;
        for (const FakePathFinder::Call& c : primary4.calls)
            if (c.name == "SetTravelTypeFlag" && c.u == 0x30) saw30 = true;
        CHECK(saw30);
        CHECK(call(primary4, "SetStartingCostArray") != nullptr);   // the common clear still happens
    }


    // ==== 0x10008bf74 SetupPathFinderForLot ====
    {
        cSC4TrafficSimulator sim3(&host, &random);
        sim3.InitGrid(8, 8);
        host.sourceRect = {1, 2, 3, 4};
        FakePathFinder finder;
        FakeLot shopLot;
        CHECK(!sim3.SetupPathFinderForLot(nullptr, &shopLot));
        CHECK(!sim3.SetupPathFinderForLot(&finder, nullptr));
        CHECK(!sim3.SetupPathFinderForLot(&finder, &shopLot));   // no trips at all: the refresh is tried once
        CHECK(shopLot.refreshes == 1 && finder.calls.empty());
        cSC4TripData t1, t2, t3;
        t1.count = 10; t1.index = 0; t1.type = 1;
        t2.count = 4; t2.index = 2; t2.type = 1;
        t3.count = 9; t3.index = 2; t3.type = 1;
        shopLot.trips = {t1, t2, t3};
        CHECK(sim3.SetupPathFinderForLot(&finder, &shopLot));
        const FakePathFinder::Call* start = FindCall(finder, "SetStartRect");
        CHECK(start != nullptr && start->rect.x0 == 1 && start->rect.z0 == 2 && start->rect.z1 == 4);
        const FakePathFinder::Call* destinationFlag = FindCall(finder, "SetDestinationFlag");
        CHECK(destinationFlag != nullptr && destinationFlag->u == 2);         // the last trip's type 1
        CHECK(FindCall(finder, "SetMaxTripLength")->f == 90.0f);
        // index 2 is the heaviest (4 + 9 > 10): its cost array
        CHECK(finder.calls.back().name == "SetStartingCostArray");
        CHECK(finder.calls.back().p == sim3.MutableTunables().values_ee0.data());
        // a lot without trips after the refresh configures nothing
        FakeLot emptyLot;
        FakePathFinder finder2;
        CHECK(!sim3.SetupPathFinderForLot(&finder2, &emptyLot));
        CHECK(emptyLot.refreshes == 1 && finder2.calls.empty());
        // type 3 uses the secondary-path branch (flag 1, travel types 0x30)
        FakeLot busLot;
        cSC4TripData t4;
        t4.count = 3; t4.index = 1; t4.type = 3;
        busLot.trips = {t4};
        FakePathFinder finder3;
        CHECK(sim3.SetupPathFinderForLot(&finder3, &busLot));
        CHECK(FindCall(finder3, "SetFlag")->u == 1);
        bool saw30b = false;
        for (const FakePathFinder::Call& c : finder3.calls)
            if (c.name == "SetTravelTypeFlag" && c.u == 0x30) saw30b = true;
        CHECK(saw30b);
        CHECK(destinationFlag != nullptr && FindCall(finder3, "SetDestinationFlag")->u == 8);
        // a type that is neither a standard trip nor a bus/ferry one: the common setters run, the answer is false
        FakeLot oddLot;
        cSC4TripData t5;
        t5.count = 1; t5.index = 0; t5.type = 5;
        oddLot.trips = {t5};
        FakePathFinder finder4;
        CHECK(!sim3.SetupPathFinderForLot(&finder4, &oddLot));
        CHECK(FindCall(finder4, "SetFlag") == nullptr && FindCall(finder4, "SetStartingCostArray") != nullptr);
    }

    if (failures) return 1;
    std::printf("sc4_trafficsimulator: all passed\n");
    return 0;
}
