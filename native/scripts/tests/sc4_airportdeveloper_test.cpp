// cSC4AirportDeveloper — unit test. Drives the recovered airport developer through a fake
// IAirportDeveloperHost (the base's fake host plus the real recovered cSC4BudgetSimulator and
// cSC4TrafficSimulator) and checks every airport function against the behaviour recovered from the binary:
// the four COM views and the shared reference count, the identity (0xca270e9d / zone 11 / history 0x6a3e9032 /
// advisor 0x6a4119f0 / group 0x1508), the department budget Init creates (id 0x6a4119f0, group 0xaa369059,
// name 0x6a3ff01c/0xea493564, max funding 1.2, line item 1 with display flag 1 cleared), the six advisor
// notifications and their removal, the funding ratio that scales GetAirportUsageValues / GetMaintenanceCost /
// GetNumAirportTrips, the six SendAdvisorUpgradeMessage ids, DoMessage's interception (it grows the ports),
// the version-2 stream round trip, and the DoMonthlyTick office-traffic split over the subnetworks each
// airport touches (0x3320 + 0x3330 occupants, capacity * funding ratio, guarded against zero funding).
//
// The vtable slot order of the built class was compared slot by slot with the original airport vtable at
// 0x100a6f700 (29 slots: the 15 interface virtuals, the destructor pair, then FinalRelease/Write/Read/
// GetGZCLSID/DoMessage/UpdateBudget followed by the seven growth virtuals) and the three secondary vtables.
//
// Real game data: the lots are real cSC4Lot objects, the exemplars are real cSCPropertyHolder + cSCProperty +
// cRZVariant, the streams are the port memory streams, the department is the recovered cSC4DepartmentBudget
// and the traffic data comes from the recovered cSC4TrafficSimulator.

#include "sc4/AirportDeveloper.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/SCProperty.h"

namespace {

int g_failures = 0;

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

using namespace sc4;

bool Near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

//----------------------------------------------------------------------------------------------- services

struct FakeSimulator final : IPortSimulatorApi {
    struct Agent {
        cIGZMessageTarget2* target = nullptr;
        int interval = 0;
        std::string name;
        std::uint32_t flags = 0;
    };
    int refs = 0;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    std::vector<Agent> added;
    std::vector<cIGZMessageTarget2*> removed;
    void AddAgent(cIGZMessageTarget2* target, int interval, const char* name,
                  std::uint32_t flags) override {
        added.push_back({target, interval, name ? name : "", flags});
    }
    void RemoveAgent(cIGZMessageTarget2* target) override { removed.push_back(target); }
};

struct FakeAdvisorSystem final : IPortAdvisorSystemApi {
    int refs = 0;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    std::vector<std::pair<int, void*>> events;
    void PushEvent(int event, void* subject) override { events.emplace_back(event, subject); }
};

struct FakeCity final : IPortCityApi {
    int refs = 0;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    FakeSimulator* simulator = nullptr;
    FakeAdvisorSystem* advisorSystem = nullptr;
    IPortSimulatorApi* GetSimulator() override { return simulator; }
    IPortAdvisorSystemApi* GetAdvisorSystem() override { return advisorSystem; }
};

struct FakeZoneManager final : IPortZoneManagerApi {
    int refs = 0;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    int clearCalls = 0;
    void ClearZone(const PortCellRegion&) override { ++clearCalls; }
};

// Every lot the fakes create starts with two references (the caller's and the lot list's); the fixture
// drops whatever is left of them when it goes away.
std::vector<cSC4Lot*> g_testLots;
// Shutdown (the owner of the port lots and configurations) does nothing on a developer that was only Read.
void FreeReadState(cSC4PortDeveloper* d) {
    for (cPortLot* port : d->PortLots()) delete port;
    for (cPortTypeConfig* config : d->Configurations()) delete config;
}
void ReleaseTestLots() {
    for (cSC4Lot* lot : g_testLots) {
        for (std::uint32_t n = lot->AddRef(); n > 0; --n) lot->Release();   // the last Release deletes the lot
    }
    g_testLots.clear();
}

struct FakeLotManager final : IPortLotManagerApi {
    int refs = 0;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    std::map<std::pair<int, int>, cSC4Lot*> lots;
    std::vector<cSC4Lot*> created;
    struct CreateCall {
        int x = 0, z = 0, sizeX = 0, sizeZ = 0, facing = 0;
    };
    std::vector<CreateCall> createCalls;
    int getCalls = 0;
    cSC4Lot* GetLot(int x, int z, bool) override {
        ++getCalls;
        auto it = lots.find({x, z});
        return it == lots.end() ? nullptr : it->second;
    }
    cSC4Lot* GetOccupantLot(cISC4Occupant*) override { return nullptr; }
    bool CreateLot(int x, int z, int sizeX, int sizeZ, int facing, cSC4Lot** out) override {
        createCalls.push_back({x, z, sizeX, sizeZ, facing});
        auto* lot = new cSC4Lot(x, z, sizeX, sizeZ, facing);
        g_testLots.push_back(lot);
        lot->AddRef();
        lot->AddRef();
        created.push_back(lot);
        lots[{x, z}] = lot;
        *out = lot;
        return true;
    }
};

struct FakeBudget final : IPortBudgetApi {
    int refs = 0;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    std::vector<std::int64_t> withdrawals;
    void WithdrawFunds(std::int64_t amount) override { withdrawals.push_back(amount); }
};

struct FakeDemand final : IPortDemandApi {
    struct Effect {
        std::uint32_t buildingType = 0;
        float oldUsage = 0;
        float newUsage = 0;
    };
    int refs = 0;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    std::vector<Effect> effects;
    void UpdateOccupantEffects(std::uint32_t buildingType, const SC4Percentage& oldUsage,
                               const SC4Percentage& newUsage) override {
        effects.push_back({buildingType, oldUsage.value, newUsage.value});
    }
};

struct FakeLotConfiguration final : IPortLotConfiguration {
    int refs = 1;
    std::uint32_t id = 0;
    int growthStage = 1;
    std::vector<int> compatibleZones;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    std::uint32_t GetID() const override { return id; }
    int GetGrowthStage() const override { return growthStage; }
    bool IsCompatibleWithZoneType(int zoneType) const override {
        return compatibleZones.empty() ||
               std::find(compatibleZones.begin(), compatibleZones.end(), zoneType) !=
                   compatibleZones.end();
    }
};

struct FakeLotConfigurationManager final : IPortLotConfigurationManagerApi {
    int refs = 0;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    std::map<std::uint32_t, FakeLotConfiguration*> configurations;
    IPortLotConfiguration* GetLotConfiguration(std::uint32_t id) override {
        auto it = configurations.find(id);
        return it == configurations.end() ? nullptr : it->second;
    }
};

struct FakeLotDeveloper final : IPortLotDeveloperApi {
    int refs = 0;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    int constructs = 0;
    bool ConstructDefaultLot(cSC4Lot*, IPortLotConfiguration*, std::uint32_t, bool) override {
        ++constructs;
        return true;
    }
    void StartDemolishLot(cSC4Lot*) override {}
    void EndDemolishLot(cSC4Lot*) override {}
};

struct FakeBuildingDevelopment final : IPortBuildingDevelopmentApi {
    int refs = 0;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    FakeLotDeveloper* lotDeveloper = nullptr;
    IPortLotDeveloperApi* GetLotDeveloper() override { return lotDeveloper; }
};

struct FakeResourceManager final : IPortResourceApi {
    int refs = 0;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    std::map<std::uint32_t, cISCPropertyHolder*> exemplars;
    cISCPropertyHolder* LoadExemplar(std::uint32_t instance) override {
        auto it = exemplars.find(instance);
        if (it == exemplars.end()) return nullptr;
        it->second->AddRef();
        return it->second;
    }
};

struct FakeMessageServer final : cIGZMessageServer2 {
    int refs = 0;
    std::vector<std::pair<cIGZMessageTarget2*, std::uint32_t>> added, removed;
    struct Sent {
        std::uint32_t type = 0;
        std::uint64_t data1 = 0;
        std::uint64_t data2 = 0;
    };
    std::vector<Sent> sent;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZUnknown) return false;
        *out = static_cast<cIGZUnknown*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool MessageSend(cIGZMessage2* message) override {
        auto* standard = static_cast<cIGZMessage2Standard*>(message);
        sent.push_back({standard->GetType(), standard->GetData1(), standard->GetData2()});
        return true;
    }
    bool MessagePost(cIGZMessage2*, bool) override { return true; }
    bool AddNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        added.emplace_back(target, type);
        return true;
    }
    bool RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        removed.emplace_back(target, type);
        return true;
    }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return static_cast<std::uint32_t>(refs); }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

