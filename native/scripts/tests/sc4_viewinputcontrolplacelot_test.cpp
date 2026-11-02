// cSC4ViewInputControlPlaceLot: the placement tool's two modes, its footprint geometry, the validity /
// feedback state machine and the commit path, all through the IPlaceLotHost seams.
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sc4/ViewInputControlPlaceLot.h"
#include "vic_fakes.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace sc4;

namespace {

// ---------------------------------------------------------------------------------------------------
// Renderer-side fakes
// ---------------------------------------------------------------------------------------------------
struct FakeEffect;
struct FakeEffects;

struct FakeEffect final : IPlaceLotEffect {
    FakeEffects* owner = nullptr;
    std::string name;
    int refs = 0;
    bool started = false;
    bool released = false;
    int startCalls = 0, stopCalls = 0;
    cS3DTransform transform;
    bool hasTransform = false;
    std::vector<float> vectorParams;
    std::uint32_t finishMessage = 0;
    void Start(int) override { started = true; ++startCalls; }
    void Stop(int) override { started = false; ++stopCalls; }
    bool IsStarted() override { return started; }
    void SetEffectTransform(const cS3DTransform& value) override { transform = value; hasTransform = true; }
    void SetFinishMessage(std::uint32_t message, std::uint32_t) override { finishMessage = message; }
    void SetVectorParams(std::uint32_t, const float* values, int count) override {
        vectorParams.assign(values, values + count);
    }
    void SetVoidParams(std::uint32_t, void**, int) override {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { --refs; if (refs <= 0) released = true; return refs < 0 ? 0u : static_cast<std::uint32_t>(refs); }
};

struct FakeEffects final : IPlaceLotEffects {
    std::deque<FakeEffect> effects;
    std::vector<std::string> names;
    IPlaceLotEffect* Create(const char* name) override {
        names.push_back(name ? name : "");
        effects.emplace_back();
        FakeEffect& effect = effects.back();
        effect.owner = this;
        effect.name = name ? name : "";
        effect.refs = 1;
        return &effect;
    }
    FakeEffect* Find(const std::string& name) {
        for (FakeEffect& effect : effects) if (effect.name == name) return &effect;
        return nullptr;
    }
    int Count(const std::string& name) {
        int n = 0;
        for (FakeEffect& effect : effects) if (effect.name == name) ++n;
        return n;
    }
};

struct FakeCoverage final : IPlaceLotCoverageHelper {
    std::uint32_t categories = 0;
    int recalcs = 0;
    void SetCategories(std::uint32_t value) override { categories = value; }
    void RecalcCoverage() override { ++recalcs; }
};

struct FakeKeyAccelerator final : cIGZWinKeyAccelerator {
    int calls = 0;
    std::uint32_t key = 0, modifiers = 0;
    bool down = false;
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool RegisterAccelerator(std::uint32_t, std::uint32_t) override { return false; }
    bool RegisterAccelerator(std::uint32_t, const cGZMessage&) override { return false; }
    bool UnregisterAccelerator(std::uint32_t) override { return false; }
    bool IsAcceleratorPresent(std::uint32_t, bool, bool) override { return false; }
    bool TestForAccelerator(const cGZMessage&, std::uint32_t&) override { return false; }
    bool TestForAccelerator(const cGZMessage&, cGZMessage&) override { return false; }
    bool TestForAccelerator(std::uint32_t k, std::uint32_t m, bool d, std::uint32_t&) override {
        ++calls; key = k; modifiers = m; down = d; return true;
    }
    bool TestForAccelerator(std::uint32_t, std::uint32_t, bool, cGZMessage&) override { return false; }
    int GetKeysForAccelerator(std::uint32_t, std::uint32_t*, std::uint32_t&) override { return 0; }
    bool ProcessAccelerator(const cGZMessage&) override { return false; }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
};

struct FakeHost;

struct FakeView final : IPlaceLotView {
    FakeHost* host = nullptr;
    bool top = true;
    cSC4ViewInputControl* topControl = nullptr;
    bool terrainResult = true;
    float pick[3] = {0, 0, 0};
    int ended = -1;
    FakeCoverage coverage;
    FakeKeyAccelerator accelerator;
    std::vector<std::uint32_t> cursorTextCleared;
    std::vector<std::uint32_t> costSet;
    std::vector<bool> costCleared;
    std::string lastCursorText;
    std::uint32_t lastCursorTextId = 0;
    std::uint32_t lastSelectedBuilding = 0, lastSelectedLot = 0;
    std::vector<std::uint32_t> addedGlobalEffects, removedGlobalEffects;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    void EndInput(int result) override { ended = result; }
    cSC4ViewInputControl* TopControl() override { return top ? topControl : nullptr; }
    bool PickTerrain(int, int, float out[3], bool) override {
        std::memcpy(out, pick, sizeof(pick));
        return terrainResult;
    }
    cIGZWinKeyAccelerator* KeyAccelerator() override { return &accelerator; }
    void SetCursorText(std::uint32_t id, std::uint32_t, std::uint32_t, const cIGZString* text,
                       int) override {
        lastCursorTextId = id;
        lastCursorText = text ? text->ToChar() : "";
    }
    void ClearCursorText(std::uint32_t id) override { cursorTextCleared.push_back(id); }
    void SetCostIndicator(std::uint32_t cost) override { costSet.push_back(cost); }
    void ClearCostIndicator(bool) override { costCleared.push_back(true); }
    IPlaceLotCoverageHelper* CoverageHelper() override { return &coverage; }
    std::uint32_t LastSelectedBuilding() override { return lastSelectedBuilding; }
    std::uint32_t LastSelectedLot() override { return lastSelectedLot; }
    void SetLastSelectedBuilding(std::uint32_t value) override { lastSelectedBuilding = value; }
    void SetLastSelectedLot(std::uint32_t value) override { lastSelectedLot = value; }
    void AddGlobalEffect(std::uint32_t id) override { addedGlobalEffects.push_back(id); }
    void RemoveGlobalEffect(std::uint32_t id) override { removedGlobalEffects.push_back(id); }
};

// ---------------------------------------------------------------------------------------------------
// Game-side fakes
// ---------------------------------------------------------------------------------------------------
struct FakeLocalGrid final : IPlaceLotLocalGrid {
    int inits = 0, shutdowns = 0;
    int showCalls = 0;
    bool shown = false;
    int positions = 0;
    cS3DVector3 lastPosition;
    void Init() override { ++inits; }
    void Shutdown() override { ++shutdowns; }
    void SetPosition(const cS3DVector3& position) override { ++positions; lastPosition = position; }
    void ShowEffect(bool show) override { ++showCalls; shown = show; }
};

struct FakeTerrainQuery final : IPlaceLotTerrainQuery {
    bool water = false;
    int calls = 0;
    bool IsWater(float, float) override { ++calls; return water; }
};

struct FakeTerrain final : IPlaceLotTerrain {
    float cell = 16.0f;
    int cellCountX = 64, cellCountZ = 64;
    FakeTerrainQuery query;
    float CellSize() override { return cell; }
    int CellCountX() override { return cellCountX; }
    int CellCountZ() override { return cellCountZ; }
    IPlaceLotTerrainQuery* Query() override { return &query; }
};

struct FakeNetworkCell final : IPlaceLotNetworkCell {
    std::uint32_t networkFlags = 0;   // the network types present (bit per type)
    std::uint32_t flags = 0;          // Flags()
    std::uint32_t edgeTypes = 0;      // EdgeTypes(networkType)
    std::uint8_t edgeFlags = 0;       // EdgeFlags()
    bool HasAnyFlag(std::uint32_t mask) override { return (networkFlags & mask) != 0; }
    std::uint32_t Flags() override { return flags; }
    std::uint32_t EdgeTypes(std::uint32_t) override { return edgeTypes; }
    std::uint8_t EdgeFlags() override { return edgeFlags; }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
};

struct FakeNetworkMap final : IPlaceLotNetworkMap {
    std::deque<FakeNetworkCell> storage;
    std::map<std::pair<int, int>, FakeNetworkCell*> cells;
    FakeNetworkCell* Make(int x, int z) {
        storage.emplace_back();
        FakeNetworkCell* cell = &storage.back();
        cells[{x, z}] = cell;
        return cell;
    }
    IPlaceLotNetworkCell* GetCell(int x, int z, std::uint32_t, int) override {
        auto it = cells.find({x, z});
        return it == cells.end() ? nullptr : it->second;
    }
};

struct FakeMissionTarget final : IPlaceLotMissionTarget {
    std::int32_t rect[4] = {0, 0, 0, 0};
    const std::int32_t* Rect() override { return rect; }
};
struct FakeMission final : IPlaceLotMission {
    bool active = false;
    FakeMissionTarget target;
    bool IsActive() override { return active; }
    IPlaceLotMissionTarget* Target() override { return &target; }
};

struct FakeLotDeveloper final : IPlaceLotLotDeveloper {
    std::int64_t price = 0;
    bool constructResult = true;
    int constructs = 0;
    bool lastDoConstruction = false;
    std::int64_t LotPrice(const SC4Rect<int>&) override { return price; }
    bool ConstructDefaultLot(cSC4Lot*, cSC4LotConfiguration*, std::uint32_t, bool doConstruction) override {
        ++constructs;
        lastDoConstruction = doConstruction;
        return constructResult;
    }
};

struct FakeBuildingDevelopment final : IPlaceLotBuildingDevelopment {
    std::vector<std::uint32_t> types;
    std::int64_t price = 0;
    std::set<std::uint32_t> police, fire;
    bool coverageRadiusResult = true;
    FakeLotDeveloper lotDeveloper;
    std::uint32_t resolvedType = 0;
    std::vector<std::uint32_t> resolvedIds;
    void GetAllBuildingTypes(std::vector<std::uint32_t>& out) override { out = types; }
    bool BuildingTypeName(std::uint32_t, cIGZString& out) override { out.AssignFromCString("name"); return true; }
    std::int64_t BuildingPrice(std::uint32_t) override { return price; }
    IPlaceLotLotDeveloper* LotDeveloper() override { return &lotDeveloper; }
    bool ResolveBuildingType(const std::vector<std::uint32_t>& ids, std::uint32_t& out) override {
        resolvedIds = ids;
        out = resolvedType;
        return resolvedType != 0;
    }
    bool BuildingIsPoliceStation(std::uint32_t type) override { return police.count(type) != 0; }
    bool BuildingIsFireStation(std::uint32_t type) override { return fire.count(type) != 0; }
    bool BuildingCoverageRadius(std::uint32_t, int, float, float& radius, float& ring) override {
        radius = 4.0f;
        ring = 12.0f;
        return coverageRadiusResult;
    }
};

struct FakeBudget final : IPlaceLotBudget {
    std::int64_t funds = 1000;
    std::vector<std::int64_t> withdrawals, deposits;
    std::int64_t Funds() override { return funds; }
    void Deposit(std::int64_t amount) override { deposits.push_back(amount); funds += amount; }
    void Withdraw(std::int64_t amount) override { withdrawals.push_back(amount); funds -= amount; }
};

struct FakeLotManager final : IPlaceLotLotManager {
    int canCreateResult = 8;
    bool createResult = true;
    int canCreateCalls = 0;
    std::vector<int> facings;
    std::vector<SC4Rect<int>> rects;
    bool hasLot = false;
    SC4Rect<int> createdRect;
    int createdFacing = 0;
    bool createdArg7 = false, createdArg8 = false;
    int CanCreatePloppedLot(const SC4Rect<int>& rect, int facing, cSC4LotConfiguration*) override {
        ++canCreateCalls;
        rects.push_back(rect);
        facings.push_back(facing);
        return canCreateResult;
    }
    bool CreateLotIfPossible(int x, int z, int sizeX, int sizeZ, int facing, bool arg7, bool arg8,
                             cSC4LotConfiguration*, cSC4Lot*& out) override {
        createdRect = SC4Rect<int>{x, z, x + sizeX - 1, z + sizeZ - 1};
        createdFacing = facing;
        createdArg7 = arg7;
        createdArg8 = arg8;
        out = hasLot ? reinterpret_cast<cSC4Lot*>(lotStorage) : nullptr;
        return createResult;
    }
    alignas(8) unsigned char lotStorage[0x40] = {};
};

struct FakeZoneManager final : IPlaceLotZoneManager {
    bool result = true;
    std::vector<int> zoneTypes;
    std::vector<SC4Rect<int>> rects;
    std::vector<std::array<bool, 5>> args;
    bool PlaceZone(const SC4Rect<int>& rect, int zoneType, bool a1, bool a2, bool a3, bool a4,
                   bool a5) override {
        zoneTypes.push_back(zoneType);
        rects.push_back(rect);
        args.push_back({a1, a2, a3, a4, a5});
        return result;
    }
};

struct FakeDemolition final : IPlaceLotDemolition {
    bool result = true;
    std::int64_t cost = 0;
    std::vector<std::uint32_t> flags;
    std::vector<SC4Rect<int>> rects;
    bool DemolishRegion(const SC4Rect<int>& rect, std::uint32_t value, std::int64_t* outCost) override {
        flags.push_back(value);
        rects.push_back(rect);
        if (outCost) *outCost = cost;
        return result;
    }
};

struct FakeProp;
struct FakePropManager;

struct FakeProp final : IPlaceLotProp {
    std::uint32_t id = 0;
    int refs = 1;
    int orientation = 0;
    bool placed = false;
    bool preview = true;
    std::vector<cS3DVector3> positions;
    std::vector<s3d::SC4DrawAttributes> attributes;
    int hides = 0;
    int Orientation() override { return orientation; }
    void SetOrientation(int value) override { orientation = value; }
    bool IsPlaced() override { return placed; }
    bool ShowInPreview() override { return preview; }
    void Show(const cS3DVector3& position, const s3d::SC4DrawAttributes& attrs) override {
        placed = true;
        positions.push_back(position);
        attributes.push_back(attrs);
    }
    void Hide() override { placed = false; ++hides; }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { --refs; return refs < 0 ? 0u : static_cast<std::uint32_t>(refs); }
};

struct FakePropManager final : IPlaceLotPropManager {
    std::set<std::uint32_t> existing;
    bool buildingProp = true;
    std::deque<FakeProp> props;
    bool PropTypeExists(std::uint32_t id) override { return existing.count(id) != 0; }
    IPlaceLotProp* CreateProp(std::uint32_t id) override {
        if (!existing.count(id)) return nullptr;
        props.emplace_back();
        FakeProp& prop = props.back();
        prop.id = id;
        return &prop;
    }
    IPlaceLotProp* CreateBuildingProp(std::uint32_t type) override {
        if (!buildingProp) return nullptr;
        props.emplace_back();
        FakeProp& prop = props.back();
        prop.id = type;
        return &prop;
    }
    FakeProp* Find(std::uint32_t id) {
        for (FakeProp& prop : props) if (prop.id == id) return &prop;
        return nullptr;
    }
};

struct FakeCity final : IPlaceLotCity {
    float cell = 16.0f;
    FakeNetworkMap networkMap;
    FakeMission mission;
    std::vector<std::pair<int, int>> toCellCalls;
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        cellX = static_cast<int>(std::floor(x / cell));
        cellZ = static_cast<int>(std::floor(z / cell));
        toCellCalls.push_back({cellX, cellZ});
    }
    void CellToPosition(int cellX, int cellZ, float& x, float& z) override {
        x = (static_cast<float>(cellX) + 0.5f) * cell;
        z = (static_cast<float>(cellZ) + 0.5f) * cell;
    }
    IPlaceLotNetworkMap* NetworkMap() override { return &networkMap; }
    IPlaceLotMission* Mission() override { return &mission; }
};

struct FakeFilter final : IPlaceLotFilter {
    int refs = 1;
    bool Matches(cSC4LotConfiguration*) override { return true; }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { --refs; return refs < 0 ? 0u : static_cast<std::uint32_t>(refs); }
};

struct FakeChooser final : IPlaceLotChooser {
    std::uint32_t lotConfiguration = 0;
    std::uint32_t buildingType = 0;
    int lotPicks = 0, buildingPicks = 0;
    std::vector<std::uint32_t> lastLotIds;
    std::vector<std::uint32_t> lastBuildingTypes;
    std::uint32_t PickLotConfiguration(std::uint32_t, const std::unordered_set<std::uint32_t>& ids) override {
        ++lotPicks;
        lastLotIds.assign(ids.begin(), ids.end());
        return lotConfiguration;
    }
    std::uint32_t PickBuildingType(std::uint32_t, const std::vector<std::uint32_t>& types) override {
        ++buildingPicks;
        lastBuildingTypes = types;
        return buildingType;
    }
};

struct FakeServer final : cIGZMessageServer2 {
    std::vector<std::uint32_t> added, removed;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 2; }
    std::uint32_t Release() override { return 1; }
    bool MessageSend(cIGZMessage2*) override { return false; }
    bool MessagePost(cIGZMessage2*, bool) override { return false; }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t id) override { added.push_back(id); return true; }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t id) override { removed.push_back(id); return true; }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return false; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return false; }
    bool OnTick(std::uint32_t) override { return false; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 1; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

struct FakeMessage final : cIGZMessage2 {
    std::uint32_t type = 0;
    bool Create(std::uint32_t, void**) const override { return false; }
    std::uint32_t GetType() const override { return type; }
    void SetType(std::uint32_t value) override { type = value; }
    bool IsEqual(const cIGZMessage2&) const override { return false; }
    bool IsLess(const cIGZMessage2&) const override { return false; }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
};

struct FakeHost final : IPlaceLotHost, IViewInputHost {
    FakeView view;
    vic_fakes::Mgr windowManager;
    FakeServer server;
    FakeTerrain terrain;
    FakeCity city;
    FakeBuildingDevelopment building;
    FakeBudget budget;
    FakeLotManager lots;
    FakeZoneManager zones;
    FakeDemolition demolition;
    FakePropManager props;
    FakeEffects effects;
    FakeLocalGrid localGrid;
    FakeChooser chooser;
    FakeFilter* lastFilter = nullptr;
    std::unordered_set<std::uint32_t> filterIds;
    std::map<std::uint32_t, cSC4LotConfiguration*> configurations;
    std::vector<cSC4LotConfigurationObject*> primaryObjects;
    std::map<std::uint32_t, bool> boolProperties;
    std::map<std::uint32_t, std::uint32_t> uintProperties;
    std::set<std::pair<std::uint32_t, std::uint32_t>> groups;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> strings;
    int rewardNetworkType = 0;
    int mouseMoved = 0;
    int utilityCalls = 0;
    std::uint32_t lastConfigId = 0;

    // IViewInputHost
    IViewInputView* View3D() override { return &view; }
    cIGZWinMgr* WindowManager() override { return &windowManager; }
    int soundPlays = 0;
    void PlaySound(int, std::uint32_t) override {}                      // IViewInputHost
    void PlaySound(std::uint32_t, std::uint32_t) override { ++soundPlays; }   // IPlaceLotHost
    // IPlaceLotHost
    cIGZMessageServer2* MessageServer() override { return &server; }
    IPlaceLotView* View() override { return &view; }
    IPlaceLotTerrain* Terrain() override { return &terrain; }
    IPlaceLotCity* City() override { return &city; }
    IPlaceLotNetworkMap* NetworkMap() override { return &city.networkMap; }
    IPlaceLotBuildingDevelopment* BuildingDevelopment() override { return &building; }
    IPlaceLotBudget* Budget() override { return &budget; }
    IPlaceLotLotManager* LotManager() override { return &lots; }
    IPlaceLotZoneManager* ZoneManager() override { return &zones; }
    IPlaceLotDemolition* Demolition() override { return &demolition; }
    IPlaceLotPropManager* PropManager() override { return &props; }
    IPlaceLotEffects* Effects() override { return &effects; }
    IPlaceLotLocalGrid* CreateLocalGrid() override { return &localGrid; }
    cSC4LotConfiguration* GetLotConfiguration(std::uint32_t id) override {
        lastConfigId = id;
        auto it = configurations.find(id);
        return it == configurations.end() ? nullptr : it->second;
    }
    int GetLotConfigurationIDsByFilter(std::unordered_set<std::uint32_t>& out, IPlaceLotFilter*) override {
        out = filterIds;
        return static_cast<int>(out.size());
    }
    IPlaceLotFilter* CreatePlopFilter(bool, std::uint32_t) override {
        lastFilter = new FakeFilter();
        return lastFilter;
    }
    void GetObjectIDArray(const cSC4LotConfigurationObject*, std::vector<std::uint32_t>& out) override {
        out.clear();
        if (lastConfigId != 0) out.push_back(lastConfigId);
    }
    void LoadString(std::uint32_t group, std::uint32_t instance, cIGZString& out) override {
        strings.push_back({group, instance});
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "error_%08x", instance);
        out.AssignFromCString(buffer);
    }
    void ConnectLotUtilities(cSC4Lot*, const cS3DVector3&) override { ++utilityCalls; }
    void MouseMoved() override { ++mouseMoved; }
    bool BuildingPropertyBool(std::uint32_t buildingType, std::uint32_t property) override {
        auto it = boolProperties.find(property);
        (void)buildingType;
        return it != boolProperties.end() && it->second;
    }
    bool BuildingPropertyUint32(std::uint32_t, std::uint32_t property, std::uint32_t& out) override {
        auto it = uintProperties.find(property);
        if (it == uintProperties.end()) return false;
        out = it->second;
        return true;
    }
    bool OccupantGroup(std::uint32_t buildingType, std::uint32_t group) override {
        return groups.count({buildingType, group}) != 0;
    }
    int RewardNetworkType(std::uint8_t) override { return rewardNetworkType; }
    IPlaceLotChooser* Chooser() override { return &chooser; }
};

