// cSC4TractDeveloper — unit test. Exercises the recovered data layout / COM plumbing, the tunables, the
// save stream, the style rotation, the candidate-lot pool, the bounding-rect and capacity helpers, the
// build/growth attempt bookkeeping and its queries, the existing-lot sweep, Decline, the lot
// configuration filter/suitability pass and Grow's per-pass setup.
//
// The engine services are fakes behind ITractDeveloperHost; the lots are real cSC4Lot objects (Lot.cpp
// with a fake ILotHost) and the lot configurations are real cSC4LotConfiguration objects loaded from
// property holders (LotConfiguration.cpp), so the growth developer, the lot configuration manager and
// the occupant path are the only neighbours that are faked.

#include "sc4/TractDeveloper.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/LotConfiguration.h"
#include "sc4/LotConfigurationManager.h"
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

#define FAKE_SERVICE_BOILERPLATE                    \
    int refs = 0;                                   \
    void AddRef() override { ++refs; }              \
    void Release() override { if (refs > 0) --refs; } \
    int inits = 0;                                  \
    int shutdowns = 0;                              \
    void Init() override { ++inits; }               \
    void Shutdown() override { ++shutdowns; }

//----------------------------------------------------------------------------------------------- services

struct FakeCity : ITractCityApi {
    FAKE_SERVICE_BOILERPLATE
    int cellsX = 64, cellsZ = 64;
    std::vector<cIGZMessageTarget2*> removed;
    int CellCountX() override { return cellsX; }
    int CellCountZ() override { return cellsZ; }
    void RemoveDeveloper(cIGZMessageTarget2* view) override { removed.push_back(view); }
};

struct FakeLotManager : ITractLotManagerApi {
    FAKE_SERVICE_BOILERPLATE
    std::map<std::pair<int, int>, cSC4Lot*> lots;
    int getCalls = 0;
    cSC4Lot* GetLot(int x, int z, bool) override {
        ++getCalls;
        auto it = lots.find({x, z});
        return it == lots.end() ? nullptr : it->second;
    }
    // Every distinct lot on the cells, in x-then-z order, with a reference each.
    void Collect(std::list<cRZAutoRefCount<cSC4Lot>>& out, int x0, int z0, int x1, int z1) {
        std::set<cSC4Lot*> seen;
        for (const auto& entry : out) seen.insert(entry.get());
        for (int x = x0; x <= x1; ++x)
            for (int z = z0; z <= z1; ++z) {
                auto it = lots.find({x, z});
                if (it != lots.end() && seen.insert(it->second).second) out.push_back(cRZAutoRefCount<cSC4Lot>(it->second));
            }
    }
    int GetLotsInCellRect(std::list<cRZAutoRefCount<cSC4Lot>>& out, int x0, int z0, int x1, int z1, bool) override {
        const std::size_t before = out.size();
        Collect(out, x0, z0, x1, z1);
        return static_cast<int>(out.size() - before);
    }
    int canCreate = 7;
    int CanCreateLot(int, int, int, int, bool, bool, cSC4LotConfiguration*) override { return canCreate; }
    // Edge 1 = the column west of the rect, 2 = the row north, 4 = east, 8 = south (the fake's own convention).
    int GetLotsSurroundingRect(const std::int32_t r[4], std::list<cRZAutoRefCount<cSC4Lot>>& out, int edge) override {
        const std::size_t before = out.size();
        if (edge == 1) Collect(out, r[0] - 1, r[1], r[0] - 1, r[3]);
        if (edge == 2) Collect(out, r[0], r[1] - 1, r[2], r[1] - 1);
        if (edge == 4) Collect(out, r[2] + 1, r[1], r[2] + 1, r[3]);
        if (edge == 8) Collect(out, r[0], r[3] + 1, r[2], r[3] + 1);
        return static_cast<int>(out.size() - before);
    }
    int GetLotsSurroundingLot(cSC4Lot* lot, std::list<cRZAutoRefCount<cSC4Lot>>& out, int edge) override {
        std::int32_t r[4];
        lot->GetBoundingRect(r);
        return GetLotsSurroundingRect(r, out, edge);
    }
    bool createFails = false;
    std::vector<cSC4Lot*> created;      // the manager's reference to each lot it made
    std::vector<int> createdFacings;
    bool CreateLotIfPossible(int x, int z, int sizeX, int sizeZ, int facing, cSC4Lot*& out) override {
        out = nullptr;
        if (createFails) return false;
        out = new cSC4Lot(x, z, sizeX, sizeZ, facing < 0 ? 0 : facing);
        out->AddRef();                  // the caller's
        out->AddRef();                  // the manager's
        out->UpdateZoneType();
        created.push_back(out);
        createdFacings.push_back(facing);
        for (int cx = x; cx < x + sizeX; ++cx)
            for (int cz = z; cz < z + sizeZ; ++cz) lots[{cx, cz}] = out;
        return true;
    }
    std::vector<cSC4Lot*> deleted;
    void DeleteLot(cSC4Lot* lot) override {
        for (auto it = lots.begin(); it != lots.end();) it = it->second == lot ? lots.erase(it) : std::next(it);
        deleted.push_back(lot);
        lot->Release();                 // the manager's
    }
    std::vector<cSC4Lot*> added;        // lots a test placed (the manager's reference)
    ~FakeLotManager() override {
        for (cSC4Lot* lot : added)
            if (std::find(deleted.begin(), deleted.end(), lot) == deleted.end()) lot->Release();
        for (cSC4Lot* lot : created)
            if (std::find(deleted.begin(), deleted.end(), lot) == deleted.end()) lot->Release();
    }
};

struct FakeLotDeveloper : ITractLotDeveloperApi {
    FAKE_SERVICE_BOILERPLATE
    struct Abandoned {
        cSC4Lot* lot;
        int reason;
    };
    std::vector<Abandoned> abandoned;
    std::vector<std::uint32_t> reoccupied;
    int constructions = 0;
    cSC4Lot* lastLot = nullptr;
    cSC4LotConfiguration* lastConfiguration = nullptr;
    std::uint32_t lastBuilding = 0;
    void StartConstruction(cSC4Lot* lot, cSC4LotConfiguration* configuration, std::uint32_t building, bool) override {
        ++constructions;
        lastLot = lot;
        lastConfiguration = configuration;
        lastBuilding = building;
    }
    float GetAreaSlope(const std::int32_t*, bool) override { return 0.0f; }
    void AbandonBuilding(cSC4Lot* lot, int reason) override { abandoned.push_back({lot, reason}); }
    void ReoccupyBuilding(cSC4Lot*, std::uint32_t type) override { reoccupied.push_back(type); }
};

struct FakeBuildingDevelopment : ITractBuildingDevelopmentApi {
    FAKE_SERVICE_BOILERPLATE
    bool cheats = true;
    std::map<std::uint32_t, BuildingProfile> profiles;
    std::uint16_t nextObjectId = 0x500;
    std::vector<std::uint32_t> failed;
    bool CheatCodesEnabled() override { return cheats; }
    const BuildingProfile& GetBuildingProfile(std::uint32_t type) const override {
        static const BuildingProfile empty;
        auto it = profiles.find(type);
        return it == profiles.end() ? empty : it->second;
    }
    std::uint16_t GetFreeConstructionResources() override { return nextObjectId++; }
    bool claim = true;
    bool ClaimConstructionResources(std::uint16_t) override { return claim; }
    void ReleaseConstructionResources(std::uint16_t amount) override { failed.push_back(amount); }
    std::map<std::uint32_t, std::vector<std::uint32_t>> families;
    bool GetBuildingKeyFromType(std::uint32_t type, cGZPersistResourceKey& key) override {
        key = cGZPersistResourceKey{0x6534284a, 0, type};
        return true;
    }
    void GetBuildingTypesByFamily(std::unordered_set<std::uint32_t>& out, std::uint32_t family) override {
        auto it = families.find(family);
        if (it != families.end()) out.insert(it->second.begin(), it->second.end());
    }
    bool CheatIgnorePlumbing() override { return false; }
    bool CheatIgnorePower() override { return false; }
    bool CheatIgnoreCaps() override { return true; }
    bool NeedsAnchorBuilding(const std::int32_t*, std::int32_t) override { return false; }
    std::map<std::pair<std::uint32_t, std::int32_t>, IGrowthDeveloperApi*> developers;
    IGrowthDeveloperApi* GetDeveloper(std::uint32_t purpose, std::int32_t wealth) override {
        auto it = developers.find({purpose, wealth});
        return it == developers.end() ? nullptr : it->second;
    }
};

struct FakeZoneManager : ITractZoneManagerApi {
    FAKE_SERVICE_BOILERPLATE
    int zone = 2;
    int ZoneTypeAt(int, int) override { return zone; }
};

struct FakeService : ITractDeveloperService {
    int refs = 0;
    int inits = 0;
    int shutdowns = 0;
    void AddRef() override { ++refs; }
    void Release() override { if (refs > 0) --refs; }
    void Init() override { ++inits; }
    void Shutdown() override { ++shutdowns; }
};

struct FakeValueMap : ITractValueMap {
    int tractSize = 4;
    int shift = 2;
    std::map<std::pair<int, int>, int> cells;
    int defaultValue = 0;
    std::int32_t GetTractSize() const override { return tractSize; }
    std::int32_t GetTractShift() const override { return shift; }
    std::int32_t GetCellValue(int x, int z) const override {
        auto it = cells.find({x, z});
        return it == cells.end() ? defaultValue : it->second;
    }
};

struct FakeDemand : ITractDemandApi {
    float value = 0.0f;
    float GetValue() const override { return value; }
};