// The budget host the department budget talks to (name lookups and the funding messages).
struct FakeBudgetHost final : IBudgetHost {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> localizedNames;
    std::vector<std::uint32_t> neededFundingMessages;
    bool GetBudgetItemForPurpose(cISCPropertyHolder*, std::uint32_t, BudgetItem&) override {
        return false;
    }
    bool GetBudgetItemInfo(cISCPropertyHolder*, std::vector<BudgetItem>&) override { return false; }
    void ChangeBudgetItemCost(cISCPropertyHolder*, std::uint32_t, std::int64_t) override {}
    void ChangeBudgetItemLocalFunding(cISCPropertyHolder*, std::uint32_t, float) override {}
    void NeededFundingChanged(cSC4DepartmentBudget*) override {
        neededFundingMessages.push_back(kBudgetMessageNeededFundingChanged);
    }
    void FundingPercentageChanged(cSC4DepartmentBudget*) override {}
    void PostBudgetMessage(std::uint32_t, cSC4DepartmentBudget*, std::uint32_t, std::uint32_t) override {}
    void NotifyLocalFundingChanged(cISCPropertyHolder*) override {}
    std::string LoadLocalizedString(std::uint32_t group, std::uint32_t instance) override {
        localizedNames.emplace_back(group, instance);
        return "Airport";
    }
    std::string BuildingName(std::uint32_t) override { return "Airport"; }
};

//----------------------------------------------------------------------------------------------- hosts

// The airport host: the base's fake services plus the two views only the airport needs (the same objects the
// real host would return from Budget()/City()).
struct FakeAirportHost final : IAirportDeveloperHost {
    FakeMessageServer* messages = nullptr;
    FakeSimulator* simulator = nullptr;
    FakeCity* city = nullptr;
    FakeZoneManager* zoneManager = nullptr;
    FakeLotManager* lotManager = nullptr;
    FakeBudget* budget = nullptr;
    FakeDemand* demand = nullptr;
    FakeBuildingDevelopment* buildingDevelopment = nullptr;
    FakeLotConfigurationManager* lotConfigurationManager = nullptr;
    FakeResourceManager* resourceManager = nullptr;
    cSC4BudgetSimulator* budgetSimulator = nullptr;
    cSC4TrafficSimulator* trafficSimulator = nullptr;

    cIGZMessageServer2* MessageServer() override { return messages; }
    IPortSimulatorApi* Simulator() override { return simulator; }
    IPortCityApi* City() override { return city; }
    IPortZoneManagerApi* ZoneManager() override { return zoneManager; }
    IPortLotManagerApi* LotManager() override { return lotManager; }
    IPortBudgetApi* Budget() override { return budget; }
    IPortDemandApi* Demand() override { return demand; }
    IPortBuildingDevelopmentApi* BuildingDevelopment() override { return buildingDevelopment; }
    IPortLotConfigurationManagerApi* LotConfigurationManager() override { return lotConfigurationManager; }
    IPortResourceApi* ResourceManager() override { return resourceManager; }
    cSC4Lot* MessageLot(cIGZUnknown*) override { return nullptr; }
    cSC4BudgetSimulator* BudgetSimulator() override { return budgetSimulator; }
    cSC4TrafficSimulator* TrafficSimulator() override { return trafficSimulator; }
};

