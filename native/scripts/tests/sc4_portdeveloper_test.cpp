// cSC4PortDeveloper — unit test. Drives the recovered base object through a fake IPortDeveloperHost and a
// concrete test developer (the class is abstract, like the original: the airport/seaport developers supply
// the eight virtuals). It covers the three COM views and the shared reference count, Init/Shutdown (the
// simulator agent, the four services and the three notifications), the port type exemplars and their growth
// stages, DoLotInserted / doLotStateChanged / the placed-building and lot-state messages, the whole monthly
// simulation (efficiency, usage, demand, condition and the single advisor upgrade), the stage growth through
// swapLot (demolish, CreateLot, ConstructDefaultLot, the budget withdrawal), the stream round trip and the
// remaining queries, plus the documented quirks (Release deletes below 2, GetLeastEfficientPort takes the
// first lot, Read indexes the configuration vector unchecked, the dead first doLotStateChanged argument).
//
// Real game data: the lots are real cSC4Lot objects, the exemplars are real cSCPropertyHolder +
// cSCProperty + cRZVariant, the streams are the port memory streams and the messages are real
// cRZMessage2Standard objects.

#include "sc4/PortDeveloper.h"

#include <algorithm>
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

// The two port identities of the derived developers (cSC4AirportDeveloper / cSC4SeaportDeveloper ctors).
constexpr std::uint32_t kAirportDeveloperId = 0xca270e9d;
constexpr std::uint16_t kAirportZone = 11;
constexpr std::uint32_t kAirportHistory = 0x6a3e9032;
constexpr std::uint32_t kAirportAdvisor = 0x6a4119f0;
constexpr std::uint32_t kAirportGroup = 0x1508;
constexpr std::uint32_t kSeaportDeveloperId = 0xea29ad92;
constexpr std::uint16_t kSeaportZone = 12;
constexpr std::uint32_t kSeaportHistory = 0xea3e904a;
constexpr std::uint32_t kSeaportAdvisor = 0x4a4155ab;
constexpr std::uint32_t kSeaportGroup = 0x1509;

// The exemplar property ids of cPortTypeConfig::LoadConfiguration.
constexpr std::uint32_t kPropPortTypes = 0x8a270fc3;
constexpr std::uint32_t kPropStageCount = 0xca271780;
constexpr std::uint32_t kPropLotConfigs = 0xca271781;
constexpr std::uint32_t kPropBudgets = 0xca271783;
constexpr std::uint32_t kPropCapacities = 0xca271784;
constexpr std::uint32_t kPropThreshold = 0xca271785;
constexpr std::uint32_t kPropCosts = 0xca271786;
constexpr std::uint32_t kPropSizeClass = 0xca271787;
constexpr std::uint32_t kPropRows = 0xca271788;
constexpr std::uint32_t kPropColumns = 0xca271789;
constexpr std::uint32_t kPropGrid = 0xca271800;

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
    int getSimulatorCalls = 0;
    int getAdvisorCalls = 0;
    IPortSimulatorApi* GetSimulator() override {
        ++getSimulatorCalls;
        return simulator;
    }
    IPortAdvisorSystemApi* GetAdvisorSystem() override {
        ++getAdvisorCalls;
        return advisorSystem;
    }
};

struct FakeZoneManager final : IPortZoneManagerApi {
    int refs = 0;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    int clearCalls = 0;
    PortCellRegion lastRegion;
    void ClearZone(const PortCellRegion& region) override {
        ++clearCalls;
        lastRegion = region;
    }
};