struct FakeGrowthDeveloper : IGrowthDeveloperApi {
    std::int32_t developerType = 0x1010;
    std::int32_t purpose = 1;
    std::int32_t wealth = 1;
    ConstructionProfile construction;
    FakeValueMap desirability, capacity, population, maxCapacity;
    FakeDemand demand;
    std::int32_t GetDeveloperType() const override { return developerType; }
    std::int32_t GetPurpose() const override { return purpose; }
    std::int32_t GetWealth() const override { return wealth; }
    void GetConstructionInfo(ConstructionProfile& out) const override { out = construction; }
    ITractValueMap* GetDesirabilityMap() const override { return const_cast<FakeValueMap*>(&desirability); }
    ITractValueMap* GetCapacityMap() const override { return const_cast<FakeValueMap*>(&capacity); }
    ITractValueMap* GetPopulationMap() const override { return const_cast<FakeValueMap*>(&population); }
    ITractValueMap* GetMaxCapacityMap() const override { return const_cast<FakeValueMap*>(&maxCapacity); }
    ITractDemandApi* GetSourceDemand() const override { return const_cast<FakeDemand*>(&demand); }
    std::map<std::uint32_t, SC4Percentage> styles;
    std::int32_t type = 0;
    std::int32_t GetType() const override { return type; }
    std::uint8_t GetPowerSupplyCap() const override { return 0; }
    std::uint8_t GetWaterSupplyCap() const override { return 0; }
    std::uint8_t GetFireCoverageCap() const override { return 0; }
    std::uint8_t GetParkCap() const override { return 0; }
    std::uint32_t GetMonopolyUse() const override { return 0; }
    const std::map<std::uint32_t, SC4Percentage>& GetStylePercentages() const override { return styles; }
};

struct FakeLotHost : ILotHost {
    LotBuildingProfile profile;
    int day = 100;
    std::uint8_t zone = 2;
    std::uint8_t ZoneTypeAt(int, int) override { return zone; }
    std::int32_t SimulatorDay() override { return day; }
    float TerrainAltitude(float, float) override { return 270.0f; }
    const LotBuildingProfile* BuildingProfile(std::uint32_t type) override { return type ? &profile : nullptr; }
    float SlopeOf(const cSC4Lot&) override { return 1.0f; }
    std::uint32_t DeveloperType(std::uint32_t purpose, std::int32_t wealth) override {
        return purpose * 0x100 + static_cast<std::uint32_t>(wealth);
    }
    void OnDeveloperLotAbandoned(std::uint16_t) override {}
    std::uint8_t LandValueAt(int, int) override { return 0; }
    std::uint32_t RandomPercent() override { return 0; }
    cISC4LotConfigurationRef* LotConfigurationById(std::uint32_t) override { return nullptr; }
    void PostLotMessage(std::uint32_t, const cSC4Lot&, std::uint32_t, std::uint32_t, std::uint32_t,
                        std::int32_t) override {}
    void SetLotHighlight(int, int, bool, float) override {}
    std::int32_t TrafficJobs(const cSC4Lot&, int) override { return 0; }
    void SetTrafficJobs(const cSC4Lot&, int, std::int32_t) override {}
    float ResidentialTravelFactor(int, int) override { return 1.0f; }
    float TravelVolumeFactor() override { return 1.0f; }
    void TravelDistribution(int, float out[3]) override { out[0] = out[1] = out[2] = 0; }
    void JobsPerOccupant(float out[4], int) override { for (int i = 0; i < 4; ++i) out[i] = 0; }
    void ExemplarJobs(const cSC4Lot&, float*) override {}
    bool ExemplarProperty(const cSC4Lot&, std::uint32_t, std::int32_t&) override { return false; }
    bool PowerRequirementExceeded(const cSC4Lot&, const LotBuildingProfile&, bool& available) override {
        available = false;
        return false;
    }
    bool TrafficHasJobAccess(const cSC4Lot&, int) override { return false; }
    void NeighborLots(const cSC4Lot&, int, std::vector<cSC4Lot*>&) override {}
    void GetLotOccupants(const cSC4Lot&, std::vector<cIGZUnknown*>&) override {}
    std::set<std::pair<int, int>> roadLots;   // lots (by bounding-rect origin) with a road on their front
    bool RoadAccessCells(const cSC4Lot& lot, int) override {
        std::int32_t r[4];
        lot.GetBoundingRect(r);
        return roadLots.count({r[0], r[1]}) != 0;
    }
    bool IsFerryTerminal(const cSC4Lot&) override { return false; }
    bool FerryTerminalRoadAccess(const cSC4Lot&) override { return false; }
    cIGZUnknown* FindBuildingOccupant(const cSC4Lot&) override { return nullptr; }
    void WriteSerializable(cIGZOStream&, cIGZUnknown*) override {}
    cIGZUnknown* ReadSerializable(cIGZIStream&, std::uint32_t) override { return nullptr; }
};

struct FakeMessageServer : cIGZMessageServer2 {
    int refs = 0;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 1 || iid == kGZIID_cIGZMessageServer2) {
            *out = static_cast<cIGZMessageServer2*>(this);
            AddRef();
            return true;
        }
        return false;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return refs > 0 ? static_cast<std::uint32_t>(--refs) : 0u; }
    bool MessageSend(cIGZMessage2*) override { return true; }
    bool MessagePost(cIGZMessage2*, bool) override { return true; }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t) override { return true; }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t) override { return true; }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return static_cast<std::uint32_t>(refs); }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

struct FakeHost : ITractDeveloperHost {
    FakeHost() {
        manager = new cSC4LotConfigurationManager();
        manager->AddRef();      // the test's reference; Init/Shutdown add and drop theirs
    }
    ~FakeHost() override { if (manager) manager->Release(); }
    cSC4LotConfigurationManager* manager = nullptr;
    FakeMessageServer server;
    FakeCity city;
    FakeBuildingDevelopment buildingDevelopment;
    FakeService landValue, plumbing, power, traffic;
    FakeLotDeveloper lotDeveloper;
    FakeLotManager lotManager;
    FakeZoneManager zoneManager;
    FakeGrowthDeveloper growthDeveloper;
    std::map<std::uint32_t, FakeGrowthDeveloper*> growthDevelopers;

    bool simulatorPresent = true;
    std::vector<std::pair<int, std::string>> registeredCallbacks;
    int registeredFilterCount = 0;
    std::vector<const cISC4LotConfigurationFilter*> unregisteredFilters;
    std::vector<cSC4LotConfiguration*> configurations;
    bool configurationsPresent = true;
    TractTunables tunablesResult;
    bool tunablesPresent = true;
    int day = 700;
    bool provideCity = true;
    bool provideManager = true;
    std::vector<std::pair<int, int>> visitedCells;
    bool iterateCells = true;

    cIGZMessageServer2* MessageServer() override { return &server; }
    bool SimulatorPresent() override { return simulatorPresent; }
    void RegisterSimulatorCallback(int id, cIGZMessageTarget2*, const char* name) override {
        registeredCallbacks.emplace_back(id, name);
    }
    ITractCityApi* City() override { return provideCity ? &city : nullptr; }
    ITractBuildingDevelopmentApi* BuildingDevelopment() override { return &buildingDevelopment; }
    ITractDeveloperService* LandValue() override { return &landValue; }
    cSC4LotConfigurationManager* LotConfigurationManager() override { return provideManager ? manager : nullptr; }
    ITractLotDeveloperApi* LotDeveloper() override { return &lotDeveloper; }
    ITractLotManagerApi* LotManager() override { return &lotManager; }
    ITractDeveloperService* Plumbing() override { return &plumbing; }
    ITractDeveloperService* Power() override { return &power; }
    ITractDeveloperService* Traffic() override { return &traffic; }
    ITractZoneManagerApi* ZoneManager() override { return &zoneManager; }
    IGrowthDeveloperApi* GrowthDeveloper(std::uint32_t type) override {
        auto it = growthDevelopers.find(type);
        return it == growthDevelopers.end() ? &growthDeveloper : it->second;
    }
    bool LotConfigurationsByFilter(std::vector<cSC4LotConfiguration*>& out,
                                   cISC4LotConfigurationFilter* filter) override {
        if (!configurationsPresent) return 0;
        for (cSC4LotConfiguration* configuration : configurations) {
            if (filter && !filter->Matches(configuration)) continue;
            configuration->AddRef();
            out.push_back(configuration);
        }
        return static_cast<int>(out.size());
    }
    void RegisterLotConfigurationFilter(cISC4LotConfigurationFilter* filter) override {
        ++registeredFilterCount;
        lastFilter = filter;
    }
    void UnregisterLotConfigurationFilter(cISC4LotConfigurationFilter* filter) override {
        unregisteredFilters.push_back(filter);
    }
    bool ReadTunables(TractTunables& io) override {
        if (tunablesPresent) io = tunablesResult;
        return tunablesPresent;
    }
    std::int32_t SimulatorDay() override { return day; }
    std::uint32_t RandomBelow(std::uint32_t range) override { return range ? 0 : 0; }
    void ForEachCellInTract(int tractX, int tractZ, int shift,
                            const std::function<void(int, int)>& fn) override {
        if (!iterateCells) return;
        const int x0 = tractX << shift, z0 = tractZ << shift;
        const int x1 = (tractX + 1) << shift, z1 = (tractZ + 1) << shift;
        for (int z = z0; z < z1; ++z) {
            for (int x = x0; x < x1; ++x) {
                visitedCells.emplace_back(x, z);
                fn(x, z);
            }
        }
    }