// A host that only implements the base's seam: the airport must report Init failure instead of faulting.
struct FakeBaseOnlyHost final : IPortDeveloperHost {
    FakeAirportHost* inner = nullptr;
    cIGZMessageServer2* MessageServer() override { return inner->messages; }
    IPortSimulatorApi* Simulator() override { return inner->simulator; }
    IPortCityApi* City() override { return inner->city; }
    IPortZoneManagerApi* ZoneManager() override { return inner->zoneManager; }
    IPortLotManagerApi* LotManager() override { return inner->lotManager; }
    IPortBudgetApi* Budget() override { return inner->budget; }
    IPortDemandApi* Demand() override { return inner->demand; }
    IPortBuildingDevelopmentApi* BuildingDevelopment() override { return inner->buildingDevelopment; }
    IPortLotConfigurationManagerApi* LotConfigurationManager() override {
        return inner->lotConfigurationManager;
    }
    IPortResourceApi* ResourceManager() override { return inner->resourceManager; }
    cSC4Lot* MessageLot(cIGZUnknown*) override { return nullptr; }
};

//----------------------------------------------------------------------------------------------- fixture

// The real property holders used as the port type exemplars.
struct Exemplars {
    std::vector<std::unique_ptr<cSCPropertyHolder>> holders;
    cSCPropertyHolder* Make() {
        holders.push_back(std::make_unique<cSCPropertyHolder>());
        cSCPropertyHolder* holder = holders.back().get();
        holder->AddRef();
        return holder;
    }
    static void Uint(cSCPropertyHolder* holder, std::uint32_t id, std::uint32_t value) {
        cRZVariant variant;
        variant.SetValUint32(value);
        holder->AddProperty(id, &variant, true);
    }
    static void Uint8(cSCPropertyHolder* holder, std::uint32_t id, std::uint8_t value) {
        cRZVariant variant;
        variant.SetValUint8(value);
        holder->AddProperty(id, &variant, true);
    }
    static void Float(cSCPropertyHolder* holder, std::uint32_t id, float value) {
        cRZVariant variant;
        variant.SetValFloat32(value);
        holder->AddProperty(id, &variant, true);
    }
    static void Uints(cSCPropertyHolder* holder, std::uint32_t id,
                      std::initializer_list<std::uint32_t> values) {
        std::vector<std::uint32_t> data(values);
        cRZVariant variant;
        variant.RefUint32(data.data(), static_cast<std::uint32_t>(data.size()));
        holder->AddProperty(id, &variant, true);
    }
    static void Int64s(cSCPropertyHolder* holder, std::uint32_t id,
                       std::initializer_list<std::int64_t> values) {
        std::vector<std::int64_t> data(values);
        cRZVariant variant;
        variant.RefSint64(data.data(), static_cast<std::uint32_t>(data.size()));
        holder->AddProperty(id, &variant, true);
    }
};

// The exemplar property ids of cPortTypeConfig::LoadConfiguration.
constexpr std::uint32_t kPropPortTypes = 0x8a270fc3;
constexpr std::uint32_t kPropStageCount = 0xca271780;
constexpr std::uint32_t kPropLotConfigs = 0xca271781;
constexpr std::uint32_t kPropBudgets = 0xca271783;
constexpr std::uint32_t kPropCapacities = 0xca271784;
constexpr std::uint32_t kPropThreshold = 0xca271785;
constexpr std::uint32_t kPropCosts = 0xca271786;
constexpr std::uint32_t kPropSizeClass = 0xca271787;

// The six airport advisor ids, in the original registration order.
constexpr std::uint32_t kAdvisorIds[6] = {
    kAirportMessageSize1First, kAirportMessageSize1Later, kAirportMessageSize2First,
    kAirportMessageSize2Later, kAirportMessageSize3First, kAirportMessageSize3Later,
};

struct Fixture {
    FakeMessageServer messages;
    FakeSimulator simulator;
    FakeAdvisorSystem advisorSystem;
    FakeCity city;
    FakeZoneManager zoneManager;
    FakeLotManager lotManager;
    FakeBudget budget;
    FakeDemand demand;
    FakeLotDeveloper lotDeveloper;
    FakeBuildingDevelopment buildingDevelopment;
    FakeLotConfigurationManager lotConfigurationManager;
    FakeResourceManager resourceManager;
    FakeAirportHost airportHost;
    FakeBaseOnlyHost baseOnlyHost;
    FakeBudgetHost budgetHost;
    Exemplars exemplars;
    cSC4BudgetSimulator budgetSimulator;
    cSC4TrafficSimulator trafficSimulator{nullptr, nullptr};

    std::vector<std::unique_ptr<FakeLotConfiguration>> configs;
    cSC4AirportDeveloper* developer = nullptr;

    Fixture() {
        city.simulator = &simulator;
        city.advisorSystem = &advisorSystem;
        buildingDevelopment.lotDeveloper = &lotDeveloper;
        airportHost.messages = &messages;
        airportHost.simulator = &simulator;
        airportHost.city = &city;
        airportHost.zoneManager = &zoneManager;
        airportHost.lotManager = &lotManager;
        airportHost.budget = &budget;
        airportHost.demand = &demand;
        airportHost.buildingDevelopment = &buildingDevelopment;
        airportHost.lotConfigurationManager = &lotConfigurationManager;
        airportHost.resourceManager = &resourceManager;
        airportHost.budgetSimulator = &budgetSimulator;
        airportHost.trafficSimulator = &trafficSimulator;
        baseOnlyHost.inner = &airportHost;
        SetPortDeveloperHost(&airportHost);
        SetAirportDeveloperHost(&airportHost);
        SetBudgetHost(&budgetHost);
    }
    ~Fixture() {
        if (developer) {
            if (developer->IsInitialized()) developer->Shutdown();
            for (cPortLot* port : developer->PortLots()) delete port;   // Read without Init: Shutdown never ran
            for (cPortTypeConfig* config : developer->Configurations()) delete config;
            delete developer;
            developer = nullptr;
        }
        ReleaseTestLots();
        budgetSimulator.RemoveDepartmentBudget(kAirportAdvisorEventId);
        SetAirportDeveloperHost(nullptr);
        SetPortDeveloperHost(nullptr);
        SetBudgetHost(nullptr);
    }