// Every lot the fakes create starts with two references (the caller's and the lot list's); the fixture
// drops whatever is left of them when it goes away.
std::vector<cSC4Lot*> g_testLots;
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
    std::map<std::pair<int, int>, cSC4Lot*> lots;                    // GetLot by cell
    std::map<cISC4Occupant*, cSC4Lot*> occupantLots;                 // GetOccupantLot
    std::vector<cSC4Lot*> created;                                   // CreateLot results (not owned)
    int getCalls = 0;
    int occupantLotCalls = 0;
    struct CreateCall {
        int x = 0, z = 0, sizeX = 0, sizeZ = 0, facing = 0;
    };
    std::vector<CreateCall> createCalls;
    bool createResult = true;
    cSC4Lot* GetLot(int x, int z, bool) override {
        ++getCalls;
        auto it = lots.find({x, z});
        return it == lots.end() ? nullptr : it->second;
    }
    cSC4Lot* GetOccupantLot(cISC4Occupant* occupant) override {
        ++occupantLotCalls;
        auto it = occupantLots.find(occupant);
        return it == occupantLots.end() ? nullptr : it->second;
    }
    // Like the zone developer's CreateLot: the new lot carries two references (the caller's and the lot
    // list's), so the port's cPortLot releases one later and the lot is not destroyed by that release.
    bool CreateLot(int x, int z, int sizeX, int sizeZ, int facing, cSC4Lot** out) override {
        createCalls.push_back({x, z, sizeX, sizeZ, facing});
        if (!createResult) {
            *out = nullptr;
            return false;
        }
        auto* lot = new cSC4Lot(x, z, sizeX, sizeZ, facing);
        g_testLots.push_back(lot);
        lot->AddRef();
        lot->AddRef();
        created.push_back(lot);
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

// The lot configuration the port looks up by id (the three calls it makes).
struct FakeLotConfiguration final : IPortLotConfiguration {
    int refs = 1;
    std::uint32_t id = 0;
    int growthStage = 1;
    std::vector<int> compatibleZones;
    bool allZones = false;
    mutable int getIdCalls = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    std::uint32_t GetID() const override {
        ++getIdCalls;
        return id;
    }
    int GetGrowthStage() const override { return growthStage; }
    bool IsCompatibleWithZoneType(int zoneType) const override {
        return allZones || std::find(compatibleZones.begin(), compatibleZones.end(), zoneType) !=
                               compatibleZones.end();
    }
};

struct FakeLotConfigurationManager final : IPortLotConfigurationManagerApi {
    int refs = 0;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    std::map<std::uint32_t, FakeLotConfiguration*> configurations;
    std::vector<std::uint32_t> requested;
    IPortLotConfiguration* GetLotConfiguration(std::uint32_t id) override {
        requested.push_back(id);
        auto it = configurations.find(id);
        return it == configurations.end() ? nullptr : it->second;
    }
};

struct FakeLotDeveloper final : IPortLotDeveloperApi {
    int refs = 0;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    struct Construct {
        cSC4Lot* lot = nullptr;
        IPortLotConfiguration* configuration = nullptr;
        std::uint32_t objectId = 0;
        bool flag = false;
    };
    std::vector<Construct> constructs;
    std::vector<cSC4Lot*> startDemolish;
    std::vector<cSC4Lot*> endDemolish;
    bool constructResult = true;
    bool ConstructDefaultLot(cSC4Lot* lot, IPortLotConfiguration* configuration,
                             std::uint32_t objectId, bool flag) override {
        constructs.push_back({lot, configuration, objectId, flag});
        return constructResult;
    }
    void StartDemolishLot(cSC4Lot* lot) override { startDemolish.push_back(lot); }
    void EndDemolishLot(cSC4Lot* lot) override { endDemolish.push_back(lot); }
};

struct FakeBuildingDevelopment final : IPortBuildingDevelopmentApi {
    int refs = 0;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    FakeLotDeveloper* lotDeveloper = nullptr;
    int getCalls = 0;
    IPortLotDeveloperApi* GetLotDeveloper() override {
        ++getCalls;
        return lotDeveloper;
    }
};

struct FakeResourceManager final : IPortResourceApi {
    int refs = 0;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    std::map<std::uint32_t, cISCPropertyHolder*> exemplars;
    std::vector<std::uint32_t> requested;
    cISCPropertyHolder* LoadExemplar(std::uint32_t instance) override {
        requested.push_back(instance);
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

struct FakeHost final : IPortDeveloperHost {
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
    cSC4Lot* stateMessageLot = nullptr;

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
    cSC4Lot* MessageLot(cIGZUnknown* payload) override {
        return payload ? stateMessageLot : nullptr;
    }
};

// cISC4Occupant for the placed-building message (type + group + property holder).
struct FakeOccupant final : cISC4Occupant {
    int refs = 0;
    std::uint32_t type = 0;
    std::uint32_t group = 0;
    cISCPropertyHolder* holder = nullptr;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    cISCPropertyHolder* AsPropertyHolder() override { return holder; }
    std::uint32_t GetType() const override { return type; }
    void GetPosition(float*) override {}
    void SetPosition(const float*) override {}
    void GetBoundingBox(float*, float*) override {}
    void GetBoundingCityCells(SC4CellRect&) override {}
    void SetRemovalFlags(int) override {}
    void UnsetRemovalFlags(int) override {}
    bool CanRemove(int) override { return true; }
    void PostOccupantMessage(std::uint32_t, std::uint32_t) override {}
    std::uint32_t GetHighlight() const override { return 0; }
    void SetHighlight(int, bool) override {}
    void SetVisibility(bool, bool) override {}
    cISC43DPlaceableObject* GetPlaceableObject() const override { return nullptr; }
    cISC43DPlaceableObject* GetOrCreatePlaceableObject() override { return nullptr; }
    void SetPlaceableObject(cISC43DPlaceableObject*) override {}
    bool IsOccupantGroup(std::uint32_t value) const override { return value == group; }
    void AddOccupantGroup(std::uint32_t) override {}
    bool GetOccupantGroups(std::set<std::uint32_t>&) const override { return false; }
    bool GetOccupantManagerBBox(std::uint8_t*) const override { return false; }
    void SetOccupantManagerBBox(const std::uint8_t*) override {}
    bool GetLotTag(std::uint32_t&) const override { return false; }
    void SetLotTag(std::uint32_t) override {}
    void SetFlag(std::uint32_t) override {}
    void SetAllFlags(std::uint32_t) override {}
    void ClearFlag(std::uint32_t) override {}
    bool IsFlagSet(std::uint32_t) override { return false; }
    std::uint32_t GetFlags() override { return 0; }
};

struct FakePropertyHolder final : cISCPropertyHolder {
    std::set<std::uint32_t> properties;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 0; }
    bool HasProperty(std::uint32_t id) const override { return properties.count(id) != 0; }
    bool GetPropertyList(cIGZUnknownList**) const override { return false; }
    cISCProperty* GetProperty(std::uint32_t) const override { return nullptr; }
    bool GetProperty(std::uint32_t, std::uint32_t&) const override { return false; }
    bool GetProperty(std::uint32_t, cIGZString&) const override { return false; }
    bool GetProperty(std::uint32_t, std::uint32_t, void**) const override { return false; }
    bool GetProperty(std::uint32_t, void*, std::uint32_t&) const override { return false; }
    void AddProperty(cISCProperty*, bool) override {}
    void AddProperty(std::uint32_t, const cIGZVariant*, bool) override {}
    void AddProperty(std::uint32_t, std::uint32_t, bool) override {}
    void AddProperty(std::uint32_t, const cIGZString&, bool) override {}
    void AddProperty(std::uint32_t, cIGZUnknown*, bool) override {}
    void AddProperty(std::uint32_t, const void*, std::uint32_t, bool) override {}
    void CopyAddProperty(cISCProperty*, bool) override {}
    bool RemoveProperty(std::uint32_t, bool) override { return false; }
    void RemoveAllProperties(bool) override {}
    void EnumProperties(void (*)(cISCProperty*, void*), void*) const override {}
    void EnumProperties(bool (*)(cISCProperty*, void*), void (*)(cISCProperty*, void*), void*) const override {}
    void CompactProperties() override {}
};

int g_destroyed = 0;   // how many TestPortDeveloper objects Release() destroyed

// A concrete developer: the eight virtuals the airport/seaport developers implement, with the arguments and
// the results recorded.
struct TestPortDeveloper final : cSC4PortDeveloper {
    std::uint32_t simulatorType = 0xea3957ad;
    std::int64_t funding = 0;
    std::map<cPortLot*, std::uint32_t> trips;
    int budgetUpdates = 0;
    int stageChanges = 0;
    int newLots = 0;
    int upgrades = 0;
    int monthlyTicks = 0;
    std::vector<cPortLot*> stageChangeArgs;
    std::vector<cPortLot*> newLotArgs;
    std::vector<cPortLot*> upgradeArgs;

    ~TestPortDeveloper() override {
        // Shutdown (the owner of these) is a no-op on a developer that was never Init-ed but did Read.
        for (cPortLot* port : PortLots()) delete port;
        for (cPortTypeConfig* config : Configurations()) delete config;
        ++g_destroyed;
    }

    void Configure(std::uint32_t developerId, std::uint16_t zoneType, std::uint32_t historyCategory,
                   std::uint32_t advisorEvent, std::uint32_t group) {
        SetIdentity(developerId, zoneType, historyCategory, advisorEvent, group);
    }

    std::uint32_t GetSimulatorType() const override { return simulatorType; }
    std::uint32_t GetTripsForPort(cPortLot& port) override {
        auto it = trips.find(&port);
        return it == trips.end() ? 0u : it->second;
    }
    void OnStageChange(cPortLot& port) override {
        ++stageChanges;
        stageChangeArgs.push_back(&port);
    }
    void OnNewLotCreated(cPortLot& port) override {
        ++newLots;
        newLotArgs.push_back(&port);
    }
    void SendAdvisorUpgradeMessage(cPortLot& port) override {
        ++upgrades;
        upgradeArgs.push_back(&port);
    }
    void DoMonthlyTick() override { ++monthlyTicks; }
    void UpdateBudget() override { ++budgetUpdates; }
    std::int64_t GetAllocatedFunding() override { return funding; }
};

//----------------------------------------------------------------------------------------------- fixture

// Owns the real property holders used as the port type exemplars.
struct Exemplars {
    std::vector<std::unique_ptr<cSCPropertyHolder>> holders;
    cSCPropertyHolder* Make() {
        holders.push_back(std::make_unique<cSCPropertyHolder>());
        cSCPropertyHolder* holder = holders.back().get();
        holder->AddRef();   // the fixture's own reference; LoadExemplar adds one the port releases
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
    static void Floats(cSCPropertyHolder* holder, std::uint32_t id,
                       std::initializer_list<float> values) {
        std::vector<float> data(values);
        cRZVariant variant;
        variant.RefFloat32(data.data(), static_cast<std::uint32_t>(data.size()));
        holder->AddProperty(id, &variant, true);
    }
};

// Two port types (0x111 / 0x222) with two stages each, plus the developer exemplar that lists them.
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
    FakeHost host;
    Exemplars exemplars;
    std::vector<std::unique_ptr<FakeLotConfiguration>> configs;

    TestPortDeveloper* developer = nullptr;

    Fixture() {
        city.simulator = &simulator;
        city.advisorSystem = &advisorSystem;
        buildingDevelopment.lotDeveloper = &lotDeveloper;
        host.messages = &messages;
        host.simulator = &simulator;
        host.city = &city;
        host.zoneManager = &zoneManager;
        host.lotManager = &lotManager;
        host.budget = &budget;
        host.demand = &demand;
        host.buildingDevelopment = &buildingDevelopment;
        host.lotConfigurationManager = &lotConfigurationManager;
        host.resourceManager = &resourceManager;
        SetPortDeveloperHost(&host);
    }
    ~Fixture() {
        SetPortDeveloperHost(nullptr);
        delete developer;
        ReleaseTestLots();
    }

    // Release() destroys the developer below a count of 2; drop the fixture's pointer first.
    void ReleaseDeveloper() {
        TestPortDeveloper* released = developer;
        developer = nullptr;
        CHECK(released->Release() == 0);
    }

    // The developer exemplar (instance = the port developer id) lists the two port type ids; each type has
    // its own exemplar with the stage arrays.
    void BuildExemplars(std::uint32_t developerId = kSeaportDeveloperId) {
        cSCPropertyHolder* list = exemplars.Make();
        Exemplars::Uints(list, kPropPortTypes, {0x111, 0x222});
        resourceManager.exemplars[developerId] = list;

        cSCPropertyHolder* type1 = exemplars.Make();
        Exemplars::Uint(type1, kPropStageCount, 2);
        Exemplars::Uints(type1, kPropLotConfigs, {0x1000, 0x1001});
        Exemplars::Int64s(type1, kPropBudgets, {100, 150});
        Exemplars::Uints(type1, kPropCapacities, {100, 200});
        Exemplars::Int64s(type1, kPropCosts, {1000, 2000});
        Exemplars::Float(type1, kPropThreshold, 50.0f);   // = 0.5
        Exemplars::Uint8(type1, kPropSizeClass, 1);
        Exemplars::Uint8(type1, kPropRows, 2);
        Exemplars::Uint8(type1, kPropColumns, 2);
        Exemplars::Floats(type1, kPropGrid, {1.0f, 2.0f, 3.0f, 4.0f});
        resourceManager.exemplars[0x111] = type1;

        cSCPropertyHolder* type2 = exemplars.Make();
        Exemplars::Uint(type2, kPropStageCount, 1);
        Exemplars::Uints(type2, kPropLotConfigs, {0x2000});
        Exemplars::Int64s(type2, kPropBudgets, {300});
        Exemplars::Uints(type2, kPropCapacities, {400});
        Exemplars::Int64s(type2, kPropCosts, {5000});
        Exemplars::Float(type2, kPropThreshold, 25.0f);   // = 0.25
        Exemplars::Uint8(type2, kPropSizeClass, 3);
        resourceManager.exemplars[0x222] = type2;
    }

    // One lot configuration per stage id; growth stage 1 (the only one DoLotInserted accepts).
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

    TestPortDeveloper* MakeDeveloper(std::uint32_t developerId = kSeaportDeveloperId,
                                     std::uint16_t zoneType = kSeaportZone) {
        developer = new TestPortDeveloper();
        developer->Configure(developerId, zoneType, kSeaportHistory, kSeaportAdvisor, kSeaportGroup);
        return developer;
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
};

//----------------------------------------------------------------------------------------------- tests

void TestLayout() {
    CHECK(sizeof(cSC4PortDeveloper) == 0xb0);
    CHECK(sizeof(cPortLot) == 0x40);
    CHECK(sizeof(cPortTypeConfig) == 0x50);
    CHECK(sizeof(cPortGrowthRule) == 0x20);
    CHECK(cSC4PortDeveloper::kOffsetPortLots == 0x68);
    CHECK(cSC4PortDeveloper::kOffsetConfigurations == 0x80);
    CHECK(cSC4PortDeveloper::kOffsetPortTypeIds == 0x98);
    CHECK(cPortLot::kOffsetStage == 0x08);
    CHECK(cPortLot::kOffsetConfig == 0x10);
    CHECK(cPortLot::kOffsetLot == 0x30);
    CHECK(cPortTypeConfig::kOffsetStages == 0x18);
    CHECK(cPortTypeConfig::kOffsetGrid == 0x38);
}

void TestCom() {
    Fixture f;
    g_destroyed = 0;
    auto* d = new TestPortDeveloper();
    d->Configure(kSeaportDeveloperId, kSeaportZone, kSeaportHistory, kSeaportAdvisor, kSeaportGroup);
    CHECK(d->RefCount() == 0);
    CHECK(d->AddRef() == 1);
    CHECK(d->AddRef() == 2);

    cISC4PortDeveloper* portView = d;
    cIGZMessageTarget2* targetView = d;
    cIGZSerializable* serializableView = d;
    void* out = nullptr;
    CHECK(portView->QueryInterface(1, &out));
    CHECK(out == static_cast<cISC4PortDeveloper*>(d));
    CHECK(portView->QueryInterface(kPortGZIIDPortDeveloper, &out));
    CHECK(out == static_cast<cISC4PortDeveloper*>(d));
    CHECK(targetView->QueryInterface(0x452294aa, &out));
    CHECK(out == static_cast<cIGZMessageTarget2*>(d));   // the +8 view
    CHECK(out != static_cast<void*>(d));
    CHECK(serializableView->QueryInterface(0xe4fda3d4, &out));
    CHECK(out == static_cast<cIGZSerializable*>(d));     // the +0x10 view
    CHECK(!portView->QueryInterface(0xdeadbeef, &out));
    CHECK(!portView->QueryInterface(1, nullptr));
    CHECK(d->RefCount() == 6);   // two manual AddRefs + four successful queries

    // The class id through the serializable view and the derived simulator type.
    CHECK(serializableView->GetGZCLSID() == kPortDeveloperClassId);
    CHECK(portView->GetSimulatorType() == 0xea3957ad);

    CHECK(portView->AddRef() == 7);
    CHECK(targetView->AddRef() == 8);
    CHECK(serializableView->AddRef() == 9);
    CHECK(serializableView->Release() == 8);
    CHECK(targetView->Release() == 7);
    CHECK(portView->Release() == 6);
    CHECK(portView->Release() == 5);
    CHECK(portView->Release() == 4);
    CHECK(portView->Release() == 3);
    CHECK(portView->Release() == 2);
    CHECK(portView->Release() == 1);
    CHECK(g_destroyed == 0);
    // Below 2 Release runs FinalRelease (Shutdown + delete) and reports 0.
    CHECK(portView->Release() == 0);
    CHECK(g_destroyed == 1);
}

void TestInitShutdown() {
    Fixture f;
    f.BuildExemplars();
    TestPortDeveloper* d = f.MakeDeveloper();
    CHECK(d->RefCount() == 0);
    CHECK(d->Init());
    CHECK(d->IsInitialized());
    CHECK(d->AddRef() == 1);

    // The ini exemplar (instance = the developer id) lists the two port types, each one loaded in turn.
    CHECK(d->TunablesWereRead());
    CHECK(d->PortDeveloperId() == kSeaportDeveloperId);
    CHECK(d->PortTypeCount() == 2);
    CHECK(d->PortTypeIds().size() == 2);
    CHECK(d->PortTypeIds()[0] == 0x111);
    CHECK(d->PortTypeIds()[1] == 0x222);
    CHECK(f.resourceManager.requested.size() == 3);
    CHECK(f.resourceManager.requested[0] == kSeaportDeveloperId);
    CHECK(f.resourceManager.requested[1] == 0x111);
    CHECK(f.resourceManager.requested[2] == 0x222);
    CHECK(d->Configurations().size() == 2);

    const cPortTypeConfig* first = d->Configurations()[0];
    CHECK(first->TypeIndex() == 0);
    CHECK(first->StageCount() == 2);
    CHECK(Near(first->Threshold(), 0.5f));
    CHECK(first->SizeClass() == 1);
    CHECK(first->Stages().size() == 2);
    CHECK(first->Stages()[0].stage == 0);
    CHECK(first->Stages()[0].lotConfigurationId == 0x1000);
    CHECK(first->Stages()[0].budget == 100);
    CHECK(first->Stages()[0].capacity == 100);
    CHECK(first->Stages()[0].cost == 1000);
    CHECK(first->Stages()[1].lotConfigurationId == 0x1001);
    CHECK(first->Stages()[1].budget == 150);
    CHECK(first->Stages()[1].capacity == 200);
    CHECK(first->Stages()[1].cost == 2000);
    CHECK(first->Grid().size() == 2);
    CHECK(first->Grid()[0].size() == 2);
    CHECK(first->Grid()[0][0] == 1.0f && first->Grid()[1][1] == 4.0f);

    const cPortTypeConfig* second = d->Configurations()[1];
    CHECK(second->TypeIndex() == 1);
    CHECK(second->StageCount() == 1);
    CHECK(Near(second->Threshold(), 0.25f));
    CHECK(second->SizeClass() == 3);
    CHECK(second->Stages()[0].lotConfigurationId == 0x2000);
    CHECK(second->Grid().empty());

    // The simulator agent: the +8 message view, interval 6, the class name and flags 2.
    CHECK(f.simulator.added.size() == 1);
    CHECK(f.simulator.added[0].target == static_cast<cIGZMessageTarget2*>(d));
    CHECK(f.simulator.added[0].interval == 6);
    CHECK(f.simulator.added[0].name == "cSC4PortDeveloper");
    CHECK(f.simulator.added[0].flags == 2);
    CHECK(f.simulator.refs == 0);

    // The three notifications, on the message view.
    CHECK(f.messages.added.size() == 3);
    CHECK(f.messages.added[0].first == static_cast<cIGZMessageTarget2*>(d));
    CHECK(f.messages.added[0].second == 0xe8bda2e7);
    CHECK(f.messages.added[1].second == 0x26d31ec1);
    CHECK(f.messages.added[2].second == 0x99ef1142);

    // The four services were taken and referenced; the message server too.
    CHECK(f.city.refs == 1);
    CHECK(f.zoneManager.refs == 1);
    CHECK(f.lotManager.refs == 1);
    CHECK(f.budget.refs == 1);
    CHECK(f.messages.refs == 1);
    CHECK(d->MessageServerForTest() == &f.messages);

    // A second Init is a no-op.
    CHECK(d->Init());
    CHECK(f.messages.added.size() == 3);
    CHECK(f.simulator.added.size() == 1);

    CHECK(d->Shutdown());
    CHECK(!d->IsInitialized());
    CHECK(f.messages.removed.size() == 3);
    CHECK(f.messages.removed[0].second == 0xe8bda2e7);
    CHECK(f.messages.removed[1].second == 0x26d31ec1);
    CHECK(f.messages.removed[2].second == 0x99ef1142);
    CHECK(f.city.refs == 0);
    CHECK(f.zoneManager.refs == 0);
    CHECK(f.lotManager.refs == 0);
    CHECK(f.budget.refs == 0);
    CHECK(f.messages.refs == 0);
    // The agent is removed through the *global* city's simulator.
    CHECK(f.city.getSimulatorCalls == 1);
    CHECK(f.simulator.removed.size() == 1);
    CHECK(f.simulator.removed[0] == static_cast<cIGZMessageTarget2*>(d));
    // A second Shutdown does nothing.
    CHECK(d->Shutdown());
    CHECK(f.simulator.removed.size() == 1);
    f.ReleaseDeveloper();
}

void TestTunables() {
    Fixture f;
    f.BuildExemplars();
    SetPortDeveloperHost(&f.host);

    // A developer without an identity loads nothing and leaves the flag clear (the original gates the whole
    // read on +0x20 being non-zero).
    auto* bare = new TestPortDeveloper();
    CHECK(!bare->TunablesWereRead());
    bare->readTunableVars();
    CHECK(!bare->TunablesWereRead());
    CHECK(f.resourceManager.requested.empty());
    CHECK(bare->PortTypeCount() == 0);
    delete bare;

    // A missing exemplar sets the flag but yields no port types.
    auto* missing = new TestPortDeveloper();
    missing->Configure(0x999, kSeaportZone, 0, 0, 0);
    missing->readTunableVars();
    CHECK(missing->TunablesWereRead());
    CHECK(missing->PortTypeCount() == 0);
    CHECK(missing->Configurations().empty());
    delete missing;

    // An exemplar without the port type list property behaves the same.
    cSCPropertyHolder* empty = f.exemplars.Make();
    f.resourceManager.exemplars[0x888] = empty;
    auto* noList = new TestPortDeveloper();
    noList->Configure(0x888, kSeaportZone, 0, 0, 0);
    noList->readTunableVars();
    CHECK(noList->TunablesWereRead());
    CHECK(noList->PortTypeCount() == 0);
    CHECK(noList->Configurations().empty());
    delete noList;

    // Reading twice does not load twice.
    auto* normal = new TestPortDeveloper();
    normal->Configure(kSeaportDeveloperId, kSeaportZone, kSeaportHistory, kSeaportAdvisor, kSeaportGroup);
    normal->readTunableVars();
    const std::size_t requests = f.resourceManager.requested.size();
    CHECK(normal->TunablesWereRead());
    normal->readTunableVars();
    CHECK(f.resourceManager.requested.size() == requests);
    CHECK(normal->Configurations().size() == 2);
    delete normal;

    // A configuration whose exemplar has mismatched stage arrays is clamped to the shortest one.
    cSCPropertyHolder* shortArrays = f.exemplars.Make();
    Exemplars::Uint(shortArrays, kPropStageCount, 3);
    Exemplars::Uints(shortArrays, kPropLotConfigs, {0x3000, 0x3001});
    Exemplars::Int64s(shortArrays, kPropBudgets, {10});
    Exemplars::Uints(shortArrays, kPropCapacities, {20, 30});
    Exemplars::Int64s(shortArrays, kPropCosts, {40, 50, 60});
    f.resourceManager.exemplars[0x333] = shortArrays;
    cSCPropertyHolder* list = f.exemplars.Make();
    Exemplars::Uints(list, kPropPortTypes, {0x333});
    f.resourceManager.exemplars[0x777] = list;
    auto* clamped = new TestPortDeveloper();
    clamped->Configure(0x777, kSeaportZone, 0, 0, 0);
    clamped->readTunableVars();
    CHECK(clamped->Configurations().size() == 1);
    CHECK(clamped->Configurations()[0]->StageCount() == 3);
    CHECK(clamped->Configurations()[0]->Stages().size() == 1);   // the shortest array
    CHECK(clamped->Configurations()[0]->Stages()[0].budget == 10);
    delete clamped;

    // A configuration exemplar with the four arrays missing loads no stages but is still created.
    cSCPropertyHolder* noArrays = f.exemplars.Make();
    Exemplars::Uint(noArrays, kPropStageCount, 1);
    f.resourceManager.exemplars[0x444] = noArrays;
    cSCPropertyHolder* list2 = f.exemplars.Make();
    Exemplars::Uints(list2, kPropPortTypes, {0x444});
    f.resourceManager.exemplars[0x555] = list2;
    auto* emptyStages = new TestPortDeveloper();
    emptyStages->Configure(0x555, kSeaportZone, 0, 0, 0);
    emptyStages->readTunableVars();
    CHECK(emptyStages->Configurations().size() == 1);
    CHECK(emptyStages->Configurations()[0]->StageCount() == 1);
    CHECK(emptyStages->Configurations()[0]->Stages().empty());
    delete emptyStages;
}

void TestDoLotInserted() {
    Fixture f;
    f.BuildExemplars();
    TestPortDeveloper* d = f.MakeDeveloper();
    CHECK(d->Init());
    const int budgetUpdatesBefore = d->budgetUpdates;

    // The first stage's lot configuration id (0x1000) matches the first port type.
    cPortLot* port = f.InsertLot(5, 6, 0x1000);
    CHECK(port != nullptr);
    CHECK(d->PortLotCount() == 1);
    CHECK(port->Stage() == 0);
    CHECK(port->Config() == d->Configurations()[0]);
    CHECK((port->CachedLot() == f.lotManager.lots[{5, 6}]));
    CHECK(port->X() == 5 && port->Z() == 6);
    CHECK(!port->LotIsStale());
    CHECK(!port->IsBeingRebuilt());
    CHECK(Near(port->Efficiency(), 1.0f));
    CHECK(Near(port->Usage(), 0.0f));
    CHECK(port->Trips() == 0);
    CHECK(d->budgetUpdates == budgetUpdatesBefore + 1);
    CHECK(d->newLots == 1);
    CHECK(d->newLotArgs.size() == 1 && d->newLotArgs[0] == port);

    // The list is push_front: the second insert becomes the front element.
    cPortLot* second = f.InsertLot(7, 8, 0x2000);
    CHECK(second != nullptr);
    CHECK(d->PortLotCount() == 2);
    CHECK(d->PortLots().front() == second);
    CHECK(second->Config() == d->Configurations()[1]);

    // A configuration id that no port type starts with, a growth stage other than 1, and a lot without a
    // configuration are all rejected (the temporary record is deleted).
    cSC4Lot* unknown = Fixture::MakeLot(9, 9);
    unknown->SetLotConfiguration(f.AddConfiguration(0x9999));
    d->DoLotInserted(unknown);
    CHECK(d->PortLotCount() == 2);
    cSC4Lot* stageTwo = Fixture::MakeLot(10, 10);
    stageTwo->SetLotConfiguration(f.AddConfiguration(0x1000, 2));
    d->DoLotInserted(stageTwo);
    CHECK(d->PortLotCount() == 2);
    cSC4Lot* noConfig = Fixture::MakeLot(11, 11);
    d->DoLotInserted(noConfig);
    CHECK(d->PortLotCount() == 2);

    // Shutdown releases the lots and deletes the configurations; the ports are gone.
    CHECK(d->Shutdown());
    CHECK(d->PortLotCount() == 0);
    CHECK(d->Configurations().empty());
    f.ReleaseDeveloper();
}

void TestDoLotStateChanged() {
    Fixture f;
    f.BuildExemplars();
    TestPortDeveloper* d = f.MakeDeveloper();
    CHECK(d->Init());

    FakeLotConfiguration* config = f.AddConfiguration(0x1000);
    config->compatibleZones = {kSeaportZone};
    cPortLot* port = f.InsertLot(5, 6, 0x1000);
    CHECK(port != nullptr);
    cSC4Lot* lot = f.lotManager.lots[{5, 6}];

    // A state other than 5 does nothing.
    d->doLotStateChanged(lot, 2, 4);
    CHECK(d->PortLotCount() == 1);
    // An incompatible zone does nothing.
    config->compatibleZones.clear();
    d->doLotStateChanged(lot, 2, 5);
    CHECK(d->PortLotCount() == 1);
    // A compatible zone with a lot that is not a port does nothing.
    config->compatibleZones = {kSeaportZone};
    cSC4Lot* other = Fixture::MakeLot(50, 60);
    other->SetLotConfiguration(config);
    d->doLotStateChanged(other, 2, 5);
    CHECK(d->PortLotCount() == 1);
    // The rebuilding flag is cleared and the record kept.
    port->SetIsBeingRebuilt(true);
    d->doLotStateChanged(lot, 2, 5);
    CHECK(d->PortLotCount() == 1);
    CHECK(!port->IsBeingRebuilt());
    // Otherwise the record is removed: the demand is taken back, the footprint is de-zoned and the budget
    // is refreshed.
    const int budgetUpdates = d->budgetUpdates;
    const std::size_t demandEffects = f.demand.effects.size();
    d->doLotStateChanged(lot, 2, 5);
    CHECK(d->PortLotCount() == 0);
    CHECK(f.demand.effects.size() == demandEffects + 1);
    CHECK(f.demand.effects.back().buildingType == lot->GetBuildingType(false));
    CHECK(Near(f.demand.effects.back().oldUsage, 0.0f));
    CHECK(Near(f.demand.effects.back().newUsage, 0.0f));
    CHECK(f.zoneManager.clearCalls == 1);
    CHECK(f.zoneManager.lastRegion.x0 == 5 && f.zoneManager.lastRegion.z0 == 6);
    CHECK(f.zoneManager.lastRegion.x1 == 5 && f.zoneManager.lastRegion.z1 == 6);
    CHECK(f.zoneManager.lastRegion.rowCount == 1);
    CHECK(f.zoneManager.lastRegion.bitCount == 1);
    CHECK(f.zoneManager.lastRegion.wordCount == 1);
    CHECK(f.zoneManager.lastRegion.Test(5, 6));
    CHECK(!f.zoneManager.lastRegion.Test(6, 6));
    CHECK(d->budgetUpdates == budgetUpdates + 1);
    f.ReleaseDeveloper();
}

void TestStream() {
    Fixture f;
    f.BuildExemplars();
    TestPortDeveloper* d = f.MakeDeveloper();
    CHECK(d->Init());
    cPortLot* first = f.InsertLot(10, 20, 0x1000);
    cPortLot* second = f.InsertLot(30, 40, 0x2000);
    CHECK(first && second);
    second->SetStage(1);
    CHECK(d->PortLotCount() == 2);

    PortMemoryOStream out;
    CHECK(d->Write(out));
    // Version (uint16) 1, developer id, zone type, count, then per port: stage, type index, x, z.
    const std::vector<unsigned char>& bytes = out.bytes();
    CHECK(bytes.size() == 2 + 4 + 2 + 2 + 2 * (4 + 2 + 2 + 2));
    CHECK(bytes[0] == 1 && bytes[1] == 0);

    // Read into a fresh developer with the same host: the exemplars load from the stored developer id.
    auto* reader = new TestPortDeveloper();
    reader->Configure(kSeaportDeveloperId, kSeaportZone, kSeaportHistory, kSeaportAdvisor, kSeaportGroup);
    PortMemoryIStream in(bytes.data(), bytes.size());
    CHECK(reader->Read(in));
    CHECK(reader->PortDeveloperId() == kSeaportDeveloperId);
    CHECK(reader->ZoneType() == kSeaportZone);
    CHECK(reader->PortLotCount() == 2);
    // Write walks the list front to back (the second lot first); Read push_fronts every record, so the
    // list ends up in the reverse order of the file.
    const cPortLot* readFirst = reader->PortLots().front();
    CHECK(readFirst->X() == 10 && readFirst->Z() == 20);
    CHECK(readFirst->Stage() == 0);
    CHECK(readFirst->Config() == reader->Configurations()[0]);
    CHECK(readFirst->LotIsStale());   // Read marks the cached lot stale
    const cPortLot* readSecond = *std::next(reader->PortLots().begin());
    CHECK(readSecond->X() == 30 && readSecond->Z() == 40);
    CHECK(readSecond->Stage() == 1);
    CHECK(readSecond->Config() == reader->Configurations()[1]);
    delete reader;

    // A different version is rejected.
    PortMemoryOStream badOut;
    badOut.SetUint16(2);
    PortMemoryIStream badIn(badOut.bytes().data(), badOut.bytes().size());
    auto* badReader = new TestPortDeveloper();
    CHECK(!badReader->Read(badIn));
    CHECK(badReader->PortLotCount() == 0);
    delete badReader;

    // An out-of-range configuration index yields a record without a configuration (no bounds check in the
    // original; the port only guards the vector access).
    PortMemoryOStream oddOut;
    oddOut.SetUint16(1);
    oddOut.SetUint32(kSeaportDeveloperId);
    oddOut.SetUint16(kSeaportZone);
    oddOut.SetUint16(1);
    oddOut.SetUint32(0);
    oddOut.SetUint16(7);   // config index 7
    oddOut.SetSint16(1);
    oddOut.SetSint16(2);
    PortMemoryIStream oddIn(oddOut.bytes().data(), oddOut.bytes().size());
    auto* oddReader = new TestPortDeveloper();
    CHECK(oddReader->Read(oddIn));
    CHECK(oddReader->PortLotCount() == 1);
    CHECK(oddReader->PortLots().front()->Config() == nullptr);
    delete oddReader;
    f.ReleaseDeveloper();
}

void TestMonthlySimulation() {
    Fixture f;
    f.BuildExemplars();
    TestPortDeveloper* d = f.MakeDeveloper();
    CHECK(d->Init());
    cPortLot* port = f.InsertLot(10, 20, 0x1000);
    CHECK(port != nullptr);
    cSC4Lot* lot = f.lotManager.lots[{10, 20}];

    // Funding 200 / required 100 = allocation 2; trips 40 -> threshold 200 -> efficiency 0.8.
    d->funding = 200;
    d->trips[port] = 40;
    f.demand.effects.clear();
    f.messages.sent.clear();
    d->doMonthlySimulation();
    CHECK(Near(port->Efficiency(), 0.8f));
    CHECK(Near(port->Usage(), 0.4f));   // usage 40 / capacity 100
    CHECK(f.demand.effects.size() == 1);
    CHECK(f.demand.effects[0].buildingType == lot->GetBuildingType(false));
    CHECK(Near(f.demand.effects[0].oldUsage, 0.0f));
    CHECK(Near(f.demand.effects[0].newUsage, 0.4f));
    CHECK(lot->GetCondition() == 0);   // threshold 0.5 < 0.8 and funding > 0
    CHECK(d->upgrades == 0);           // state 0 is not an upgrade state
    CHECK(f.messages.sent.size() == 1);
    {
        CHECK(f.messages.sent[0].type == 0x89efa536);
        CHECK(f.messages.sent[0].data1 == kSeaportHistory);
        CHECK(f.messages.sent[0].data2 == 80);   // round(0.8 * 100)
    }

    // Trips 96 with funding 100 (allocation 1) -> efficiency 0.04, under the 0.5 threshold; with no
    // simulator day the condition stays 0.
    d->funding = 100;
    d->trips[port] = 96;
    lot->SetState(kLotStateLivable, 0, 0);
    f.messages.sent.clear();
    d->doMonthlySimulation();
    CHECK(Near(port->Efficiency(), 0.04f));
    CHECK(lot->GetCondition() == 0);
    // state 3 is one of the upgrade states and the port is triggered (96 >= 96% of 100) -> one message.
    CHECK(d->upgrades == 1);
    CHECK(d->upgradeArgs.size() == 1 && d->upgradeArgs[0] == port);
    CHECK(f.messages.sent.size() == 1);
    {
        CHECK(f.messages.sent[0].data2 == 4);   // round(0.04 * 100)
    }

    // The condition 1 lots are skipped entirely; only the history record remains.
    lot->SetCondition(1);
    d->trips[port] = 40;
    f.demand.effects.clear();
    f.messages.sent.clear();
    d->doMonthlySimulation();
    CHECK(f.demand.effects.empty());
    CHECK(f.messages.sent.size() == 1);
    CHECK(f.messages.sent[0].type == 0x89efa536);

    // The monthly message runs the virtual tick and then the simulation.
    const int ticks = d->monthlyTicks;
    lot->SetCondition(0);
    f.messages.sent.clear();
    cRZMessage2Standard message;
    message.SetType(0x66956816);
    CHECK(d->DoMessage(&message));
    CHECK(d->monthlyTicks == ticks + 1);
    CHECK(f.messages.sent.size() == 1);

    // No ports: nothing happens at all.
    CHECK(d->Shutdown());
    f.messages.sent.clear();
    d->doMonthlySimulation();
    CHECK(f.messages.sent.empty());
    f.ReleaseDeveloper();
}

void TestGrowth() {
    Fixture f;
    f.BuildExemplars();
    TestPortDeveloper* d = f.MakeDeveloper();
    CHECK(d->Init());
    cPortLot* port = f.InsertLot(10, 20, 0x1000);
    CHECK(port != nullptr);
    cSC4Lot* oldLot = f.lotManager.lots[{10, 20}];
    FakeLotConfiguration* nextConfig = f.AddConfiguration(0x1001);

    // Not triggered below 96% of the current stage's capacity.
    d->trips[port] = 95;
    d->GrowPortsToNextStage();
    CHECK(f.lotManager.createCalls.empty());
    CHECK(port->Stage() == 0);

    // Triggered: the lot is demolished and rebuilt with stage 1's configuration.
    d->trips[port] = 96;
    d->GrowPortsToNextStage();
    CHECK(f.lotManager.createCalls.size() == 1);
    CHECK(f.lotManager.createCalls[0].x == 10);
    CHECK(f.lotManager.createCalls[0].z == 20);
    CHECK(f.lotManager.createCalls[0].sizeX == 1);
    CHECK(f.lotManager.createCalls[0].sizeZ == 1);
    CHECK(f.lotManager.createCalls[0].facing == 0);
    CHECK(f.lotDeveloper.startDemolish.size() == 1);
    CHECK(f.lotDeveloper.startDemolish[0] == oldLot);
    CHECK(f.lotDeveloper.endDemolish.size() == 1);
    CHECK(f.lotDeveloper.endDemolish[0] == oldLot);
    CHECK(f.lotDeveloper.constructs.size() == 1);
    CHECK(f.lotDeveloper.constructs[0].lot == f.lotManager.created.back());
    CHECK(f.lotDeveloper.constructs[0].configuration == nextConfig);
    CHECK(f.lotDeveloper.constructs[0].objectId == 0);
    CHECK(f.lotDeveloper.constructs[0].flag);
    CHECK(port->CachedLot() == f.lotManager.created.back());
    CHECK(port->Stage() == 1);
    CHECK(f.budget.withdrawals.size() == 1);
    CHECK(f.budget.withdrawals[0] == 2000);   // the *next* stage's cost
    CHECK(d->stageChanges == 1);
    CHECK(d->stageChangeArgs[0] == port);
    CHECK(d->budgetUpdates >= 2);

    // A failed construction leaves the stage unchanged, spends nothing and skips the callbacks.
    cPortLot* fresh = f.InsertLot(30, 40, 0x1000);
    CHECK(fresh != nullptr);
    d->trips[fresh] = 96;
    f.lotDeveloper.constructResult = false;
    const std::size_t withdrawals = f.budget.withdrawals.size();
    const int stageChanges = d->stageChanges;
    d->GrowPortsToNextStage();
    CHECK(fresh->Stage() == 0);
    CHECK(f.budget.withdrawals.size() == withdrawals);
    CHECK(d->stageChanges == stageChanges);

    // A failed CreateLot has the same effect.
    f.lotDeveloper.constructResult = true;
    f.lotManager.createResult = false;
    d->GrowPortsToNextStage();
    CHECK(fresh->Stage() == 0);
    CHECK(f.budget.withdrawals.size() == withdrawals);
    CHECK(d->Shutdown());
    f.ReleaseDeveloper();
}

void TestQueries() {
    Fixture f;
    f.BuildExemplars();
    TestPortDeveloper* d = f.MakeDeveloper();
    CHECK(d->Init());
    cPortLot* first = f.InsertLot(10, 20, 0x1000);
    cPortLot* second = f.InsertLot(30, 40, 0x2000);
    CHECK(first && second);
    cSC4Lot* firstLot = f.lotManager.lots[{10, 20}];
    cSC4Lot* secondLot = f.lotManager.lots[{30, 40}];

    // Budgets: stage 0 of type 1 (100) + stage 0 of type 2 (300).
    CHECK(d->GetRequiredBudget() == 400);

    // Efficiency: the average of the two records (1.0 with no ports).
    first->SetEfficiency(0.5f);
    second->SetEfficiency(1.0f);
    CHECK(Near(d->GetAvgEfficiency(), 0.75f));
    CHECK(d->GetNumPorts(1 << 1, 0x1f) == 1);   // size class 1 = the first port
    CHECK(d->GetNumPorts(1 << 3, 0x1f) == 1);   // size class 3 = the second port
    CHECK(d->GetNumPorts(1 << 1, 1 << 0) == 1);
    CHECK(d->GetNumPorts(1 << 1, 1 << 1) == 0);
    CHECK(d->GetNumPortsBySize(1) == 1);
    CHECK(d->GetNumPortsBySize(3) == 1);
    CHECK(d->GetNumPortsBySize(2) == 0);

    // Usage values: trips (0) capped at the stage capacity.
    std::uint32_t usage = 99;
    std::uint32_t capacity = 99;
    CHECK(d->GetPortUsageValues(secondLot, usage, capacity));
    CHECK(usage == 0);
    CHECK(capacity == 400);   // stage 0 of type 2
    CHECK(!d->GetPortUsageValues(nullptr, usage, capacity));
    CHECK(usage == 0 && capacity == 0);

    // GetPortUsageValues uses the virtual trips.
    d->trips[second] = 500;
    CHECK(d->GetPortUsageValues(secondLot, usage, capacity));
    CHECK(usage == 400);   // capped at the capacity
    CHECK(capacity == 400);

    // GetLeastEfficientPort: the *first* record of the list (push_front puts the second lot in front) and
    // not the least efficient one (the original never advances its iterator).
    SC4Percentage percentage;
    PortPoint location;
    CHECK(d->GetLeastEfficientPort(percentage, location));
    CHECK(Near(percentage.value, 1.0f));
    CHECK(location.x == 30 && location.z == 40);

    // The lot queries.
    cPortLot* found = nullptr;
    CHECK(d->GetPortLot(secondLot, found));
    CHECK(found == second);
    cPortLot* notFound = nullptr;
    CHECK(!d->GetPortLot(nullptr, notFound));
    CHECK(notFound == nullptr);
    std::vector<cSC4Lot*> lots;
    d->GetAllPortLots(lots);
    CHECK(lots.size() == 2);
    CHECK(std::find(lots.begin(), lots.end(), firstLot) != lots.end());
    for (cSC4Lot* lot : lots) lot->Release();   // the references GetAllPortLots took

    // The advisor event and the history message.
    d->SendAdvisorMessage(0x1234);
    CHECK(f.advisorSystem.events.size() == 1);
    CHECK(f.advisorSystem.events[0].first == 0x1234);
    CHECK(f.advisorSystem.events[0].second == nullptr);
    CHECK(f.advisorSystem.refs == 0);
    f.messages.sent.clear();
    d->RecordHistoryData(0x55, 0x66);
    CHECK(f.messages.sent.size() == 1);
    {
        CHECK(f.messages.sent[0].type == 0x89efa536);
        CHECK(f.messages.sent[0].data1 == 0x55);
        CHECK(f.messages.sent[0].data2 == 0x66);
    }

    // The post-city-init message refreshes every cached lot.
    const int getCalls = f.lotManager.getCalls;
    first->SetLotIsStale(true);
    second->SetLotIsStale(true);
    cRZMessage2Standard refresh;
    refresh.SetType(0x26d31ec1);
    CHECK(d->DoMessage(&refresh));
    CHECK(f.lotManager.getCalls == getCalls + 2);
    CHECK(!first->LotIsStale());
    CHECK(!second->LotIsStale());

    // Unknown message types are ignored.
    cRZMessage2Standard other;
    other.SetType(0x12345678);
    CHECK(d->DoMessage(&other));
    CHECK(d->DoMessage(nullptr));
    CHECK(d->Shutdown());
    f.ReleaseDeveloper();
}

void TestMessages() {
    Fixture f;
    f.BuildExemplars();
    TestPortDeveloper* d = f.MakeDeveloper();
    CHECK(d->Init());

    // The message payload objects outlive the messages that reference them.
    FakePropertyHolder holder;
    FakePropertyHolder payload;

    // The placed-building message: an occupant of the port building type in the port's occupant group.
    FakeOccupant occupant;
    occupant.type = 0x278128a0;
    occupant.group = kSeaportGroup;
    occupant.holder = &holder;
    cSC4Lot* lot = Fixture::MakeLot(12, 34);
    FakeLotConfiguration* lotConfiguration = f.AddConfiguration(0x1000);
    lotConfiguration->allZones = true;
    lot->SetLotConfiguration(lotConfiguration);
    f.lotManager.occupantLots[&occupant] = lot;

    cRZMessage2Standard placed;
    placed.SetType(0x99ef1142);
    placed.SetVoid1(&occupant);
    CHECK(d->DoMessage(&placed));
    CHECK(d->PortLotCount() == 1);
    CHECK(d->PortLots().front()->CachedLot() == lot);
    CHECK(f.lotManager.occupantLotCalls == 1);
    CHECK(occupant.refs == 0);   // the message target's reference is balanced

    // The excluded property suppresses the record; so does a wrong group and a wrong type.
    FakePropertyHolder excludedHolder;
    excludedHolder.properties.insert(0x29244db);
    occupant.holder = &excludedHolder;
    cRZMessage2Standard excluded;
    excluded.SetType(0x99ef1142);
    excluded.SetVoid1(&occupant);
    CHECK(d->DoMessage(&excluded));
    CHECK(d->PortLotCount() == 1);
    occupant.holder = &holder;
    occupant.group = 0x1508;
    cRZMessage2Standard wrongGroup;
    wrongGroup.SetType(0x99ef1142);
    wrongGroup.SetVoid1(&occupant);
    CHECK(d->DoMessage(&wrongGroup));
    CHECK(d->PortLotCount() == 1);
    occupant.group = kSeaportGroup;
    occupant.type = 0x1234;
    cRZMessage2Standard wrongType;
    wrongType.SetType(0x99ef1142);
    wrongType.SetVoid1(&occupant);
    CHECK(d->DoMessage(&wrongType));
    CHECK(d->PortLotCount() == 1);
    // A null payload is ignored (the original would fault).
    cRZMessage2Standard noPayload;
    noPayload.SetType(0x99ef1142);
    CHECK(d->DoMessage(&noPayload));

    // The lot state message: state 5 removes the record; the payload is resolved by the host.
    occupant.type = 0x278128a0;
    cRZMessage2Standard stateMessage;
    stateMessage.SetType(0xe8bda2e7);
    stateMessage.SetData1(2);
    stateMessage.SetData2(5);
    stateMessage.SetIGZUnknown(&payload);
    f.host.stateMessageLot = lot;
    CHECK(d->DoMessage(&stateMessage));
    CHECK(d->PortLotCount() == 0);
    CHECK(f.zoneManager.clearCalls == 1);
    // Without a resolved lot nothing happens.

    // A lot state message whose payload the host does not resolve.
    cRZMessage2Standard stateNoLot;
    stateNoLot.SetType(0xe8bda2e7);
    stateNoLot.SetData1(2);
    stateNoLot.SetData2(5);
    stateNoLot.SetIGZUnknown(&payload);
    f.host.stateMessageLot = nullptr;
    CHECK(d->DoMessage(&stateNoLot));
    CHECK(d->PortLotCount() == 0);
    CHECK(d->Shutdown());
    f.ReleaseDeveloper();
}

void TestNullHost() {
    // Without a host the port reports failure instead of faulting (the original dereferences the globals).
    SetPortDeveloperHost(nullptr);
    auto* d = new TestPortDeveloper();
    d->Configure(kSeaportDeveloperId, kSeaportZone, kSeaportHistory, kSeaportAdvisor, kSeaportGroup);
    d->readTunableVars();
    CHECK(d->TunablesWereRead());
    CHECK(d->Configurations().empty());
    CHECK(!d->Init());
    // The original sets the initialised byte before it touches the services, so the second call is a no-op
    // that reports success.
    CHECK(d->Init());
    CHECK(d->Shutdown());
    CHECK(d->Release() == 0);
}

}  // namespace

int main() {
    TestLayout();
    TestCom();
    TestInitShutdown();
    TestTunables();
    TestDoLotInserted();
    TestDoLotStateChanged();
    TestStream();
    TestMonthlySimulation();
    TestGrowth();
    TestQueries();
    TestMessages();
    TestNullHost();
    if (g_failures == 0) std::puts("sc4_portdeveloper ok");
    return g_failures == 0 ? 0 : 1;
}