    struct RoadCell : ITractNetworkCellApi {
        bool HasNetworkType(int type) const override { return type == 0; }
    } roadCell;
    struct Network : ITractNetworkManagerApi {
        ITractNetworkCellApi* cell = nullptr;
        ITractNetworkCellApi* GetNetworkAt(int, int, std::uint32_t, int) override { return cell; }
    } network;
    bool roads = false;   // every cell next to a lot has a road
    ITractNetworkManagerApi* NetworkManager() override {
        network.cell = roads ? &roadCell : nullptr;
        return &network;
    }
    std::uint8_t LandValueAt(int, int) override { return 0; }
    bool IsCellWatered(int, int) override { return true; }
    std::uint8_t PoweredAt(int, int) override { return 2; }
    bool FireProtectedAt(int, int) override { return true; }
    std::int16_t ParkEffectAt(int, int) override { return 1; }
    bool HasTripLengthAt(int, int) override { return true; }
    int demolitions = 0;
    void DemolishLots(const std::list<cRZAutoRefCount<cSC4Lot>>& lots, std::uint32_t) override {
        demolitions += static_cast<int>(lots.size());
    }
    void BuildingExemplarBool(const cGZPersistResourceKey&, std::uint32_t, bool&) override {}
    std::map<std::uint32_t, std::vector<std::uint32_t>> exemplarArrays;   // property -> values (any building)
    bool BuildingExemplarUint32s(const cGZPersistResourceKey&, std::uint32_t property, std::vector<std::uint32_t>& out) override {
        auto it = exemplarArrays.find(property);
        if (it == exemplarArrays.end()) return false;
        out = it->second;
        return true;
    }
    int redisplays = 0;
    void RedisplayTerrain(const std::int32_t*) override { ++redisplays; }

    const cISC4LotConfigurationFilter* lastFilter = nullptr;
};

// The production filter is a private nested type; the host only ever sees the interface. This helper
// asks it the same question the manager does.
bool FilterMatches(const cISC4LotConfigurationFilter* filter, cSC4LotConfiguration* configuration) {
    return filter && const_cast<cISC4LotConfigurationFilter*>(filter)->Matches(configuration);
}

//----------------------------------------------------------------------------------------------- helpers

cSC4LotConfiguration* MakeConfiguration(std::uint32_t id, std::uint8_t growthStage,
                                        std::vector<std::uint8_t> purposes,
                                        std::vector<std::uint8_t> wealthTypes, std::uint16_t minCapacity,
                                        std::uint16_t maxCapacity, std::vector<std::uint32_t> styles,
                                        bool withBuilding = true, std::vector<std::uint8_t> size = {2, 2},
                                        std::uint8_t zone = 2) {
    cSCPropertyHolder holder;
    holder.AddRef();
    cSCPropertyHelp::SetPropertyValue(&holder, kLotConfigVersion, static_cast<std::uint8_t>(2));
    cSCPropertyHelp::SetPropertyValue(&holder, kLotConfigSize, size);
    cSCPropertyHelp::SetPropertyValue(&holder, kLotConfigGrowthStage, growthStage);
    cSCPropertyHelp::SetPropertyValue(&holder, kLotConfigPurposeTypes, purposes);
    cSCPropertyHelp::SetPropertyValue(&holder, kLotConfigWealthTypes, wealthTypes);
    cSCPropertyHelp::SetPropertyValue(&holder, kLotConfigZoneTypes, std::vector<std::uint8_t>{zone});
    if (withBuilding) {
        // version 2 object: type 0 (building), flag 0, orientation 0, x/y/z 0, limits 0, value, id, 2 extras
        cSCPropertyHelp::SetPropertyValue(&holder, kLotConfigObjectFirst,
                                          std::vector<std::uint32_t>{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x1234, 7,
                                                                     0, 0});
    }
    auto* configuration = new cSC4LotConfiguration();
    configuration->AddRef();
    configuration->LoadFromProperties(holder, id, 0);
    configuration->SetBuildingInformation(minCapacity, maxCapacity, std::move(styles), true);
    return configuration;
}

//----------------------------------------------------------------------------------------------- main test

void TestLayoutAndCom() {
    cSC4TractDeveloper* developer = new cSC4TractDeveloper();
    CHECK(developer->RefCount() == 0);
    CHECK(developer->GetGZCLSID() == 0x2990c142);
    CHECK(developer->GetCurrentStyle() == 0);
    CHECK(developer->GetYearsBetweenStyleChanges() == 0);
    CHECK(!developer->IsUsingAllStylesAtOnce());
    CHECK(developer->GetActiveStyles().empty());
    CHECK(cSC4TractDeveloper::kClassSize == 0x268);

    developer->UseAllStylesAtOnce(true);
    CHECK(developer->IsUsingAllStylesAtOnce());
    developer->UseAllStylesAtOnce(false);
    CHECK(!developer->IsUsingAllStylesAtOnce());

    developer->SetYearsBetweenStyleChanges(12);
    CHECK(developer->GetYearsBetweenStyleChanges() == 12);

    // three views and one shared reference count
    void* out = nullptr;
    CHECK(developer->QueryInterface(1, &out) && out == static_cast<cISC4TractDeveloper*>(developer));
    CHECK(developer->RefCount() == 1);
    void* message = nullptr;
    CHECK(developer->QueryInterface(0x452294aa, &message) &&
          message == static_cast<cIGZMessageTarget2*>(developer));
    void* serializable = nullptr;
    CHECK(developer->QueryInterface(0xe4fda3d4, &serializable) &&
          serializable == static_cast<cIGZSerializable*>(developer));
    CHECK(developer->RefCount() == 3);
    CHECK(!developer->QueryInterface(0x12345678, &out));
    CHECK(developer->AddRef() == 4);
    CHECK(developer->Release() == 3);
    CHECK(developer->Release() == 2);
    CHECK(developer->Release() == 1);
    CHECK(developer->Release() == 0);   // deletes the object

    // Candidate/BuildAttempt/GrowthAttempt record sizes are asserted in the header; the growth record
    // holds the grown pass first and the failed pass second.
    CHECK(sizeof(GrowthRecord) == 0xe0);
    CHECK(sizeof(GrowthAttemptRecord) == 0x70);
    CHECK(sizeof(BuildAttemptRecord) == 0x58);
    CHECK(sizeof(CandidateLot) == 0xf0);
}

void TestStyles() {
    cSC4TractDeveloper developer;
    std::vector<std::uint32_t> styles{0x2000, 0x2001, 0x2002};
    developer.SetActiveStyles(styles);
    CHECK(developer.GetActiveStyles().size() == 3);
    CHECK(developer.GetCurrentStyle() == 0x2000);
    // the index follows the current style into the new list
    developer.SetActiveStyles(std::vector<std::uint32_t>{0x3000, 0x2000, 0x3001});
    CHECK(developer.GetCurrentStyle() == 0x2000);
    developer.SetActiveStyles(std::vector<std::uint32_t>{0x3000, 0x3001});
    CHECK(developer.GetCurrentStyle() == 0x3001);   // current style gone: the kept index is reused
}

void TestTunables() {
    FakeHost host;
    SetTractDeveloperHost(&host);
    host.tunablesResult.capacityFactor = 2.5f;
    host.tunablesResult.declineFactor = 0.5f;
    host.tunablesResult.aggregationPenalty = 7.0f;
    host.tunablesResult.allowTakeOver = false;
    host.tunablesResult.styles = {0x2000, 0x2001};
    host.tunablesResult.hasYearsBetweenStyleChanges = true;
    host.tunablesResult.yearsBetweenStyleChanges = 5;

    cSC4TractDeveloper developer;
    developer.ReadSimulatorTunableValues();
    CHECK(developer.GetActiveStyles().size() == 2 && developer.GetActiveStyles()[1] == 0x2001);
    CHECK(developer.GetCurrentStyle() == 0x2000);
    CHECK(developer.GetYearsBetweenStyleChanges() == 5);

    // without the years property the default 0x28 is used, and an out-of-range style index wraps to 0
    FakeHost host2;
    host2.tunablesResult.styles = {0x2000, 0x2001, 0x2002};
    SetTractDeveloperHost(&host2);
    cSC4TractDeveloper developer2;
    developer2.ReadSimulatorTunableValues();
    CHECK(developer2.GetYearsBetweenStyleChanges() == 0x28);
    CHECK(developer2.GetCurrentStyle() == 0x2000);
    SetTractDeveloperHost(nullptr);
}

void TestStream() {
    cSC4TractDeveloper developer;
    developer.SetActiveStyles(std::vector<std::uint32_t>{0x2000, 0x2005, 0x2009});
    developer.SetYearsBetweenStyleChanges(9);
    developer.UseAllStylesAtOnce(true);

    PortMemoryOStream out;
    CHECK(developer.Write(out));

    cSC4TractDeveloper loaded;
    PortMemoryIStream in(out.bytes().data(), out.bytes().size());
    CHECK(loaded.Read(in));
    CHECK(loaded.GetActiveStyles().size() == 3 && loaded.GetActiveStyles()[1] == 0x2005);
    CHECK(loaded.GetYearsBetweenStyleChanges() == 9);
    CHECK(loaded.IsUsingAllStylesAtOnce());

    // version 1: the three default styles are installed and all styles are used at once
    std::vector<std::uint8_t> version1;
    auto put16 = [&version1](std::uint16_t v) {
        version1.push_back(static_cast<std::uint8_t>(v & 0xff));
        version1.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    auto put32 = [&version1](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) version1.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xff));
    };
    put16(1);
    put32(2);    // style index
    put32(7);    // years in style
    cSC4TractDeveloper v1;
    PortMemoryIStream in1(version1.data(), version1.size());
    CHECK(v1.Read(in1));
    CHECK(v1.GetActiveStyles().size() == 3 && v1.GetActiveStyles()[0] == 0x2000 &&
          v1.GetActiveStyles()[2] == 0x2002);
    CHECK(!v1.IsUsingAllStylesAtOnce());   // version 1 left the object in single-style mode

    // a future version is rejected
    std::vector<std::uint8_t> version4;
    version4.push_back(4);
    version4.push_back(0);
    cSC4TractDeveloper v4;
    PortMemoryIStream in4(version4.data(), version4.size());
    CHECK(!v4.Read(in4));

    // truncated stream
    std::vector<std::uint8_t> truncated;
    truncated.push_back(3);
    truncated.push_back(0);
    cSC4TractDeveloper vt;
    PortMemoryIStream int3(truncated.data(), truncated.size());
    CHECK(!vt.Read(int3));
}

