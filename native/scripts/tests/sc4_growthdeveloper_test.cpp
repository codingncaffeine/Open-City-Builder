// cSC4GrowthDeveloper — unit test. Drives the recovered COM object through a fake IGrowthDeveloperHost:
// the three views and their shared reference count, Init/Shutdown of every service (and the two message
// registrations plus the two simulator agents), every tunable of the ini exemplar, the six tract maps and
// their instance ids, SetTractSize and the map-update cursor, DoMessage (lot population, maximum capacity,
// day and idle ticks), the capacity-map population spread, the whole twenty-stage map update (growth list,
// decline list, pruning, styles, demand, population map), EstimateDesirability, the construction profile,
// the zone conversion table, the stream round trip and the remaining queries.
//
// The simulator services and their maps are fakes (the growth planner that owns the class is not recovered),
// the lots are real cSC4Lot objects behind a fake ILotHost, and the maps are real cSC4SimGrid<T> objects.

#include "sc4/GrowthDeveloper.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZRandom.h"
#include "sc4/Lot.h"
#include "sc4/SCPropertyHelp.h"

namespace {

int g_failures = 0;

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (false)

using namespace sc4;

//----------------------------------------------------------------------------------------------- grid city
struct FakeSimGridCity final : ISimGridCity {
    std::int32_t cellsX = 8, cellsZ = 8;
    float cellX = 16.0f, cellZ = 16.0f;
    std::int32_t CitySizeXInCells() const override { return cellsX; }
    std::int32_t CitySizeZInCells() const override { return cellsZ; }
    float CellSizeXMeters() const override { return cellX; }
    float CellSizeZMeters() const override { return cellZ; }
};

template <class T>
cSC4SimGrid<T>* NewGrid(int tractSize, std::uint32_t instanceId = 0) {
    auto* grid = new cSC4SimGrid<T>();
    grid->Init();
    grid->AddRef();
    grid->SetTractSize(tractSize);
    if (instanceId) grid->SetInstanceID(instanceId);
    return grid;
}

template <class T>
void DeleteGrid(cSC4SimGrid<T>*& grid) {
    if (!grid) return;
    grid->Release();
    grid = nullptr;
}

#define FAKE_SERVICE_BOILERPLATE                    \
    int refs = 0;                                   \
    int releases = 0;                               \
    void AddRef() override { ++refs; }              \
    void Release() override { ++releases; }         \
    int inits = 0;                                  \
    int shutdowns = 0;                              \
    void Init() override { ++inits; }               \
    void Shutdown() override { ++shutdowns; }

//----------------------------------------------------------------------------------------------- services
struct FakeService : IGrowthDeveloperService {
    FAKE_SERVICE_BOILERPLATE
};

struct FakeCity final : IGrowthCityApi {
    FAKE_SERVICE_BOILERPLATE
    FakeService occupantManager;
    int removeCalls = 0;
    cIGZMessageTarget2* removedView = nullptr;
    int positionCalls = 0;
    float lastPositionX = 0, lastPositionZ = 0;
    int cellX = 0, cellZ = 0;
    IGrowthDeveloperService* OccupantManager() override { return &occupantManager; }
    void RemoveDeveloper(cIGZMessageTarget2* view) override {
        ++removeCalls;
        removedView = view;
    }
    void PositionToCell(float x, float z, int& outX, int& outZ) override {
        ++positionCalls;
        lastPositionX = x;
        lastPositionZ = z;
        outX = cellX;
        outZ = cellZ;
    }
};

struct FakeBuildingDev final : IGrowthBuildingDevelopmentApi {
    FAKE_SERVICE_BOILERPLATE
    BuildingProfile profile;
    std::list<cRZAutoRefCount<cISC4Occupant>> specialBuildings;
    std::vector<std::pair<int, int>> tractUpdates;
    std::vector<std::array<int, 3>> maxCapacityCalls;
    float maxCapacity = 0.0f;
    float stageMultiplier = 1.0f;
    std::uint32_t profileAsked = 0;   // the last building type whose profile was asked for
    const BuildingProfile& GetBuildingProfile(std::uint32_t type) override { profileAsked = type; return profile; }
    void GetSpecialBuildings(std::list<cRZAutoRefCount<cISC4Occupant>>& out) override { out = specialBuildings; }
    float GetMaxCapacity(int zoneType, int purpose, int wealth) override {
        maxCapacityCalls.push_back({zoneType, purpose, wealth});
        return maxCapacity;
    }
    float GetStagePopulationMultiplier() override { return stageMultiplier; }
    void UpdateTractPopulation(cISC4GrowthDeveloper*, int x, int z) override { tractUpdates.push_back({x, z}); }
};

struct FakeLandValue final : IGrowthLandValueApi {
    FAKE_SERVICE_BOILERPLATE
    cSC4SimGrid<std::uint8_t>* intrinsic = nullptr;
    cSC4SimGrid<std::uint8_t>* total = nullptr;
    cSC4SimGrid<float>* slope = nullptr;
    ~FakeLandValue() override {
        DeleteGrid(intrinsic);
        DeleteGrid(total);
        DeleteGrid(slope);
    }
    cSC4SimGrid<std::uint8_t>* TotalLandValueMap() override { return total; }
    cSC4SimGrid<std::uint8_t>* IntrinsicLandValueMap() override { return intrinsic; }
    cSC4SimGrid<float>* SlopeMap() override { return slope; }
};

struct FakeZoneManager final : IGrowthZoneManagerApi {
    FAKE_SERVICE_BOILERPLATE
    std::function<int(int, int)> zoneAt;
    int ZoneTypeAt(int x, int z) override { return zoneAt ? zoneAt(x, z) : 0; }
};

struct FakeResidential final : IGrowthResidentialApi {
    FAKE_SERVICE_BOILERPLATE
    cSC4SimGrid<std::uint8_t>* wealth[4] = {nullptr, nullptr, nullptr, nullptr};
    cSC4SimGrid<float>* map138 = nullptr;
    cSC4SimGrid<float>* map140 = nullptr;
    ~FakeResidential() override {
        for (auto*& grid : wealth) DeleteGrid(grid);
        DeleteGrid(map138);
        DeleteGrid(map140);
    }
    cSC4SimGrid<std::uint8_t>* WealthMap(int index) override {
        return index >= 1 && index <= 3 ? wealth[index] : nullptr;
    }
    cSC4SimGrid<float>* Map138() override { return map138; }
    cSC4SimGrid<float>* Map140() override { return map140; }
};

struct FakeCommercial final : IGrowthCommercialApi {
    FAKE_SERVICE_BOILERPLATE
    std::map<std::uint32_t, cSC4SimGrid<std::uint8_t>*> maps;
    ~FakeCommercial() override {
        for (auto& entry : maps) DeleteGrid(entry.second);
    }
    cSC4SimGrid<std::uint8_t>* DeveloperMap(std::uint32_t type) override {
        auto it = maps.find(type);
        return it == maps.end() ? nullptr : it->second;
    }
};

struct FakePollution final : IGrowthPollutionApi {
    FAKE_SERVICE_BOILERPLATE
    cSC4SimGrid<std::int16_t>* maps[4] = {nullptr, nullptr, nullptr, nullptr};
    ~FakePollution() override {
        for (auto*& grid : maps) DeleteGrid(grid);
    }
    cSC4SimGrid<std::int16_t>* TypeMap(int type) override {
        return type >= 0 && type < 4 ? maps[type] : nullptr;
    }
};

struct FakeAura final : IGrowthAuraApi {
    FAKE_SERVICE_BOILERPLATE
    cSC4SimGrid<std::int8_t>* map68 = nullptr;
    cSC4SimGrid<std::int16_t>* map70 = nullptr;
    cSC4SimGrid<std::int16_t>* map78 = nullptr;
    ~FakeAura() override {
        DeleteGrid(map68);
        DeleteGrid(map70);
        DeleteGrid(map78);
    }
    cSC4SimGrid<std::int8_t>* Int8Map() override { return map68; }
    cSC4SimGrid<std::int16_t>* Int16MapA() override { return map70; }
    cSC4SimGrid<std::int16_t>* Int16MapB() override { return map78; }
};

struct FakeCrime final : IGrowthCrimeApi {
    FAKE_SERVICE_BOILERPLATE
    cSC4SimGrid<std::uint8_t>* map = nullptr;
    ~FakeCrime() override { DeleteGrid(map); }
    cSC4SimGrid<std::uint8_t>* CrimeMap() override { return map; }
};

struct FakeTrafficDensity final : IGrowthTrafficDensityApi {
    cSC4SimGrid<std::uint8_t>* map = nullptr;
    std::uint8_t GetValue(int x, int z) const override { return map ? map->GetCellValue(x, z) : 0; }
};

struct FakeTraffic final : IGrowthTrafficApi {
    FAKE_SERVICE_BOILERPLATE
    FakeTrafficDensity density;
    cSC4SimGrid<std::uint8_t>* congestion = nullptr;
    ~FakeTraffic() override {
        DeleteGrid(density.map);
        DeleteGrid(congestion);
    }
    IGrowthTrafficDensityApi* Density() override { return &density; }
    cSC4SimGrid<std::uint8_t>* CongestionMap() override { return congestion; }
};

struct FakeDemand final : IGrowthDemandApi {
    int refs = 0;
    float value = 0.0f;
    float supply = 0.0f;
    std::vector<float> supplyCalls;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    float GetValue() const override { return value; }
    void AddToSupply(float amount) override {
        supplyCalls.push_back(amount);
        supply += amount;
    }
};

struct FakeDemandSimulator final : IGrowthDemandSimulatorApi {
    FAKE_SERVICE_BOILERPLATE
    std::map<std::pair<std::uint32_t, std::uint32_t>, FakeDemand*> channels;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> requests;
    IGrowthDemandApi* GetChannel(std::uint32_t type, std::uint32_t flags) override {
        requests.push_back({type, flags});
        auto it = channels.find({type, flags});
        return it == channels.end() ? nullptr : it->second;
    }
};

struct FakeNeighbors final : IGrowthNeighborsApi {
    FAKE_SERVICE_BOILERPLATE
    int connected = 0;
    int all = 0;
    int ConnectedCityCount() override { return connected; }
    int AllCityCount() override { return all; }
};

struct FakeMessages final : cIGZMessageServer2 {
    int refs = 0;
    std::vector<std::pair<cIGZMessageTarget2*, std::uint32_t>> added, removed;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZUnknown) return false;
        *out = static_cast<cIGZUnknown*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool MessageSend(cIGZMessage2*) override { return true; }
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

//----------------------------------------------------------------------------------------------- the host
struct FakeHost final : IGrowthDeveloperHost {
    FakeMessages* messages = nullptr;
    FakeCity* city = nullptr;
    FakeBuildingDev* buildingDev = nullptr;
    FakeLandValue* landValue = nullptr;
    FakeTraffic* traffic = nullptr;
    FakeZoneManager* zoneManager = nullptr;
    FakeResidential* residential = nullptr;
    FakeCommercial* commercial = nullptr;
    FakePollution* pollution = nullptr;
    FakeAura* aura = nullptr;
    FakeCrime* crime = nullptr;
    FakeDemandSimulator* demandSimulator = nullptr;
    FakeNeighbors* neighbors = nullptr;

