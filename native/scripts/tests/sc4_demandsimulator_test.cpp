// cSC4DemandSimulator — behavioural test: the demand channels are built from exemplars, linked through the
// economy-index table, refreshed from the lot census and the tax/economy modifiers, exported to the region,
// and serialized. The fake host gives every seam a deterministic answer.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/DemandSimulator.h"

using namespace sc4;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECK_NEAR(a, b) do { const double va = (a), vb = (b); if (std::fabs(va - vb) > 1e-3) { \
    std::printf("FAIL line %d: %s = %.6f, expected %.6f\n", __LINE__, #a, va, vb); ++failures; } } while (0)

namespace {

cGZPersistResourceKey Key(std::uint32_t type, std::uint32_t group, std::uint32_t instance) {
    return cGZPersistResourceKey{type, group, instance};
}

struct Exemplar {
    cSCPropertyHolder holder;
};

struct FakeLotState;

struct FakeOccupant final : IDemandOccupant {
    cISCPropertyHolder* holder = nullptr;
    cISCPropertyHolder* AsPropertyHolder() override { return holder; }
};

struct FakeLot : IDemandLot {
    bool hasCapacity = true;
    std::uint32_t buildingType = 0x42;
    std::int32_t wealth = 1;
    std::uint16_t capacity = 100;
    bool HasCapacity() const override { return hasCapacity; }
    std::uint32_t GetBuildingType(bool) const override { return buildingType; }
    std::int32_t GetOccupantWealth() const override { return wealth; }
    std::uint16_t GetCapacity(std::uint32_t, bool) const override { return capacity; }
};

struct FakeLotState final : IDemandLotState {
    std::uint32_t buildingType = 0x42;
    FakeOccupant occupant;
    std::uint32_t refs = 1;
    bool QueryInterface(std::uint32_t, void** out) override { *out = this; return true; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    std::uint32_t GetBuildingType(bool) override { return buildingType; }
    IDemandOccupant* GetBuilding() const override { return const_cast<FakeOccupant*>(&occupant); }
};

struct FakeCity final : IDemandRegionCity {
    int x = 0, z = 0, width = 10, height = 10;
    float workforce = 0.5f;
    float tax = 7.0f;
    std::map<std::uint32_t, int> population;
    std::map<std::uint32_t, int> extrapolated;
    std::map<std::uint32_t, int> allowable;
    std::vector<std::pair<std::uint32_t, float>> growth;
    void GetPosition(int& ox, int& oz) const override { ox = x; oz = z; }
    void GetCitySize(int& w, int& h) const override { w = width; h = height; }
    float WorkforcePercentage() const override { return workforce; }
    float GetTaxRate(std::uint32_t) const override { return tax; }
    int GetPopulation(std::uint32_t purpose) const override {
        auto it = population.find(purpose);
        return it == population.end() ? 0 : it->second;
    }
    int GetExtrapolatedPopulation(std::uint32_t purpose) const override {
        auto it = extrapolated.find(purpose);
        return it == extrapolated.end() ? 0 : it->second;
    }
    int GetAllowableExtrapolation(std::uint32_t purpose) const override {
        auto it = allowable.find(purpose);
        return it == allowable.end() ? 0 : it->second;
    }
    void ExtrapolateGrowth(std::uint32_t purpose, float amount) override { growth.emplace_back(purpose, amount); }
};

struct FakeConnection final : IDemandConnection {
    std::uint32_t type = 0;
    const void* city = nullptr;
    cISCPropertyHolder* holder = nullptr;
    cISCPropertyHolder* PropertyHolder() override { return holder; }
    std::uint32_t GetType() const override { return type; }
    const void* CityOnOtherSide() const override { return city; }
};

struct FakeDeveloper final : IDemandDeveloper {
    std::uint32_t type = 1;
    std::uint32_t developerType = 0x1010;
    cSC4Demand* source = nullptr;
    std::int16_t desirability = 100;
    std::uint32_t GetType() const override { return type; }
    std::uint32_t GetDeveloperType() const override { return developerType; }
    cSC4Demand* GetSourceDemand() const override { return source; }
    std::int16_t DesirabilityAt(unsigned, unsigned) const override { return desirability; }
};

struct FakeHistory {
    float minimum = 0.0f, maximum = 10.0f, average = 5.0f;
};

}  // namespace

struct FakeHost final : IDemandSimulatorHost {
    // the lot of a lot-state message: the test sends a FakeLotState (the game's host resolves a cISC4Lot)
    bool MessageLot(cIGZUnknown* unknown, std::uint32_t& buildingType, cISCPropertyHolder*& building) override {
        auto* lot = static_cast<FakeLotState*>(unknown);
        buildingType = lot->GetBuildingType(false);
        building = lot->occupant.holder;
        return true;
    }
    std::map<cGZPersistResourceKey, std::unique_ptr<Exemplar>, cGZPersistResourceKeyOrder> exemplars;
    std::map<std::uint32_t, cGZPersistResourceKey> buildingKeys;
    std::vector<std::uint32_t> buildingTypes;
    std::set<std::uint32_t> notifications;
    std::vector<int> agents;
    int agentRemovals = 0;
    int shutdownCallbacks = 0;
    std::uint32_t day = 1000;
    std::uint64_t cycles = 0;
    float msToCycles = 1000.0f;
    int cellX = 2, cellZ = 1;
    std::uint32_t airportTrips = 3, seaportTrips = 1;
    std::vector<FakeHistory> historyCalls;
    std::vector<std::pair<std::uint32_t, std::int32_t>> records;
    std::vector<std::uint32_t> queueLengths;
    std::map<std::uint32_t, float> nation;
    std::map<std::uint32_t, float> ordinance;
    std::vector<FakeConnection> connections;
    std::list<IDemandRegionCity*> allCities, connectedCities;
    IDemandRegionCity* currentCity = nullptr;
    std::map<std::uint32_t, cISCPropertyHolder*> propertyHolders;   // traffic property holders
    std::uint32_t trafficArrived = 0;
    std::map<std::uint32_t, const DemandBuildingProfile*> profiles;
    std::map<std::uint32_t, IDemandDeveloper*> developers;
    std::vector<IDemandDeveloper*> developerList;
    std::map<std::uint32_t, std::int32_t> lastBuildDay;
    std::map<std::uint32_t, IDemandLot*> lots;
    std::map<std::uint32_t, float> workforce, eq;
    std::vector<std::pair<std::uint32_t, std::int32_t>> posts;

    Exemplar& MakeExemplar(const cGZPersistResourceKey& key) {
        auto& slot = exemplars[key];
        if (!slot) slot = std::make_unique<Exemplar>();
        return *slot;
    }
    Exemplar* Find(const cGZPersistResourceKey& key) {
        auto it = exemplars.find(key);
        return it == exemplars.end() ? nullptr : it->second.get();
    }

    void AddShutdownCallback(bool (*)(void*), void*) override { ++shutdownCallbacks; }
    void AddNotification(cIGZMessageTarget2*, std::uint32_t message) override { notifications.insert(message); }
    void RemoveNotification(cIGZMessageTarget2*, std::uint32_t message) override { notifications.erase(message); }
    void AddSimulatorAgent(cIGZMessageTarget2*, int interval) override { agents.push_back(interval); }
    void RemoveSimulatorAgent(cIGZMessageTarget2*) override { ++agentRemovals; }
    void PostMessage(std::uint32_t type, std::uint32_t data1, std::int32_t data2) override {
        posts.emplace_back(data1, data2);
        (void)type;
    }
    std::uint32_t CurrentDayNumber() override { return day; }
    std::uint64_t NowCycles() override { return cycles; }
    float MillisecondsToCycles() override { return msToCycles; }
    void ExemplarKeysOfType(std::uint32_t, std::vector<cGZPersistResourceKey>& out) override {
        for (const auto& entry : exemplars) out.push_back(entry.first);
    }
    cISCPropertyHolder* LoadExemplar(const cGZPersistResourceKey& key) override {
        Exemplar* exemplar = Find(key);
        return exemplar ? &exemplar->holder : nullptr;
    }
    void ReleaseExemplar(cISCPropertyHolder*) override {}
    void CityCellCount(int& x, int& z) override { x = cellX; z = cellZ; }
    std::uint32_t AirportTrips() override { return airportTrips; }
    std::uint32_t SeaportTrips() override { return seaportTrips; }
    IDemandRegionCity* CurrentRegionCity() override { return currentCity; }
    void HistoryMinMaxAverage(std::uint32_t, std::uint32_t, std::uint32_t, float& minimum, float& maximum,
                              float& average) override {
        FakeHistory h = historyCalls.empty() ? FakeHistory{} : historyCalls.front();
        minimum = h.minimum;
        maximum = h.maximum;
        average = h.average;
        historyCalls.push_back(h);
    }
    void HistoryRecord(std::uint32_t series, std::int32_t value) override { records.emplace_back(series, value); }
    void HistoryQueueLength(std::uint32_t series, std::uint32_t length) override {
        queueLengths.push_back(series);
        (void)length;
    }
    float NationDemandModifier(std::uint32_t purpose) override {
        auto it = nation.find(purpose);
        return it == nation.end() ? 0.0f : it->second;
    }
    float OrdinanceFactor(std::uint32_t property, std::uint32_t) override {
        auto it = ordinance.find(property);
        return it == ordinance.end() ? 0.0f : it->second;
    }
    void AllConnections(std::vector<IDemandConnection*>& out) override {
        for (FakeConnection& connection : connections) out.push_back(&connection);
    }
    void ConnectionsOfType(std::uint32_t type, std::vector<IDemandConnection*>& out) override {
        for (FakeConnection& connection : connections)
            if (connection.type == type) out.push_back(&connection);
    }
    void AllConnectedCities(std::list<IDemandRegionCity*>& out) override { out = allCities; }
    void ConnectedCities(std::list<IDemandRegionCity*>& out) override { out = connectedCities; }
    std::uint32_t TrafficArrived(cISCPropertyHolder*, std::uint32_t) override { return trafficArrived; }
    const DemandBuildingProfile* BuildingProfile(std::uint32_t type) override {
        auto it = profiles.find(type);
        return it == profiles.end() ? nullptr : it->second;
    }
    std::uint32_t DeveloperType(std::uint32_t purpose, std::int32_t wealth) override {
        switch (purpose) {
        case 1: return (wealth - 1) < 3 ? static_cast<std::uint32_t>((wealth - 1) * 0x10 + 0x1010) : 0;
        case 2: return (wealth - 1) < 3 ? static_cast<std::uint32_t>((wealth - 1) * 0x10 + 0x3110) : 0;
        case 3: return wealth == 2 ? 0x3320u : (wealth == 3 ? 0x3330u : 0u);
        case 5: return 0x4100;
        case 6: return 0x4200;
        case 7: return 0x4300;
        case 8: return 0x4400;
        default: return 0;
        }
    }
    IDemandDeveloper* Developer(std::uint32_t developerType) override {
        auto it = developers.find(developerType);
        return it == developers.end() ? nullptr : it->second;
    }
    void Developers(std::vector<IDemandDeveloper*>& out) override { out = developerList; }
    std::int32_t DayOfLastSuccessfulBuild(std::uint32_t developerType) override {
        auto it = lastBuildDay.find(developerType);
        return it == lastBuildDay.end() ? 0 : it->second;
    }
    IDemandLot* LotAt(int x, int z, int) override {
        auto it = lots.find(static_cast<std::uint32_t>(z) * 100u + static_cast<std::uint32_t>(x));
        return it == lots.end() ? nullptr : it->second;
    }
    float CellWorkforcePercent(unsigned x, unsigned z) override {
        auto it = workforce.find(static_cast<std::uint32_t>(z) * 100u + static_cast<std::uint32_t>(x));
        return it == workforce.end() ? 0.0f : it->second;
    }
    float CellEQ(unsigned x, unsigned z) override {
        auto it = eq.find(static_cast<std::uint32_t>(z) * 100u + static_cast<std::uint32_t>(x));
        return it == eq.end() ? 0.0f : it->second;
    }
    void AllBuildingTypes(std::vector<std::uint32_t>& out) override { out = buildingTypes; }
    bool BuildingKeyFromType(std::uint32_t type, cGZPersistResourceKey& key) override {
        auto it = buildingKeys.find(type);
        if (it == buildingKeys.end()) return false;
        key = it->second;
        return true;
    }
};

namespace {

// demand exemplar type property
void SetExemplarType(Exemplar& e, std::uint32_t type) {
    cSCPropertyHelp::SetPropertyValue(&e.holder, 0x10, type);
}

void AddDemandExemplar(FakeHost& host, std::uint32_t id, std::uint32_t group = 1) {
    Exemplar& e = host.MakeExemplar(Key(0x6534284a, group, id));
    SetExemplarType(e, 3);
    cSCPropertyHelp::SetPropertyValue(&e.holder, kDemandPropMax, std::int32_t{100});
    cSCPropertyHelp::SetPropertyValue(&e.holder, kDemandPropMin, std::int32_t{-100});
}

void AddIntProperty(FakeHost& host, const cGZPersistResourceKey& key, std::uint32_t id, std::int32_t value) {
    cSCPropertyHelp::SetPropertyValue(&host.MakeExemplar(key).holder, id, value);
}

}  // namespace

int main() {
    cSC4DemandSimulator::ResetStaticStateForTest();

    FakeHost host;
    SetDemandSimulatorHost(&host);

    // ---- the tuning exemplar -----------------------------------------------------------------------
    Exemplar& tuning = host.MakeExemplar(Key(kDemandTuningType, kDemandTuningGroup, kDemandTuningInstance));
    SetExemplarType(tuning, 0x1c);
    cSCPropertyHelp::SetPropertyValue(&tuning.holder, kDemandTunePropTaxCurve,
                                      std::vector<float>{0.0f, 0.0f, 100.0f, 5.0f, 200.0f, 10.0f});
    cSCPropertyHelp::SetPropertyValue(&tuning.holder, kDemandTunePropEconomyCurve,
                                      std::vector<float>{0.0f, 0.0f, 1.0f, 1.0f});
    cSCPropertyHelp::SetPropertyValue(&tuning.holder, kDemandTunePropHistoryDays, std::uint32_t{90});
    std::vector<float> utilization(13, 0.0f);
    utilization[2] = 1.0f;
    cSCPropertyHelp::SetPropertyValue(&tuning.holder, kDemandTunePropUtilization, utilization);
    cSCPropertyHelp::SetPropertyValue(&tuning.holder, kDemandTunePropSalaryWeights,
                                      std::vector<std::uint32_t>(12, 100u));

    // ---- the demand exemplars ----------------------------------------------------------------------
    for (std::uint32_t id : {0x1001u, 0x1010u, 0x1011u, 0x1020u, 0x1021u, 0x1030u, 0x1031u, 0x2000u, 0x3001u, 0x4001u,
                             0x2111u, 0x2112u, 0x2113u, 0x2114u, 0x2121u, 0x2122u, 0x2123u, 0x2124u,
                             0x2131u, 0x2132u, 0x2133u, 0x2134u,
                             0x3111u, 0x3121u, 0x3131u, 0x3321u, 0x3331u,
                             0x4101u, 0x4201u, 0x4301u, 0x4401u,
                             0x21010u, 0x21020u, 0x21030u, 0x23110u, 0x23120u, 0x23130u,
                             0x23320u, 0x23330u, 0x24100u, 0x24200u, 0x24300u, 0x24400u,
                             0x4800u, 0x4900u, 0x3800u,
                             0x12010u, 0x12020u, 0x12030u,
                             0x12111u, 0x12112u, 0x12113u, 0x12114u,
                             0x11010u, 0x13110u, 0x13111u,
                             0x90001u, 0x90002u, 0x90003u, 0x90004u, 0x90005u, 0x90006u,
                             0x90007u, 0x90008u, 0x90009u}) {
        AddDemandExemplar(host, id);
    }
    // the tax channels need a supply so that GetNeutralTaxRate() sees 100 + 50 + 50
    AddIntProperty(host, Key(0x6534284a, 1, 0x1001), kDemandPropSupply, 100);
    AddIntProperty(host, Key(0x6534284a, 1, 0x3001), kDemandPropSupply, 50);
    AddIntProperty(host, Key(0x6534284a, 1, 0x4001), kDemandPropSupply, 50);

    // link owner 0x90001: supply 10, demand 20, a 50% supply link to 0x90002 and a 25% supply->demand link
    // to 0x90003; 0x90005 demands a cap of 0.4 on 0x90006; 0x90007 sums the supply of 0x90008/0x90009.
    {
        Exemplar& owner = host.MakeExemplar(Key(0x6534284a, 1, 0x90001));
        SetExemplarType(owner, 3);
        cSCPropertyHelp::SetPropertyValue(&owner.holder, kDemandPropSupply, std::int32_t{10});
        cSCPropertyHelp::SetPropertyValue(&owner.holder, kDemandPropDemand, std::int32_t{20});
        cSCPropertyHelp::SetPropertyValue(&owner.holder, kDemandPropLinkLo, std::vector<std::uint32_t>{0x90002, 50});
        cSCPropertyHelp::SetPropertyValue(&owner.holder, kDemandPropLinkSupply, std::vector<std::uint32_t>{0x90003, 25});

        Exemplar& cap = host.MakeExemplar(Key(0x6534284a, 1, 0x90005));
        SetExemplarType(cap, 3);
        cSCPropertyHelp::SetPropertyValue(&cap.holder, kDemandPropSupply, std::int32_t{10});
        cSCPropertyHelp::SetPropertyValue(&cap.holder, kDemandPropDemand, std::int32_t{4});
        cSCPropertyHelp::SetPropertyValue(&cap.holder, kDemandPropLinkCap, std::vector<std::uint32_t>{0x90006});

        Exemplar& sum = host.MakeExemplar(Key(0x6534284a, 1, 0x90007));
        SetExemplarType(sum, 3);
        cSCPropertyHelp::SetPropertyValue(&sum.holder, kDemandPropLinkSum, std::vector<std::uint32_t>{0x90008, 0x90009});

        AddIntProperty(host, Key(0x6534284a, 1, 0x90008), kDemandPropSupply, 10);
        AddIntProperty(host, Key(0x6534284a, 1, 0x90009), kDemandPropSupply, 20);
    }

    // region-use flags for the census channels and the regional supply channel
    for (std::uint32_t id : {0x1011u, 0x2112u, 0x1021u, 0x1031u}) {
        cSCPropertyHelp::SetPropertyValue(&host.MakeExemplar(Key(0x6534284a, 1, id)).holder,
                                          kDemandPropRegionUse, std::uint8_t{2});
    }
    cSCPropertyHelp::SetPropertyValue(&host.MakeExemplar(Key(0x6534284a, 1, 0x21010)).holder,
                                      kDemandPropRegionUse, std::uint8_t{1});
    cSCPropertyHelp::SetPropertyValue(&host.MakeExemplar(Key(0x6534284a, 1, 0x31010)).holder,
                                      kDemandPropRegionUse, std::uint8_t{0});
    AddDemandExemplar(host, 0x31010);

    // ---- the building / occupant-effect exemplar ---------------------------------------------------
    const cGZPersistResourceKey buildingKey = Key(0x6534284a, 0x27812840, 0x42);
    {
        Exemplar& building = host.MakeExemplar(buildingKey);
        SetExemplarType(building, 2);
        cSCPropertyHelp::SetPropertyValue(&building.holder, kDemandPropEffectDemand,
                                          std::vector<std::uint32_t>{0x90002, 300});
        cSCPropertyHelp::SetPropertyValue(&building.holder, kDemandPropEffectDemandAux,
                                          std::vector<float>{0.5f});
        cSCPropertyHelp::SetPropertyValue(&building.holder, kDemandPropEffectSupply,
                                          std::vector<std::uint32_t>{0x90003, 200});
    }
    host.buildingTypes = {0x42};
    host.buildingKeys[0x42] = buildingKey;
    host.buildingKeys[0x43] = buildingKey;

    DemandBuildingProfile residentialProfile;
    residentialProfile.purpose = 1;
    residentialProfile.wealth = 1;
    residentialProfile.capacities.push_back({{0x1010, 100}, {0x1020, 40}});
    host.profiles[0x42] = &residentialProfile;

    FakeLot lot;
    host.lots[0] = &lot;                          // (x=0, z=0)
    host.lots[1] = &lot;                          // (x=1, z=0)
    host.workforce[0] = 0.5f;
    host.workforce[1] = 0.5f;
    host.eq[0] = 100.0f;                          // 100 / 50.1 + 1 = 2.996 -> bucket 2
    host.eq[1] = 100.0f;

    FakeDeveloper residentialDeveloper;
    host.developers[0x1010] = &residentialDeveloper;

    for (std::uint32_t purpose : {0x1010u, 0x1020u, 0x1030u, 0x3110u, 0x3120u, 0x3130u,
                                  0x3320u, 0x3330u, 0x4100u, 0x4200u, 0x4300u, 0x4400u})
        host.nation[purpose] = 0.25f;
    host.ordinance[0x2a651010] = 0.5f;            // 0x21010 only

    // ---- construction / interface ------------------------------------------------------------------
    auto* sim = new cSC4DemandSimulator();
    sim->AddRef();
    CHECK(sim->GetGZCLSID() == 0x2990bff2);
    CHECK(sim->GetSimulatorType() == 0x279681d7);
    CHECK(sim->State() == 3);
    void* iface = nullptr;
    CHECK(sim->QueryInterface(0x47967e16, &iface));
    CHECK(iface == static_cast<cISC4DemandSimulator*>(sim));
    static_cast<cIGZUnknown*>(iface)->Release();
    CHECK(sim->QueryInterface(0x452294aa, &iface));
    static_cast<cIGZUnknown*>(iface)->Release();
    CHECK(sim->QueryInterface(0xe4fda3d4, &iface));
    static_cast<cIGZUnknown*>(iface)->Release();
    CHECK(sim->QueryInterface(1, &iface));
    static_cast<cIGZUnknown*>(iface)->Release();
    CHECK(!sim->QueryInterface(0x12345678, &iface));

    // ---- Init --------------------------------------------------------------------------------------
    CHECK(sim->Init());
    CHECK(sim->IsInitialised());
    CHECK(host.shutdownCallbacks == 1);
    CHECK(host.notifications.size() == 5);
    CHECK((host.agents == std::vector<int>{0, 4, 6, 10}));
    CHECK(host.queueLengths.size() == 12);
    CHECK(sim->TaxCurve() && sim->EconomyCurve());
    CHECK(sim->HistoryDays() == 90);
    CHECK(sim->UtilizationWeights().size() == 13 && sim->UtilizationWeights()[2] == 1.0f);
    CHECK(sim->SalaryWeights()[0] == 100 && sim->SalaryWeights()[11] == 100);
    CHECK(sim->DemandsForTest().size() >= 30);

    // tax curve over the three neutral-tax supplies (100 + 50 + 50 = 200 -> 10)
    CHECK_NEAR(sim->GetNeutralTaxRate(), 10.0);
    CHECK(sim->GetDemand(0x21000, 0x10) == sim->GetDemand(0x21010, 0));
    CHECK(sim->GetDemand(0x21010, 0) == sim->DemandsForTest().at(0x21010));

    // DoDemandIndexLinks ran during Init: the sum link settled 0x90007 at 30, the cap at 0.4
    CHECK_NEAR(sim->GetDemand(0x90007, 0)->QuerySupplyValue(), 30.0);
    CHECK_NEAR(sim->GetDemand(0x90006, 0)->GetDemandCap(), 0.4);

    // applying the 0x90001 links by hand
    sim->GetDemand(0x90001, 0)->AddToSupplyValue(10.0f);
    sim->DoDemandIndexLinks();
    CHECK_NEAR(sim->GetDemand(0x90002, 0)->QuerySupplyValue(), 5.0);         // 10 * 50%
    CHECK_NEAR(sim->GetDemand(0x90003, 0)->QueryDemandValue(), 2.5);         // 10 * 25%

    // ---- occupant effects --------------------------------------------------------------------------
    auto* effectDemand = sim->GetDemand(0x90002, 0);
    auto* effectSupply = sim->GetDemand(0x90003, 0);
    const float demandNewBefore = effectDemand->QueryNewDemand();
    const float supplyNewBefore = effectSupply->QueryNewSupply();
    sim->AddAgentEffect(0x42, true, SC4Percentage(0.5f));
    CHECK_NEAR(effectDemand->QueryNewDemand(), demandNewBefore + 0.25);      // aux 0.5 * scale 0.5
    CHECK_NEAR(effectSupply->QueryNewSupply(), supplyNewBefore + 100.0);     // 200 * 0.5
    sim->UpdateOccupantEffects(0x42, SC4Percentage(1.0f), SC4Percentage(1.0f));   // equal -> no change
    CHECK_NEAR(effectDemand->QueryNewDemand(), demandNewBefore + 0.25);
    sim->UpdateOccupantEffects(0x42, SC4Percentage(0.5f), SC4Percentage(0.0f));   // removing 0.5 undoes it
    CHECK_NEAR(effectDemand->QueryNewDemand(), demandNewBefore);
    CHECK_NEAR(effectSupply->QueryNewSupply(), supplyNewBefore);

    // ---- tax / economy modifiers, then the census --------------------------------------------------
    sim->DoMonthlySimulation();                    // state 3 -> 0
    CHECK(sim->State() == 0);
    sim->DoIdle(5);                                // state 0 -> 1
    CHECK(sim->State() == 1);
    sim->DoIdle(5);                                // census over a 2x1 city -> state 2
    CHECK(sim->State() == 2);
    CHECK_NEAR(sim->CensusForTest().at(0x2112), 100.0);   // 2 cells * (100 * 0.5)
    CHECK_NEAR(sim->CensusForTest().at(0x1011), 100.0);
    CHECK_NEAR(sim->GetDemand(0x1011, 0)->QueryNewDemand(), 100.0);
    CHECK_NEAR(sim->GetDemand(0x2112, 0)->QueryNewDemand(), 100.0);
    sim->DoIdle(5);                                // state 2 -> 3, records the history
    CHECK(sim->State() == 3);
    sim->DoDemandIndexLinks();                     // fold the pending supply/demand into the channels
    CHECK_NEAR(sim->GetDemand(0x1011, 0)->QueryDemandValue(), 100.0);
    CHECK_NEAR(sim->GetDemand(0x2112, 0)->QueryDemandValue(), 100.0);

    // the tax modifier of 0x21010: curve(10 * 0.1 / 10) + 0.5 - 1 = 0.1 - 0.5
    CHECK_NEAR(sim->GetDemand(0x21010, 0)->GetTaxModifier(), -0.4);
    CHECK_NEAR(sim->GetDemand(0x21010, 0)->GetEconomyModifier(), 0.25);
    CHECK_NEAR(sim->GetDemand(0x21020, 0)->GetTaxModifier(), -0.9);
    CHECK_NEAR(sim->GetDemand(0x23320, 0)->GetEconomyModifier(), 0.25);
    CHECK(!host.records.empty());

    // ---- daily simulation / messages ---------------------------------------------------------------
    host.records.clear();
    auto* day = new cRZMessage2Standard();
    day->AddRef();
    day->SetType(kDemandMessageDay);
    CHECK(sim->DoMessage(day));
    CHECK(!host.records.empty());
    auto* month = new cRZMessage2Standard();
    month->AddRef();
    month->SetType(kDemandMessageMonth);
    CHECK(sim->DoMessage(month));
    auto* connections = new cRZMessage2Standard();
    connections->AddRef();
    connections->SetType(kDemandMessageConnectionCreated);
    connections->SetData1(1);
    connections->SetData2(2);
    connections->SetData3(3);
    CHECK(sim->DoMessage(connections));
    auto* tunables = new cRZMessage2Standard();
    tunables->AddRef();
    tunables->SetType(kDemandMessageReadTunables);
    CHECK(sim->DoMessage(tunables));
    auto* links = new cRZMessage2Standard();
    links->AddRef();
    links->SetType(kDemandMessageIndexLinks);
    CHECK(sim->DoMessage(links));
    auto* interval0 = new cRZMessage2Standard();
    interval0->AddRef();
    interval0->SetType(kDemandMessageInterval0);
    CHECK(sim->DoMessage(interval0));
    auto* idle = new cRZMessage2Standard();
    idle->AddRef();
    idle->SetType(kDemandMessageIdleTime);
    idle->SetData3(5);
    CHECK(sim->DoMessage(idle));
    CHECK(sim->DoMessage(nullptr));

    // ---- the lot-state message ---------------------------------------------------------------------
    FakeLotState lotState;
    auto* lotMessage = new cRZMessage2Standard();
    lotMessage->AddRef();
    lotMessage->SetType(kDemandMessageLotStateChanged);
    lotMessage->SetIGZUnknown(&lotState);
    lotMessage->SetData1(2);                       // occupied before
    lotMessage->SetData2(0);                       // empty now
    lotMessage->SetData3(0);                       // building type from the lot
    const float capSupplyBefore = sim->GetDemand(0x1020, 0)->QueryNewSupply();
    const float agentDemandBefore = effectDemand->QueryNewDemand();
    CHECK(sim->DoMessage(lotMessage));
    CHECK_NEAR(sim->GetDemand(0x1020, 0)->QueryNewSupply(), capSupplyBefore - 40.0);   // capacity 0x1020
    CHECK_NEAR(effectDemand->QueryNewDemand(), agentDemandBefore - 0.5);               // effect, add=false
    lotMessage->SetData1(2);
    lotMessage->SetData2(2);                       // both occupied -> no change
    const float unchangedDemand = effectDemand->QueryNewDemand();
    CHECK(sim->DoMessage(lotMessage));
    CHECK_NEAR(effectDemand->QueryNewDemand(), unchangedDemand);
    lotMessage->SetIGZUnknown(nullptr);

    // ---- EvaluateConnectionUtilization -------------------------------------------------------------
    FakeCity cityA, cityB, cityC, cityD, cityE, cityF, cityG;
    FakeConnection all1;
    all1.type = 1;
    all1.holder = &tuning.holder;
    host.trafficArrived = 1000;
    host.connections.push_back(all1);
    auto addCityConnection = [&](std::uint32_t type, FakeCity& city) {
        FakeConnection c;
        c.type = type;
        c.city = &city;
        host.connections.push_back(c);
    };
    addCityConnection(3, cityA);
    addCityConnection(0xc, cityB);
    addCityConnection(8, cityC);
    addCityConnection(2, cityD);
    addCityConnection(0xa, cityE);
    addCityConnection(1, cityF);
    addCityConnection(7, cityG);
    host.airportTrips = 3;
    host.seaportTrips = 1;
    const float util3800Before = sim->GetDemand(0x3800, 0)->QueryNewSupply();
    const float util4800Before = sim->GetDemand(0x4800, 0)->QueryNewSupply();
    const float old130 = sim->Value130();
    const float old134 = sim->Value134();
    sim->EvaluateConnectionUtilization();
    // A = {A,B} -> log 2 * 1 * 1.442695 = 1; + 1 * 3 (airport) + 20 * 1 (seaport)
    CHECK_NEAR(sim->Value130(), 24.0);
    // seven connections match the 0x150e type mask (1, 2, 3, 8, 10, 12), each 1000 arrived * 20
    CHECK_NEAR(sim->Value134(), 140002.3f);
    CHECK_NEAR(sim->GetDemand(0x3800, 0)->QueryNewSupply(), util3800Before + (24.0 - old130));
    CHECK_NEAR(sim->GetDemand(0x4800, 0)->QueryNewSupply(), util4800Before + (140002.3f - old134));

    // ---- ResetRegionalDemands ----------------------------------------------------------------------
    cityA.extrapolated[0x21010] = 100;
    cityB.extrapolated[0x21010] = 50;
    host.allCities = {&cityA, &cityB};
    host.connectedCities = {&cityB};
    host.currentCity = &cityA;
    sim->GetDemand(0x21010, 0)->SetSupplyValue(5.0f);
    sim->ResetRegionalDemands(true);
    CHECK_NEAR(sim->GetDemand(0x31010, 0)->QueryNewSupply(), 50.0);   // sumAll 50 - (0 + 0)
    host.currentCity = nullptr;

    // ---- ExportDemand ------------------------------------------------------------------------------
    host.connectedCities = {&cityA, &cityB};
    sim->ResetRegionalDemands(false);
    cityA.tax = 1.0f;
    cityB.tax = 2.0f;
    cityA.allowable[0x1010] = 500;
    cityB.allowable[0x1010] = 20;
    cityA.population[0x1010] = 10;
    cityA.population[0x1011] = 5;
    cityA.population[0x2111] = 40;
    cityA.population[0x2112] = 30;
    cityA.population[0x2113] = 20;
    cityA.population[0x2114] = 10;
    auto* sourceDemand = new cSC4Demand(sim);
    sourceDemand->SetDemandValue(300.0f);
    FakeDeveloper exporter;
    exporter.type = 1;
    exporter.developerType = 0x1010;
    exporter.source = sourceDemand;
    host.developerList = {&exporter};
    host.lastBuildDay[0x1010] = static_cast<std::int32_t>(host.day) - 100;
    const float exportSupplyBefore = sim->GetDemand(0x11010, 0)->QueryNewSupply();
    sim->ExportDemand();
    CHECK_NEAR(sim->GetDemand(0x11010, 0)->QueryNewSupply(), exportSupplyBefore + 300.0);
    CHECK_NEAR(sim->GetDemand(0x12111, 0)->QueryNewSupply(), 30.0);    // 40 * (0.5*0.5*300) / 100
    CHECK_NEAR(sim->GetDemand(0x12112, 0)->QueryNewSupply(), 22.5);
    CHECK(cityA.growth.size() >= 5);                                    // 0x1010 + 0x2111..0x2114
    bool sawGrowth = false;
    for (const auto& entry : cityA.growth)
        if (entry.first == 0x1010 && std::fabs(entry.second - 300.0f) < 1e-3f) sawGrowth = true;
    CHECK(sawGrowth);

    // ExtrapolateBusinessCensus on a commercial demand type
    const cGZPersistResourceKey jobsKey = Key(0x6534284a, 0xc7bb4816, 0x3111);
    Exemplar& jobs = host.MakeExemplar(jobsKey);
    cSCPropertyHelp::SetPropertyValue(&jobs.holder, kDemandJobsProperty,
                                      std::vector<std::uint32_t>{0x2010, 100, 0x2020, 50, 0x2030, 25});
    cityA.population[0x3110] = 10;
    cityA.population[0x3111] = 5;
    const float jobsBefore = sim->GetDemand(0x12010, 0)->QueryNewDemand();
    const float jobs2Before = sim->GetDemand(0x12020, 0)->QueryNewDemand();
    const float jobs3Before = sim->GetDemand(0x12030, 0)->QueryNewDemand();
    sim->ExtrapolateBusinessCensus(&cityA, 0x3110, 10.0f);
    CHECK_NEAR(sim->GetDemand(0x12010, 0)->QueryNewDemand(), jobsBefore + 5.0);   // 1.0 * 5
    CHECK_NEAR(sim->GetDemand(0x12020, 0)->QueryNewDemand(), jobs2Before + 2.5);  // 0.5 * 5
    CHECK_NEAR(sim->GetDemand(0x12030, 0)->QueryNewDemand(), jobs3Before + 1.25); // 0.25 * 5
    CHECK_NEAR(sim->GetDemand(0x13111, 0)->QueryNewSupply(), 5.0);

    // ---- GetJobsBySensus / population summary ------------------------------------------------------
    CHECK(sim->GetJobsBySensus(0x1011) == 0);
    float jobsOut[4] = {0, 0, 0, 0};
    sim->CalculateJobsPerUnitOfDemand(jobsOut, 0x3111, 0);
    CHECK_NEAR(jobsOut[1], 1.0);
    CHECK_NEAR(jobsOut[2], 0.5);
    CHECK_NEAR(jobsOut[3], 0.25);
    std::map<std::uint32_t, int> summary;
    sim->GetLocalPopulationSummary(summary);
    CHECK(summary.count(0x1011) == 1 && summary[0x1011] == 100);
    CHECK(summary.count(0x21010) == 1);

    // ---- persistence -------------------------------------------------------------------------------
    PortMemoryOStream out;
    CHECK(sim->Write(out));
    CHECK(out.bytes().size() > 6 && out.bytes()[0] == 2 && out.bytes()[1] == 0);
    auto* copy = new cSC4DemandSimulator();
    copy->AddRef();
    PortMemoryIStream in(out.bytes().data(), out.bytes().size());
    CHECK(copy->Read(in));
    CHECK(copy->DemandsForTest().size() == sim->DemandsForTest().size());
    CHECK_NEAR(copy->DemandsForTest().at(0x1011)->QueryDemandValue(),
               sim->GetDemand(0x1011, 0)->QueryDemandValue());
    CHECK_NEAR(copy->DemandsForTest().at(0x21010)->GetTaxModifier(),
               sim->GetDemand(0x21010, 0)->GetTaxModifier());
    const unsigned char badVersion[2] = {3, 0};
    PortMemoryIStream badStream(badVersion, 2);
    CHECK(!copy->Read(badStream));
    copy->Release();   // read but never initialised: its destructor frees the demands it read

    // ---- shutdown ----------------------------------------------------------------------------------
    CHECK(sim->Shutdown());
    CHECK(!sim->IsInitialised());
    CHECK(host.notifications.empty());
    CHECK(host.agentRemovals == 1);
    CHECK(sim->DemandsForTest().empty());

    delete sourceDemand;
    day->Release();
    month->Release();
    connections->Release();
    tunables->Release();
    links->Release();
    interval0->Release();
    idle->Release();
    lotMessage->Release();
    sim->Release();
    SetDemandSimulatorHost(nullptr);
    std::printf(failures ? "FAILED (%d)\n" : "OK\n", failures);
    return failures != 0;
}