void TestDoMessage() {
    cSC4TractDeveloper developer;
    developer.SetActiveStyles(std::vector<std::uint32_t>{0x2000, 0x2001, 0x2002});
    developer.SetYearsBetweenStyleChanges(2);
    // the message target's DoMessage is reached through the secondary view
    cIGZMessageTarget2* target = static_cast<cIGZMessageTarget2*>(&developer);

    for (int i = 0; i < 10; ++i) developer.DoMessage(nullptr);
    CHECK(developer.GetCurrentStyle() == 0x2000);   // no message of the tick type yet

    cRZMessage2Standard message;
    message.SetType(0x66956817);
    // style index 0, years 0: first tick sets years 1 (2 <= 1 false); second tick wraps and advances
    CHECK(target->DoMessage(&message));
    CHECK(developer.GetCurrentStyle() == 0x2000);
    CHECK(target->DoMessage(&message));
    CHECK(developer.GetCurrentStyle() == 0x2001);
    CHECK(target->DoMessage(&message));
    CHECK(target->DoMessage(&message));
    CHECK(developer.GetCurrentStyle() == 0x2002);
    CHECK(target->DoMessage(&message));
    CHECK(target->DoMessage(&message));
    // quirk: the index wraps one step early, at size, not size - 1
    CHECK(developer.GetCurrentStyle() == 0x2000);
    CHECK(target->DoMessage(nullptr));
    message.SetType(0x12345678);
    CHECK(target->DoMessage(&message));
    CHECK(developer.GetCurrentStyle() == 0x2000);
}

void TestCandidatePool() {
    cSC4TractDeveloper::ClearCandidatePool();
    cSC4TractDeveloper developer;
    CHECK(developer.CandidatePoolSize() == 0);
    CandidateLot* first = developer.NewCandidateLot();
    CHECK(first != nullptr);
    CHECK(developer.CandidatePoolSize() == 9);   // ten were allocated
    first->kind = 3;
    first->score = 5.0f;
    first->sizeX = 4;
    first->neighbours.insert(reinterpret_cast<cSC4Lot*>(0x1000));
    developer.DeleteCandidateLot(first);
    CHECK(developer.CandidatePoolSize() == 10);
    CandidateLot* second = developer.NewCandidateLot();
    CHECK(second == first);                     // the pool hands back the same object
    CHECK(second->kind == 0 && second->score == 0.0f && second->sizeX == 0 && second->neighbours.empty());

    std::vector<CandidateLot*> list{second, developer.NewCandidateLot()};
    developer.ClearCandidateList(list);
    CHECK(list.empty());

    // CompareCandidateLots is a strict weak ordering on the score
    CandidateLot a, b;
    a.score = 1.0f;
    b.score = 2.0f;
    CHECK(cSC4TractDeveloper::CompareCandidateLots(&a, &b));
    CHECK(!cSC4TractDeveloper::CompareCandidateLots(&b, &a));
    CHECK(!cSC4TractDeveloper::CompareCandidateLots(&a, &a));
    cSC4TractDeveloper::ClearCandidatePool();
}

void TestBoundingRect() {
    cSC4TractDeveloper developer;
    CandidateLot c;
    CHECK(c.rectX0 == -1 && c.rectZ0 == -1 && c.rectX1 == -1 && c.rectZ1 == -1);
    CHECK(c.existingCapacity == -1 && c.abandonedCapacity == -1);

    c.facing = 0;
    c.x = 5;
    c.z = 5;
    c.sizeX = 3;
    c.sizeZ = 2;
    developer.GetBoundingRect(&c);
    CHECK(c.rectX0 == 3 && c.rectZ0 == 5 && c.rectX1 == 5 && c.rectZ1 == 6);

    c = CandidateLot();
    c.facing = 1;
    c.x = 5;
    c.z = 5;
    c.sizeX = 3;
    c.sizeZ = 2;
    developer.GetBoundingRect(&c);
    CHECK(c.rectX0 == 4 && c.rectZ0 == 3 && c.rectX1 == 5 && c.rectZ1 == 5);

    c = CandidateLot();
    c.facing = 2;
    c.x = 5;
    c.z = 5;
    c.sizeX = 3;
    c.sizeZ = 2;
    developer.GetBoundingRect(&c);
    CHECK(c.rectX0 == 5 && c.rectZ0 == 4 && c.rectX1 == 7 && c.rectZ1 == 5);

    c = CandidateLot();
    c.facing = 3;
    c.x = 5;
    c.z = 5;
    c.sizeX = 3;
    c.sizeZ = 2;
    developer.GetBoundingRect(&c);
    CHECK(c.rectX0 == 5 && c.rectZ0 == 5 && c.rectX1 == 6 && c.rectZ1 == 7);

    // unknown facing leaves the rectangle untouched
    c = CandidateLot();
    c.facing = 9;
    c.x = 5;
    developer.GetBoundingRect(&c);
    CHECK(c.rectX0 == -1 && c.rectZ0 == -1 && c.rectX1 == -1 && c.rectZ1 == -1);

    // facing -1 takes the lot's own footprint
    FakeLotHost lotHost;
    SetLotHost(&lotHost);
    auto* lot = new cSC4Lot(10, 20, 3, 2, 2);
    c = CandidateLot();
    c.facing = -1;
    c.lot = lot;
    developer.GetBoundingRect(&c);
    CHECK(c.facing == 2 && c.x == 10 && c.z == 21 && c.sizeX == 3 && c.sizeZ == 2);
    CHECK(c.rectX0 == 10 && c.rectZ0 == 20 && c.rectX1 == 12 && c.rectZ1 == 21);
    delete lot;
    SetLotHost(nullptr);
}

void TestExistingLotsAndDecline() {
    FakeHost host;
    FakeLotHost lotHost;
    SetTractDeveloperHost(&host);
    SetLotHost(&lotHost);
    host.growthDeveloper.desirability.shift = 1;   // tract (2,3) covers cells x 4..5, z 6..7
    cSC4TractDeveloper developer;
    CHECK(developer.Init());

    auto* first = new cSC4Lot(4, 6, 1, 1, 0);
    auto* second = new cSC4Lot(5, 7, 1, 1, 0);
    host.lotManager.lots[{4, 6}] = first;
    host.lotManager.lots[{5, 7}] = second;
    host.lotManager.lots[{8, 8}] = second;   // outside the tract

    GrowthProfile profile;
    profile.developer = &host.growthDeveloper;
    profile.tractX = 2;
    profile.tractZ = 3;
    std::list<int> growOutcomes;
    bool growDeclined = true;
    developer.Grow(profile, growOutcomes, growDeclined);
    CHECK(developer.ExistingLots().size() == 2);   // each pass lists the tract's lots
    host.visitedCells.clear();

    developer.ListExistingLots();
    CHECK(developer.ExistingLots().size() == 2);
    CHECK(developer.ExistingLots().count(first) == 1 && developer.ExistingLots().count(second) == 1);
    CHECK(host.visitedCells.size() == 4);   // the whole 2x2 tract was visited

    // Decline: only livable lots of the developer's purpose/wealth and 59+ days old are abandoned
    lotHost.profile.purpose = 1;
    lotHost.profile.wealth = 1;
    lotHost.profile.capacities = {{{0x1010, 12}}};
    lotHost.day = 100;
    first->SetBuildingType(0x1234);
    second->SetBuildingType(0x1234);
    first->SetState(kLotStateLivable, 1, 0);
    second->SetState(kLotStateLivable, 1, 0);
    CHECK(first->GetState() == kLotStateLivable);
    CHECK(first->GetOccupantWealth() == 1);
    lotHost.day = 200;
    CHECK(first->GetDaysSinceLastStateChange() == 100);

    host.buildingDevelopment.profiles[0x1234] = lotHost.profile;
    CHECK(developer.Decline(profile) == 24);
    CHECK(host.lotDeveloper.abandoned.size() == 2);
    if (host.lotDeveloper.abandoned.size() == 2) {
        CHECK(host.lotDeveloper.abandoned[0].reason == 6);
        CHECK(host.lotDeveloper.abandoned[0].lot == first);
        CHECK(host.lotDeveloper.abandoned[1].lot == second);
    }

    // a different wealth does not decline
    host.lotDeveloper.abandoned.clear();
    host.growthDeveloper.wealth = 2;
    CHECK(developer.Decline(profile) == 0);
    CHECK(host.lotDeveloper.abandoned.empty());

    developer.Shutdown();
    delete first;
    delete second;
    SetLotHost(nullptr);
    SetTractDeveloperHost(nullptr);
}