    // Release() destroys the developer below a count of 2; drop the fixture's pointer first.
    void ReleaseDeveloper() {
        cSC4AirportDeveloper* released = developer;
        developer = nullptr;
        CHECK(released->Release() == 0);
    }

    // Four port types: size class 1 with two stages, size class 2, size class 3 and an unknown size class.
    void BuildExemplars() {
        cSCPropertyHolder* list = exemplars.Make();
        Exemplars::Uints(list, kPropPortTypes, {0x111, 0x222, 0x333, 0x444});
        resourceManager.exemplars[kAirportDeveloperId] = list;

        cSCPropertyHolder* type1 = exemplars.Make();
        Exemplars::Uint(type1, kPropStageCount, 2);
        Exemplars::Uints(type1, kPropLotConfigs, {0x1000, 0x1001});
        Exemplars::Int64s(type1, kPropBudgets, {100, 150});
        Exemplars::Uints(type1, kPropCapacities, {100, 200});
        Exemplars::Int64s(type1, kPropCosts, {1000, 2000});
        Exemplars::Float(type1, kPropThreshold, 50.0f);   // 0.5
        Exemplars::Uint8(type1, kPropSizeClass, 1);
        resourceManager.exemplars[0x111] = type1;

        cSCPropertyHolder* type2 = exemplars.Make();
        Exemplars::Uint(type2, kPropStageCount, 1);
        Exemplars::Uints(type2, kPropLotConfigs, {0x2000});
        Exemplars::Int64s(type2, kPropBudgets, {300});
        Exemplars::Uints(type2, kPropCapacities, {400});
        Exemplars::Int64s(type2, kPropCosts, {5000});
        Exemplars::Float(type2, kPropThreshold, 25.0f);   // 0.25
        Exemplars::Uint8(type2, kPropSizeClass, 2);
        resourceManager.exemplars[0x222] = type2;

        cSCPropertyHolder* type3 = exemplars.Make();
        Exemplars::Uint(type3, kPropStageCount, 1);
        Exemplars::Uints(type3, kPropLotConfigs, {0x3000});
        Exemplars::Int64s(type3, kPropBudgets, {500});
        Exemplars::Uints(type3, kPropCapacities, {500});
        Exemplars::Int64s(type3, kPropCosts, {9000});
        Exemplars::Float(type3, kPropThreshold, 25.0f);
        Exemplars::Uint8(type3, kPropSizeClass, 3);
        resourceManager.exemplars[0x333] = type3;

        cSCPropertyHolder* type4 = exemplars.Make();
        Exemplars::Uint(type4, kPropStageCount, 1);
        Exemplars::Uints(type4, kPropLotConfigs, {0x4000});
        Exemplars::Int64s(type4, kPropBudgets, {50});
        Exemplars::Uints(type4, kPropCapacities, {50});
        Exemplars::Int64s(type4, kPropCosts, {500});
        Exemplars::Float(type4, kPropThreshold, 25.0f);
        Exemplars::Uint8(type4, kPropSizeClass, 9);       // no advisor message for this class
        resourceManager.exemplars[0x444] = type4;
    }

    FakeLotConfiguration* AddConfiguration(std::uint32_t id, int growthStage = 1) {
        auto existing = lotConfigurationManager.configurations.find(id);
        if (existing != lotConfigurationManager.configurations.end() &&
            existing->second->growthStage == growthStage) {
            return existing->second;
        }
        auto config = std::make_unique<FakeLotConfiguration>();
        config->id = id;
        config->growthStage = growthStage;
        FakeLotConfiguration* raw = config.get();
        configs.push_back(std::move(config));
        lotConfigurationManager.configurations[id] = raw;
        return raw;
    }

    // A 1x1 lot at cell (x, z); two references like CreateLot (the caller's and the lot list's).
    static cSC4Lot* MakeLot(int x, int z, int facing = 0) {
        auto* lot = new cSC4Lot(x, z, 1, 1, facing);
        g_testLots.push_back(lot);
        lot->AddRef();
        lot->AddRef();
        return lot;
    }

    // Places a lot whose configuration is the given stage id and runs DoLotInserted.
    cPortLot* InsertLot(int x, int z, std::uint32_t configurationId, int growthStage = 1) {
        cSC4Lot* lot = MakeLot(x, z);
        FakeLotConfiguration* config = AddConfiguration(configurationId, growthStage);
        lot->SetLotConfiguration(config);
        lotManager.lots[{x, z}] = lot;
        developer->DoLotInserted(lot);
        return developer->PortLotCount() == 0 ? nullptr : developer->PortLots().front();
    }

    cSC4AirportDeveloper* Make() {
        developer = new cSC4AirportDeveloper();
        return developer;
    }

    cSC4DepartmentBudget* Department() {
        return budgetSimulator.GetDepartmentBudget(kAirportAdvisorEventId);
    }
    cSC4LineItem* Line() { return Department() ? Department()->GetLineItem(1) : nullptr; }

    static void SetSubnetworks(cSC4Lot* lot, std::initializer_list<std::uint32_t> ids) {
        std::vector<std::uint32_t> data(ids);
        cRZVariant variant;
        variant.RefUint32(data.data(), static_cast<std::uint32_t>(data.size()));
        lot->Properties().AddProperty(kLotSubnetworksProperty, &variant, true);
    }
};

//----------------------------------------------------------------------------------------------- tests

