// cSC4SeaportDeveloper — unit test. Drives the recovered seaport developer through a fake ISeaportHost (which
// is also the base's IPortDeveloperHost) plus real cSC4Lot objects, real cSCPropertyHolder/cSCProperty/
// cRZVariant exemplars, real cRZMessage2Standard messages and the port memory streams. It covers
//   * the object layout and identity (base at +8, the +0xb8..+0xdc members, 0xe0 bytes),
//   * the four COM views and the shared reference count,
//   * Init / Shutdown (the base's agent + services + three messages, the seaport department and its line item,
//     the two seaport messages, the water map build, the department reference),
//   * the sea-access map (water bit 0x80, the four border bits, CityHasSea, GetCellSeaAccess, the monthly
//     CalculateWaterAccessible check and the advisor event),
//   * the trip-capacity feed (GetTripsForPort through the city's traffic simulator, UpdateTripCapacity with
//     the department funding ratio, the sums of GetNumSeaportTrips),
//   * the seaport budget (GetAllocatedFunding, UpdateBudget, GetSeaportUsageValues, GetMaintenanceCost),
//   * OnNewLotCreated's terrain flattening (all four facings, the grid-size gate, the world-position quirk),
//   * the message routes, the version-2 stream gate and the queries,
//   * cSC4LotPropFilter's inclusion rules.
//
// The fake city's CellCenterToPosition mirrors the recovered cSC4City: (cell + 0.5) * 16 m.

#include "sc4/SeaportDeveloper.h"

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