void TestSuitability() {
    FakeHost host;
    SetTractDeveloperHost(&host);
    host.growthDeveloper.purpose = 1;
    host.growthDeveloper.wealth = 1;
    host.buildingDevelopment.cheats = true;

    cSC4TractDeveloper developer;
    CHECK(developer.Init());
    GrowthProfile profile;
    profile.developer = &host.growthDeveloper;
    profile.tractX = 0;
    profile.tractZ = 0;
    profile.growthStage = 2;   // the tract has reached stage 2, so stage 1 and 2 configurations fit
    std::list<int> outcomes;
    bool declined = true;
    developer.Grow(profile, outcomes, declined);
    CHECK(!declined);
    CHECK(developer.CurrentAttempt().histograms.size() == 13);
    developer.SetSecondAttempt(false);

    // purpose mismatch / wealth mismatch / growth stage
    cSC4LotConfiguration* wrongPurpose = MakeConfiguration(1, 1, {2}, {1}, 0, 10, {});
    cSC4LotConfiguration* wrongWealth = MakeConfiguration(2, 1, {1}, {2}, 0, 10, {});
    cSC4LotConfiguration* highStage = MakeConfiguration(3, 3, {1}, {1}, 0, 10, {});
    CHECK(developer.IsLotConfigurationSuitable(wrongPurpose) == kTractOutcomePurposeMismatch);
    CHECK(developer.IsLotConfigurationSuitable(wrongWealth) == kTractOutcomeWealthMismatch);
    CHECK(developer.IsLotConfigurationSuitable(highStage) == kTractOutcomeGrowthStageTooHigh);
    CHECK(developer.Histograms()[kHistogramSuitable][kTractOutcomePurposeMismatch] == 1);
    CHECK(developer.Histograms()[kHistogramSuitable][kTractOutcomeWealthMismatch] == 1);
    CHECK(developer.Histograms()[kHistogramSuitable][kTractOutcomeGrowthStageTooHigh] == 1);

    // a configuration without a building object rejects with outcome 0
    cSC4LotConfiguration* empty = MakeConfiguration(4, 1, {1}, {1}, 0, 10, {}, false);
    CHECK(developer.IsLotConfigurationSuitable(empty) == kTractOutcomeNone);

    // style mismatch: the pass grew a tract with style 0x2000 and the config takes only 0x2001
    host.tunablesResult.styles = {0x2000};
    developer.ReadSimulatorTunableValues();
    CHECK(developer.GetActiveStyles().size() == 1 && developer.GetActiveStyles()[0] == 0x2000);
    cSC4LotConfiguration* wrongStyle = MakeConfiguration(5, 1, {1}, {1}, 0, 10, {0x2001});
    CHECK(developer.IsLotConfigurationSuitable(wrongStyle) == kTractOutcomeStyleMismatch);
    cSC4LotConfiguration* matchingStyle = MakeConfiguration(6, 1, {1}, {1}, 0, 10, {0x2000});
    CHECK(developer.IsLotConfigurationSuitable(matchingStyle) == kTractOutcomeSuccess);

    // maximumBuildAttempts_ is 1 here: a configuration whose maximum capacity is 0 is too small
    cSC4LotConfiguration* tooSmall = MakeConfiguration(7, 1, {1}, {1}, 0, 0, {0x2000});
    CHECK(developer.IsLotConfigurationSuitable(tooSmall) == kTractOutcomeCapacityTooLow);
    CHECK(developer.Histograms()[kHistogramSuitable][kTractOutcomeSuccess] == 1);

    wrongPurpose->Release();
    wrongWealth->Release();
    highStage->Release();
    empty->Release();
    wrongStyle->Release();
    matchingStyle->Release();
    tooSmall->Release();
    developer.Shutdown();
    SetTractDeveloperHost(nullptr);
}

void TestConfigurationPass() {
    FakeHost host;
    SetTractDeveloperHost(&host);
    host.growthDeveloper.purpose = 1;
    host.growthDeveloper.wealth = 1;

    cSC4TractDeveloper developer;
    CHECK(developer.Init());
    GrowthProfile profile;
    profile.developer = &host.growthDeveloper;
    profile.growthStage = 2;
    std::list<int> outcomes;
    bool declined = true;
    developer.Grow(profile, outcomes, declined);
    CHECK(developer.SecondAttempt());   // the fruitless first pass was followed by a second one
    developer.SetSecondAttempt(false);

    cSC4LotConfiguration* suitable = MakeConfiguration(11, 1, {1}, {1}, 0, 10, {});
    cSC4LotConfiguration* small = MakeConfiguration(12, 1, {1}, {1}, 0, 0, {});
    host.configurations = {suitable, small};
    CHECK(developer.ListPossibleLotConfigurations() == kTractOutcomeSuccess);
    CHECK(developer.CachedFilterCount() == 1);
    CHECK(developer.CachedFilter(1, 1) != nullptr);
    CHECK(developer.CachedFilter(1, 2) == nullptr);
    CHECK(host.registeredFilterCount == 1);
    CHECK(developer.Histograms()[kHistogramConfigurations][kTractOutcomeSuccess] == 1);
    CHECK(developer.Histograms()[kHistogramConfigurations][kTractOutcomeCapacityTooLow] == 1);
    CHECK(developer.SuitableConfigurations().size() == 1);
    CHECK(developer.SuitableConfigurations().count(suitable) == 1);

    // the same {wealth, purpose} pair reuses the cached filter
    developer.ListPossibleLotConfigurations();
    CHECK(developer.CachedFilterCount() == 1);
    CHECK(host.registeredFilterCount == 1);
    // and the filter matches only configurations of that purpose and wealth
    CHECK(FilterMatches(host.lastFilter, suitable));
    CHECK(!FilterMatches(nullptr, suitable));
    host.growthDeveloper.wealth = 2;
    cSC4LotConfiguration* otherWealth = MakeConfiguration(13, 1, {1}, {2}, 0, 10, {});
    host.configurations = {suitable, otherWealth};
    CHECK(developer.ListPossibleLotConfigurations() == kTractOutcomeSuccess);
    CHECK(developer.CachedFilterCount() == 2);   // a different wealth gets its own filter
    CHECK(host.registeredFilterCount == 2);

    // no configurations at all: outcome 5
    host.configurationsPresent = false;
    CHECK(developer.ListPossibleLotConfigurations() == kTractOutcomeNoConfiguration);

    suitable->Release();
    small->Release();
    otherWealth->Release();
    developer.Shutdown();
    CHECK(host.unregisteredFilters.size() == 2);
    SetTractDeveloperHost(nullptr);
}

void TestAttemptRecords() {
    FakeHost host;
    SetTractDeveloperHost(&host);
    host.growthDeveloper.developerType = 0x1010;
    host.growthDeveloper.purpose = 1;
    host.growthDeveloper.wealth = 1;
    host.growthDeveloper.desirability.shift = 2;
    host.day = 900;

    cSC4TractDeveloper developer;
    CHECK(developer.Init());
    GrowthProfile profile;
    profile.developer = &host.growthDeveloper;
    profile.tractX = 3;
    profile.tractZ = 4;
    std::list<int> outcomes;
    bool declined = true;
    host.buildingDevelopment.cheats = false;   // Grow records nothing without the debug build
    CHECK(developer.Grow(profile, outcomes, declined) == 0);
    CHECK(!declined);
    host.buildingDevelopment.cheats = true;
    // two passes (tract size 0: 0*0 + 1), both without configurations, so no outcome is listed
    CHECK(outcomes.empty());

    // Grow's per-pass setup
    const GrowthAttemptRecord& attempt = developer.CurrentAttempt();
    CHECK(attempt.day == 900);
    CHECK(attempt.developer == &host.growthDeveloper);
    CHECK(attempt.tractX == 3 && attempt.tractZ == 4);
    CHECK(attempt.histograms.size() == 13);
    CHECK(attempt.histograms[0].size() == 28);
    CHECK(attempt.objectId == 0x500);
    CHECK(host.buildingDevelopment.nextObjectId == 0x501);

    // RecordBuildAttempt is a no-op unless the cheat/debug build is on
    host.buildingDevelopment.cheats = false;
    developer.RecordBuildAttempt();
    developer.RecordGrowthAttempt();
    CHECK(developer.BuildAttempts().empty());
    CHECK(developer.GrowthDatabase().empty());
    host.buildingDevelopment.cheats = true;

    // a build attempt copies the candidate lots deeply
    CandidateLot* candidate = developer.NewCandidateLot();
    candidate->kind = 2;
    candidate->score = 3.0f;
    developer.Candidates().push_back(candidate);
    developer.RecordBuildAttempt();
    CHECK(developer.BuildAttempts().size() == 1);
    const BuildAttemptRecord& record = developer.BuildAttempts().front();
    CHECK(record.candidates.size() == 1);
    CHECK(record.candidates[0] != candidate);
    CHECK(record.candidates[0]->score == 3.0f && record.candidates[0]->kind == 2);
    record.candidates[0]->score = 9.0f;
    CHECK(candidate->score == 3.0f);   // deep copy
    CHECK(record.outcomeCount == 0 && record.attempted == 1);   // Grow left the developer on its second attempt

    // nothing was built: RecordGrowthAttempt writes the *failed* attempt
    developer.RecordGrowthAttempt();
    const auto& byType = developer.GrowthDatabase();
    CHECK(byType.size() == 1);
    const auto& byTract = byType.at(0x1010);
    CHECK(byTract.size() == 1);
    CHECK(byTract.at({3, 4}).failed.day == 900);
    CHECK(byTract.at({3, 4}).failed.attempts.size() == 1);
    CHECK(byTract.at({3, 4}).failed.attempts.front().candidates.size() == 1);
    CHECK(byTract.at({3, 4}).failed.histograms.size() == 13);
    CHECK(byTract.at({3, 4}).grown.day == 0);

    // nothing has grown yet, so the successful query finds nothing; the failed one finds the record
    host.growthDevelopers[0x1010] = &host.growthDeveloper;
    std::list<std::uint32_t> types{0x1010};
    CHECK(developer.GetLastSuccessfulBuild(3 << 2, 4 << 2, types) == nullptr);
    const GrowthAttemptRecord* failed = developer.GetLastFailedBuild(3 << 2, 4 << 2, 0x1010);
    CHECK(failed != nullptr && failed->day == 900);
    CHECK(developer.GetLastFailedBuild(3, 4, 0x2222) == nullptr);

    // a built pass (builtCount >= 1) writes the grown attempt and clears the failed one
    developer.AccumulateBuiltCapacity(7);
    CHECK(developer.BuiltCapacity() == 7);
    developer.CurrentAttempt().day = 950;
    developer.CurrentAttempt().attempts.clear();
    developer.CurrentAttempt().histograms.assign(13, std::vector<std::uint32_t>(28, 0));
    developer.RecordGrowthAttempt();
    const GrowthRecord& recordAfterBuild = developer.GrowthDatabase().at(0x1010).at({3, 4});
    CHECK(recordAfterBuild.grown.day == 950);
    // RecordGrowthAttempt replaces only the record of its own kind, so the earlier failed pass keeps
    // answering the failed query (its day is 900, not 0).
    CHECK(recordAfterBuild.failed.day == 900);
    const GrowthRecord* successful = developer.GetLastSuccessfulBuild(3 << 2, 4 << 2, types);
    CHECK(successful != nullptr && successful->grown.day == 950);
    CHECK(developer.GetLastFailedBuild(3 << 2, 4 << 2, 0x1010) != nullptr);   // the failed pass is kept

    // FreeGrowthDatabase empties the map and frees the attempt copies
    developer.FreeGrowthDatabase();
    CHECK(developer.GrowthDatabase().empty());
    developer.Shutdown();
    SetTractDeveloperHost(nullptr);
}