void TestLayoutAndIdentity() {
    static_assert(sizeof(cSC4AirportDeveloper) == 0xc0,
                  "the department member after the 0xb8 original layout makes the port 0xc0");
    static_assert(alignof(cSC4AirportDeveloper) == 8, "the object stays pointer aligned");
    static_assert(sizeof(cSC4PortDeveloper) == 0xb0, "the base layout must not change");
    CHECK(cSC4AirportDeveloper::kOriginalClassSize == 0xb8);
    CHECK(cSC4AirportDeveloper::kOffsetAirportInterface == 0);
    CHECK(cSC4AirportDeveloper::kOffsetPortDeveloper == 8);

    Fixture f;
    cSC4AirportDeveloper* d = f.Make();
    // The interface branch at +0x00 (this), the concrete base at +0x08 and its two secondary views.
    CHECK(reinterpret_cast<const char*>(static_cast<cSC4PortDeveloper*>(d)) -
              reinterpret_cast<const char*>(d) == 8);
    CHECK(reinterpret_cast<const char*>(static_cast<cIGZMessageTarget2*>(d)) -
              reinterpret_cast<const char*>(d) == 0x10);
    CHECK(reinterpret_cast<const char*>(static_cast<cIGZSerializable*>(d)) -
              reinterpret_cast<const char*>(d) == 0x18);
    // The identity the inlined constructor writes (0x1000c8034).
    CHECK(d->PortDeveloperId() == kAirportDeveloperId);
    CHECK(d->PortDeveloperId() == 0xca270e9d);
    CHECK(d->ZoneType() == kAirportZoneType);
    CHECK(d->ZoneType() == 11);
    CHECK(d->HistoryCategory() == kAirportHistoryCategory);
    CHECK(d->HistoryCategory() == 0x6a3e9032);
    CHECK(d->AdvisorEventId() == kAirportAdvisorEventId);
    CHECK(d->AdvisorEventId() == 0x6a4119f0);
    CHECK(d->BuildingGroup() == kAirportBuildingGroup);
    CHECK(d->BuildingGroup() == 0x1508);
    CHECK(d->GetSimulatorType() == 0xea3955c4);
    CHECK(d->GetGZCLSID() == 0xaa2049a1);
    CHECK(d->DepartmentBudget() == nullptr);
    CHECK(d->RefCount() == 0);
    delete d;
    f.developer = nullptr;
}

void TestCom() {
    Fixture f;
    cSC4AirportDeveloper* d = f.Make();
    CHECK(d->AddRef() == 1);
    void* p = nullptr;
    CHECK(d->QueryInterface(kGZIID_cISC4AirportDeveloper, &p));
    CHECK(p == static_cast<cISC4AirportDeveloper*>(d));
    CHECK(d->RefCount() == 2);
    CHECK(d->QueryInterface(1, &p));   // iid 1 falls through to the base
    CHECK(p == static_cast<cISC4PortDeveloper*>(static_cast<cSC4PortDeveloper*>(d)));
    CHECK(d->RefCount() == 3);
    CHECK(d->QueryInterface(kPortGZIIDPortDeveloper, &p));
    CHECK(p == static_cast<cISC4PortDeveloper*>(static_cast<cSC4PortDeveloper*>(d)));
    CHECK(d->RefCount() == 4);
    CHECK(d->QueryInterface(kPortGZIIDMessageTarget2, &p));
    CHECK(p == static_cast<cIGZMessageTarget2*>(d));
    CHECK(d->RefCount() == 5);
    CHECK(d->QueryInterface(kPortGZIIDSerializable, &p));
    CHECK(p == static_cast<cIGZSerializable*>(d));
    CHECK(d->RefCount() == 6);
    CHECK(!d->QueryInterface(0x12345678, &p));
    CHECK(d->RefCount() == 6);
    CHECK(!d->QueryInterface(kGZIID_cISC4AirportDeveloper, nullptr));
    // AsISC4PortDeveloper returns the concrete base's port view (this + 8).
    CHECK(d->AsISC4PortDeveloper() ==
          static_cast<cISC4PortDeveloper*>(static_cast<cSC4PortDeveloper*>(d)));
    // The base's views answer the other airport queries with the base implementation.
    float average = d->GetAvgEfficiency();
    CHECK(Near(average, 1.0f));   // no port lots yet
    SC4Percentage efficiency{0.5f};
    PortPoint location;
    CHECK(!d->GetLeastEfficientPort(efficiency, location));
    std::vector<cSC4Lot*> lots;
    d->GetAllPortLots(lots);
    CHECK(lots.empty());
    CHECK(d->GetNumPorts(0x1f, 0x1f) == 0);
    CHECK(d->GetNumAirports(0) == 0);
    CHECK(d->GetNumAirportTrips() == 0);
    CHECK(d->GetAllocatedFunding() == 0);
    // A null message is consumed like the base does, and the budget calls before Init are no-ops.
    CHECK(d->DoMessage(nullptr));
    d->UpdateBudget();
    CHECK(d->GetAllocatedFunding() == 0);
    // The base's Release destroys at a count below 2 (the count is not decremented).
    CHECK(d->Release() == 5);
    CHECK(d->Release() == 4);
    CHECK(d->Release() == 3);
    CHECK(d->Release() == 2);
    CHECK(d->Release() == 1);
    CHECK(d->Release() == 0);
    f.developer = nullptr;
}