// The exemplar property ids of cPortTypeConfig::LoadConfiguration (PortDeveloper.h).
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
    int refs = 0;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    std::vector<cIGZMessageTarget2*> added, removed;
    void AddAgent(cIGZMessageTarget2* target, int, const char*, std::uint32_t) override {
        added.push_back(target);
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

struct FakeTraffic final : ISeaportTrafficApi {
    int refs = 0;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    struct CapacityCall {
        cISCPropertyHolder* source = nullptr;
        std::uint32_t travelType = 0;
        std::uint32_t capacity = 0;
    };
    struct ArrivedCall {
        cISCPropertyHolder* source = nullptr;
        std::uint32_t travelType = 0;
    };
    std::vector<CapacityCall> capacities;
    std::vector<ArrivedCall> arrived;
    std::uint32_t trips = 0;
    std::uint32_t GetTrafficArrived(cISCPropertyHolder* source, std::uint32_t travelType) override {
        arrived.push_back({source, travelType});
        return trips;
    }
    void SetMaxTripCapacity(cISCPropertyHolder* source, std::uint32_t travelType,
                            std::uint32_t capacity) override {
        capacities.push_back({source, travelType, capacity});
    }
};

struct FakeCity final : ISeaportCityApi {
    int refs = 0;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    FakeSimulator* simulator = nullptr;
    FakeAdvisorSystem* advisorSystem = nullptr;
    FakeTraffic* traffic = nullptr;
    struct CellCall {
        int x = 0, z = 0;
        float worldX = 0, worldZ = 0;
    };
    std::vector<CellCall> cellCalls;
    IPortSimulatorApi* GetSimulator() override { return simulator; }
    IPortAdvisorSystemApi* GetAdvisorSystem() override { return advisorSystem; }
    ISeaportTrafficApi* GetTrafficSimulator() override { return traffic; }
    void CellCenterToPosition(std::int32_t cellX, std::int32_t cellZ, float& x, float& z) override {
        x = (static_cast<float>(cellX) + 0.5f) * 16.0f;
        z = (static_cast<float>(cellZ) + 0.5f) * 16.0f;
        cellCalls.push_back({cellX, cellZ, x, z});
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

struct FakeLotManager final : IPortLotManagerApi {
    int refs = 0;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    std::map<std::pair<int, int>, cSC4Lot*> lots;
    std::map<cISC4Occupant*, cSC4Lot*> occupantLots;
    std::vector<cSC4Lot*> created;
    cSC4Lot* GetLot(int x, int z, bool) override {
        auto it = lots.find({x, z});
        return it == lots.end() ? nullptr : it->second;
    }
    cSC4Lot* GetOccupantLot(cISC4Occupant* occupant) override {
        auto it = occupantLots.find(occupant);
        return it == occupantLots.end() ? nullptr : it->second;
    }
    bool CreateLot(int x, int z, int sizeX, int sizeZ, int facing, cSC4Lot** out) override {
        auto* lot = new cSC4Lot(x, z, sizeX, sizeZ, facing);
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
    int refs = 0;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    void UpdateOccupantEffects(std::uint32_t, const SC4Percentage&, const SC4Percentage&) override {}
};

struct FakeLotConfiguration final : IPortLotConfiguration {
    int refs = 1;
    std::uint32_t id = 0;
    int growthStage = 1;
    std::vector<int> compatibleZones{12};
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    std::uint32_t GetID() const override { return id; }
    int GetGrowthStage() const override { return growthStage; }
    bool IsCompatibleWithZoneType(int zoneType) const override {
        return std::find(compatibleZones.begin(), compatibleZones.end(), zoneType) != compatibleZones.end();
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
    bool constructResult = true;
    bool ConstructDefaultLot(cSC4Lot*, IPortLotConfiguration*, std::uint32_t, bool) override {
        return constructResult;
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
        std::uint64_t data1 = 0, data2 = 0;
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

// The seaport budget department / line item.
struct FakeLineItem final : ISeaportLineItemApi {
    int refs = 0;
    std::uint32_t id = 0;
    std::int64_t fullExpenses = 0;
    std::int64_t currentExpenses = 0;
    bool displayFlag1 = true;
    int displayFlagCalls = 0;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    std::int64_t GetFullExpenses() const override { return fullExpenses; }
    std::int64_t GetCurrentExpenses() const override { return currentExpenses; }
    void SetFullExpenses(std::int64_t value) override { fullExpenses = value; }
    void SetDisplayFlag(std::uint32_t flag, bool on) override {
        ++displayFlagCalls;
        if (flag == 1) displayFlag1 = on;
    }
};

struct FakeDepartment final : ISeaportDepartmentBudgetApi {
    int refs = 0;
    std::uint32_t id = 0;
    std::uint32_t group = 0;
    std::uint32_t nameGroup = 0, nameInstance = 0;
    float maxFunding = 0.0f;
    int nameCalls = 0, maxFundingCalls = 0, getLineItemCalls = 0;
    ~FakeDepartment() { for (auto& entry : lineItems) delete entry.second; }
    std::map<std::uint32_t, FakeLineItem*> lineItems;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    void SetDepartmentName(std::uint32_t group_, std::uint32_t instance) override {
        ++nameCalls;
        nameGroup = group_;
        nameInstance = instance;
    }
    void SetMaxAllowedFundingPercentage(const SC4Percentage& percentage) override {
        ++maxFundingCalls;
        maxFunding = percentage.value;
    }
    ISeaportLineItemApi* CreateLineItem(std::uint32_t itemId, bool) override {
        auto* item = new FakeLineItem();
        item->id = itemId;
        lineItems[itemId] = item;
        return item;
    }
    ISeaportLineItemApi* GetLineItem(std::uint32_t itemId) override {
        ++getLineItemCalls;
        auto it = lineItems.find(itemId);
        return it == lineItems.end() ? nullptr : it->second;
    }
};

struct FakeBudgetSim final : ISeaportBudgetSimApi {
    ~FakeBudgetSim() { for (auto& entry : departments) delete entry.second; }
    std::map<std::uint32_t, FakeDepartment*> departments;
    int getCalls = 0, createCalls = 0;
    ISeaportDepartmentBudgetApi* CreateDepartmentBudget(std::uint32_t id, std::uint32_t group) override {
        ++createCalls;
        auto* department = new FakeDepartment();
        department->id = id;
        department->group = group;
        departments[id] = department;
        return department;
    }
    ISeaportDepartmentBudgetApi* GetDepartmentBudget(std::uint32_t id) override {
        ++getCalls;
        auto it = departments.find(id);
        return it == departments.end() ? nullptr : it->second;
    }
};

// The prop occupant view SetLotPropsState puts into a state.
struct FakePropOccupant final : ISeaportPropOccupantApi {
    int refs = 0;
    std::uint32_t state = 0;
    int setCalls = 0;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    void SetState(std::uint32_t value) override {
        ++setCalls;
        state = value;
    }
};

// cISC4Occupant for the lot-prop query and the placed-building message.
struct FakeOccupant final : cISC4Occupant {
    int refs = 0;
    std::uint32_t type = 0;
    std::uint32_t group = 0;
    float position[3] = {0.0f, 0.0f, 0.0f};
    cISCPropertyHolder* holder = nullptr;
    FakePropOccupant* prop = nullptr;   // the cISC4PropOccupant view, when it is a prop
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cISC4PropOccupant && prop) {
            *out = static_cast<ISeaportPropOccupantApi*>(prop);
            return true;
        }
        return false;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    cISCPropertyHolder* AsPropertyHolder() override { return holder; }
    std::uint32_t GetType() const override { return type; }
    void GetPosition(float* xyz) override {
        xyz[0] = position[0];
        xyz[1] = position[1];
        xyz[2] = position[2];
    }
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

// The occupant manager: applies the filter to its own occupant list inside the cell rectangle.
struct FakeOccupantManager final : ISeaportOccupantManagerApi {
    std::vector<cISC4Occupant*> occupants;
    struct Query {
        int low[2] = {0, 0};
        int high[2] = {0, 0};
        std::uint32_t maxCount = 0;
        cISC4OccupantFilter* filter = nullptr;
    };
    std::vector<Query> queries;
    int releaseCalls = 0;
    std::size_t lastReturned = 0;
    void GetOccupantsByStandardCityCells(OccupantList& out, const int* a, const int* b,
                                         cISC4OccupantFilter* filter, std::uint32_t maxCount) override {
        queries.push_back({{a[0], a[1]}, {b[0], b[1]}, maxCount, filter});
        for (cISC4Occupant* occupant : occupants) {
            if (maxCount != 0xffffffffu && out.size() >= maxCount) break;
            if (filter && !filter->IsOccupantIncluded(occupant)) continue;
            occupant->AddRef();
            out.push_back(occupant);
        }
        lastReturned = out.size();
    }
    void ReleaseOccupantList(OccupantList& list) override {
        ++releaseCalls;
        for (cISC4Occupant* occupant : list) occupant->Release();
        list.clear();
    }
};

struct FakeTerrain final : ISeaportTerrainApi {
    int refs = 0;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    int width = 0, height = 0;
    std::set<std::pair<int, int>> water;
    struct AltitudeCall {
        int x = 0, z = 0;
        float altitude = 0;
    };
    std::vector<AltitudeCall> altitudes;
    std::int32_t CellCountX() override { return width; }
    std::int32_t CellCountZ() override { return height; }
    bool CellHasWater(std::uint32_t x, std::uint32_t z) override {
        return water.count({static_cast<int>(x), static_cast<int>(z)}) != 0;
    }
    void SetAltitudeAtVertex(std::int32_t x, std::int32_t z, float altitude) override {
        altitudes.push_back({x, z, altitude});
    }
};

struct FakeHost final : ISeaportHost {
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
    FakeTraffic* traffic = nullptr;
    FakeTerrain* terrain = nullptr;
    FakeBudgetSim* budgetSim = nullptr;
    FakeOccupantManager* occupantManager = nullptr;

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

    ISeaportCityApi* SeaportCity() override { return city; }
    ISeaportBudgetSimApi* BudgetSimulator() override { return budgetSim; }
    ISeaportTrafficApi* TrafficSimulator() override { return traffic; }
    ISeaportTerrainApi* Terrain() override { return terrain; }
    ISeaportOccupantManagerApi* OccupantManager() override { return occupantManager; }
    ISeaportPropOccupantApi* AsPropOccupant(cISC4Occupant* occupant) override {
        void* out = nullptr;
        if (!occupant->QueryInterface(kGZIID_cISC4PropOccupant, &out)) return nullptr;
        return static_cast<ISeaportPropOccupantApi*>(out);
    }
};

//----------------------------------------------------------------------------------------------- fixture

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
    static void Floats(cSCPropertyHolder* holder, std::uint32_t id,
                       std::initializer_list<float> values) {
        std::vector<float> data(values);
        cRZVariant variant;
        variant.RefFloat32(data.data(), static_cast<std::uint32_t>(data.size()));
        holder->AddProperty(id, &variant, true);
    }
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
    FakeTraffic traffic;
    FakeTerrain terrain;
    FakeBudgetSim budgetSim;
    FakeOccupantManager occupantManager;
    FakeHost host;
    Exemplars exemplars;
    std::vector<std::unique_ptr<FakeLotConfiguration>> configurations;

    cSC4SeaportDeveloper* developer = nullptr;

    Fixture() {
        city.simulator = &simulator;
        city.advisorSystem = &advisorSystem;
        city.traffic = &traffic;
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
        host.traffic = &traffic;
        host.terrain = &terrain;
        host.budgetSim = &budgetSim;
        host.occupantManager = &occupantManager;
        SetPortDeveloperHost(&host);
        SetSeaportHost(&host);
    }
    ~Fixture() {
        if (developer) developer->Shutdown();   // frees the port lots and type configurations Init/Read created
        SetSeaportHost(nullptr);
        SetPortDeveloperHost(nullptr);
        delete developer;
        for (auto& entry : lotManager.lots) {   // the lots this fixture built hold the test's and the manager's reference
            entry.second->Release();
            entry.second->Release();
        }
    }

    // The developer exemplar (instance = the seaport developer id) lists one port type; that type has two
    // stages and a 2x2 per-vertex height grid.
    void BuildExemplars(bool withGrid = true) {
        cSCPropertyHolder* list = exemplars.Make();
        Exemplars::Uints(list, kPropPortTypes, {0x111});
        resourceManager.exemplars[kSeaportPortDeveloperId] = list;

        cSCPropertyHolder* type1 = exemplars.Make();
        Exemplars::Uint(type1, kPropStageCount, 2);
        Exemplars::Uints(type1, kPropLotConfigs, {0x1000, 0x1001});
        Exemplars::Int64s(type1, kPropBudgets, {100, 150});
        Exemplars::Uints(type1, kPropCapacities, {200, 400});
        Exemplars::Int64s(type1, kPropCosts, {1000, 2000});
        Exemplars::Float(type1, kPropThreshold, 50.0f);   // = 0.5
        Exemplars::Uint8(type1, kPropSizeClass, 1);
        if (withGrid) {
            Exemplars::Uint8(type1, kPropRows, 2);
            Exemplars::Uint8(type1, kPropColumns, 2);
            Exemplars::Floats(type1, kPropGrid, {1.0f, 2.0f, 3.0f, 4.0f});
        }
        resourceManager.exemplars[0x111] = type1;
    }

    FakeLotConfiguration* AddConfiguration(std::uint32_t id, int growthStage = 1) {
        auto config = std::make_unique<FakeLotConfiguration>();
        config->id = id;
        config->growthStage = growthStage;
        FakeLotConfiguration* raw = config.get();
        configurations.push_back(std::move(config));
        lotConfigurationManager.configurations[id] = raw;
        return raw;
    }

    cSC4SeaportDeveloper* MakeDeveloper() {
        developer = new cSC4SeaportDeveloper();
        return developer;
    }

    // Places a lot whose configuration is `configurationId` and runs the base's DoLotInserted.
    cPortLot* InsertLot(int x, int z, std::uint32_t configurationId, int sizeX = 1, int sizeZ = 1,
                        int facing = 0) {
        auto* lot = new cSC4Lot(x, z, sizeX, sizeZ, facing);
        lot->AddRef();
        lot->AddRef();
        FakeLotConfiguration* config = AddConfiguration(configurationId, 1);
        lot->SetLotConfiguration(config);
        lotManager.lots[{x, z}] = lot;
        developer->DoLotInserted(lot);
        return developer->PortLotCount() == 0 ? nullptr : developer->PortLots().front();
    }

    // The department seam needs direct access to the line item the seaport created.
    FakeLineItem* SeaportLineItem() {
        auto it = budgetSim.departments.find(kSeaportDepartmentId);
        if (it == budgetSim.departments.end()) return nullptr;
        auto item = it->second->lineItems.find(kSeaportLineItemId);
        return item == it->second->lineItems.end() ? nullptr : item->second;
    }
};

// A subclass only to observe the destruction the base view's Release triggers.
int g_destroyed = 0;
struct CountingDeveloper final : cSC4SeaportDeveloper {
    ~CountingDeveloper() override { ++g_destroyed; }
};

//----------------------------------------------------------------------------------------------- tests

void TestLayoutAndIdentity() {
    CHECK(sizeof(cSC4SeaportDeveloper) == 0xe0);
    CHECK(cSC4SeaportDeveloper::kClassSize == 0xe0);
    CHECK(cSC4SeaportDeveloper::kOffsetWaterAccessCheckPending == 0xb8);
    CHECK(cSC4SeaportDeveloper::kOffsetForceWaterMapRebuild == 0xb9);
    CHECK(cSC4SeaportDeveloper::kOffsetCityHasSea == 0xba);
    CHECK(cSC4SeaportDeveloper::kOffsetWaterMap == 0xc0);
    CHECK(cSC4SeaportDeveloper::kOffsetWaterMapWidth == 0xd8);
    CHECK(cSC4SeaportDeveloper::kOffsetWaterMapHeight == 0xdc);
    CHECK(cSC4LotPropFilter::kOriginalClassSize == 0x28);

    cSC4SeaportDeveloper developer;
    // The identity the constructor writes (the same values the airport/seaport factories use).
    CHECK(developer.PortDeveloperId() == kSeaportPortDeveloperId);
    CHECK(developer.PortDeveloperId() == 0xea29ad92u);
    CHECK(developer.ZoneType() == kSeaportZoneType);
    CHECK(developer.ZoneType() == 12);
    CHECK(developer.HistoryCategory() == kSeaportHistoryCategory);
    CHECK(developer.AdvisorEventId() == kSeaportAdvisorEvent);
    CHECK(developer.BuildingGroup() == kSeaportBuildingGroup);
    CHECK(developer.BuildingGroup() == 0x1509u);
    CHECK(developer.GetSimulatorType() == 0xea3957adu);
    CHECK(developer.GetGZCLSID() == 0xea230a28u);
    CHECK(developer.GetNumSeaports() == 0);
    CHECK(!developer.CityHasSea());
    CHECK(developer.GetCellSeaAccess(0, 0) == 0);
    CHECK(developer.WaterMap().empty());
    CHECK(!developer.WaterAccessCheckPending());
    CHECK(!developer.ForceWaterMapRebuild());
    CHECK(!developer.IsInitialized());
    CHECK(developer.RefCount() == 0);

    // The base subobject sits at +0x08: AsISC4PortDeveloper returns it.
    cISC4PortDeveloper* portView = developer.AsISC4PortDeveloper();
    CHECK(portView != nullptr);
    CHECK(static_cast<cSC4PortDeveloper*>(&developer) ==
          static_cast<cSC4PortDeveloper*>(dynamic_cast<cSC4PortDeveloper*>(portView)));
    CHECK(reinterpret_cast<char*>(portView) - reinterpret_cast<char*>(&developer) == 0x08);
}

void TestCom() {
    Fixture f;
    auto* d = f.MakeDeveloper();
    CHECK(d->RefCount() == 0);
    CHECK(d->AddRef() == 1);
    CHECK(d->AddRef() == 2);

    cISC4SeaportDeveloper* seaportView = d;
    cISC4PortDeveloper* portView = d->AsISC4PortDeveloper();
    cIGZMessageTarget2* targetView = d;
    cIGZSerializable* serializableView = d;

    void* out = nullptr;
    CHECK(seaportView->QueryInterface(kSeaportDeveloperIID, &out));
    CHECK(out == static_cast<cISC4SeaportDeveloper*>(d));
    CHECK(seaportView->QueryInterface(1, &out));
    CHECK(out == portView);                                     // the +0x08 view
    CHECK(seaportView->QueryInterface(kPortGZIIDPortDeveloper, &out));
    CHECK(out == portView);
    CHECK(targetView->QueryInterface(kSeaportDeveloperIID, &out));
    CHECK(targetView->QueryInterface(0x452294aa, &out));
    CHECK(out == static_cast<cIGZMessageTarget2*>(d));
    CHECK(serializableView->QueryInterface(0xe4fda3d4, &out));
    CHECK(out == static_cast<cIGZSerializable*>(d));
    CHECK(!seaportView->QueryInterface(0xdeadbeef, &out));
    CHECK(!seaportView->QueryInterface(kSeaportDeveloperIID, nullptr));
    CHECK(d->RefCount() == 8);   // two manual + six successful queries

    // The seaport view answers the interface directly.
    CHECK(seaportView->GetSimulatorType() == kSeaportSimulatorType);
    CHECK(seaportView->AsISC4PortDeveloper() == portView);
    CHECK(seaportView->CityHasSea() == false);
    CHECK(seaportView->GetNumSeaports() == 0);
    CHECK(serializableView->GetGZCLSID() == kSeaportDeveloperClassId);
    CHECK(d->GetGZCLSID() == kSeaportDeveloperClassId);

    // The count is shared by all four views and Release deletes below 2.
    CHECK(portView->AddRef() == 9);
    CHECK(targetView->AddRef() == 10);
    CHECK(serializableView->Release() == 9);
    CHECK(targetView->Release() == 8);
    f.developer = nullptr;
    delete d;   // the fixture/stack objects must not Release to zero (FinalRelease deletes)
}

void TestInitShutdown() {
    Fixture f;
    f.BuildExemplars();
    auto* d = f.MakeDeveloper();
    d->AddRef();
    CHECK(d->Init());
    CHECK(d->IsInitialized());

    // The base's side: one simulator agent.
    CHECK(f.simulator.added.size() == 1);
    // The seaport department: created, named 0x6a3ff01c / 0xca493af3, 120% max funding, one hidden line item.
    CHECK(f.budgetSim.createCalls == 1);
    auto it = f.budgetSim.departments.find(kSeaportDepartmentId);
    CHECK(it != f.budgetSim.departments.end());
    FakeDepartment* department = it->second;
    CHECK(department->id == 0x4a4155abu);
    CHECK(department->group == 0xaa369059u);
    CHECK(department->nameCalls == 1);
    CHECK(department->nameGroup == 0x6a3ff01cu);
    CHECK(department->nameInstance == 0xca493af3u);
    CHECK(department->maxFundingCalls == 1);
    CHECK(Near(department->maxFunding, 1.2f));
    FakeLineItem* lineItem = f.SeaportLineItem();
    CHECK(lineItem != nullptr);
    CHECK(lineItem->displayFlagCalls == 1);
    CHECK(!lineItem->displayFlag1);
    CHECK(department->refs == 1);   // Init's AddRef

    // The base's three notifications and the seaport's two.
    auto registered = [&](std::uint32_t type) {
        return std::count_if(f.messages.added.begin(), f.messages.added.end(),
                             [&](const auto& entry) { return entry.second == type; });
    };
    CHECK(registered(kMessageLotStateChanged) == 1);
    CHECK(registered(kMessagePortPostCityInit) == 1);
    CHECK(registered(kMessagePortBuildingPlaced) == 1);
    CHECK(registered(kSeaportMessageGrowPorts) == 1);
    CHECK(registered(kSeaportMessageWaterMapDirty) == 1);

    // A second Init is a no-op (the object was already initialised).
    CHECK(d->Init());
    CHECK(f.budgetSim.createCalls == 1);
    CHECK(department->refs == 1);
    CHECK(f.messages.added.size() == 5);

    // Shutdown removes the two seaport messages, then the base removes its three and its services, then the
    // department reference is dropped.
    CHECK(d->Shutdown());
    CHECK(!d->IsInitialized());
    CHECK(f.messages.removed.size() == 5);
    CHECK(f.messages.removed[0].second == kSeaportMessageGrowPorts);
    CHECK(f.messages.removed[1].second == kSeaportMessageWaterMapDirty);
    CHECK(department->refs == 0);
    CHECK(f.simulator.removed.size() == 1);
    CHECK(f.messages.refs == 0);            // the base released the message server
    CHECK(f.budget.refs == 0);
    CHECK(f.lotManager.refs == 0);
    CHECK(f.zoneManager.refs == 0);
    CHECK(f.city.refs == 0);

    // Shutdown of a non-initialised object does nothing.
    CHECK(d->Shutdown());
    CHECK(f.messages.removed.size() == 5);
}

void TestReleaseDestroys() {
    // Release below a count of 2 runs FinalRelease (Shutdown, then the deleting destructor), also when it is
    // called through the cISC4PortDeveloper view at +0x08.
    Fixture f;
    f.BuildExemplars();
    f.terrain.width = 2;
    f.terrain.height = 2;
    g_destroyed = 0;
    auto* d = new CountingDeveloper();
    CHECK(d->AddRef() == 1);
    CHECK(d->Init());
    FakeDepartment* department = f.budgetSim.departments[kSeaportDepartmentId];
    CHECK(department->refs == 1);
    cISC4PortDeveloper* portView = d->AsISC4PortDeveloper();
    CHECK(portView->AddRef() == 2);
    CHECK(portView->Release() == 1);
    CHECK(g_destroyed == 0);
    CHECK(portView->Release() == 0);   // Shutdown + delete through the +0x08 view
    CHECK(g_destroyed == 1);
    CHECK(department->refs == 0);
    CHECK(f.messages.removed.size() == 5);
}

void TestInitExistingDepartment() {
    Fixture f;
    f.BuildExemplars();
    // Pre-create the department: Init must reuse it and not touch its name / funding.
    FakeDepartment* existing =
        static_cast<FakeDepartment*>(f.budgetSim.CreateDepartmentBudget(kSeaportDepartmentId, 0));
    f.budgetSim.createCalls = 0;
    auto* d = f.MakeDeveloper();
    d->AddRef();
    CHECK(d->Init());
    CHECK(f.budgetSim.createCalls == 0);
    CHECK(existing->nameCalls == 0);
    CHECK(existing->maxFundingCalls == 0);
    CHECK(existing->refs == 1);
    d->Shutdown();
    CHECK(existing->refs == 0);
}

void TestNoTerrainOrBudget() {
    Fixture f;
    f.BuildExemplars();
    f.host.terrain = nullptr;   // BuildWaterAccessMap leaves the map alone
    f.host.budgetSim = nullptr;
    auto* d = f.MakeDeveloper();
    d->AddRef();
    CHECK(d->Init());           // the seaport still initialises
    CHECK(d->WaterMap().empty());
    CHECK(!d->CityHasSea());
    CHECK(!d->GetAllocatedFunding());     // no line item -> 0
    d->UpdateBudget();                    // no line item -> no-op
    CHECK(d->GetMaintenanceCost(nullptr) == 0);
}

void TestWaterMap() {
    Fixture f;
    f.BuildExemplars();
    f.terrain.width = 4;
    f.terrain.height = 3;
    auto* d = f.MakeDeveloper();
    d->AddRef();
    CHECK(d->Init());   // Init builds the map
    CHECK(d->WaterMapWidth() == 4);
    CHECK(d->WaterMapHeight() == 3);
    CHECK(d->WaterMap().size() == 12);
    CHECK(!d->CityHasSea());   // no water at all

    // Water in the middle (inside the map, not touching a border) keeps CityHasSea false.
    f.terrain.water = {{1, 1}};
    d->BuildWaterAccessMap();
    CHECK(!d->CityHasSea());
    CHECK(d->GetCellSeaAccess(1, 1) == kSeaportWaterBit);
    CHECK(d->GetCellSeaAccess(0, 0) == 0);

    // A water region on the north-west corner carries the marks of both borders it reaches: the whole region
    // gets 1 | 2, not only the cells that sit on a border.
    f.terrain.water = {{0, 0}, {1, 0}, {1, 1}};
    d->BuildWaterAccessMap();
    CHECK(d->CityHasSea());
    CHECK(d->GetCellSeaAccess(0, 0) == (kSeaportWaterBit | kSeaportBorderNorth | kSeaportBorderWest));
    CHECK(d->GetCellSeaAccess(1, 0) == (kSeaportWaterBit | kSeaportBorderNorth | kSeaportBorderWest));
    CHECK(d->GetCellSeaAccess(1, 1) == (kSeaportWaterBit | kSeaportBorderNorth | kSeaportBorderWest));
    CHECK(d->GetCellSeaAccess(2, 2) == 0);

    // Two separate regions reach different borders: the north-west corner gets 1 and 2, the south-east
    // corner gets 8 and 4.
    f.terrain.water = {{0, 0}, {3, 2}};
    d->BuildWaterAccessMap();
    CHECK(d->GetCellSeaAccess(0, 0) == (kSeaportWaterBit | 1 | 2));
    CHECK(d->GetCellSeaAccess(3, 2) == (kSeaportWaterBit | 8 | 4));

    // One ring shaped region touching every border: every cell of the region carries all four bits.
    f.terrain.water = {{0, 0}, {1, 0}, {2, 0}, {3, 0}, {0, 1}, {3, 1}, {0, 2}, {1, 2}, {2, 2}, {3, 2}};
    d->BuildWaterAccessMap();
    CHECK(d->GetCellSeaAccess(0, 0) == (kSeaportWaterBit | 1 | 2 | 4 | 8));
    CHECK(d->GetCellSeaAccess(3, 2) == (kSeaportWaterBit | 1 | 2 | 4 | 8));
    CHECK(d->GetCellSeaAccess(3, 0) == (kSeaportWaterBit | 1 | 2 | 4 | 8));
    CHECK(d->GetCellSeaAccess(0, 2) == (kSeaportWaterBit | 1 | 2 | 4 | 8));
    CHECK(d->GetCellSeaAccess(1, 0) == (kSeaportWaterBit | 1 | 2 | 4 | 8));

    // Out of range reads are clamped to 0 (the original reads past the vector).
    CHECK(d->GetCellSeaAccess(4, 0) == 0);
    CHECK(d->GetCellSeaAccess(0, 3) == 0);
    CHECK(d->GetCellSeaAccess(0xffffffffu, 0) == 0);

    // The rebuild flag is cleared by a build, and BuildWaterAccessMap always resizes to the terrain.
    d->SetForceWaterMapRebuild(true);
    f.terrain.width = 2;
    f.terrain.height = 2;
    f.terrain.water = {};
    d->BuildWaterAccessMap();
    CHECK(!d->ForceWaterMapRebuild());
    CHECK(d->WaterMap().size() == 4);
    CHECK(!d->CityHasSea());
}

void TestTripsAndCapacity() {
    Fixture f;
    f.BuildExemplars();
    f.terrain.width = 4;
    f.terrain.height = 4;
    auto* d = f.MakeDeveloper();
    d->AddRef();
    CHECK(d->Init());
    CHECK(d->PortLotCount() == 0);

    cPortLot* port = f.InsertLot(1, 1, 0x1000);
    CHECK(port != nullptr);
    CHECK(d->PortLotCount() == 1);
    CHECK(d->GetNumSeaports() == 1);

    // DoLotInserted ran UpdateBudget (required budget = stage 0's budget = 100) and OnNewLotCreated.
    FakeLineItem* lineItem = f.SeaportLineItem();
    CHECK(lineItem != nullptr);
    CHECK(lineItem->fullExpenses == 100);
    // OnNewLotCreated fed the traffic simulator the stage capacity (200) scaled by the funding ratio; the
    // line item has no expenses yet, so the ratio is 0 / 100 = 0 -> capacity 0.
    CHECK(!f.traffic.capacities.empty());
    CHECK(f.traffic.capacities.back().travelType == 3);
    CHECK(f.traffic.capacities.back().capacity == 0);
    CHECK(f.traffic.capacities.back().source == static_cast<cISCPropertyHolder*>(&port->GetLot()->Properties()));

    // Fully funded: capacity 200.
    lineItem->currentExpenses = 100;
    d->UpdateTripCapacity(*port);
    CHECK(f.traffic.capacities.back().capacity == 200);
    // Half funded: 100.
    lineItem->currentExpenses = 50;
    d->UpdateTripCapacity(*port);
    CHECK(f.traffic.capacities.back().capacity == 100);
    // Over funded: 200 * 1.5 = 300.
    lineItem->currentExpenses = 150;
    d->UpdateTripCapacity(*port);
    CHECK(f.traffic.capacities.back().capacity == 300);

    // OnStageChange forwards to UpdateTripCapacity.
    const std::size_t capacityCalls = f.traffic.capacities.size();
    d->OnStageChange(*port);
    CHECK(f.traffic.capacities.size() == capacityCalls + 1);

    // The stage capacity follows the port's stage.
    port->SetStage(1);
    lineItem->currentExpenses = 100;
    d->UpdateTripCapacity(*port);
    CHECK(f.traffic.capacities.back().capacity == 400);

    // GetTripsForPort goes through the city's traffic simulator, AddRef/Release around the query.
    f.traffic.trips = 77;
    const int refsBefore = f.traffic.refs;
    CHECK(d->GetTripsForPort(*port) == 77);
    CHECK(f.traffic.refs == refsBefore);
    CHECK(f.traffic.arrived.back().travelType == 3);
    CHECK(f.traffic.arrived.back().source == static_cast<cISCPropertyHolder*>(&port->GetLot()->Properties()));
    // GetNumSeaportTrips sums the virtual GetTripsForPort over the port list.
    CHECK(d->GetNumSeaportTrips() == 77);

    // The seaport usage values: the base's trips/capacity, capacity scaled by the funding ratio and the
    // usage capped at it. The base recomputes both values, so only the traffic figure drives the usage.
    port->SetStage(0);
    lineItem->fullExpenses = 100;
    lineItem->currentExpenses = 50;   // half funding -> capacity 200 * 0.5 = 100
    std::uint32_t usage = 0;
    std::uint32_t capacity = 0;
    f.traffic.trips = 150;
    CHECK(d->GetSeaportUsageValues(port->GetLot(), usage, capacity));
    CHECK(capacity == 100);
    CHECK(usage == 100);   // 150 trips capped at the funded capacity
    f.traffic.trips = 40;
    CHECK(d->GetSeaportUsageValues(port->GetLot(), usage, capacity));
    CHECK(capacity == 100);
    CHECK(usage == 40);
    CHECK(!d->GetSeaportUsageValues(nullptr, usage, capacity));

    // GetMaintenanceCost reads the stage *budget* (100), scaled by the same ratio.
    CHECK(d->GetMaintenanceCost(port->GetLot()) == 50);
    lineItem->currentExpenses = 100;
    CHECK(d->GetMaintenanceCost(port->GetLot()) == 100);
    port->SetStage(1);
    CHECK(d->GetMaintenanceCost(port->GetLot()) == 150);   // stage 1's budget
    CHECK(d->GetMaintenanceCost(nullptr) == 0);
    port->SetStage(0);

    // GetAllocatedFunding is the line item's current expenses.
    lineItem->currentExpenses = 1234;
    CHECK(d->GetAllocatedFunding() == 1234);

    // A second port participates in the sums; UpdateBudget then writes both ports' stage budgets.
    cPortLot* second = f.InsertLot(2, 2, 0x1000);
    CHECK(second != nullptr);
    f.traffic.trips = 10;
    CHECK(d->GetNumSeaportTrips() == 20);
    d->UpdateBudget();
    CHECK(lineItem->fullExpenses == 200);   // 100 + 100
}

void TestMonthlyTick() {
    Fixture f;
    f.BuildExemplars();
    f.terrain.width = 4;
    f.terrain.height = 4;
    f.terrain.water = {{0, 0}, {0, 1}};   // the north-west column, so the port at (0, 1) sits on water
    auto* d = f.MakeDeveloper();
    d->AddRef();
    CHECK(d->Init());
    CHECK(d->CityHasSea());

    // A port on open water: the water region's marks reach it.
    cPortLot* wet = f.InsertLot(0, 1, 0x1000);
    CHECK(wet != nullptr);
    CHECK(d->WaterAccessCheckPending());   // OnNewLotCreated set it
    f.advisorSystem.events.clear();
    d->DoMonthlyTick();
    CHECK(!d->WaterAccessCheckPending());
    CHECK(f.advisorSystem.events.empty());   // port (0,1) is adjacent to the water at (0,0)
    CHECK(!f.traffic.capacities.empty());    // the trip capacity of every port was refreshed

    // A port far from the water has no sea access: the monthly check flags the props and raises the advisor
    // event once.
    cPortLot* dry = f.InsertLot(3, 3, 0x1000);
    CHECK(dry != nullptr);
    FakePropOccupant prop;
    FakeOccupant occupant;
    occupant.type = 0x278128a0;   // not a prop: the filter drops it
    occupant.position[0] = (3.5f) * 16.0f;
    occupant.position[2] = (3.5f) * 16.0f;
    FakeOccupant propOccupant;
    propOccupant.type = 0x278128a0;
    propOccupant.prop = &prop;
    propOccupant.position[0] = occupant.position[0];
    propOccupant.position[2] = occupant.position[2];
    f.occupantManager.occupants = {&occupant, &propOccupant};
    d->SetWaterAccessCheckPending(true);
    f.advisorSystem.events.clear();
    f.traffic.capacities.clear();
    d->DoMonthlyTick();
    // The two ports' capacities were refreshed before the check.
    CHECK(f.traffic.capacities.size() == 2);
    // The prop query: cell rectangle {3,3}..{3,3}, filter box from both cell centres to the next cell centres.
    CHECK(f.occupantManager.queries.size() == 1);
    CHECK(f.occupantManager.queries.back().low[0] == 3);
    CHECK(f.occupantManager.queries.back().low[1] == 3);
    CHECK(f.occupantManager.queries.back().high[0] == 3);
    CHECK(f.occupantManager.queries.back().high[1] == 3);
    CHECK(f.occupantManager.queries.back().maxCount == 0xffffffffu);
    CHECK(f.occupantManager.queries.back().filter != nullptr);
    // Only the occupant whose type is the prop type is returned by the filter, so only it is put into state
    // 1 (the type check happens in the filter before the prop view is asked for).
    CHECK(f.occupantManager.lastReturned == 0);   // both fake occupants are building-occupant typed
    CHECK(prop.setCalls == 0);
    CHECK(f.advisorSystem.events.size() == 1);
    CHECK(f.advisorSystem.events[0].first == static_cast<int>(kSeaportAdvisorNoSeaAccess));

    // With a prop typed occupant carrying the property, the prop state is set and the release list is used.
    f.occupantManager.occupants.clear();
    FakeOccupant realProp;
    realProp.type = kOccupantTypeProp;
    realProp.prop = &prop;
    realProp.position[0] = (3.5f) * 16.0f;
    realProp.position[2] = (3.5f) * 16.0f;
    cSCPropertyHolder holder;
    holder.AddProperty(kSeaportWaterAccessProperty, 7u, true);
    realProp.holder = &holder;
    f.occupantManager.occupants = {&realProp};
    d->SetWaterAccessCheckPending(true);
    f.occupantManager.queries.clear();
    f.advisorSystem.events.clear();
    d->DoMonthlyTick();
    CHECK(f.occupantManager.queries.size() == 1);
    CHECK(f.occupantManager.lastReturned == 1);
    CHECK(prop.setCalls == 1);
    CHECK(prop.state == 1);
    CHECK(f.occupantManager.releaseCalls >= 1);
    CHECK(f.advisorSystem.events.size() == 1);

    // Without pending work the tick only refreshes the capacities and does not query the occupants.
    f.occupantManager.queries.clear();
    f.advisorSystem.events.clear();
    d->DoMonthlyTick();
    CHECK(f.occupantManager.queries.empty());
    CHECK(f.advisorSystem.events.empty());

    // The forced rebuild runs BuildWaterAccessMap first (the flag clears).
    d->SetForceWaterMapRebuild(true);
    d->DoMonthlyTick();
    CHECK(!d->ForceWaterMapRebuild());
}

void TestOnNewLotCreatedTerrain() {
    // The exemplar's height grid is 2x2, so only a lot whose interior is 2x2 (a 3x3 lot) matches. Each facing
    // is checked with a fresh fixture because the port list accumulates.
    const float expected[4][4] = {
        {1.0f, 3.0f, 2.0f, 4.0f},   // facing 0: grid[x - x0][z - z0]
        {4.0f, 3.0f, 2.0f, 1.0f},   // facing 1: grid[z1 - 1 - z][x1 - 1 - x]
        {4.0f, 2.0f, 3.0f, 1.0f},   // facing 2: grid[x1 - 1 - x][z1 - 1 - z]
        {1.0f, 2.0f, 3.0f, 4.0f},   // facing 3: grid[z - z0][x - x0]
    };
    for (int facing = 0; facing < 4; ++facing) {
        Fixture f;
        f.BuildExemplars();
        f.terrain.width = 8;
        f.terrain.height = 8;
        auto* d = f.MakeDeveloper();
        d->AddRef();
        CHECK(d->Init());

        auto* lot = new cSC4Lot(2, 2, 3, 3, facing);
        lot->AddRef();
        lot->AddRef();
        lot->SetLotConfiguration(f.AddConfiguration(0x1000, 1));
        f.lotManager.lots[{2, 2}] = lot;
        d->DoLotInserted(lot);
        CHECK(d->PortLotCount() == 1);
        CHECK(d->WaterAccessCheckPending());   // OnNewLotCreated set the flag

        // Four cells -> four SetAltitudeAtVertex calls; every cell was converted once through the city.
        CHECK(f.terrain.altitudes.size() == 4);
        CHECK(f.city.cellCalls.size() == 4);
        // The cell centre of (2, 2) is world (40, 40); the truncated world position is the vertex index (the
        // original's quirk - 16 * cell + 8 instead of the cell index).
        CHECK(f.terrain.altitudes[0].x == 40);
        CHECK(f.terrain.altitudes[0].z == 40);
        CHECK(f.terrain.altitudes[1].x == 56);   // x = 3 -> (3.5 * 16) = 56
        for (int i = 0; i < 4; ++i) {
            CHECK(Near(f.terrain.altitudes[static_cast<std::size_t>(i)].altitude, expected[facing][i]));
        }
        // The terrain's AddRef / Release are balanced.
        CHECK(f.terrain.refs == 0);
    }

    // A 2x2 lot does not match the 2x2 grid ((2 - 1) * (2 - 1) = 1 != 4): the terrain is still AddRef'd but
    // no vertex is written.
    Fixture f;
    f.BuildExemplars();
    f.terrain.width = 8;
    f.terrain.height = 8;
    auto* d = f.MakeDeveloper();
    d->AddRef();
    CHECK(d->Init());
    auto* small = new cSC4Lot(5, 5, 2, 2, 0);
    small->AddRef();
    small->AddRef();
    small->SetLotConfiguration(f.AddConfiguration(0x1000, 1));
    f.lotManager.lots[{5, 5}] = small;
    d->DoLotInserted(small);
    CHECK(f.terrain.altitudes.empty());
    CHECK(f.terrain.refs == 0);

    // A port type without a height grid never touches the terrain at all.
    Fixture noGrid;
    noGrid.BuildExemplars(false);
    noGrid.terrain.width = 8;
    noGrid.terrain.height = 8;
    auto* g = noGrid.MakeDeveloper();
    g->AddRef();
    CHECK(g->Init());
    CHECK(g->Configurations().size() == 1);
    CHECK(g->Configurations().front()->Grid().empty());
    auto* gridLot = new cSC4Lot(2, 2, 3, 3, 0);
    gridLot->AddRef();
    gridLot->AddRef();
    gridLot->SetLotConfiguration(noGrid.AddConfiguration(0x1000, 1));
    noGrid.lotManager.lots[{2, 2}] = gridLot;
    g->DoLotInserted(gridLot);
    CHECK(noGrid.terrain.altitudes.empty());
    CHECK(noGrid.terrain.refs == 0);
}

void TestMessages() {
    Fixture f;
    f.BuildExemplars();
    f.terrain.width = 4;
    f.terrain.height = 4;
    auto* d = f.MakeDeveloper();
    d->AddRef();
    CHECK(d->Init());
    f.terrain.water = {{2, 2}};

    cRZMessage2Standard message;
    // Message 0x6a3b0afd rebuilds the water map and marks the monthly check pending.
    d->SetWaterAccessCheckPending(false);
    message.SetType(kSeaportMessageWaterMapDirty);
    CHECK(d->DoMessage(&message));
    CHECK(d->WaterAccessCheckPending());
    CHECK(f.terrain.water.size() == 1);
    CHECK(d->WaterMap().size() == 16);

    // Message 0x8a394dc4 grows the ports (needs a triggered port, which needs a traffic figure); with no
    // ports it is a harmless no-op.
    message.SetType(kSeaportMessageGrowPorts);
    CHECK(d->DoMessage(&message));

    // Everything else falls through to the base: the monthly tick calls our DoMonthlyTick.
    const int refsBefore = f.traffic.refs;
    message.SetType(kMessagePortMonthly);
    CHECK(d->DoMessage(&message));
    CHECK(d->GetAllocatedFunding() == 0);
    CHECK(f.traffic.refs == refsBefore);

    // An unknown message type and a null message are ignored.
    message.SetType(0x12345678);
    CHECK(d->DoMessage(&message));
    CHECK(d->DoMessage(nullptr));
}

void TestStream() {
    Fixture f;
    f.BuildExemplars();
    f.terrain.width = 2;
    f.terrain.height = 2;
    auto* d = f.MakeDeveloper();
    d->AddRef();
    CHECK(d->Init());
    cPortLot* port = f.InsertLot(1, 1, 0x1000);
    CHECK(port != nullptr);
    port->SetStage(1);

    PortMemoryOStream out;
    CHECK(d->Write(out));
    const std::vector<unsigned char>& bytes = out.bytes();
    // Seaport version 2, then the base's version-1 record: version, developer id, zone type, port count.
    CHECK(bytes.size() == 2 + 2 + 4 + 2 + 2 + 1 * (4 + 2 + 2 + 2));
    CHECK(bytes[0] == 2 && bytes[1] == 0);
    CHECK(bytes[2] == 1 && bytes[3] == 0);
    CHECK(bytes[4] == 0x92 && bytes[5] == 0xad && bytes[6] == 0x29 && bytes[7] == 0xea);
    CHECK(bytes[8] == 12 && bytes[9] == 0);
    CHECK(bytes[10] == 1 && bytes[11] == 0);   // one port

    // Read it back into a fresh developer (a second fixture, so its own hosts are installed).
    Fixture readFixture;
    readFixture.BuildExemplars();
    readFixture.terrain.width = 2;
    readFixture.terrain.height = 2;
    auto* copy = readFixture.MakeDeveloper();
    copy->AddRef();
    CHECK(copy->Init());
    PortMemoryIStream in(bytes.data(), bytes.size());
    CHECK(copy->Read(in));
    CHECK(copy->PortLotCount() == 1);
    CHECK(copy->PortLots().front()->Stage() == 1);
    CHECK(copy->PortDeveloperId() == kSeaportPortDeveloperId);
    CHECK(copy->ZoneType() == kSeaportZoneType);

    // A version other than 2 is rejected before the base's payload (the seaport record's gate).
    PortMemoryOStream bad;
    bad.SetUint16(1);
    bad.SetUint32(kSeaportPortDeveloperId);
    PortMemoryIStream badIn(bad.bytes().data(), bad.bytes().size());
    CHECK(!copy->Read(badIn));
    CHECK(copy->PortLotCount() == 1);   // unchanged
}

void TestLotPropFilter() {
    // Type gate, box and property gate of cSC4LotPropFilter.
    cSC4LotPropFilter filter(10.0f, 20.0f, 30.0f, 40.0f, 0);
    CHECK(Near(filter.MinX(), 10.0f));
    CHECK(Near(filter.MinZ(), 20.0f));
    CHECK(Near(filter.MaxX(), 30.0f));
    CHECK(Near(filter.MaxZ(), 40.0f));
    CHECK(filter.Property() == 0);

    FakeOccupant occupant;
    occupant.type = kOccupantTypeProp;
    occupant.position[0] = 10.0f;
    occupant.position[2] = 20.0f;
    CHECK(filter.IsOccupantIncluded(&occupant));         // the box is inclusive
    occupant.position[0] = 30.0f;
    occupant.position[2] = 40.0f;
    CHECK(filter.IsOccupantIncluded(&occupant));
    occupant.position[0] = 30.5f;
    CHECK(!filter.IsOccupantIncluded(&occupant));
    occupant.position[0] = 20.0f;
    occupant.position[2] = 19.9f;
    CHECK(!filter.IsOccupantIncluded(&occupant));
    occupant.position[2] = 30.0f;
    occupant.type = 0x278128a0;   // buildings are not props
    CHECK(!filter.IsOccupantIncluded(&occupant));
    CHECK(!filter.IsOccupantIncluded(nullptr));

    // The property gate.
    cSC4LotPropFilter withProperty(0.0f, 0.0f, 100.0f, 100.0f, kSeaportWaterAccessProperty);
    occupant.type = kOccupantTypeProp;
    occupant.position[0] = 50.0f;
    occupant.position[2] = 50.0f;
    CHECK(!withProperty.IsOccupantIncluded(&occupant));   // no holder
    cSCPropertyHolder holder;
    occupant.holder = &holder;
    CHECK(!withProperty.IsOccupantIncluded(&occupant));   // holder without the property
    holder.AddProperty(kSeaportWaterAccessProperty, 1u, true);
    CHECK(withProperty.IsOccupantIncluded(&occupant));
    holder.RemoveProperty(kSeaportWaterAccessProperty, true);
    CHECK(!withProperty.IsOccupantIncluded(&occupant));

    // The filter's reference count deletes it at 0 (cSC4OccupantFilter::Release).
    auto* heapFilter = new cSC4LotPropFilter(0.0f, 0.0f, 1.0f, 1.0f, 0);
    CHECK(heapFilter->AddRef() == 1);
    CHECK(heapFilter->Release() == 0);   // deleted here
}

}  // namespace

int main() {
    TestLayoutAndIdentity();
    TestCom();
    TestReleaseDestroys();
    TestInitShutdown();
    TestInitExistingDepartment();
    TestNoTerrainOrBudget();
    TestWaterMap();
    TestTripsAndCapacity();
    TestMonthlyTick();
    TestOnNewLotCreatedTerrain();
    TestMessages();
    TestStream();
    TestLotPropFilter();
    if (g_failures == 0) {
        std::printf("sc4_seaportdeveloper ok\n");
        return 0;
    }
    std::printf("sc4_seaportdeveloper: %d failure(s)\n", g_failures);
    return 1;
}