    std::int32_t simDate = 0;
    bool simulatorPresent = true;
    std::vector<std::array<std::int64_t, 3>> agents;   // interval, flags, 0
    std::vector<std::string> agentNames;

    int sliceBeginCalls = 0;
    std::uint32_t lastSliceMilliseconds = 0;
    int sliceChecks = 0;
    int sliceExpireAfter = -1;      // -1 = never; otherwise the slice expires on that check
    std::uint32_t randomRaw = 0x12345678;

    std::vector<cIGZSerializable*> writtenGrids;
    std::vector<cIGZSerializable*> gridsToRead;
    cSC4Lot* messageLot = nullptr;

    cIGZMessageServer2* MessageServer() override { return messages; }
    IGrowthCityApi* City() override { return city; }
    IGrowthBuildingDevelopmentApi* BuildingDevelopment() override { return buildingDev; }
    IGrowthLandValueApi* LandValue() override { return landValue; }
    IGrowthTrafficApi* Traffic() override { return traffic; }
    IGrowthZoneManagerApi* ZoneManager() override { return zoneManager; }
    IGrowthResidentialApi* Residential() override { return residential; }
    IGrowthCommercialApi* Commercial() override { return commercial; }
    IGrowthPollutionApi* Pollution() override { return pollution; }
    IGrowthAuraApi* Aura() override { return aura; }
    IGrowthCrimeApi* Crime() override { return crime; }
    IGrowthDemandSimulatorApi* DemandSimulator() override { return demandSimulator; }
    IGrowthNeighborsApi* Neighbors() override { return neighbors; }
    std::int32_t SimDateNumber() override { return simDate; }
    bool SimulatorPresent() override { return simulatorPresent; }
    void AddSimulatorAgent(cIGZMessageTarget2*, int interval, const char* name, std::uint32_t flags) override {
        agents.push_back({interval, static_cast<std::int64_t>(flags), 0});
        agentNames.emplace_back(name ? name : "");
    }
    bool BuildingFitsInZone(std::uint32_t buildingPurpose, std::uint32_t zoneType) override {
        // cSC4BuildingDevelopmentSimulator::BuildingFitsInZone @ 0x100150a86.
        if (buildingPurpose == zoneType) return true;
        switch (zoneType) {
        case 2: return buildingPurpose == 1;
        case 3: return buildingPurpose - 1 < 2;
        case 5: return buildingPurpose == 4;
        case 6: return (buildingPurpose | 1) == 5;
        case 9: return buildingPurpose == 8;
        case 0xb:
        case 0xc:
        case 0xf: return buildingPurpose == 0;
        default: return false;
        }
    }
    std::uint64_t BeginSlice(std::uint32_t milliseconds) override {
        ++sliceBeginCalls;
        lastSliceMilliseconds = milliseconds;
        sliceChecks = 0;
        return 0x1000;
    }
    bool SliceExpired(std::uint64_t) override {
        ++sliceChecks;
        return sliceExpireAfter >= 0 && sliceChecks >= sliceExpireAfter;
    }
    std::uint32_t RandomUint32Raw() override { return randomRaw; }
    cSC4Lot* MessageLot(cIGZMessage2*) override { return messageLot; }
    void WriteGrid(cIGZOStream&, cIGZSerializable* grid) override { writtenGrids.push_back(grid); }
    cIGZSerializable* ReadGrid(cIGZIStream&) override {
        if (gridsToRead.empty()) return nullptr;
        cIGZSerializable* grid = gridsToRead.front();
        gridsToRead.erase(gridsToRead.begin());
        return grid;
    }
};

//----------------------------------------------------------------------------------------------- lot host
// cSC4Lot needs its ILotHost for the capacity query; only the building profile and the developer type
// mapping matter here.
struct FakeLotHost final : ILotHost {
    LotBuildingProfile profile;
    std::uint32_t developerType = 0x1010;
    std::uint8_t ZoneTypeAt(int, int) override { return 0; }
    std::int32_t SimulatorDay() override { return 0; }
    float TerrainAltitude(float, float) override { return 0.0f; }
    const LotBuildingProfile* BuildingProfile(std::uint32_t) override { return &profile; }
    float SlopeOf(const cSC4Lot&) override { return 0.0f; }
    std::uint32_t DeveloperType(std::uint32_t, std::int32_t) override { return developerType; }
    void OnDeveloperLotAbandoned(std::uint16_t) override {}
    std::uint8_t LandValueAt(int, int) override { return 0; }
    std::uint32_t RandomPercent() override { return 0; }
    cISC4LotConfigurationRef* LotConfigurationById(std::uint32_t) override { return nullptr; }
    void PostLotMessage(std::uint32_t, const cSC4Lot&, std::uint32_t, std::uint32_t, std::uint32_t,
                        std::int32_t) override {}
    void SetLotHighlight(int, int, bool, float) override {}
    std::int32_t TrafficJobs(const cSC4Lot&, int) override { return 0; }
    void SetTrafficJobs(const cSC4Lot&, int, std::int32_t) override {}
    float ResidentialTravelFactor(int, int) override { return 0.0f; }
    float TravelVolumeFactor() override { return 0.0f; }
    void TravelDistribution(int, float out[3]) override { out[0] = out[1] = out[2] = 0.0f; }
    void JobsPerOccupant(float out[4], int) override { out[0] = out[1] = out[2] = out[3] = 0.0f; }
    void ExemplarJobs(const cSC4Lot&, float out[4]) override { out[0] = out[1] = out[2] = out[3] = 0.0f; }
    bool ExemplarProperty(const cSC4Lot&, std::uint32_t, std::int32_t&) override { return false; }
    bool PowerRequirementExceeded(const cSC4Lot&, const LotBuildingProfile&, bool& available) override {
        available = true;
        return false;
    }
    bool TrafficHasJobAccess(const cSC4Lot&, int) override { return false; }
    void UnitLotShutdown(cIGZUnknown*, const cSC4Lot&) override {}
    void NeighborLots(const cSC4Lot&, int, std::vector<cSC4Lot*>&) override {}
    void GetLotOccupants(const cSC4Lot&, std::vector<cIGZUnknown*>&) override {}
    bool RoadAccessCells(const cSC4Lot&, int) override { return false; }
    bool IsFerryTerminal(const cSC4Lot&) override { return false; }
    bool FerryTerminalRoadAccess(const cSC4Lot&) override { return false; }
    cIGZUnknown* FindBuildingOccupant(const cSC4Lot&) override { return nullptr; }
    void WriteSerializable(cIGZOStream&, cIGZUnknown*) override {}
    cIGZUnknown* ReadSerializable(cIGZIStream&, std::uint32_t) override { return nullptr; }
};

// A special building (landmark) occupant: only the property holder and the position matter. Implemented on
// cISC4Occupant directly so the test does not have to link the whole occupant class.
struct FakeSpecialOccupant final : cISC4Occupant {
    cSCPropertyHolder holder;
    float position[3] = {0.0f, 0.0f, 0.0f};
    int refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override {
        --refs;
        if (refs <= 0) delete this;
        return static_cast<std::uint32_t>(refs);
    }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    cISCPropertyHolder* AsPropertyHolder() override { return &holder; }
    std::uint32_t GetType() const override { return 0x278128a0; }
    void GetPosition(float* xyz) override {
        xyz[0] = position[0];
        xyz[1] = position[1];
        xyz[2] = position[2];
    }
    void SetPosition(const float*) override {}
    void GetBoundingBox(float* minXyz, float* maxXyz) override {
        for (int i = 0; i < 3; ++i) {
            minXyz[i] = position[i];
            maxXyz[i] = position[i];
        }
    }
    void GetBoundingCityCells(SC4CellRect& rect) override { rect = SC4CellRect{}; }
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
    bool IsOccupantGroup(std::uint32_t) const override { return false; }
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

// A lot configuration that only counts the discarded GetID call of UpdatePopulationCount.
struct FakeLotConfiguration final : cISC4LotConfigurationRef {
    int refs = 1;
    mutable int getIds = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    std::uint32_t GetID() const override { ++getIds; return 0x1234; }
};

//----------------------------------------------------------------------------------------------- fixture
struct Fixture {
    FakeSimGridCity simGridCity;
    FakeMessages messages;
    FakeCity city;
    FakeBuildingDev buildingDev;
    FakeLandValue landValue;
    FakeTraffic traffic;
    FakeZoneManager zoneManager;
    FakeResidential residential;
    FakeCommercial commercial;
    FakePollution pollution;
    FakeAura aura;
    FakeCrime crime;
    FakeDemandSimulator demandSimulator;
    FakeNeighbors neighbors;
    FakeDemand demandSource;
    FakeDemand demandRci;
    FakeDemand demandNeighbor;
    FakeDemand demandFloor;
    FakeHost host;
    FakeLotHost lotHost;
    cSCPropertyHolder* exemplar = nullptr;
    cSC4GrowthDeveloper* developer = nullptr;