void TestInitDepartmentAndShutdown() {
    Fixture f;
    f.BuildExemplars();
    cSC4AirportDeveloper* d = f.Make();
    CHECK(!d->IsInitialized());
    CHECK(d->Init());
    CHECK(d->IsInitialized());
    CHECK(d->TunablesWereRead());
    CHECK(d->PortTypeCount() == 4);
    CHECK(d->Configurations().size() == 4);
    // The base registers three notifications and the airport six more.
    CHECK(f.messages.added.size() == 9);
    std::set<std::uint32_t> airportNotifications;
    cIGZMessageTarget2* target = static_cast<cIGZMessageTarget2*>(d);
    for (const auto& entry : f.messages.added) {
        CHECK(entry.first == target);
        airportNotifications.insert(entry.second);
    }
    for (std::uint32_t id : kAdvisorIds) CHECK(airportNotifications.count(id) == 1);
    CHECK(airportNotifications.size() == 9);

    // The department budget created through the recovered simulator.
    cSC4DepartmentBudget* department = f.Department();
    CHECK(department != nullptr);
    CHECK(department == d->DepartmentBudget());
    CHECK(department->GetDepartmentID() == kAirportAdvisorEventId);
    CHECK(department->GetBudgetGroup() == kAirportDepartmentGroup);
    CHECK(Near(department->GetMaxAllowedFundingPercentage(), 1.2f));
    CHECK(Near(department->GetFundingPercentage(), 1.0f));
    // SetDepartmentName asked the budget host for (0x6a3ff01c, 0xea493564).
    CHECK(!f.budgetHost.localizedNames.empty());
    CHECK(f.budgetHost.localizedNames.back().first == kAirportDepartmentNameGroup);
    CHECK(f.budgetHost.localizedNames.back().second == kAirportDepartmentNameInstance);
    cSC4LineItem* line = f.Line();
    CHECK(line != nullptr);
    CHECK(line->GetID() == 1);
    CHECK(line->GetDisplayFlags() == 4);   // default 5, display flag 1 cleared
    CHECK(line->GetFullExpenses() == 0);   // UpdateBudget has not run yet
    CHECK(d->GetAllocatedFunding() == 0);

    // A second Init is a no-op that reports success (the base's initialised flag is already set).
    CHECK(d->Init());
    CHECK(f.messages.added.size() == 9);

    CHECK(d->Shutdown());
    CHECK(!d->IsInitialized());
    CHECK(d->DepartmentBudget() == nullptr);
    // The six airport removals, then the base's three.
    CHECK(f.messages.removed.size() == 9);
    CHECK(f.messages.removed[0].second == kAirportMessageSize1First);
    CHECK(f.messages.removed[1].second == kAirportMessageSize1Later);
    CHECK(f.messages.removed[2].second == kAirportMessageSize2First);
    CHECK(f.messages.removed[3].second == kAirportMessageSize2Later);
    CHECK(f.messages.removed[4].second == kAirportMessageSize3First);
    CHECK(f.messages.removed[5].second == kAirportMessageSize3Later);
    // A second Shutdown is a no-op.
    CHECK(d->Shutdown());
    CHECK(f.messages.removed.size() == 9);
    f.ReleaseDeveloper();

    // The same developer id in a fresh fixture reuses the department; a second developer AddRefs it.
    Fixture g;
    g.BuildExemplars();
    cSC4AirportDeveloper* first = g.Make();
    CHECK(first->Init());
    cSC4AirportDeveloper* second = new cSC4AirportDeveloper();
    CHECK(second->Init());
    CHECK(second->DepartmentBudget() == first->DepartmentBudget());
    CHECK(second->Shutdown());
    delete second;
    CHECK(first->Shutdown());
    g.ReleaseDeveloper();
}

void TestMissingAirportHost() {
    // No airport host installed (only the base's seam): the airport cannot reach the budget simulator and
    // reports failure after the base has already initialised (the original would dereference the global).
    Fixture f;
    f.BuildExemplars();
    SetAirportDeveloperHost(nullptr);
    SetPortDeveloperHost(&f.baseOnlyHost);
    cSC4AirportDeveloper* d = f.Make();
    CHECK(!d->Init());
    CHECK(d->IsInitialized());   // the base sets its flag before it touches the services
    CHECK(d->DepartmentBudget() == nullptr);
    CHECK(d->Shutdown());
    CHECK(f.messages.removed.size() == 9);   // the airport removals are attempted, then the base's
    delete d;
    f.developer = nullptr;
    SetPortDeveloperHost(&f.airportHost);
    SetAirportDeveloperHost(&f.airportHost);
}