void TestScoreCandidateLots() {
    FakeHost host;
    FakeLotHost lotHost;
    SetTractDeveloperHost(&host);
    SetLotHost(&lotHost);
    cSC4TractDeveloper developer;
    CHECK(developer.Init());

    host.growthDeveloper.purpose = 1;      // residential: network weights 10 / 2 / 1
    host.growthDeveloper.developerType = 0x1010;
    host.growthDeveloper.desirability.shift = 0;
    GrowthProfile profile;
    profile.developer = &host.growthDeveloper;
    profile.tractX = 0;
    profile.tractZ = 0;
    std::list<int> outcomes;
    bool declined = true;
    developer.Grow(profile, outcomes, declined);

    lotHost.profile.purpose = 1;
    lotHost.profile.wealth = 1;
    lotHost.profile.capacities = {{{0x1010, 12}}};
    host.buildingDevelopment.profiles[0x1234] = lotHost.profile;
    auto* lot = new cSC4Lot(1, 1, 1, 1, 0);
    lot->SetBuildingType(0x1234);
    lot->SetState(kLotStateLivable, 1, 0);   // not abandoned: all 12 go to the existing capacity

    CandidateLot* existing = developer.NewCandidateLot();
    existing->kind = 2;
    existing->lot = lot;
    existing->maxGrowthStage = 1;
    developer.Candidates().push_back(existing);

    // a candidate that is already measured keeps its capacity fields
    CandidateLot* measured = developer.NewCandidateLot();
    measured->kind = 2;
    measured->existingCapacity = 5;
    measured->abandonedCapacity = 0;
    measured->rails[0] = 1;
    measured->avenues[0] = 2;
    measured->roads[0] = 3;
    developer.Candidates().push_back(measured);

    CHECK(developer.ScoreCandidateLots() == 0);
    // measured: 5 * 1.0 + 0 * 0.2 + 1*10 + 2*2 + 3*1 = 22
    CHECK(measured->score == 22.0f);
    // existing: 12 * 1.0 (existing weight) and the four zero network edges
    CHECK(existing->score == 12.0f);
    CHECK(existing->existingCapacity == 12 && existing->abandonedCapacity == 0);
    CHECK(existing->maxWealth == 1);   // the profile's wealth, which is what the original stores at +0x44

    // the comparator orders the list by score
    CHECK(cSC4TractDeveloper::CompareCandidateLots(existing, measured));
    CHECK(!cSC4TractDeveloper::CompareCandidateLots(measured, existing));

    // the lot's growth stage comes from its configuration, which the lot holds as the manager's reference (by id): a stage 3
    // configuration raises the candidate's highest stage to 3 (the reference itself is no configuration and was read as one)
    struct Reference final : cISC4LotConfigurationRef {
        std::uint32_t id = 0;
        bool QueryInterface(std::uint32_t, void**) override { return false; }
        std::uint32_t AddRef() override { return 2; }
        std::uint32_t Release() override { return 1; }
        std::uint32_t GetID() const override { return id; }
    } reference;
    cSC4LotConfiguration* staged = MakeConfiguration(0x5150, 3, {1}, {1}, 1, 20, {0x2000});
    CHECK(host.manager->AddLotConfiguration(staged, true));
    reference.id = 0x5150;
    lot->SetLotConfiguration(&reference);
    existing->existingCapacity = existing->abandonedCapacity = -1;   // not measured yet
    existing->maxGrowthStage = 1;
    CHECK(developer.ScoreCandidateLots() == 0);
    CHECK(existing->maxGrowthStage == 3);
    lot->SetLotConfiguration(nullptr);
    host.manager->AddLotConfiguration(staged, false);   // out of the process-wide table again
    staged->Release();

    delete lot;
    developer.ClearCandidateList(developer.Candidates());
    developer.Shutdown();
    SetLotHost(nullptr);
    SetTractDeveloperHost(nullptr);
}

void TestInitAndShutdown() {
    FakeHost host;
    SetTractDeveloperHost(&host);
    cSC4TractDeveloper* developer = new cSC4TractDeveloper();
    CHECK(developer->Init());
    CHECK(developer->IsInitialized());
    CHECK(host.city.refs == 1);
    CHECK(host.buildingDevelopment.refs == 1);
    CHECK(host.landValue.refs == 1);
    CHECK(host.lotDeveloper.refs == 1);
    CHECK(host.lotManager.refs == 1);
    CHECK(host.plumbing.refs == 1 && host.power.refs == 1 && host.traffic.refs == 1);
    CHECK(host.zoneManager.refs == 1);
    CHECK(host.server.refs == 1);
    CHECK(host.registeredCallbacks.size() == 2);
    CHECK(host.registeredCallbacks[0].first == 6 && host.registeredCallbacks[1].first == 7);
    CHECK(host.registeredCallbacks[0].second == "cSC4TractDeveloper");

    // a second Init is a no-op
    CHECK(developer->Init());
    CHECK(host.server.refs == 1);

    developer->AddRef();
    CHECK(developer->Release() == 0);
    CHECK(host.city.removed.size() == 1);
    CHECK(host.city.removed[0] == static_cast<cIGZMessageTarget2*>(developer) ||
          host.city.removed[0] != nullptr);
    CHECK(host.city.refs == 0 && host.buildingDevelopment.refs == 0 && host.landValue.refs == 0);
    CHECK(host.lotDeveloper.refs == 0 && host.lotManager.refs == 0 && host.zoneManager.refs == 0);
    CHECK(host.server.refs == 0);

    // a missing global makes Init fail and keeps the object uninitialised
    FakeHost bare;
    bare.provideCity = false;
    SetTractDeveloperHost(&bare);
    cSC4TractDeveloper* failing = new cSC4TractDeveloper();
    CHECK(!failing->Init());
    CHECK(!failing->IsInitialized());
    CHECK(bare.buildingDevelopment.refs == 0);   // the city is acquired first
    failing->AddRef();
    CHECK(failing->Release() == 0);               // Shutdown is a no-op on an uninitialised object
    SetTractDeveloperHost(nullptr);
}

void TestNullHost() {
    SetTractDeveloperHost(nullptr);
    cSC4TractDeveloper developer;
    CHECK(!developer.Init());
    CHECK(developer.Decline(GrowthProfile()) == 0);
    std::list<int> outcomes;
    bool declined = true;
    CHECK(developer.Grow(GrowthProfile(), outcomes, declined) == 0);
    CHECK(!declined);
    CHECK(developer.ListPossibleLotConfigurations() == kTractOutcomeNone);
    developer.RecordBuildAttempt();
    developer.RecordGrowthAttempt();
    developer.ListExistingLots();
    CHECK(developer.GetLastSuccessfulBuild(0, 0, {}) == nullptr);
    CHECK(developer.GetLastFailedBuild(0, 0, 0x1010) == nullptr);
    developer.FreeGrowthDatabase();
}

}  // namespace