    static constexpr int kTractSize = 4;       // 2 x 2 tracts over the fake city's 8 x 8 cells

    Fixture() {
        SetSimGridCity(&simGridCity);
        messages.AddRef();                      // the host owns one reference
        host.messages = &messages;
        host.city = &city;
        host.buildingDev = &buildingDev;
        host.landValue = &landValue;
        host.traffic = &traffic;
        host.zoneManager = &zoneManager;
        host.residential = &residential;
        host.commercial = &commercial;
        host.pollution = &pollution;
        host.aura = &aura;
        host.crime = &crime;
        host.demandSimulator = &demandSimulator;
        host.neighbors = &neighbors;
        host.simDate = 100;
        SetGrowthDeveloperHost(&host);

        landValue.intrinsic = NewGrid<std::uint8_t>(kTractSize);
        landValue.total = NewGrid<std::uint8_t>(kTractSize);
        landValue.slope = NewGrid<float>(kTractSize);
        for (int x = 0; x < 2; ++x)
            for (int z = 0; z < 2; ++z) {
                landValue.intrinsic->SetTractValue(x, z, 50);
                landValue.total->SetTractValue(x, z, 50);
                landValue.slope->SetTractValue(x, z, 0.0f);
            }
        demandSimulator.channels[{0x3001, 0}] = &demandRci;
        demandSimulator.channels[{0x3301, 0x10000}] = &demandNeighbor;
        demandSimulator.channels[{0x1001, 0}] = &demandFloor;
        demandSimulator.channels[{0x3001, 0x20000}] = &demandSource;
        demandRci.value = 40.0f;
        demandNeighbor.value = 20.0f;
        demandFloor.value = 30.0f;
        demandSource.value = 25.0f;
    }

    ~Fixture() {
        if (developer) {
            developer->Shutdown();
            developer->Release();
            developer = nullptr;
        }
        if (exemplar) exemplar->Release();
        SetGrowthDeveloperHost(nullptr);
        SetSimGridCity(nullptr);
        SetLotHost(nullptr);
        messages.Release();
    }

    // The ini exemplar: the developer identity, the tract size, the land value curve (0..100 -> 0..10) and
    // `styleCurves` identical 0..100 -> 0..10 style curves.
    void MakeExemplar(std::uint32_t subType = 0x1010, std::uint8_t type = 1, std::uint8_t purpose = 1,
                      std::uint8_t wealth = 1, int styleCurves = 3) {
        exemplar = new cSCPropertyHolder();
        exemplar->AddRef();
        cSCPropertyHelp::SetPropertyValue(exemplar, 0x21, subType);
        cSCPropertyHelp::SetPropertyValue(exemplar, 0x87cd6300, type);
        cSCPropertyHelp::SetPropertyValue(exemplar, 0x87cd6330, wealth);
        cSCPropertyHelp::SetPropertyValue(exemplar, 0x87cd6332, purpose);
        cSCPropertyHelp::SetPropertyValue(exemplar, 0x87cd6310, static_cast<std::uint8_t>(kTractSize));
        cSCPropertyHelp::SetPropertyValue(exemplar, 0x87cd6340, std::vector<float>{0.0f, 0.0f, 100.0f, 10.0f});
        cSCPropertyHelp::SetPropertyValue(exemplar, 0x87cd6360, static_cast<std::uint8_t>(styleCurves));
        for (int i = 0; i < styleCurves; ++i)
            cSCPropertyHelp::SetPropertyValue(exemplar, 0x87cd6361 + static_cast<std::uint32_t>(i),
                                              std::vector<float>{0.0f, 0.0f, 100.0f, 10.0f});
    }