void TestQueriesFundingAndAdvisor() {
    Fixture f;
    f.BuildExemplars();
    cSC4AirportDeveloper* d = f.Make();
    CHECK(d->Init());

    cPortLot* size1 = f.InsertLot(10, 10, 0x1000);
    cPortLot* size2 = f.InsertLot(12, 12, 0x2000);
    cPortLot* size3 = f.InsertLot(14, 14, 0x3000);
    CHECK(size1 && size2 && size3);
    CHECK(d->PortLotCount() == 3);
    CHECK(d->GetNumAirports(0) == 3);   // every port
    CHECK(d->GetNumAirports(1) == 1);   // size class 1
    CHECK(d->GetNumAirports(2) == 1);
    CHECK(d->GetNumAirports(3) == 1);
    CHECK(d->GetNumAirports(4) == 0);
    CHECK(d->GetNumPorts(0x1f, 0x1f) == 3);
    std::vector<cSC4Lot*> lots;
    d->GetAllPortLots(lots);
    CHECK(lots.size() == 3);
    for (cSC4Lot* lot : lots) lot->Release();

    // UpdateBudget ran from DoLotInserted: stage-0 budgets 100 + 300 + 500.
    cSC4LineItem* line = f.Line();
    CHECK(line != nullptr);
    CHECK(line->GetFullExpenses() == 900);
    CHECK(d->GetAllocatedFunding() == line->GetCurrentExpenses());
    // A 50% funding ratio.
    line->SetCurrentExpenses(450);
    CHECK(d->GetAllocatedFunding() == 450);

    size1->SetTrips(37);
    size2->SetTrips(5);
    size3->SetTrips(8);
    CHECK(d->GetTripsForPort(*size1) == 37);
    CHECK(d->GetNumAirportTrips() == 50);

    cSC4Lot* lot1 = size1->CachedLot();
    std::uint32_t usage = 0, capacity = 0;
    CHECK(d->GetAirportUsageValues(lot1, usage, capacity));
    CHECK(capacity == 50);    // 100 * 450/900
    CHECK(usage == 37);
    size1->SetTrips(80);
    CHECK(d->GetAirportUsageValues(lot1, usage, capacity));
    CHECK(capacity == 50);
    CHECK(usage == 50);       // clamped to the scaled capacity
    CHECK(d->GetMaintenanceCost(lot1) == 50);   // 100 * 0.5

    // Stage 1 of the size-class-1 type: budget 150, capacity 200.
    size1->SetStage(1);
    CHECK(d->GetMaintenanceCost(lot1) == 75);   // 150 * 0.5
    CHECK(d->GetAirportUsageValues(lot1, usage, capacity));
    CHECK(capacity == 100);   // 200 * 0.5
    CHECK(usage == 80);
    CHECK(d->GetMaintenanceCost(nullptr) == 0);
    std::uint32_t unusedUsage = 0, unusedCapacity = 0;
    CHECK(!d->GetAirportUsageValues(nullptr, unusedUsage, unusedCapacity));

    // The airport's no-op stage callbacks.
    d->OnStageChange(*size1);
    d->OnNewLotCreated(*size1);
    CHECK(size1->Stage() == 1);

    // One advisor event per size class: the first id at stage 0, the second afterwards. The unknown size
    // class 9 sends event 0.
    cPortLot* unknown = f.InsertLot(16, 16, 0x4000);
    CHECK(unknown != nullptr);
    d->SendAdvisorUpgradeMessage(*size1);            // stage 1 -> 0xcb3be815
    size1->SetStage(0);
    d->SendAdvisorUpgradeMessage(*size1);            // stage 0 -> 0xea394b74
    d->SendAdvisorUpgradeMessage(*size2);            // stage 0, size 2 -> 0xeb3be415
    size2->SetStage(1);
    d->SendAdvisorUpgradeMessage(*size2);            // stage 1 -> 0x2b3be44a
    size2->SetStage(0);
    d->SendAdvisorUpgradeMessage(*size3);            // stage 0, size 3 -> 0x6b3be6d8
    size3->SetStage(1);
    d->SendAdvisorUpgradeMessage(*size3);            // stage 1 -> 0xb3be81a
    d->SendAdvisorUpgradeMessage(*unknown);          // size 9 -> event 0
    CHECK(f.advisorSystem.events.size() == 7);
    CHECK(f.advisorSystem.events[0].first == static_cast<int>(kAirportMessageSize1Later));
    CHECK(f.advisorSystem.events[1].first == static_cast<int>(kAirportMessageSize1First));
    CHECK(f.advisorSystem.events[2].first == static_cast<int>(kAirportMessageSize2First));
    CHECK(f.advisorSystem.events[3].first == static_cast<int>(kAirportMessageSize2Later));
    CHECK(f.advisorSystem.events[4].first == static_cast<int>(kAirportMessageSize3First));
    CHECK(f.advisorSystem.events[5].first == static_cast<int>(kAirportMessageSize3Later));
    CHECK(f.advisorSystem.events[6].first == 0);
    for (const auto& event : f.advisorSystem.events) CHECK(event.second == nullptr);

    CHECK(d->Shutdown());
    f.ReleaseDeveloper();
}

void TestStreams() {
    Fixture f;
    f.BuildExemplars();
    cSC4AirportDeveloper* d = f.Make();
    CHECK(d->Init());
    cPortLot* first = f.InsertLot(10, 20, 0x1000);
    cPortLot* second = f.InsertLot(30, 40, 0x2000);
    CHECK(first && second);
    second->SetStage(0);
    CHECK(d->PortLotCount() == 2);

    PortMemoryOStream out;
    CHECK(d->Write(out));
    // Airport version 2, then the base's version 1, developer id, zone type, count and two records.
    const std::vector<unsigned char>& bytes = out.bytes();
    CHECK(bytes.size() == 2 + 2 + 4 + 2 + 2 + 2 * (4 + 2 + 2 + 2));
    CHECK(bytes[0] == 2 && bytes[1] == 0);
    CHECK(bytes[2] == 1 && bytes[3] == 0);

    PortMemoryIStream in(bytes.data(), bytes.size());
    auto* reader = new cSC4AirportDeveloper();
    CHECK(reader->Read(in));
    CHECK(reader->PortDeveloperId() == kAirportDeveloperId);
    CHECK(reader->ZoneType() == kAirportZoneType);
    CHECK(reader->PortLotCount() == 2);
    CHECK(reader->Configurations().size() == 4);
    CHECK(reader->PortLots().front()->X() == 10);   // Read push_fronts, so the file's order is reversed
    CHECK(reader->PortLots().front()->LotIsStale());
    FreeReadState(reader);
    delete reader;

    // A stream whose airport version is not 2 is rejected before the base header is read.
    PortMemoryOStream airportBadOut;
    airportBadOut.SetUint16(1);
    PortMemoryIStream airportBadIn(airportBadOut.bytes().data(), airportBadOut.bytes().size());
    auto* airportBadReader = new cSC4AirportDeveloper();
    CHECK(!airportBadReader->Read(airportBadIn));
    CHECK(airportBadReader->PortLotCount() == 0);
    delete airportBadReader;

    // The airport version is right but the base version is not 1: the base rejects the payload.
    PortMemoryOStream baseBadOut;
    baseBadOut.SetUint16(2);
    baseBadOut.SetUint16(2);
    PortMemoryIStream baseBadIn(baseBadOut.bytes().data(), baseBadOut.bytes().size());
    auto* baseBadReader = new cSC4AirportDeveloper();
    CHECK(!baseBadReader->Read(baseBadIn));
    CHECK(baseBadReader->PortLotCount() == 0);
    delete baseBadReader;

    CHECK(d->Shutdown());
    f.ReleaseDeveloper();
}

