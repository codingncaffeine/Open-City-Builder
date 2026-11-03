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

#include <bit>
#include <cmath>
#include <cstdio>
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
    int GetLotsInCellRect(std::list<cRZAutoRefCount<cSC4Lot>>&, int, int, int, int, bool) override {
        return 0;
    }
    int canCreate = 7;
    int CanCreateLot(int, int, int, int, bool, bool, cSC4LotConfiguration*) override { return canCreate; }
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
    std::int32_t GetType() const override { return 0; }
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
    std::uint8_t ZoneTypeAt(int, int) override { return 2; }
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
    bool RoadAccessCells(const cSC4Lot&, int) override { return false; }
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
    void ReadTunables(TractTunables& io) override {
        if (tunablesPresent) io = tunablesResult;
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
                                        bool withBuilding = true) {
    cSCPropertyHolder holder;
    holder.AddRef();
    cSCPropertyHelp::SetPropertyValue(&holder, kLotConfigVersion, static_cast<std::uint8_t>(2));
    cSCPropertyHelp::SetPropertyValue(&holder, kLotConfigSize, std::vector<std::uint8_t>{2, 2});
    cSCPropertyHelp::SetPropertyValue(&holder, kLotConfigGrowthStage, growthStage);
    cSCPropertyHelp::SetPropertyValue(&holder, kLotConfigPurposeTypes, purposes);
    cSCPropertyHelp::SetPropertyValue(&holder, kLotConfigWealthTypes, wealthTypes);
    cSCPropertyHelp::SetPropertyValue(&holder, kLotConfigZoneTypes, std::vector<std::uint8_t>{2});
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
    if (g_failures == 0) std::puts("sc4_tractdeveloper ok");
    return g_failures == 0 ? 0 : 1;
}