// A usable lot configuration: 2x2, one building object and whatever extra objects the test adds.
cSC4LotConfiguration* MakeConfiguration(std::uint32_t id, std::uint8_t sizeX, std::uint8_t sizeZ,
                                        std::uint32_t buildingId, std::uint32_t buildingExtra) {
    cSC4LotConfiguration* configuration = new cSC4LotConfiguration();
    cSC4LotConfigurationObject building;
    building.type = 0;
    building.orientation = 0;
    building.x = 8.0f;
    building.y = 0.0f;
    building.z = 8.0f;
    building.maxX = 8.0f;
    building.maxZ = 8.0f;
    building.minX = -8.0f;
    building.minZ = -8.0f;
    building.id = buildingId;
    if (buildingExtra != 0) building.extra.push_back(buildingExtra);
    configuration->MakeGeneric(id, sizeX, sizeZ, building);
    return configuration;
}

void AddObject(cSC4LotConfiguration* configuration, std::uint8_t type, float x, float z, float maxX,
               float maxZ, float minX, float minZ, std::uint32_t extra = 0, std::uint8_t flags = 0) {
    cSC4LotConfigurationObject object;
    object.type = type;
    object.flags = flags;
    object.orientation = 0;
    object.x = x;
    object.y = 0.0f;
    object.z = z;
    object.maxX = maxX;
    object.maxZ = maxZ;
    object.minX = minX;
    object.minZ = minZ;
    if (extra != 0) object.extra.push_back(extra);
    configuration->GetLotConfigurationObjectArray().push_back(object);
}

}  // namespace