void TestDoMessageGrowth() {
    Fixture f;
    f.BuildExemplars();
    cSC4AirportDeveloper* d = f.Make();
    CHECK(d->Init());
    cPortLot* port = f.InsertLot(10, 20, 0x1000);
    CHECK(port != nullptr);
    f.AddConfiguration(0x1001);
    port->SetTrips(96);   // 96% of stage 1's capacity (100)

    // One of the six advisor ids: the airport grows the port instead of forwarding to the base.
    cRZMessage2Standard advisor;
    advisor.SetType(kAirportMessageSize1First);
    const std::size_t sentBefore = f.messages.sent.size();
    CHECK(d->DoMessage(&advisor));
    CHECK(f.lotManager.createCalls.size() == 1);
    CHECK(port->Stage() == 1);
    CHECK(f.lotDeveloper.constructs == 1);
    CHECK(f.budget.withdrawals.size() == 1);
    CHECK(f.budget.withdrawals[0] == 2000);   // the next stage's construction cost
    CHECK(f.messages.sent.size() == sentBefore);   // no history message: the base did not run
    // UpdateBudget ran in GrowPortsToNextStage: stage 1's budget is 150.
    CHECK(f.Line()->GetFullExpenses() == 150);

    // Another advisor id also runs the growth path (nothing triggers at the last stage).
    cRZMessage2Standard advisor2;
    advisor2.SetType(kAirportMessageSize3Later);
    CHECK(d->DoMessage(&advisor2));
    CHECK(port->Stage() == 1);
    CHECK(f.lotManager.createCalls.size() == 1);

    // Everything else is the base's: the monthly tick runs the base's doMonthlySimulation and records history.
    cRZMessage2Standard monthly;
    monthly.SetType(0x66956816);
    CHECK(d->DoMessage(&monthly));
    CHECK(f.messages.sent.size() == sentBefore + 1);
    CHECK(f.messages.sent.back().type == 0x89efa536);
    CHECK(f.messages.sent.back().data1 == kAirportHistoryCategory);

    // A message with no known type is still consumed successfully.
    cRZMessage2Standard other;
    other.SetType(0x12345678);
    CHECK(d->DoMessage(&other));

    CHECK(d->Shutdown());
    f.ReleaseDeveloper();
}

void TestDoMonthlyTick() {
    Fixture f;
    f.BuildExemplars();
    cSC4AirportDeveloper* d = f.Make();
    CHECK(d->Init());

    // Three subnetworks: 1 and 2 carry office occupants, 0 is unused.
    f.trafficSimulator.Subnetworks().assign(3, SubnetworkInfo{});
    f.trafficSimulator.Subnetworks()[1].occupantsByType[kAirportOfficeMediumType] = 30;
    f.trafficSimulator.Subnetworks()[1].occupantsByType[kAirportOfficeHighType] = 10;
    f.trafficSimulator.Subnetworks()[2].occupantsByType[kAirportOfficeMediumType] = 60;

    cPortLot* port1 = f.InsertLot(10, 10, 0x1000);   // capacity 100
    cPortLot* port2 = f.InsertLot(12, 12, 0x2000);   // capacity 400
    cPortLot* port3 = f.InsertLot(14, 14, 0x3000);   // capacity 500, no subnetwork property
    CHECK(port1 && port2 && port3);
    Fixture::SetSubnetworks(port1->CachedLot(), {1});
    Fixture::SetSubnetworks(port2->CachedLot(), {1, 2});
    // 50% funding: full 900 (100 + 300 + 500), current 450.
    CHECK(f.Line()->GetFullExpenses() == 900);
    f.Line()->SetCurrentExpenses(450);

    d->DoMonthlyTick();
    // Subnetwork 1: (100*0.5)/((100+400)*0.5) * 40 = 8 for port1, 32 for port2.
    // Subnetwork 2: (400*0.5)/(400*0.5) * 60 = 60 for port2.
    CHECK(port1->Trips() == 8);
    CHECK(port2->Trips() == 32 + 60);
    CHECK(port3->Trips() == 0);
    CHECK(d->GetNumAirportTrips() == 8 + 92);

    // The trips are reset on every pass: zero funding leaves them at zero (the original divides by zero).
    f.Line()->SetCurrentExpenses(0);
    d->DoMonthlyTick();
    CHECK(port1->Trips() == 0);
    CHECK(port2->Trips() == 0);
    CHECK(port3->Trips() == 0);

    // A subnetwork id past the reported count is ignored (the original writes out of bounds).
    f.Line()->SetCurrentExpenses(450);
    Fixture::SetSubnetworks(port3->CachedLot(), {9});
    d->DoMonthlyTick();
    CHECK(port3->Trips() == 0);
    CHECK(port1->Trips() == 8);
    CHECK(port2->Trips() == 92);

    // A port whose cached lot is gone contributes nothing. Read creates records with a stale cached lot that
    // the lot manager cannot resolve (cell 99, 99 does not exist).
    PortMemoryOStream staleOut;
    staleOut.SetUint16(2);
    staleOut.SetUint16(1);
    staleOut.SetUint32(kAirportDeveloperId);
    staleOut.SetUint16(kAirportZoneType);
    staleOut.SetUint16(1);
    staleOut.SetUint32(0);        // stage
    staleOut.SetUint16(0);        // configuration index
    staleOut.SetSint16(99);
    staleOut.SetSint16(99);
    PortMemoryIStream staleIn(staleOut.bytes().data(), staleOut.bytes().size());
    auto* stale = new cSC4AirportDeveloper();
    CHECK(stale->Read(staleIn));
    CHECK(stale->PortLotCount() == 1);
    CHECK(stale->PortLots().front()->LotIsStale());
    stale->DoMonthlyTick();
    CHECK(stale->GetNumAirportTrips() == 0);
    FreeReadState(stale);
    delete stale;

    // An empty port list returns immediately.
    Fixture g;
    g.BuildExemplars();
    cSC4AirportDeveloper* empty = g.Make();
    CHECK(empty->Init());
    empty->DoMonthlyTick();
    CHECK(empty->GetNumAirportTrips() == 0);
    CHECK(g.trafficSimulator.Subnetworks().empty());

    CHECK(d->Shutdown());
    f.ReleaseDeveloper();
}

}  // namespace

int main() {
    TestLayoutAndIdentity();
    TestCom();
    TestInitDepartmentAndShutdown();
    TestMissingAirportHost();
    TestQueriesFundingAndAdvisor();
    TestStreams();
    TestDoMessageGrowth();
    TestDoMonthlyTick();
    if (g_failures == 0) std::puts("sc4_airportdeveloper ok");
    return g_failures == 0 ? 0 : 1;
}