    cSC4GrowthDeveloper* MakeDeveloper() {
        developer = new cSC4GrowthDeveloper();
        developer->AddRef();
        return developer;
    }

    // The tunables of the map-update scenarios: desirability starts at 20, the growth threshold is 30 and
    // the decline threshold 28, the population is added to the demand.
    void UseMapUpdateTunables() {
        cSCPropertyHelp::SetPropertyValue(exemplar, 0x87cd6301, static_cast<std::uint32_t>(0x3001));
        cSCPropertyHelp::SetPropertyValue(exemplar, 0x87cd6357, 20);
        cSCPropertyHelp::SetPropertyValue(exemplar, 0x87cd6355, 30);
        cSCPropertyHelp::SetPropertyValue(exemplar, 0x87cd6354, 28);
        cSCPropertyHelp::SetPropertyValue(exemplar, 0x87cd6356, true);
        developer->ReadSimulatorTunableValues();
    }
};

template <class T>
void FillGrid(cSC4SimGrid<T>* grid, T value) {
    for (int x = 0; x < grid->GetTractCountX(); ++x)
        for (int z = 0; z < grid->GetTractCountZ(); ++z) grid->SetTractValue(x, z, value);
}

//=================================================================================================== tests

// The map-update scenario: land values 50/150/0/100 -> desirability 25/30/20/30 (the curve clamps above
// 100), capacities 100/0/50/50 -> population 25/0/10/15, growth threshold 30, decline threshold 28.
void SetUpMapUpdateScenario(Fixture& fixture) {
    fixture.UseMapUpdateTunables();
    auto* landValue = fixture.landValue.intrinsic;
    landValue->SetTractValue(0, 0, 50);
    landValue->SetTractValue(1, 0, 150);
    landValue->SetTractValue(0, 1, 0);
    landValue->SetTractValue(1, 1, 100);
    FillGrid<std::uint8_t>(fixture.developer->ZoneCellCountMap(), 1);
    fixture.developer->GetCapacityMap()->SetTractValue(0, 0, 100);
    fixture.developer->GetCapacityMap()->SetTractValue(1, 0, 0);
    fixture.developer->GetCapacityMap()->SetTractValue(0, 1, 50);
    fixture.developer->GetCapacityMap()->SetTractValue(1, 1, 50);
    fixture.neighbors.connected = 1;
    fixture.neighbors.all = 2;
    fixture.buildingDev.stageMultiplier = 1.0f;
}

void TestComAndLifecycle() {
    Fixture fixture;
    fixture.MakeExemplar();
    auto* developer = fixture.MakeDeveloper();
    CHECK(!developer->IsInitialized());
    CHECK(developer->RefCount() == 1);

    void* view = nullptr;
    CHECK(developer->QueryInterface(1, &view));
    CHECK(view == static_cast<cISC4GrowthDeveloper*>(developer));
    CHECK(developer->RefCount() == 2);
    CHECK(developer->QueryInterface(kGZIID_cISC4GrowthDeveloper, &view));
    CHECK(developer->QueryInterface(kGZIID_cIGZMessageTarget2, &view));
    CHECK(view == static_cast<cIGZMessageTarget2*>(developer));
    CHECK(developer->QueryInterface(kGZIID_cISC4GrowthDeveloperSerializable, &view));
    CHECK(view == static_cast<cIGZSerializable*>(developer));
    CHECK(developer->RefCount() == 5);
    CHECK(!developer->QueryInterface(0, &view));
    CHECK(!developer->QueryInterface(0x1234, &view));
    CHECK(!developer->QueryInterface(kGZIID_cISC4GrowthDeveloper, nullptr));
    while (developer->RefCount() > 1) developer->Release();
    CHECK(developer->RefCount() == 1);

    CHECK(developer->Init(fixture.exemplar));
    CHECK(developer->IsInitialized());
    CHECK(developer->CityForTest() == &fixture.city);
    CHECK(developer->MessageServerForTest() == &fixture.messages);
    CHECK(fixture.city.refs == 1);
    CHECK(fixture.buildingDev.refs == 1);
    CHECK(fixture.city.occupantManager.refs == 1);
    CHECK(fixture.zoneManager.refs == 1);
    CHECK(fixture.landValue.refs == 1);
    CHECK(fixture.traffic.refs == 1);
    CHECK(fixture.messages.added.size() == 2);
    CHECK(fixture.messages.added[0].first == static_cast<cIGZMessageTarget2*>(developer));
    CHECK(fixture.messages.added[0].second == 0xe8bda2e7);
    CHECK(fixture.messages.added[1].second == 0x07699455);
    CHECK(fixture.host.agents.size() == 2);
    CHECK(fixture.host.agents[0][0] == 4);
    CHECK(fixture.host.agents[1][0] == 10);
    CHECK(fixture.host.agents[0][1] == 2);
    CHECK(fixture.host.agentNames[0] == "cSC4GrowthDeveloper");
    CHECK(developer->ZoneCellCountMap() != nullptr);
    CHECK(developer->ZoneCellCountMap()->GetInstanceID() == 0x1010 + 0x1000000);
    CHECK(developer->DesirabilityWorkMap()->GetInstanceID() == 0x1010 + 0x2100000);
    CHECK(developer->GetDesirabilityMap()->GetInstanceID() == 0x1010 + 0x2000000);
    CHECK(developer->GetPopulationMap()->GetInstanceID() == 0x1010 + 0x3000000);
    CHECK(developer->GetCapacityMap()->GetInstanceID() == 0x1010 + 0x5000000);
    CHECK(developer->GetMaxCapacityMap()->GetInstanceID() == 0x1010 + 0x6000000);
    CHECK(developer->GetCapacityMap()->GetTractSize() == Fixture::kTractSize);
    CHECK(developer->GetCapacityMap()->GetTractCountX() == 2);
    CHECK(developer->Cursor() != nullptr);
    CHECK(developer->Cursor()->step == 1);
    CHECK(developer->Cursor()->maxX == 1);      // SetTractSize bounded the cursor
    CHECK(developer->Cursor()->maxZ == 1);
    CHECK(developer->Init(fixture.exemplar));   // a second Init is a no-op
    CHECK(fixture.host.agents.size() == 2);
    CHECK(fixture.messages.added.size() == 2);

    CHECK(developer->Shutdown());
    CHECK(!developer->IsInitialized());
    CHECK(fixture.city.removeCalls == 1);
    CHECK(fixture.city.removedView == static_cast<cIGZMessageTarget2*>(developer));
    CHECK(fixture.messages.removed.size() == 2);
    CHECK(fixture.city.releases == 1);
    CHECK(fixture.buildingDev.releases == 1);
    CHECK(fixture.city.occupantManager.releases == 1);
    CHECK(fixture.zoneManager.releases == 1);
    CHECK(fixture.landValue.releases == 1);
    CHECK(fixture.traffic.releases == 1);
    CHECK(fixture.messages.refs == 1);          // only the host's own reference is left
    CHECK(developer->ZoneCellCountMap() == nullptr);
    CHECK(developer->Cursor() == nullptr);
    CHECK(developer->Shutdown());               // a second Shutdown is a no-op
    CHECK(fixture.city.releases == 1);
}

void TestTunables() {
    Fixture fixture;
    fixture.MakeExemplar(0x1020, 2, 5, 2, 4);
    cSCPropertyHolder* holder = fixture.exemplar;
    cSCPropertyHelp::SetPropertyValue(holder, 0x87cd6301, static_cast<std::uint32_t>(0x3001));
    cSCPropertyHelp::SetPropertyValue(holder, 0x87cd6333, std::vector<std::uint8_t>{7, 9});
    cSCPropertyHelp::SetPropertyValue(holder, 0x87cd633f, true);
    cSCPropertyHelp::SetPropertyValue(holder, 0x87cd6357, 0x40);
    cSCPropertyHelp::SetPropertyValue(holder, 0x87cd6355, 0x30);
    cSCPropertyHelp::SetPropertyValue(holder, 0x87cd6354, 0x28);
    cSCPropertyHelp::SetPropertyValue(holder, 0x87cd6356, true);
    cSCPropertyHelp::SetPropertyValue(holder, 0x87cd6373, static_cast<std::uint8_t>(1));
    cSCPropertyHelp::SetPropertyValue(holder, 0x87cd6370, static_cast<std::uint8_t>(2));
    cSCPropertyHelp::SetPropertyValue(holder, 0x87cd6371, static_cast<std::uint8_t>(4));
    cSCPropertyHelp::SetPropertyValue(holder, 0x87cd6372, static_cast<std::uint8_t>(5));
    cSCPropertyHelp::SetPropertyValue(holder, 0x87cd6380, std::vector<float>{0.1f, 0.2f, 0.3f});
    cSCPropertyHelp::SetPropertyValue(holder, 0x87cd6381, static_cast<std::uint8_t>(2));
    cSCPropertyHelp::SetPropertyValue(holder, 0x87cd63a0, std::vector<std::uint32_t>{0x11, 50, 0x22, 25});
    cSCPropertyHelp::SetPropertyValue(holder, 0xcab1ce3a, std::vector<std::uint32_t>{1, 2, 3});
    cSCPropertyHelp::SetPropertyValue(holder, 0xcab1ce3c, std::vector<std::uint32_t>{4, 5, 6});
    auto* developer = fixture.MakeDeveloper();
    CHECK(developer->Init(holder));
    CHECK(developer->GetSubType() == 0x1020);
    CHECK(developer->GetType() == 2);
    CHECK(developer->GetPurpose() == 5);
    CHECK(developer->GetWealth() == 2);
    CHECK(developer->TractSize() == Fixture::kTractSize);
    CHECK(developer->GetPowerSupplyCap() == 1);
    CHECK(developer->GetWaterSupplyCap() == 2);
    CHECK(developer->GetFireCoverageCap() == 4);
    CHECK(developer->GetParkCap() == 5);
    CHECK(developer->GetDesirabilityThreshold() == 0x30);
    CHECK(developer->DeclinedThreshold() == 0x28);
    CHECK(developer->AddPopulationToDemand());
    CHECK(developer->GetTotalCapacity() == 0);
    CHECK(developer->GetSourceDemand() == &fixture.demandSource);
    CHECK(fixture.demandSource.refs == 1);
    CHECK(fixture.demandSimulator.requests.size() == 1);
    CHECK(fixture.demandSimulator.requests[0].first == 0x3001);
    CHECK(fixture.demandSimulator.requests[0].second == 0x20000);
    CHECK(developer->GetMonopolyUse() == 2);
    CHECK(developer->GetAbandonmentThreshold(kGrowthDensityLow) == 0.1f);
    CHECK(developer->GetAbandonmentThreshold(kGrowthDensityMedium) == 0.2f);
    CHECK(developer->GetAbandonmentThreshold(kGrowthDensityHigh) == 0.3f);
    CHECK(developer->GetAbandonmentThreshold(9) == 0.0f);      // the map grows on lookup (kept)
    CHECK(developer->AbandonmentThresholds().count(9) == 1);
    CHECK(developer->GetStylePercentages().size() == 2);
    CHECK(developer->GetStylePercentages().at(0x11).value == 0.5f);
    CHECK(developer->GetStylePercentages().at(0x22).value == 0.25f);
    CHECK(developer->StyleCurves().size() == 4);
    CHECK(developer->StyleCurves()[0].PointCount() == 2);
    int low = 0, high = 0;
    developer->GetSubjectiveThresholds(0, low, high);
    CHECK(low == 1);
    CHECK(high == 4);
    developer->GetSubjectiveThresholds(2, low, high);
    CHECK(low == 3);
    CHECK(high == 6);
    developer->GetSubjectiveThresholds(3, low, high);
    CHECK(low == -4);
    CHECK(high == 0x14);
    developer->GetSubjectiveThresholds(-1, low, high);
    CHECK(low == -4);
    CHECK(high == 0x14);
    ConstructionProfile profile;
    developer->GetConstructionInfo(profile);
    CHECK(profile.flag10 == 7);
    CHECK(profile.flag11 == 9);
    CHECK(profile.purpose == 4);                 // commercial
    CHECK(profile.value12 == 5);
    CHECK(std::bit_cast<std::uint32_t>(profile.value8) == 2);
}

void TestTunableDefaults() {
    Fixture fixture;
    fixture.exemplar = new cSCPropertyHolder();
    fixture.exemplar->AddRef();
    cSCPropertyHelp::SetPropertyValue(fixture.exemplar, 0x21, static_cast<std::uint32_t>(0x1010));
    cSCPropertyHelp::SetPropertyValue(fixture.exemplar, 0x87cd6340, std::vector<float>{0.0f, 0.0f, 100.0f, 10.0f});
    auto* developer = fixture.MakeDeveloper();
    CHECK(developer->Init(fixture.exemplar));
    CHECK(developer->GetPowerSupplyCap() == 0);
    CHECK(developer->GetWaterSupplyCap() == 3);
    CHECK(developer->GetFireCoverageCap() == 3);
    CHECK(developer->GetParkCap() == 6);
    CHECK(developer->GetDesirabilityThreshold() == 0x50);
    CHECK(developer->DeclinedThreshold() == 0x50);
    CHECK(developer->GetMonopolyUse() == 1);
    CHECK(!developer->AddPopulationToDemand());   // ReadSimulatorTunableValues clears the default
    CHECK(developer->GetAbandonmentThreshold(kGrowthDensityLow) == 0.2f);
    CHECK(developer->StyleCurves().empty());
    CHECK(developer->GetSourceDemand() == nullptr);   // no demand type property
    // Without the tract size property the cursor keeps the constructor sentinels (kept quirk).
    CHECK(developer->Cursor()->maxX == 0x7fffffff);
    CHECK(developer->Cursor()->startX == static_cast<std::int32_t>(0x80000000u));
    CHECK(developer->LandValueCurve() != nullptr);
    CHECK(developer->SlopeCurve() == nullptr);        // no property, no curve
    CHECK(developer->PollutionCurves().size() == 4);
    for (const auto* curve : developer->PollutionCurves()) CHECK(curve == nullptr);
}

void TestSetTractSizeAndCreationInfo() {
    Fixture fixture;
    fixture.MakeExemplar(0x1010, 1, 1, 1, 3);
    auto* developer = fixture.MakeDeveloper();
    CHECK(developer->Init(fixture.exemplar));
    CHECK(developer->GetCapacityMap()->GetTractCountX() == 2);
    developer->SetTractSize(2);            // 8 cells / 2 = 4 x 4 tracts
    CHECK(developer->GetCapacityMap()->GetTractSize() == 2);
    CHECK(developer->GetCapacityMap()->GetTractCountX() == 4);
    CHECK(developer->DesirabilityWorkMap()->GetTractCountZ() == 4);
    CHECK(developer->Cursor()->maxX == 3);
    CHECK(developer->Cursor()->maxZ == 3);
    CHECK(developer->Cursor()->startX == 0);
    CHECK(developer->Cursor()->state == 0);
    developer->SetTractSize(2);            // no change: nothing is re-laid
    CHECK(developer->GetCapacityMap()->GetTractCountX() == 4);

    CHECK(cSC4GrowthDeveloper::ConvertZoneToType(1) == 1);
    CHECK(cSC4GrowthDeveloper::ConvertZoneToType(3) == 1);
    CHECK(cSC4GrowthDeveloper::ConvertZoneToType(4) == 2);
    CHECK(cSC4GrowthDeveloper::ConvertZoneToType(6) == 2);
    CHECK(cSC4GrowthDeveloper::ConvertZoneToType(7) == 3);
    CHECK(cSC4GrowthDeveloper::ConvertZoneToType(9) == 3);
    CHECK(cSC4GrowthDeveloper::ConvertZoneToType(0) == 0);
    CHECK(cSC4GrowthDeveloper::ConvertZoneToType(10) == 0);

    ConstructionProfile profile;
    profile.purpose = 0xdead;
    profile.value4 = 1.5f;
    developer->GetConstructionInfo(profile);
    CHECK(profile.purpose == 1);
    CHECK(profile.value4 == 0.0f);        // copied out of +0x70, which this class never writes
    CHECK(profile.value12 == 1);
    cSCPropertyHelp::SetPropertyValue(fixture.exemplar, 0x87cd6300, static_cast<std::uint8_t>(2));
    developer->ReadSimulatorTunableValues();
    developer->GetConstructionInfo(profile);
    CHECK(profile.purpose == 4);
    cSCPropertyHelp::SetPropertyValue(fixture.exemplar, 0x87cd6300, static_cast<std::uint8_t>(3));
    const std::uint32_t expected[4] = {7, 8, 8, 8};
    for (std::uint8_t purpose = 5; purpose <= 8; ++purpose) {
        cSCPropertyHelp::SetPropertyValue(fixture.exemplar, 0x87cd6332, purpose);
        developer->ReadSimulatorTunableValues();
        developer->GetConstructionInfo(profile);
        CHECK(profile.purpose == expected[purpose - 5]);
    }
    // Industrial with a purpose outside 5..8: the purpose field keeps its previous value, the rest is copied.
    cSCPropertyHelp::SetPropertyValue(fixture.exemplar, 0x87cd6332, static_cast<std::uint8_t>(1));
    developer->ReadSimulatorTunableValues();
    profile.purpose = 0xbeef;
    profile.value12 = 0;
    developer->GetConstructionInfo(profile);
    CHECK(profile.purpose == 0xbeef);
    CHECK(profile.value12 == 1);
    // An unknown type stores purpose 0.
    cSCPropertyHelp::SetPropertyValue(fixture.exemplar, 0x87cd6300, static_cast<std::uint8_t>(9));
    developer->ReadSimulatorTunableValues();
    developer->GetConstructionInfo(profile);
    CHECK(profile.purpose == 0);
}

void TestMaxCapacity() {
    Fixture fixture;
    fixture.MakeExemplar(0x1010, 1, 1, 1, 3);
    auto* developer = fixture.MakeDeveloper();
    CHECK(developer->Init(fixture.exemplar));
    fixture.zoneManager.zoneAt = [](int x, int) { return x < 4 ? 1 : 0; };
    fixture.buildingDev.maxCapacity = 12.0f;
    SC4Rect<int> rect{0, 0, 7, 7};
    developer->UpdateMaxCapacity(rect);
    CHECK(developer->ZoneCellCountMap()->GetTractValue(0, 0) == 16);   // all 16 cells fit zone 1
    CHECK(developer->ZoneCellCountMap()->GetTractValue(1, 0) == 0);
    CHECK(developer->GetMaxCapacityMap()->GetTractValue(0, 0) == 16 * 12);
    CHECK(developer->GetMaxCapacityMap()->GetTractValue(1, 1) == 0);
    // The left column of tracts (x < 4 cells) is zone 1 for both z halves: 2 tracts x 16 cells.
    CHECK(fixture.buildingDev.maxCapacityCalls.size() == 32);
    CHECK(fixture.buildingDev.maxCapacityCalls[0][0] == 1);
    CHECK(fixture.buildingDev.maxCapacityCalls[0][1] == 1);            // this->purpose
    CHECK(fixture.buildingDev.maxCapacityCalls[0][2] == 1);            // this->wealth
    // A rectangle whose start is past the tract grid is clamped to it (the original would write out of
    // bounds; the port stays inside the map).
    SC4Rect<int> clipped{8, 8, 100, 100};
    developer->UpdateMaxCapacity(clipped);
    CHECK(developer->ZoneCellCountMap()->GetTractValue(1, 1) == 0);
}

void TestPopulationCount() {
    Fixture fixture;
    fixture.MakeExemplar(0x1010, 1, 1, 1, 3);
    auto* developer = fixture.MakeDeveloper();
    CHECK(developer->Init(fixture.exemplar));
    SetLotHost(&fixture.lotHost);
    fixture.lotHost.profile.capacities.resize(1);
    fixture.lotHost.profile.capacities[0][0x1010] = 100;
    fixture.buildingDev.profile.purpose = 1;
    auto* lot = new cSC4Lot(0, 0, 2, 2, 0);
    lot->SetBuildingType(0x1000);
    developer->UpdatePopulationCount(lot, 0x1000, true);  // data3: the lot's building type, whose purpose must match
    CHECK(fixture.buildingDev.profileAsked == 0x1000);    // the building's profile, not the developer type's (0x1010)
    // 100 over four cells -> 25 each, all of them in tract (0,0); the total capacity grows by 100.
    CHECK(developer->GetCapacityMap()->GetTractValue(0, 0) == 100);
    CHECK(developer->GetTotalCapacity() == 100);
    developer->UpdatePopulationCount(lot, 0x1000, false);
    CHECK(developer->GetCapacityMap()->GetTractValue(0, 0) == 0);
    CHECK(developer->GetTotalCapacity() == 0);
    // A cell that would go negative clamps to zero and asks the lot configuration for its id (kept quirk).
    FakeLotConfiguration configuration;
    lot->SetLotConfiguration(&configuration);
    developer->UpdatePopulationCount(lot, 0x1000, false);
    CHECK(developer->GetCapacityMap()->GetTractValue(0, 0) == 0);
    CHECK(configuration.getIds > 0);
    CHECK(developer->GetTotalCapacity() == -100);
    // A purpose mismatch does nothing.
    fixture.buildingDev.profile.purpose = 9;
    developer->UpdatePopulationCount(lot, 0x1000, true);
    CHECK(developer->GetCapacityMap()->GetTractValue(0, 0) == 0);
    lot->SetLotConfiguration(nullptr);
    delete lot;
}

void TestDoMessage() {
    Fixture fixture;
    fixture.MakeExemplar(0x1010, 1, 1, 1, 3);
    auto* developer = fixture.MakeDeveloper();
    CHECK(developer->Init(fixture.exemplar));
    SetLotHost(&fixture.lotHost);
    fixture.lotHost.profile.capacities.resize(1);
    fixture.lotHost.profile.capacities[0][0x1010] = 40;
    fixture.buildingDev.profile.purpose = 1;
    auto* lot = new cSC4Lot(0, 0, 2, 2, 0);
    lot->SetBuildingType(0x1000);

    cRZMessage2Standard message;
    message.SetType(0xe8bda2e7);                     // lot state changed
    fixture.host.messageLot = lot;
    message.SetData1(0);                             // old state: no capacity
    message.SetData2(2);                             // new state: has capacity
    message.SetData3(40);                            // population
    message.SetData4(9);                             // wrong wealth -> ignored
    CHECK(developer->DoMessage(&message));
    CHECK(developer->GetCapacityMap()->GetTractValue(0, 0) == 0);
    message.SetData4(1);
    CHECK(developer->DoMessage(&message));
    CHECK(developer->GetCapacityMap()->GetTractValue(0, 0) == 40);
    CHECK(developer->GetTotalCapacity() == 40);
    // Both states have capacity: the message is ignored.
    message.SetData1(2);
    message.SetData2(3);
    CHECK(developer->DoMessage(&message));
    CHECK(developer->GetTotalCapacity() == 40);
    // No capacity and no population: the original reads the building type and drops it.
    message.SetData1(2);
    message.SetData2(0);
    message.SetData3(0);
    CHECK(developer->DoMessage(&message));
    CHECK(developer->GetTotalCapacity() == 40);

    // The maximum-capacity message carries a rectangle in its second void slot.
    fixture.zoneManager.zoneAt = [](int, int) { return 1; };
    fixture.buildingDev.maxCapacity = 3.0f;
    SC4Rect<int> rect{0, 0, 3, 3};
    message.SetType(0x07699455);
    message.SetVoid2(&rect);
    CHECK(developer->DoMessage(&message));
    CHECK(developer->ZoneCellCountMap()->GetTractValue(0, 0) == 16);
    CHECK(developer->GetMaxCapacityMap()->GetTractValue(0, 0) == 48);

    // The day tick runs a 5 ms step; the reply depends on the update age against the stage number.
    message.SetType(0x66956814);
    developer->SetMapUpdateStage(18);
    fixture.host.simDate = 1;
    CHECK(developer->DoMessage(&message));
    CHECK(fixture.host.lastSliceMilliseconds == 5);
    CHECK(developer->MapUpdateStage() == 19);        // the driver finished
    developer->SetMapUpdateStage(18);
    fixture.host.simDate = 1000;                     // much older than the stage number -> not handled
    CHECK(!developer->DoMessage(&message));
    // An idle tick passes its data3 as the budget.
    message.SetType(0x6695681a);
    message.SetData3(37);
    developer->SetMapUpdateStage(18);
    fixture.host.simDate = 1;
    CHECK(developer->DoMessage(&message));
    CHECK(fixture.host.lastSliceMilliseconds == 37);
    // Unknown messages are handled without effect.
    message.SetType(0x1234);
    CHECK(developer->DoMessage(&message));
    CHECK(developer->DoMessage(nullptr));
    delete lot;
}

void TestMapUpdate() {
    Fixture fixture;
    fixture.MakeExemplar(0x1010, 1, 1, 1, 3);
    cSC4GrowthDeveloper* d = fixture.MakeDeveloper();
    CHECK(d->Init(fixture.exemplar));
    SetUpMapUpdateScenario(fixture);

    CHECK(d->IsMapUpdateFinished());
    CHECK(d->GrowthAndDeclineListsAreValid());
    d->StartMapUpdate();
    CHECK(!d->IsMapUpdateFinished());
    for (int i = 0; i < 30 && d->MapUpdateStage() != 18; ++i) d->DoIdle(5);
    CHECK(d->MapUpdateStage() == 18);
    fixture.host.sliceExpireAfter = 3;
    d->DoIdle(5);                                   // the slice expires after two tracts
    CHECK(d->MapUpdateStage() == 18);
    CHECK(d->Cursor()->state == 1);
    CHECK(d->Cursor()->x == 1);
    CHECK(d->Cursor()->z == 0);
    fixture.host.sliceExpireAfter = -1;
    d->DoIdle(5);                                   // finishes the driver
    CHECK(d->MapUpdateStage() == 19);
    CHECK(d->Cursor()->state == 2);
    CHECK(fixture.buildingDev.tractUpdates.size() == 4);
    CHECK(fixture.buildingDev.tractUpdates[0] == std::make_pair(0, 0));
    CHECK(fixture.buildingDev.tractUpdates[1] == std::make_pair(1, 0));
    CHECK(fixture.buildingDev.tractUpdates[2] == std::make_pair(0, 1));
    CHECK(fixture.buildingDev.tractUpdates[3] == std::make_pair(1, 1));
    // The population map is the capacity times clamp(desirability / 100).
    CHECK(d->GetPopulationMap()->GetTractValue(0, 0) == 25);
    CHECK(d->GetPopulationMap()->GetTractValue(1, 0) == 0);
    CHECK(d->GetPopulationMap()->GetTractValue(0, 1) == 10);
    CHECK(d->GetPopulationMap()->GetTractValue(1, 1) == 15);
    CHECK(d->GetDesirabilityMap()->GetTractValue(0, 0) == 25);
    CHECK(d->GetDesirabilityMap()->GetTractValue(1, 1) == 30);

    d->DoIdle(5);                                   // stage 19: the demand contribution
    CHECK(d->MapUpdateStage() == 20);
    CHECK(d->PopulationTotal() == 2);
    CHECK(fixture.demandSource.supplyCalls.size() == 1);
    CHECK(fixture.demandSource.supplyCalls[0] == 2.0f);

    d->DoIdle(5);                                   // stage 20: lists, styles, demand
    CHECK(d->IsMapUpdateFinished());
    CHECK(d->GrowthAndDeclineListsAreValid());
    const auto& growth = d->GetGrowthList();
    const auto& decline = d->GetDeclineList();
    CHECK(growth.size() == 2);
    CHECK(decline.size() == 2);
    CHECK(growth[0].style == 3);                    // kept: the style index starts at the curve count
    CHECK(growth[1].style == 1);
    for (const auto& item : growth) {
        const std::int16_t value = d->DesirabilityWorkMap()->GetTractValue(item.x, item.z);
        CHECK(value >= 30);
    }
    for (const auto& item : decline) {
        const std::int16_t value = d->DesirabilityWorkMap()->GetTractValue(item.x, item.z);
        CHECK(value < 28);
    }
    bool sawRci = false, sawNeighbor = false, sawFloor = false;
    for (const auto& request : fixture.demandSimulator.requests) {
        if (request.first == 0x3001) sawRci = true;
        if (request.first == 0x3301) sawNeighbor = true;
        if (request.first == 0x1001) sawFloor = true;
    }
    CHECK(sawRci);
    CHECK(sawNeighbor);
    CHECK(sawFloor);
    // A second pass starts over.
    d->StartMapUpdate();
    CHECK(d->MapUpdateStage() == 1);
    d->DoIdle(5);
    CHECK(d->MapUpdateStage() == 2);
    CHECK(d->MapUpdateStartDate() == 100);
}

void TestSingleStyleQuirk() {
    Fixture fixture;
    fixture.MakeExemplar(0x1010, 1, 1, 1, 1);        // a single style curve
    auto* d = fixture.MakeDeveloper();
    CHECK(d->Init(fixture.exemplar));
    FillGrid<std::uint8_t>(d->ZoneCellCountMap(), 1);
    d->StartMapUpdate();
    for (int i = 0; i < 30 && d->MapUpdateStage() != 0; ++i) d->DoIdle(5);
    CHECK(d->IsMapUpdateFinished());
    CHECK(!d->GetGrowthList().empty());
    for (const auto& item : d->GetGrowthList()) CHECK(item.style == 1);   // kept quirk
}

void TestIndustrialShuffle() {
    Fixture fixture;
    fixture.MakeExemplar(0x1010, 3, 6, 1, 3);        // industrial, purpose 6 -> in the shuffle range
    auto* d = fixture.MakeDeveloper();
    CHECK(d->Init(fixture.exemplar));
    fixture.UseMapUpdateTunables();
    // Every tract is below the decline threshold so the decline list holds all four.
    cSCPropertyHelp::SetPropertyValue(fixture.exemplar, 0x87cd6354, 100);
    d->ReadSimulatorTunableValues();
    FillGrid<std::uint8_t>(d->ZoneCellCountMap(), 1);
    fixture.host.randomRaw = 0x9e3779b9u;
    d->StartMapUpdate();
    for (int i = 0; i < 40 && d->MapUpdateStage() != 0; ++i) d->DoIdle(5);
    CHECK(d->IsMapUpdateFinished());
    const auto& decline = d->GetDeclineList();
    CHECK(decline.size() == 4);
    // The list is ordered by the randomize comparator (the mask the pass drew from the host).
    for (std::size_t i = 0; i + 1 < decline.size(); ++i) {
        const GrowthItem& a = decline[i];
        const GrowthItem& b = decline[i + 1];
        cRZRandom randomA(static_cast<std::uint32_t>(a.z * 0x100000 + a.x * 0x10) ^ 0x9e3779b9u);
        const std::uint32_t valueA = randomA.RandomUint32Uniform();
        cRZRandom randomB(static_cast<std::uint32_t>(b.z * 0x100000 + b.x * 0x10) ^ 0x9e3779b9u);
        const std::uint32_t valueB = randomB.RandomUint32Uniform();
        CHECK(!(valueA < valueB));       // descending by the random draw, ties allowed
        CHECK(!GrowthDeveloperCompareRandomize(b, a));
    }
}

void TestLandmarkPass() {
    Fixture fixture;
    fixture.MakeExemplar(0x1010, 1, 1, 1, 3);
    auto* d = fixture.MakeDeveloper();
    CHECK(d->Init(fixture.exemplar));
    auto* occupant = new FakeSpecialOccupant();
    occupant->AddRef();
    // Two landmark entries; only the one whose type matches this developer's sub type is applied.
    cSCPropertyHelp::SetPropertyValue(&occupant->holder, 0x4a5f7c4b,
                                      std::vector<std::uint32_t>{0x4444, 0x1010});
    cSCPropertyHelp::SetPropertyValue(&occupant->holder, 0x4a5f7315,
                                      std::vector<float>{8.0f, 10.0f, 0.0f, 16.0f, 50.0f, 0.0f});
    fixture.buildingDev.specialBuildings.emplace_back(static_cast<cISC4Occupant*>(occupant));
    fixture.city.cellX = 0;
    fixture.city.cellZ = 0;
    FillGrid<std::int16_t>(d->DesirabilityWorkMap(), 0);
    // Stage 14 collects the special buildings, stage 15 applies one landmark effect per call.
    d->SetMapUpdateStage(14);
    d->DoIdle(5);
    CHECK(d->MapUpdateStage() == 15);
    CHECK(fixture.city.positionCalls == 0);
    d->DoIdle(5);
    CHECK(fixture.city.positionCalls == 1);
    CHECK(fixture.city.lastPositionX == 0.0f);
    CHECK(fixture.city.lastPositionZ == 0.0f);
    // radius 16 / tract size 4 = 4 tracts, peak 50 at the centre.
    CHECK(d->DesirabilityWorkMap()->GetTractValue(0, 0) == 50);
    CHECK(d->DesirabilityWorkMap()->GetTractValue(1, 0) == 38);
    CHECK(d->MapUpdateStage() == 16);     // the list is finished
    occupant->Release();                  // the fixture's list keeps its own reference
}

void TestEstimateDesirability() {
    Fixture fixture;
    fixture.MakeExemplar(0x1010, 1, 1, 1, 3);
    auto* d = fixture.MakeDeveloper();
    CHECK(d->Init(fixture.exemplar));
    // 0x108 default (0x50) plus the land value curve (5 at 50) plus the slope curve (2 at 50).
    cSCPropertyHelp::SetPropertyValue(fixture.exemplar, 0x87cd6398, std::vector<float>{0.0f, 0.0f, 100.0f, 4.0f});
    fixture.aura.map68 = NewGrid<std::int8_t>(4);
    FillGrid(fixture.aura.map68, static_cast<std::int8_t>(-5));
    cSCPropertyHelp::SetPropertyValue(fixture.exemplar, 0x87cd6397, std::vector<float>{0.0f, 0.0f, 100.0f, 10.0f});
    d->ReadSimulatorTunableValues();
    FillGrid(fixture.landValue.slope, 50.0f);
    CHECK(d->EstimateDesirability(0, 0) == 87);
    CHECK(d->EstimateDesirability(7, 7) == 87);       // the map reads by cell -> tract
}

void TestStreams() {
    Fixture fixture;
    fixture.MakeExemplar(0x1010, 1, 1, 1, 3);
    auto* d = fixture.MakeDeveloper();
    CHECK(d->Init(fixture.exemplar));
    SetUpMapUpdateScenario(fixture);
    d->StartMapUpdate();
    for (int i = 0; i < 40 && d->MapUpdateStage() != 0; ++i) d->DoIdle(5);
    CHECK(d->IsMapUpdateFinished());
    CHECK(d->PopulationTotal() == 2);
    // Give the capacity total a non-zero value.
    SetLotHost(&fixture.lotHost);
    fixture.lotHost.profile.capacities.resize(1);
    fixture.lotHost.profile.capacities[0][0x1010] = 40;
    fixture.buildingDev.profile.purpose = 1;
    auto* lot = new cSC4Lot(0, 0, 2, 2, 0);
    lot->SetBuildingType(0x1000);
    d->UpdatePopulationCount(lot, 0x1000, true);
    CHECK(d->GetTotalCapacity() == 40);
    delete lot;

    PortMemoryOStream out;
    CHECK(d->Write(out));
    CHECK(fixture.host.writtenGrids.size() == 5);
    CHECK(fixture.host.writtenGrids[0] == static_cast<cIGZSerializable*>(d->ZoneCellCountMap()));
    CHECK(fixture.host.writtenGrids[1] == static_cast<cIGZSerializable*>(d->GetDesirabilityMap()));
    CHECK(fixture.host.writtenGrids[2] == static_cast<cIGZSerializable*>(d->GetPopulationMap()));
    CHECK(fixture.host.writtenGrids[3] == static_cast<cIGZSerializable*>(d->GetCapacityMap()));
    CHECK(fixture.host.writtenGrids[4] == static_cast<cIGZSerializable*>(d->GetMaxCapacityMap()));
    CHECK(out.bytes().size() == 2 + 4 + 4);

    PortMemoryIStream in(out.bytes().data(), out.bytes().size());
    auto* zoneCells = NewGrid<std::uint8_t>(4);
    auto* desirability = NewGrid<std::int16_t>(4);
    auto* population = NewGrid<std::int16_t>(4);
    auto* capacity = NewGrid<std::uint16_t>(4);
    auto* maxCapacity = NewGrid<std::uint16_t>(4);
    desirability->SetTractValue(1, 1, 77);
    fixture.host.gridsToRead = {static_cast<cIGZSerializable*>(zoneCells),
                                static_cast<cIGZSerializable*>(desirability),
                                static_cast<cIGZSerializable*>(population),
                                static_cast<cIGZSerializable*>(capacity),
                                static_cast<cIGZSerializable*>(maxCapacity)};
    CHECK(d->Read(in));
    CHECK(d->ZoneCellCountMap() == zoneCells);
    CHECK(d->GetDesirabilityMap() == desirability);
    CHECK(d->GetPopulationMap() == population);
    CHECK(d->GetCapacityMap() == capacity);
    CHECK(d->GetMaxCapacityMap() == maxCapacity);
    CHECK(d->GetDesirabilityMap()->GetTractValue(1, 1) == 77);
    CHECK(d->PopulationTotal() == 2);
    CHECK(d->GetTotalCapacity() == 40);
    // A wrong version is rejected before anything is read.
    std::vector<unsigned char> bad = out.bytes();
    bad[0] = 2;
    PortMemoryIStream badStream(bad.data(), bad.size());
    CHECK(!d->Read(badStream));
    CHECK(d->ZoneCellCountMap() == zoneCells);
}

}  // namespace

int main() {
    TestComAndLifecycle();
    TestTunables();
    TestTunableDefaults();
    TestSetTractSizeAndCreationInfo();
    TestMaxCapacity();
    TestPopulationCount();
    TestDoMessage();
    TestMapUpdate();
    TestSingleStyleQuirk();
    TestIndustrialShuffle();
    TestLandmarkPass();
    TestEstimateDesirability();
    TestStreams();
    if (g_failures == 0) {
        std::printf("sc4_growthdeveloper ok\n");
        return 0;
    }
    std::fprintf(stderr, "sc4_growthdeveloper: %d failures\n", g_failures);
    return 1;
}