int main() {
    FakeHost host;
    SetPlaceLotHost(&host);
    SetViewInputHost(&host);

    cSC4LotConfiguration* configuration = MakeConfiguration(0x11111111, 2, 2, 0x7001, 0x7001);
    AddObject(configuration, 1, 4.0f, 4.0f, 4.0f, 4.0f, 4.0f, 4.0f, 0x7100);   // preview prop
    AddObject(configuration, 6, 0.0f, 0.0f, 8.0f, 8.0f, -8.0f, -8.0f);         // red constraint
    AddObject(configuration, 5, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);           // blue constraint
    host.configurations[0x11111111] = configuration;
    host.filterIds = {0x11111111};
    host.chooser.lotConfiguration = 0x11111111;
    host.chooser.buildingType = 0x7001;
    host.building.resolvedType = 0x7001;
    host.building.police.insert(0x7001);
    host.groups.insert({0x7001, 0x1401});
    host.props.existing = {0x7100};
    host.building.lotDeveloper.price = 500;
    host.building.price = 1000;   // the plop cost of the configuration's first building

    // --- construction, QueryInterface, the two ctors and the thunks ---------------------------------
    auto* tool = new cSC4ViewInputControlPlaceLot(0, 0, 0, false);
    host.view.topControl = tool;
    CHECK(tool->ToolKind() == 0);
    CHECK(tool->LotConfigurationId() == 0);
    CHECK(tool->Facing() == 0 && tool->InitialFacing() == 0 && tool->FeedbackFacing() == -1);
    CHECK(tool->CoverageRingRadius() == 32.0f);
    CHECK(tool->BuildingPrice() == -100000 && tool->LotCost() == -100000);
    CHECK(tool->PlacementsRemaining() == 1000);
    CHECK(tool->AddRef() == 1);
    CHECK(tool->AddRefFromMessageTarget() == 2);
    CHECK(tool->ReleaseFromMessageTarget() == 1);
    void* iface = nullptr;
    CHECK(tool->QueryInterface(kPlaceLotMessageTargetId, &iface) && iface != nullptr);
    CHECK(tool->Release() == 1);
    CHECK(tool->QueryInterface(kGZIID_cISC4ViewInputControl, &iface) && iface != nullptr);
    CHECK(tool->Release() == 1);
    CHECK(!tool->QueryInterface(0x12345678, &iface));

    // --- Init: civic mode ---------------------------------------------------------------------------
    CHECK(tool->Init());
    CHECK(tool->GetID() == kPlaceLotToolId);
    CHECK(host.server.added.size() == 6);
    CHECK(host.chooser.lotPicks == 1);
    CHECK(host.chooser.lastLotIds.size() == 1 && host.chooser.lastLotIds[0] == 0x11111111);
    CHECK(tool->LotConfigurationId() == 0x11111111);
    CHECK(!tool->TollBoothMode());
    CHECK(tool->PreviewPropObject() != nullptr);
    CHECK(tool->PreviewProps().size() == 1);
    CHECK(tool->PreviewProps()[0].prop != nullptr);
    CHECK(tool->CoverageCategories() == 1);
    CHECK(std::strcmp(tool->CoverageEffectName().ToChar(), "PlopMode_Police_Inactive") == 0);
    CHECK(std::strcmp(tool->PlacementEffectName().ToChar(), "PlopMode_Police_Plop") == 0);
    CHECK(tool->GlobalEffectId() == 10);
    CHECK(host.localGrid.inits == 1);
    CHECK(tool->CoverageEffect() != nullptr && tool->CoverageEffect()->IsStarted() == false);
    CHECK(host.effects.Count("PlopMode_Police_Inactive") == 1);
    CHECK(host.view.addedGlobalEffects.size() == 1 && host.view.addedGlobalEffects[0] == 10);
    CHECK(host.view.coverage.categories == 1);
    CHECK(host.view.coverage.recalcs == 1);
    CHECK(host.lastFilter != nullptr && host.lastFilter->refs == 0);   // created, used, released

    // --- OnKeyDown forwards to the accelerator only while the tool is on top -------------------------
    CHECK(!tool->OnKeyDown('A', 2));
    CHECK(host.view.accelerator.calls == 1 && host.view.accelerator.key == 'A' &&
          host.view.accelerator.down);
    host.view.top = false;
    CHECK(!tool->OnKeyDown('B', 0) && host.view.accelerator.calls == 1);
    host.view.top = true;

    // --- a mouse move over a placeable cell ----------------------------------------------------------
    host.view.pick[0] = 48.0f;
    host.view.pick[1] = 0.0f;
    host.view.pick[2] = 48.0f;
    host.lots.canCreateResult = 8;
    host.demolition.result = true;
    host.demolition.cost = 100;
    CHECK(tool->OnMouseMove(10, 10, 0));
    CHECK(tool->FeedbackValid());
    CHECK(tool->PreviewX() == 48.0f && tool->PreviewZ() == 48.0f);
    // Init priced the configuration's first building (Mac i386 0x2d7984): building price + lot price + demolition.
    CHECK(tool->BuildingPrice() == 1000);
    CHECK(tool->LotCost() == 1000 + 500 + 100);
    CHECK(host.effects.Count("building_footprint_ok") == 1);
    CHECK(host.effects.Count("Lot_Direction_Arrow") == 1);
    CHECK(tool->FootprintEffect() && tool->FootprintEffect()->IsStarted());
    auto* footprint = static_cast<FakeEffect*>(tool->FootprintEffect());
    CHECK(tool->DirectionEffect() && tool->DirectionEffect()->IsStarted());
    CHECK(tool->CoverageEffect() && tool->CoverageEffect()->IsStarted());
    CHECK(tool->SelectionEffectCount(true) == 1 && tool->SelectionEffectCount(false) == 1);
    CHECK(tool->CoverageActive() == false);
    CHECK(host.mouseMoved == 1);
    CHECK(host.view.costSet.size() == 1 && host.view.costSet[0] == 1600);   // a positive cost is shown
    // The footprint effect name tells the validity; the grid size is the 2x2 bucket (32).
    CHECK(tool->GridSize() == 32.0f);
    CHECK(footprint->hasTransform);
    CHECK(std::fabs(footprint->transform.scale - 32.0f * 0.99999f) < 1e-3f);
    CHECK(footprint->transform.translation[0] == 48.0f);
    // The red rectangle starts at the lot's cell rect (2..3) with the constraint offset.
    CHECK(tool->SelectionEffectCount(true) == 1);
    {
        FakeEffect* red = nullptr;
        for (FakeEffect& effect : host.effects.effects) if (effect.name == "selection_red") red = &effect;
        CHECK(red != nullptr && red->vectorParams.size() == 4);
        if (red && red->vectorParams.size() == 4) {
            // The lot is at cells 2..3; the constraint's cells are placed by its lot-local offsets.
            CHECK(red->vectorParams[0] >= 2.0f && red->vectorParams[1] >= 2.0f);
            CHECK(red->vectorParams[2] > red->vectorParams[0]);
            CHECK(red->vectorParams[3] > red->vectorParams[1]);
        }
    }

    // --- an invalid lot keeps the feedback but swaps the footprint effect ----------------------------
    host.lots.canCreateResult = 3;
    host.view.pick[0] = 48.0f;
    host.view.pick[2] = 80.0f;
    CHECK(tool->OnMouseMove(11, 11, 0));
    CHECK(!tool->FeedbackValid());
    CHECK(host.effects.Count("building_footprint_notok") == 1);
    CHECK(host.strings.size() == 1 && host.strings[0].first == kPlaceLotErrorStringGroup &&
          host.strings[0].second == 0x6a71193f);
    CHECK(std::strcmp(tool->ErrorMessage().ToChar(), "error_6a71193f") == 0);
    CHECK(host.view.lastCursorTextId == kPlaceLotCursorTextId);
    CHECK(host.view.lastCursorText == "error_6a71193f");
    // Every facing refused, so the automatic facing failed: the move only reads the reason and clears the indicator.
    CHECK(host.view.costSet.size() == 1);
    CHECK(host.view.costCleared.size() == 1);

    // --- the screen-clear rectangle suppresses validation inside the placed lot ----------------------
    host.lots.canCreateResult = 8;
    // An even-sized lot snaps the preview centre to the cell centre (48 for the pick below).
    tool->SetScreenClearRect(40, 40, 56, 56);
    host.view.pick[0] = 48.0f;
    host.view.pick[2] = 48.0f;
    CHECK(tool->OnMouseMove(12, 12, 0));
    CHECK(!tool->FeedbackValid());   // the centre is inside 40..56: no validation
    host.view.pick[0] = 80.0f;
    host.view.pick[2] = 80.0f;
    CHECK(tool->OnMouseMove(13, 13, 0));
    CHECK(tool->FeedbackValid());    // 80,80 is outside: the checks run

    // --- falling back to the automatic facing search ------------------------------------------------
    tool->SetScreenClearRect(0, 0, -1, -1);
    host.view.pick[0] = 48.0f;
    host.view.pick[2] = 48.0f;
    // North (facing 0) sees no street, east (facing 1) sees flags 0x40 on both cells -> facing 1 wins.
    FakeNetworkCell* eastA = host.city.networkMap.Make(4, 2);
    eastA->flags = 0x40;
    FakeNetworkCell* eastB = host.city.networkMap.Make(4, 3);
    eastB->flags = 0x40;
    host.lots.canCreateResult = 8;
    host.demolition.result = true;
    CHECK(tool->OnMouseMove(14, 14, 0));
    CHECK(tool->Facing() == 1);
    int street = tool->GetLotFacingStreetCount(SC4Rect<int>{2, 2, 3, 3}, configuration,
                                               cS3DVector3{48, 0, 48}, 1);
    CHECK(street == 8);

    // --- SeekCompatibleNetworks --------------------------------------------------------------------
    FakeNetworkCell* road = host.city.networkMap.Make(2, 2);
    road->networkFlags = 1u << 0;   // only the road network: compatible with a road toll booth
    SC4Rect<int> tollFootprint{2, 2, 2, 2};
    CHECK(tool->SeekCompatibleNetworks(tollFootprint, 2, 0));
    road->networkFlags = (1u << 0) | (1u << 2);   // a second network type: incompatible
    CHECK(!tool->SeekCompatibleNetworks(tollFootprint, 2, 0));

    // --- the toll booth picker ---------------------------------------------------------------------
    tool->SetScreenClearRect(0, 0, -1, -1);
    // Road cell at (5,5) with a straight east-west connection (bytes 2,0,2,0).
    FakeNetworkCell* booth = host.city.networkMap.Make(5, 5);
    booth->networkFlags = 1u << 0;
    booth->flags = 1u << 0;
    booth->edgeTypes = 0x00020002u;   // direction 0 = 2 (straight), direction 2 = 2
    std::uint32_t tollConfiguration = 0;
    int tollFacing = 0;
    cS3DVector3 tollPosition{80.0f, 0.0f, 80.0f};
    SC4Rect<int> tollRect;
    CHECK(tool->PickTollBoothLotAndOrientation(tollConfiguration, tollFacing, tollPosition, tollRect));
    CHECK(tollConfiguration == 0xebf9a440);
    CHECK(tollFacing == 2 || tollFacing == 1);
    CHECK(tollRect.x0 == 4 || tollRect.x0 == 5);

    // --- OnMouseExit -------------------------------------------------------------------------------
    CHECK(tool->OnMouseExit());
    CHECK(tool->FeedbackSecondary() == false);
    CHECK(host.view.cursorTextCleared.size() == 1 && host.view.cursorTextCleared[0] == kPlaceLotCursorTextId);
    CHECK(tool->ErrorMessage().Strlen() == 0);

    // --- DoMessage ---------------------------------------------------------------------------------
    FakeMessage message;
    message.type = 0x277a9364;
    CHECK(!tool->DoMessage(&message));
    CHECK(host.localGrid.showCalls >= 1);
    message.type = 0x49d5b83f;
    CHECK(!tool->DoMessage(&message));
    CHECK(!tool->CoverageActive());
    message.type = 0x99ef1142;
    CHECK(!tool->DoMessage(&message));
    message.type = 0x99ef1143;
    CHECK(!tool->DoMessage(&message));
    message.type = 0xa6b79602;
    CHECK(!tool->DoMessage(&message));
    CHECK(tool->FootprintEffect() == nullptr && tool->DirectionEffect() == nullptr);
    message.type = 0x12345678;
    CHECK(!tool->DoMessage(&message));

    // --- Shutdown ----------------------------------------------------------------------------------
    CHECK(tool->Shutdown());
    CHECK(host.server.removed.size() == 6);
    CHECK(host.localGrid.shutdowns == 1);
    CHECK(host.view.removedGlobalEffects.size() == 1 && host.view.removedGlobalEffects[0] == 10);
    CHECK(tool->CoverageEffect() == nullptr);
    CHECK(tool->PreviewPropObject() == nullptr && tool->PreviewProps().empty());
    CHECK(tool->Shutdown());   // idempotent
    CHECK(tool->Release() == 0);
    tool = nullptr;

    // --- building-type mode and the commit path ----------------------------------------------------
    host.server.added.clear();
    host.view.costSet.clear();
    host.view.costCleared.clear();
    host.effects.effects.clear();
    host.effects.names.clear();
    host.view.pick[0] = 48.0f;
    host.view.pick[1] = 0.0f;
    host.view.pick[2] = 48.0f;
    host.demolition.cost = 100;
    host.lots.canCreateResult = 8;
    host.lots.hasLot = true;
    host.lots.createResult = true;
    host.budget.funds = 1000;
    host.building.price = 500;
    host.building.lotDeveloper.price = 0;
    host.view.pick[0] = 48.0f;
    host.view.pick[2] = 48.0f;
    // The building-type tool was handed the type; the reward category makes it single-use.
    host.boolProperties[0xea2e078b] = true;
    auto* plop = new cSC4ViewInputControlPlaceLot(1, 0, 0x7001, true);
    host.view.topControl = plop;
    CHECK(plop->Init());
    CHECK(plop->BuildingType() == 0x7001);
    CHECK(plop->BuildingPrice() == 500);
    CHECK(plop->PreviewBuildingType() == 0x7001);
    CHECK(plop->PlacementsRemaining() == 1);
    CHECK(plop->GlobalEffectId() == 10);
    CHECK(host.chooser.buildingPicks == 0);   // the building type was handed in
    CHECK(host.view.lastSelectedLot == 0x11111111);

    CHECK(plop->OnMouseMove(20, 20, 0));
    CHECK(plop->FeedbackValid());
    CHECK(plop->LotCost() == 600);   // 500 building + 0 lot + 100 demolition
    CHECK(host.view.costSet.size() == 1 && host.view.costSet.back() == 600);
    CHECK(host.effects.Count("PlopMode_Police_Inactive") == 1);

    // Click: demolition -> two zone placements -> the lot is created and built.
    // OnMouseDownL needs the tool to think the last preview was valid; the position is picked again.
    host.demolition.flags.clear();
    host.zones.zoneTypes.clear();
    CHECK(plop->OnMouseDownL(20, 20, 0));
    CHECK(host.demolition.flags.size() == 1);   // the cached cost skips CanPlaceLot, so only the commit
    CHECK(host.demolition.flags[0] == 0x2000);
    CHECK(host.zones.zoneTypes.size() == 2 && host.zones.zoneTypes[0] == 0 && host.zones.zoneTypes[1] == 0x0f);
    CHECK(host.zones.args[0][1] == false);   // the cost is not zero
    CHECK(host.zones.args[1][1] == true);
    CHECK(host.lots.createdRect.x0 == 2 && host.lots.createdRect.z0 == 2);
    CHECK(host.lots.createdFacing == plop->Facing());
    CHECK(host.lots.createdArg7 && host.lots.createdArg8);
    CHECK(host.building.lotDeveloper.constructs == 1);
    CHECK(host.building.lotDeveloper.lastDoConstruction == false);
    CHECK(host.budget.withdrawals.size() == 1 && host.budget.withdrawals[0] == 600);
    CHECK(host.budget.funds == 400);
    CHECK(host.utilityCalls == 1);
    CHECK(host.effects.Count("placement_effects_2x2") == 1);
    CHECK(host.effects.Count("PlopMode_Police_Plop") == 1);
    CHECK(plop->CoverageActive());
    CHECK(plop->PlacementsRemaining() == 0);   // 1 -> 0
    CHECK(host.view.ended == 0);               // the old counter was 1: EndInput
    // The just-placed footprint is remembered in world units, shrunk by two.
    CHECK(plop->ScreenClearRect().x0 == 2 * 16 + 2);
    CHECK(plop->ScreenClearRect().x1 == 4 * 16 - 2);

    // While the cursor stays over that footprint the validity check is suppressed, so the tool reports
    // "not placeable" even though the lot manager would accept the cell.
    host.lots.canCreateResult = 8;
    host.view.topControl = plop;
    CHECK(plop->OnMouseMove(21, 21, 0));
    CHECK(!plop->FeedbackValid());
    {
        bool valid = true;
        std::int64_t cost = 0;
        int result = -1;
        cRZString error;
        SC4Rect<int> rect;
        CHECK(!plop->CanPlaceLot(cS3DVector3{48.0f, 0.0f, 48.0f}, plop->Facing(), rect, valid, cost,
                                 result, error));
        CHECK(valid);   // the geometry flag is filled before the clear rectangle suppresses the check
    }
    // Moving away re-enables the check.
    host.view.pick[0] = 120.0f;
    host.view.pick[2] = 120.0f;
    CHECK(plop->OnMouseMove(22, 22, 0));
    CHECK(plop->FeedbackValid());

    // Failed construction credits the money back.
    host.budget.funds = 1000;
    plop->SetScreenClearRect(0, 0, -1, -1);
    host.building.lotDeveloper.constructResult = false;
    host.view.pick[0] = 200.0f;
    host.view.pick[2] = 200.0f;
    CHECK(plop->OnMouseMove(30, 30, 0));
    const std::size_t deposits = host.budget.deposits.size();
    CHECK(!plop->OnMouseDownL(30, 30, 0));
    CHECK(host.budget.deposits.size() == deposits + 1);
    CHECK(host.building.lotDeveloper.constructs == 2);

    // --- a mission restricts the placement to its target rectangle -----------------------------------
    host.city.mission.active = true;
    host.city.mission.target.rect[0] = 99;
    host.city.mission.target.rect[1] = 99;
    host.city.mission.target.rect[2] = 100;
    host.city.mission.target.rect[3] = 100;
    plop->SetScreenClearRect(0, 0, -1, -1);
    host.building.lotDeveloper.constructResult = true;
    host.zones.zoneTypes.clear();
    host.view.pick[0] = 200.0f;
    host.view.pick[2] = 200.0f;
    CHECK(plop->OnMouseMove(31, 31, 0));
    CHECK(!plop->OnMouseDownL(31, 31, 0));
    CHECK(host.zones.zoneTypes.empty());
    host.city.mission.active = false;

    // --- the toll-booth tool: the configuration is the toll booth id --------------------------------
    host.view.costSet.clear();
    host.filterIds = {kTollBoothLotConfiguration};
    host.chooser.lotConfiguration = kTollBoothLotConfiguration;
    host.configurations[kTollBoothLotConfiguration] = configuration;
    // A landmark: no 0x1401 group, so the effect id becomes 0xf2 and CanDemolishLot uses 0x800.
    host.groups.clear();
    host.groups.insert({0x7001, 0x1302});
    auto* boothTool = new cSC4ViewInputControlPlaceLot(0, 0, 0, false);
    host.view.topControl = boothTool;
    CHECK(boothTool->Init());
    CHECK(boothTool->TollBoothMode());
    CHECK(boothTool->LotConfigurationId() == kTollBoothLotConfiguration);
    host.demolition.flags.clear();
    host.demolition.result = true;
    host.view.pick[0] = 80.0f;
    host.view.pick[2] = 80.0f;
    CHECK(boothTool->OnMouseMove(40, 40, 0));
    CHECK(boothTool->GlobalEffectId() == 0xf2);
    CHECK(host.demolition.flags.size() == 1 && host.demolition.flags[0] == 0x800);
    CHECK(boothTool->Shutdown());
    CHECK(boothTool->Release() == 0);

    // --- ChangeLotConfiguration releases and rebuilds the effects ------------------------------------
    host.configurations.clear();
    cSC4LotConfiguration* second = MakeConfiguration(0x22222222, 1, 1, 0x7002, 0x7002);
    host.configurations[0x22222222] = second;
    host.building.resolvedType = 0x7002;
    host.groups.clear();
    plop->ChangeLotConfiguration(0x22222222);
    CHECK(plop->LotConfigurationId() == 0x22222222);
    CHECK(plop->CoverageCategories() == 1);   // the tool keeps its own building type
    plop->ChangeLotConfiguration(0x22222222);   // same id: no-op
    CHECK(plop->Shutdown());
    CHECK(plop->Release() == 0);

    // --- fire category, odd-sized footprint, reward placement count and the preview prop hide ------
    cSC4LotConfiguration* third = MakeConfiguration(0x33333333, 3, 1, 0x7003, 0x7003);
    host.configurations[0x33333333] = third;
    host.building.fire.insert(0x7003);
    host.building.resolvedType = 0x7003;
    host.groups.clear();                    // no 0x1401 / 0x1302 group: no global effect id
    host.uintProperties[0xbd86fd3] = 2;     // reward category 2
    host.rewardNetworkType = 4;             // 16 - 4
    host.filterIds = {0x33333333};
    host.chooser.lotConfiguration = 0x33333333;
    auto* fireTool = new cSC4ViewInputControlPlaceLot(0, 0, 0, false);
    host.view.topControl = fireTool;
    CHECK(fireTool->Init());
    CHECK(fireTool->GlobalEffectId() == 0);
    CHECK(fireTool->CoverageCategories() == 2);
    CHECK(std::strcmp(fireTool->CoverageEffectName().ToChar(), "PlopMode_Fire_Inactive") == 0);
    CHECK(std::strcmp(fireTool->PlacementEffectName().ToChar(), "PlopMode_Fire_Plop") == 0);
    CHECK(fireTool->PlacementsRemaining() == 12);   // the reward count overrides the single-use default
    SC4Rect<int> oddRect;
    float oddX = 0.0f, oddZ = 0.0f;
    CHECK(fireTool->GetLotCenterAndCellCoverage(0, 48.0f, 48.0f, oddX, oddZ, oddRect));
    CHECK(oddRect.x0 == 2 && oddRect.x1 == 4 && oddRect.z0 == 3 && oddRect.z1 == 3);
    CHECK(oddX == 56.0f && oddZ == 56.0f);
    CHECK(fireTool->GridSize() == 64.0f);   // 3 cells * 16 = 48 -> the 0x40 bucket
    host.lots.canCreateResult = 8;
    host.demolition.result = true;
    host.view.pick[0] = 48.0f;
    host.view.pick[2] = 48.0f;
    host.soundPlays = 0;
    CHECK(fireTool->OnMouseMove(50, 50, 0));
    CHECK(fireTool->FeedbackValid());
    auto* firePreview = static_cast<FakeProp*>(fireTool->PreviewPropObject());
    CHECK(firePreview != nullptr && firePreview->placed);
    CHECK(fireTool->OnMouseExit());
    CHECK(!firePreview->placed);            // the hide path removes the preview prop
    CHECK(host.soundPlays == 0);
    host.lots.hasLot = false;                    // no lot comes back: the placement fails
    CHECK(!fireTool->OnMouseDownL(50, 50, 0));
    CHECK(host.soundPlays == 1);
    CHECK(fireTool->Shutdown());
    CHECK(fireTool->Release() == 0);

    delete configuration;
    delete second;

    SetPlaceLotHost(nullptr);
    SetViewInputHost(nullptr);
    std::printf(failures ? "viewinputcontrolplacelot: %d FAILURES\n" : "viewinputcontrolplacelot: ok\n",
                failures);
    return failures ? 1 : 0;
}