// An empty 2x2 zoned lot with road access, one suitable 2x2 configuration and one building of the right purpose, wealth and
// capacity: Grow lists the lot, accepts it (IsLotSuitable, IsLotConfigurationCompatible), picks the configuration and the
// building, claims the capacity, clears the lot and starts the construction.
void TestBuildOnEmptyLot() {
    FakeHost host;
    FakeLotHost lotHost;
    SetTractDeveloperHost(&host);
    SetLotHost(&lotHost);
    host.growthDeveloper.purpose = 1;
    host.growthDeveloper.wealth = 1;
    host.growthDeveloper.construction.purpose = 1;
    host.growthDeveloper.construction.value8 = std::bit_cast<float>(std::int32_t{1});
    host.growthDeveloper.construction.value12 = 1;
    host.growthDeveloper.desirability.tractSize = 1;
    host.growthDeveloper.desirability.shift = 2;            // tract (1,1) covers cells x 4..7, z 4..7
    BuildingProfile profile;
    profile.purpose = 1;
    profile.wealth = 1;
    profile.capacity = 1;
    host.buildingDevelopment.profiles[0x2000] = profile;
    host.buildingDevelopment.families[0] = {0x2000};
    host.roads = true;

    cSC4TractDeveloper developer;
    CHECK(developer.Init());
    auto* lot = new cSC4Lot(4, 4, 2, 2, 0);
    lot->AddRef();                                          // the lot manager's reference
    for (int x = 4; x < 6; ++x)
        for (int z = 4; z < 6; ++z) host.lotManager.lots[{x, z}] = lot;
    cSC4LotConfiguration* configuration = MakeConfiguration(21, 1, {1}, {1}, 0, 10, {});
    host.configurations = {configuration};

    GrowthProfile grow;
    grow.developer = &host.growthDeveloper;
    grow.tractX = 1;
    grow.tractZ = 1;
    grow.growthStage = 2;
    std::list<int> outcomes;
    bool declined = false;
    CHECK(developer.Grow(grow, outcomes, declined) == 1);
    CHECK(declined);                                        // the tract had a lot that could grow
    CHECK(outcomes.size() == 1 && outcomes.front() == kTractOutcomeSuccess);
    CHECK(host.lotDeveloper.constructions == 1);
    CHECK(host.lotDeveloper.lastLot == lot);
    CHECK(host.lotDeveloper.lastConfiguration == configuration);
    CHECK(host.lotDeveloper.lastBuilding == 0x2000);
    CHECK(host.demolitions == 1);
    CHECK(developer.Histograms()[kHistogramLotSuitable][kTractOutcomeSuccess] >= 1);
    CHECK(developer.Histograms()[kHistogramExisting][kTractOutcomeSuccess] == 1);
    CHECK(developer.Histograms()[kHistogramBuilding][kTractOutcomeSuccess] >= 1);

    // without construction resources the build fails (20) and nothing starts
    host.buildingDevelopment.claim = false;
    host.lotDeveloper.constructions = 0;
    outcomes.clear();
    CHECK(developer.Grow(grow, outcomes, declined) == 0);
    CHECK(host.lotDeveloper.constructions == 0);
    CHECK(!outcomes.empty() && outcomes.front() == kTractOutcomeBuildFailed);

    configuration->Release();
    developer.Shutdown();
    lot->Release();
    SetLotHost(nullptr);
    SetTractDeveloperHost(nullptr);
}

// An abandoned building in the tract is taken over before anything new is planned: Grow ends with 27 after the lot developer
// reoccupies it for the developer, counting its capacity as built. A lot with no room for the developer is passed over (15).
void TestTakeOverAbandoned() {
    FakeHost host;
    FakeLotHost lotHost;
    SetTractDeveloperHost(&host);
    SetLotHost(&lotHost);
    host.growthDeveloper.developerType = 0x1010;
    host.growthDeveloper.type = 1;                          // residential: zone 2 converts to type 1
    host.growthDeveloper.purpose = 1;
    host.growthDeveloper.wealth = 1;
    host.growthDeveloper.construction.purpose = 1;
    host.growthDeveloper.construction.value8 = std::bit_cast<float>(std::int32_t{1});
    host.growthDeveloper.construction.value12 = 1;
    host.growthDeveloper.desirability.tractSize = 1;
    host.growthDeveloper.desirability.shift = 2;            // tract (1,1) covers cells x 4..7, z 4..7
    lotHost.profile.purpose = 1;
    lotHost.profile.wealth = 1;
    lotHost.profile.capacities = {{{0x1010, 12}}};
    BuildingProfile profile;
    profile.purpose = 1;
    profile.wealth = 1;
    profile.capacity = 12;
    host.buildingDevelopment.profiles[0x1234] = profile;

    cSC4TractDeveloper developer;
    CHECK(developer.Init());
    auto* lot = new cSC4Lot(4, 4, 2, 2, 0);
    lot->AddRef();
    lot->UpdateZoneType();
    lot->SetBuildingType(0x1234);
    lot->SetState(kLotStateOccupied, 1, 0);
    lot->SetState(kLotStateLivable, 1, 0);
    lot->SetState(kLotStateAbandoned, 0, 6);
    for (int x = 4; x < 6; ++x)
        for (int z = 4; z < 6; ++z) host.lotManager.lots[{x, z}] = lot;

    GrowthProfile grow;
    grow.developer = &host.growthDeveloper;
    grow.tractX = 1;
    grow.tractZ = 1;
    grow.growthStage = 2;
    std::list<int> outcomes;
    bool declined = false;
    CHECK(developer.Grow(grow, outcomes, declined) == 12);
    CHECK(outcomes.size() == 1 && outcomes.front() == kTractOutcomeSuccess);
    CHECK(host.lotDeveloper.reoccupied.size() == 1 && host.lotDeveloper.reoccupied.front() == 0x1010);
    CHECK(host.lotDeveloper.abandoned.empty());            // an abandoned lot is not abandoned again
    CHECK(host.lotDeveloper.constructions == 0);
    CHECK(developer.Histograms()[kHistogramTakeOver][kTractOutcomeSuccess] == 1);

    // a developer the building has no room for: 15, nothing reoccupied
    host.lotDeveloper.reoccupied.clear();
    host.growthDeveloper.developerType = 0x1020;
    host.growthDeveloper.wealth = 2;
    outcomes.clear();
    developer.Grow(grow, outcomes, declined);
    CHECK(host.lotDeveloper.reoccupied.empty());
    CHECK(developer.Histograms()[kHistogramTakeOver][0xf] >= 1);   // once per pass
    CHECK(developer.Histograms()[kHistogramTakeOver][kTractOutcomeSuccess] == 0);

    developer.Shutdown();
    lot->Release();
    SetLotHost(nullptr);
    SetTractDeveloperHost(nullptr);
}

// The common setup of the growth tests below: a developer of purpose/wealth 1 (or `purpose`), every cell next to a lot has a
// road, one building 0x2000 of capacity `capacity`, tract (1,1) = cells 4..7.
void SetUpGrowth(FakeHost& host, int purpose, std::uint16_t capacity) {
    host.growthDeveloper.purpose = purpose;
    host.growthDeveloper.wealth = 1;
    host.growthDeveloper.construction.purpose = static_cast<std::uint32_t>(purpose);
    host.growthDeveloper.construction.value8 = std::bit_cast<float>(std::int32_t{1});
    host.growthDeveloper.construction.value12 = 1;
    host.growthDeveloper.desirability.tractSize = 1;
    host.growthDeveloper.desirability.shift = 2;
    BuildingProfile profile;
    profile.purpose = static_cast<std::uint32_t>(purpose);
    profile.wealth = 1;
    profile.capacity = capacity;
    host.buildingDevelopment.profiles[0x2000] = profile;
    host.buildingDevelopment.families[0] = {0x2000};
    host.roads = true;
}

cSC4Lot* AddLot(FakeHost& host, int x, int z, int sizeX, int sizeZ, int facing) {
    auto* lot = new cSC4Lot(x, z, sizeX, sizeZ, facing);
    lot->AddRef();                                          // the lot manager's reference (DeleteLot drops it)
    lot->UpdateZoneType();
    host.lotManager.added.push_back(lot);
    for (int cx = x; cx < x + sizeX; ++cx)
        for (int cz = z; cz < z + sizeZ; ++cz) host.lotManager.lots[{cx, cz}] = lot;
    return lot;
}

GrowthProfile TractOneOne(FakeHost& host) {
    GrowthProfile grow;
    grow.developer = &host.growthDeveloper;
    grow.tractX = 1;
    grow.tractZ = 1;
    grow.growthStage = 2;
    return grow;
}

// Two 1x1 lots side by side facing 0 and only a 2x1 configuration: the merge of the two is the only candidate (the left lot's
// trial shifted by one, and the right lot's own trial). Grow demolishes and deletes both, creates one 2x1 lot over them and
// starts the building on it. When the merged lot cannot be created the pass ends there (port guard, no stale lots touched).
void TestAggregation() {
    FakeHost host;
    FakeLotHost lotHost;
    SetTractDeveloperHost(&host);
    SetLotHost(&lotHost);
    SetUpGrowth(host, 1, 2);
    cSC4TractDeveloper developer;
    CHECK(developer.Init());
    cSC4Lot* left = AddLot(host, 4, 4, 1, 1, 0);
    cSC4Lot* right = AddLot(host, 5, 4, 1, 1, 0);
    cSC4LotConfiguration* configuration = MakeConfiguration(31, 1, {1}, {1}, 0, 10, {}, true, {2, 1});
    host.configurations = {configuration};
    GrowthProfile grow = TractOneOne(host);
    std::list<int> outcomes;
    bool declined = false;
    CHECK(developer.Grow(grow, outcomes, declined) == 2);
    CHECK(host.lotManager.deleted.size() == 2);
    CHECK(host.demolitions == 2);
    CHECK(host.lotManager.created.size() == 1);
    CHECK(host.redisplays == 1);
    if (host.lotManager.created.size() == 1) {
        std::int32_t r[4];
        host.lotManager.created[0]->GetBoundingRect(r);
        CHECK(r[0] == 4 && r[1] == 4 && r[2] == 5 && r[3] == 4);
        CHECK(host.lotManager.createdFacings[0] == 0);      // not a farm/industry lot: the facing is kept
        CHECK(host.lotDeveloper.lastLot == host.lotManager.created[0]);
    }
    CHECK(host.lotDeveloper.constructions == 1);
    CHECK(host.lotDeveloper.lastConfiguration == configuration);
    CHECK(developer.Histograms()[kHistogramAggregation][kTractOutcomeSuccess] == 2);   // both trials of the same rect
    CHECK(developer.Histograms()[kHistogramAggregationLegal][kTractOutcomeSuccess] == 2);

    // the merged lot cannot be created: both lots are gone, Build reports 0 and the pass ends at once
    FakeHost host2;
    SetTractDeveloperHost(&host2);
    SetUpGrowth(host2, 1, 2);
    cSC4TractDeveloper developer2;
    CHECK(developer2.Init());
    AddLot(host2, 4, 4, 1, 1, 0);
    AddLot(host2, 5, 4, 1, 1, 0);
    host2.configurations = {configuration};
    host2.lotManager.createFails = true;
    GrowthProfile grow2 = TractOneOne(host2);
    outcomes.clear();
    developer2.Grow(grow2, outcomes, declined);
    CHECK(host2.lotManager.deleted.size() == 2);            // only the first candidate's lots, once each
    CHECK(host2.lotDeveloper.constructions == 0);
    CHECK(!outcomes.empty() && outcomes.front() == 0);
    developer2.Shutdown();

    configuration->Release();
    developer.Shutdown();
    (void)left;
    (void)right;
    SetLotHost(nullptr);
    SetTractDeveloperHost(nullptr);
}

// A 2x1 lot facing 0 and only a 1x1 configuration: the lot is split into two 1x1 lots; the first takes the picked building,
// the second picks again, and both start while the built capacity stays under the target.
void TestSubdivision() {
    FakeHost host;
    FakeLotHost lotHost;
    SetTractDeveloperHost(&host);
    SetLotHost(&lotHost);
    SetUpGrowth(host, 1, 1);
    host.growthDeveloper.demand.value = 4.0f;               // desired 4 (desiredMax 6): room for both pieces
    cSC4TractDeveloper developer;
    CHECK(developer.Init());
    cSC4Lot* parent = AddLot(host, 4, 4, 2, 1, 0);
    cSC4LotConfiguration* configuration = MakeConfiguration(41, 1, {1}, {1}, 0, 10, {}, true, {1, 1});
    host.configurations = {configuration};
    GrowthProfile grow = TractOneOne(host);
    std::list<int> outcomes;
    bool declined = false;
    developer.Grow(grow, outcomes, declined);
    CHECK(!host.lotManager.deleted.empty() && host.lotManager.deleted.front() == parent);
    CHECK(host.lotManager.created.size() >= 2);
    if (host.lotManager.created.size() >= 2) {
        std::int32_t a[4], b[4];
        host.lotManager.created[0]->GetBoundingRect(a);
        host.lotManager.created[1]->GetBoundingRect(b);
        CHECK(a[0] == 5 && a[2] == 5 && a[1] == 4);         // facing 0: origin x + 1 - cw = 5, then x -= cw
        CHECK(b[0] == 4 && b[2] == 4 && b[1] == 4);
    }
    CHECK(host.lotDeveloper.constructions >= 2);
    CHECK(developer.BuiltCapacity() >= 2);
    configuration->Release();
    developer.Shutdown();
    SetLotHost(nullptr);
    SetTractDeveloperHost(nullptr);
}

// A farm developer (purpose 5) with a 5x5 block of 1x1 lots: the farm goes on the first candidate, and every other suitable
// lot reached outward from it (24, at least 15 needed) becomes a field of the type the farm's exemplar lists, facing the farm,
// in the livable state. With fewer than 15 lots around it no farm is built.
void TestBuildFarm() {
    FakeHost host;
    FakeLotHost lotHost;
    lotHost.zone = 5;
    lotHost.roadLots = {{4, 4}};                            // only the farm's own lot is on a road; fields need none
    host.zoneManager.zone = 5;
    SetTractDeveloperHost(&host);
    SetLotHost(&lotHost);
    SetUpGrowth(host, 5, 1);
    BuildingProfile field;
    field.purpose = 5;
    field.capacity = 3;
    host.buildingDevelopment.profiles[0x3000] = field;
    host.exemplarArrays[0x29b55f73] = {0x3000};
    cSC4LotConfiguration* fieldConfiguration = MakeConfiguration(0x3000, 1, {5}, {1}, 0, 10, {}, true, {1, 1}, 5);
    cSC4LotConfigurationManager::GetLotConfigurationMap()[0x3000] = fieldConfiguration;
    cSC4LotConfiguration* configuration = MakeConfiguration(51, 1, {5}, {1}, 0, 10, {}, true, {1, 1}, 5);
    host.configurations = {configuration};
    cSC4TractDeveloper developer;
    CHECK(developer.Init());
    for (int x = 2; x <= 6; ++x)
        for (int z = 2; z <= 6; ++z) AddLot(host, x, z, 1, 1, 1);
    GrowthProfile grow = TractOneOne(host);
    std::list<int> outcomes;
    bool declined = false;
    developer.Grow(grow, outcomes, declined);
    if (std::getenv("TRACT_TEST_DEBUG")) {
        for (int o : outcomes) std::fprintf(stderr, "farm outcome %d\n", o);
        const auto& h = developer.Histograms();
        for (std::size_t r = 0; r < h.size(); ++r)
            for (std::size_t o = 0; o < h[r].size(); ++o)
                if (h[r][o]) std::fprintf(stderr, " %zu:%zu=%u", r, o, h[r][o]);
        std::fprintf(stderr, "\n");
    }
    CHECK(!outcomes.empty() && outcomes.back() == kTractOutcomeSuccess);
    CHECK(host.lotDeveloper.constructions == 25);           // the farm and 24 fields
    CHECK(host.lotDeveloper.lastConfiguration == fieldConfiguration);
    CHECK(host.lotDeveloper.lastBuilding == 0x3000);
    int fields = 0;
    std::set<cSC4Lot*> seen;
    for (const auto& [cell, lot] : host.lotManager.lots)
        if (seen.insert(lot).second && lot->GetState() == 3) ++fields;
    CHECK(fields == 24);

    // a 3x3 block only has 8 lots around the farm: nothing is built
    FakeHost small;
    SetTractDeveloperHost(&small);
    small.zoneManager.zone = 5;
    SetUpGrowth(small, 5, 1);
    small.configurations = {configuration};
    cSC4TractDeveloper developer2;
    CHECK(developer2.Init());
    for (int x = 4; x <= 6; ++x)
        for (int z = 4; z <= 6; ++z) AddLot(small, x, z, 1, 1, 1);
    GrowthProfile grow2 = TractOneOne(small);
    outcomes.clear();
    developer2.Grow(grow2, outcomes, declined);
    CHECK(small.lotDeveloper.constructions == 0);
    developer2.Shutdown();

    cSC4LotConfigurationManager::GetLotConfigurationMap().erase(0x3000);
    fieldConfiguration->Release();
    configuration->Release();
    developer.Shutdown();
    SetLotHost(nullptr);
    SetTractDeveloperHost(nullptr);
}

// UpdatePopulation: a livable lot of the developer's wealth gets capacity x desirability% (jitter 0); when that would fill it,
// 1 - vacancy instead. A lot of another wealth is left alone.
void TestUpdatePopulation() {
    FakeHost host;
    FakeLotHost lotHost;
    SetTractDeveloperHost(&host);
    SetLotHost(&lotHost);
    host.tunablesResult.populationJitter = 0.0f;
    host.tunablesResult.declineJitterLow = 0.25f;
    host.tunablesResult.declineJitterHigh = 0.25f;
    host.growthDeveloper.developerType = 0x1010;
    host.growthDeveloper.wealth = 1;
    host.growthDeveloper.desirability.shift = 2;
    host.growthDeveloper.desirability.cells[{1, 1}] = 50;
    lotHost.profile.purpose = 1;
    lotHost.profile.wealth = 1;
    lotHost.profile.capacities = {{{0x1010, 12}}};
    cSC4TractDeveloper developer;
    CHECK(developer.Init());
    cSC4Lot* lot = AddLot(host, 4, 4, 1, 1, 0);
    lot->SetBuildingType(0x1234);
    lot->SetState(kLotStateOccupied, 1, 0);
    lot->SetState(kLotStateLivable, 1, 0);
    GrowthProfile grow = TractOneOne(host);
    developer.UpdatePopulation(grow);
    CHECK(lot->GetPopulation(0x1010) == 6);
    host.growthDeveloper.desirability.cells[{1, 1}] = 150;
    developer.UpdatePopulation(grow);
    CHECK(lot->GetPopulation(0x1010) == 9);                 // 12 x (1 - 0.25)
    host.growthDeveloper.wealth = 2;
    host.growthDeveloper.desirability.cells[{1, 1}] = 50;
    developer.UpdatePopulation(grow);
    CHECK(lot->GetPopulation(0x1010) == 9);
    developer.Shutdown();
    SetLotHost(nullptr);
    SetTractDeveloperHost(nullptr);
}

int main() {
    TestLayoutAndCom();
    TestStyles();
    TestTunables();
    TestStream();
    TestDoMessage();
    TestCandidatePool();
    TestBoundingRect();
    TestExistingLotsAndDecline();
    TestSuitability();
    TestConfigurationPass();
    TestAttemptRecords();
    TestScoreCandidateLots();
    TestInitAndShutdown();
    TestNullHost();
    TestBuildOnEmptyLot();
    TestTakeOverAbandoned();
    TestAggregation();
    TestSubdivision();
    TestBuildFarm();
    TestUpdatePopulation();
    if (g_failures == 0) std::puts("sc4_tractdeveloper ok");
    return g_failures == 0 ? 0 : 1;
}
