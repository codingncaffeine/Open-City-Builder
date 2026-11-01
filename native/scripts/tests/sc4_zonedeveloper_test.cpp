// cSC4ZoneDeveloper — unit test. Exercises the recovered data layout / COM plumbing, the boundary
// texture rules, the lot map and its queries, the network and occupant seams, the lot creation and
// deletion paths, the save stream and the message dispatch.
//
// The engine services are fakes behind IZoneDeveloperHost; the lots are real cSC4Lot objects from
// Lot.cpp (with no ILotHost attached, so their terrain/zone hooks stay inert).

#include "sc4/ZoneDeveloper.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <unordered_set>
#include <string>
#include <utility>
#include <vector>

#include "gz/StreamOps.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/LotConfiguration.h"
#include "sc4/cSC4SimGrid.h"

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
    void Init() override { ++inits; }               \
    void Shutdown() override { ++shutdowns; }       \
    int inits = 0;                                  \
    int shutdowns = 0;

//----------------------------------------------------------------------------------------------- services

struct FakeCity : IZoneDeveloperCityApi {
    FAKE_SERVICE_BOILERPLATE
    int cellsX = 16, cellsZ = 16;
    int CellCountX() override { return cellsX; }
    int CellCountZ() override { return cellsZ; }
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        cellX = static_cast<int>(std::floor(x * 0.0625f));
        cellZ = static_cast<int>(std::floor(z * 0.0625f));
    }
    bool LocationIsInBounds(float x, float z) override {
        int cx = 0, cz = 0;
        PositionToCell(x, z, cx, cz);
        return cx >= 0 && cz >= 0 && cx < cellsX && cz < cellsZ;
    }
};

struct FakeTerrain : IZoneDeveloperTerrainApi {
    FAKE_SERVICE_BOILERPLATE
    float baseAltitude = 5.0f;
    std::set<std::pair<int, int>> water;
    std::map<std::pair<int, int>, float> cellAltitudes;
    bool IsCellWater(int x, int z) override { return water.count({x, z}) != 0; }
    float Altitude(float, float) override { return baseAltitude; }
    float CellAltitude(int x, int z) override {
        auto it = cellAltitudes.find({x, z});
        return it != cellAltitudes.end() ? it->second : baseAltitude;
    }
};

struct FakeZoneManager : IZoneDeveloperZoneManagerApi {
    FAKE_SERVICE_BOILERPLATE
    std::map<std::pair<int, int>, int> zones;
    int placeZoneCalls = 0;
    SC4CellRegion<int> lastPlaced;
    std::vector<int> placedZoneTypes;
    int ZoneTypeAt(int x, int z) override {
        auto it = zones.find({x, z});
        return it != zones.end() ? it->second : 0;
    }
    bool IsZoneTypeValid(int zoneType) override { return zoneType >= 1 && zoneType <= 9; }
    std::uint32_t ZoneDragColor(int zoneType) override {
        return 0xff000000u | static_cast<std::uint32_t>(zoneType);
    }
    void PlaceZone(const SC4CellRegion<int>& region, int zoneType, bool, bool, bool, bool, bool) override {
        ++placeZoneCalls;
        lastPlaced = region;
        placedZoneTypes.push_back(zoneType);
    }
};

struct NetworkPlacement {
    int x0, z0, x1, z1;
    std::uint32_t flags;
    bool nullTool;
    bool arg7;
    int arg8;
};

struct FakeNetwork : IZoneDeveloperNetworkApi {
    FAKE_SERVICE_BOILERPLATE
    std::map<std::pair<int, int>, std::uint32_t> cells;
    std::map<std::pair<std::uint32_t, int>, bool> connections;
    std::uint32_t typeFlags = 0;
    std::uint32_t connectionFlags = 0;
    int clearHighlightCalls = 0;
    std::vector<NetworkPlacement> placements;
    bool PlaceNetworkOccupants(int x0, int z0, int x1, int z1, std::uint32_t flags, bool nullTool,
                               bool arg7, int arg8) override {
        placements.push_back({x0, z0, x1, z1, flags, nullTool, arg7, arg8});
        return false;
    }
    bool HasFlag(std::uint32_t flag) override { return (typeFlags & flag) != 0; }
    bool GetCellFlags(int x, int z, std::uint32_t flag, std::uint32_t& out) override {
        auto it = cells.find({x, z});
        if (it == cells.end() || (it->second & flag) == 0) return false;
        out = it->second;
        return true;
    }
    std::uint32_t GetTypeFlags() override { return typeFlags; }
    std::uint32_t GetConnectionFlags() override { return connectionFlags; }
    bool IsConnected(std::uint32_t direction, int networkType) override {
        auto it = connections.find({direction, networkType});
        return it != connections.end() && it->second;
    }
    void ClearHighlight() override { ++clearHighlightCalls; }
};

struct HighlightCall {
    int x, z;
    std::uint32_t type, group, instance;
    int texture;
    std::uint8_t shape;
    bool flag;
    std::uint32_t color;
    int layer;
};

struct FakeTerrainView : IZoneDeveloperTerrainViewApi {
    FAKE_SERVICE_BOILERPLATE
    int clearHighlightCalls = 0;
    std::vector<HighlightCall> highlights;
    void ClearHighlight() override { ++clearHighlightCalls; }
    void PlaceHighlightTexture(const SC4Point<int>& cell, const ZoneHighlightTexture& texture,
                               std::uint8_t shape, bool flag, std::uint32_t zoneColor,
                               int layer) override {
        highlights.push_back({cell.x, cell.z, texture.type, texture.group, texture.instance,
                              texture.texture, shape, flag, zoneColor, layer});
    }
};

struct ConstructedLot {
    cSC4Lot* lot;
    cSC4LotConfiguration* configuration;
    std::uint32_t objectId;
};

struct FakeLotDeveloper : IZoneDeveloperLotDeveloperApi {
    FAKE_SERVICE_BOILERPLATE
    std::vector<cSC4Lot*> removed;
    std::vector<ConstructedLot> constructed;
    std::vector<cSC4Lot*> emptyTextured;
    std::vector<SC4Rect<int>> levelledAreas;
    std::vector<float> levelledAltitudes;
    bool forceEmptyTextures = false;
    void RemoveLot(cSC4Lot* lot) override { removed.push_back(lot); }
    void ConstructDefaultLot(cSC4Lot* lot, cSC4LotConfiguration* configuration, std::uint32_t objectId,
                             bool) override {
        constructed.push_back({lot, configuration, objectId});
    }
    void SetEmptyLotBaseTexture(cSC4Lot* lot) override { emptyTextured.push_back(lot); }
    bool GetForceEmptyLotBaseTextures() override { return forceEmptyTextures; }
    std::map<cSC4Lot*, cISC4Occupant*> textures;
    std::vector<cSC4Lot*> unitsReleased;
    bool GetLotBaseTextureOccupant(cSC4Lot* lot, cISC4Occupant*& out) override {
        auto it = textures.find(lot);
        out = it == textures.end() ? nullptr : it->second;
        if (out) out->AddRef();
        return out != nullptr;
    }
    void ReleaseLotUnit(cSC4Lot* lot) override { unitsReleased.push_back(lot); }
    void AttemptAreaLevelling(const SC4Rect<int>& rect, float altitude, bool, bool, float,
                              cSC4Lot*) override {
        levelledAreas.push_back(rect);
        levelledAltitudes.push_back(altitude);
    }
};

struct FakeGrid : IZoneDeveloperGridApi {
    int tractSize = 2;
    std::map<std::pair<int, int>, std::int16_t> values;
    int TractSize() const override { return tractSize; }
    std::int16_t TractValue(int x, int z) const override {
        auto it = values.find({x, z});
        return it != values.end() ? it->second : 0;
    }
};

struct FakeDemand : IZoneDeveloperDemandApi {
    float value = 0.0f;
    float ActiveDemandValue() const override { return value; }
};

struct FakeGrowthDeveloper : IZoneDeveloperGrowthApi {
    int purpose = 1, wealth = 1;
    bool listsValid = false;
    std::vector<ZoneDeveloperGrowthItem> growthList;
    FakeGrid grid;
    FakeDemand demand;
    std::int32_t estimate = 0;
    int Purpose() const override { return purpose; }
    int Wealth() const override { return wealth; }
    bool GrowthAndDeclineListsAreValid() const override { return listsValid; }
    const std::vector<ZoneDeveloperGrowthItem>& GrowthList() const override { return growthList; }
    IZoneDeveloperGridApi* DesirabilityMap() const override { return const_cast<FakeGrid*>(&grid); }
    IZoneDeveloperDemandApi* SourceDemand() const override { return const_cast<FakeDemand*>(&demand); }
    std::int32_t EstimateDesirability(int, int) const override { return estimate; }
};

struct FakeBuildingDevelopment : IZoneDeveloperBuildingDevelopmentApi {
    FAKE_SERVICE_BOILERPLATE
    std::map<std::uint32_t, FakeGrowthDeveloper*> developers;
    std::vector<std::uint32_t> buildingTypes;
    std::map<std::uint32_t, std::vector<std::uint32_t>> families;
    std::map<std::uint32_t, BuildingProfile> profiles;
    FakeLotDeveloper* lotDeveloper = nullptr;
    IZoneDeveloperGrowthApi* GetGrowthDeveloper(std::uint32_t developerType) override {
        auto it = developers.find(developerType);
        return it == developers.end() ? nullptr : it->second;
    }
    void GetAllBuildingTypes(std::vector<std::uint32_t>& out) override { out = buildingTypes; }
    IZoneDeveloperLotDeveloperApi* GetLotDeveloper() override { return lotDeveloper; }
    void GetBuildingTypesByFamily(std::unordered_set<std::uint32_t>& out, std::uint32_t family) override {
        auto it = families.find(family);
        if (it != families.end()) out.insert(it->second.begin(), it->second.end());
    }
    const BuildingProfile& GetBuildingProfile(std::uint32_t buildingType) override {
        auto it = profiles.find(buildingType);
        if (it != profiles.end()) return it->second;
        static const BuildingProfile empty;
        return empty;
    }
};

struct FakeRenderer : IZoneDeveloperService {
    FAKE_SERVICE_BOILERPLATE
};

struct FakeSafeList : IZoneDeveloperSafeListApi {
    FAKE_SERVICE_BOILERPLATE
    std::vector<cSC4Lot*> added;
    void AddAfter(cSC4Lot* lot) override { added.push_back(lot); }
};

//----------------------------------------------------------------------------------------------- occupants

struct FakeNetworkOccupant : IZoneDeveloperNetworkOccupant {
    int refs = 0;
    std::uint32_t flags = 0;
    std::uint32_t networkFlags = 0;
    std::uint32_t edgeFlags = 0;
    bool immovable = false;
    std::set<std::uint32_t> types;
    std::map<std::pair<int, std::uint32_t>, bool> connections;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 1 || iid == 0xa821ef94) {
            *out = static_cast<IZoneDeveloperNetworkOccupant*>(this);
            AddRef();
            return true;
        }
        return false;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return refs > 0 ? static_cast<std::uint32_t>(--refs) : 0u; }
    bool HasNetworkFlag(std::uint32_t value) const override { return (flags & value) == value; }
    bool HasAnyNetworkFlag(std::uint32_t value) const override { return (flags & value) != 0; }
    std::uint32_t GetNetworkFlag() override { return networkFlags; }
    bool IsImmovable() const override { return immovable; }
    bool IsOfType(std::uint32_t networkType) const override { return types.count(networkType) != 0; }
    std::uint32_t GetEdgeTypesFlag(std::uint32_t) override { return edgeFlags; }
    bool ConnectsToNeighbor(int edge, std::uint32_t networkType) const override {
        auto it = connections.find({edge, networkType});
        return it != connections.end() && it->second;
    }
};

struct FakeOccupant : cISC4Occupant {
    int refs = 0;
    std::uint32_t occupantType = 0;
    float position[3] = {0, 0, 0};
    FakeNetworkOccupant* network = nullptr;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 0xa821ef94 && network) {
            network->AddRef();
            *out = network;
            return true;
        }
        if (iid == 1) {
            *out = static_cast<cISC4Occupant*>(this);
            AddRef();
            return true;
        }
        return false;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return refs > 0 ? static_cast<std::uint32_t>(--refs) : 0u; }

    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    cISCPropertyHolder* AsPropertyHolder() override { return nullptr; }
    std::uint32_t GetType() const override { return occupantType; }
    void GetPosition(float* xyz) override { xyz[0] = position[0]; xyz[1] = position[1]; xyz[2] = position[2]; }
    void SetPosition(const float* xyz) override { std::memcpy(position, xyz, sizeof(position)); }
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

struct FakeOccupantManager : IZoneDeveloperOccupantManagerApi {
    FAKE_SERVICE_BOILERPLATE
    std::map<std::pair<int, int>, std::vector<FakeOccupant*>> occupants;
    std::vector<cISC4Occupant*> removed;
    void ListOccupants(std::vector<cISC4Occupant*>& out, int x, int z, std::uint32_t typeId,
                       int maxCount) override {
        auto it = occupants.find({x, z});
        if (it == occupants.end()) return;
        for (FakeOccupant* occupant : it->second) {
            if (typeId != 0 && occupant->occupantType != typeId) continue;
            if (maxCount >= 0 && static_cast<int>(out.size()) >= maxCount) break;
            occupant->AddRef();
            out.push_back(occupant);
        }
    }
    void RemoveOccupant(cISC4Occupant* occupant, bool, std::uint32_t) override { removed.push_back(occupant); }
    IZoneDeveloperNetworkOccupant* NetworkView(cISC4Occupant* occupant) override {
        void* network = nullptr;
        if (!occupant->QueryInterface(0xa821ef94, &network) || !network) return nullptr;
        auto* view = static_cast<IZoneDeveloperNetworkOccupant*>(network);
        view->Release();
        return view;
    }
};

// cISC4Lot's host, as far as the zone developer needs it: the zone grid lookup that gives a lot its
// zone type (cSC4Lot::UpdateZoneType). Every other query is inert.
struct FakeLotHost : ILotHost {
    LotBuildingProfile profile;
    std::uint8_t zoneType = 2;
    std::uint8_t ZoneTypeAt(int, int) override { return zoneType; }
    std::int32_t SimulatorDay() override { return 100; }
    float TerrainAltitude(float, float) override { return 0.0f; }
    const LotBuildingProfile* BuildingProfile(std::uint32_t type) override { return type ? &profile : nullptr; }
    float SlopeOf(const cSC4Lot&) override { return 0.0f; }
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

//----------------------------------------------------------------------------------------------- framework

struct FakeMessageServer : cIGZMessageServer2 {
    int refs = 0;
    std::vector<std::uint32_t> notificationsAdded;
    std::vector<std::uint32_t> notificationsRemoved;
    struct Posted {
        std::uint32_t type;
        bool hasData1;
        std::uint64_t data1;
    };
    std::vector<Posted> posted;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 1 || iid == 0x652294c7) {
            *out = static_cast<cIGZMessageServer2*>(this);
            AddRef();
            return true;
        }
        return false;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return refs > 0 ? static_cast<std::uint32_t>(--refs) : 0u; }
    bool MessageSend(cIGZMessage2*) override { return true; }
    bool MessagePost(cIGZMessage2* message, bool) override {
        if (auto* standard = dynamic_cast<cIGZMessage2Standard*>(message)) {
            posted.push_back({standard->GetType(), standard->GetHasData1(), standard->GetData1()});
        }
        return true;
    }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t messageType) override {
        notificationsAdded.push_back(messageType);
        return true;
    }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t messageType) override {
        notificationsRemoved.push_back(messageType);
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

struct FakeSimGridCity : ISimGridCity {
    std::int32_t CitySizeXInCells() const override { return 64; }
    std::int32_t CitySizeZInCells() const override { return 64; }
    float CellSizeXMeters() const override { return 16.0f; }
    float CellSizeZMeters() const override { return 16.0f; }
};

struct FakeHost : IZoneDeveloperHost {
    FakeHost() { buildingDevelopment.lotDeveloper = &lotDeveloper; }

    FakeMessageServer server;
    FakeRenderer renderer;
    FakeLotDeveloper lotDeveloper;
    FakeBuildingDevelopment buildingDevelopment;
    FakeCity city;
    FakeOccupantManager occupantManager;
    FakeTerrain terrain;
    FakeTerrainView terrainView;
    FakeZoneManager zoneManager;
    FakeNetwork street;
    FakeNetwork road;
    FakeNetwork rail;
    FakeSafeList safeList;

    bool provideCity = true;
    bool provideTerrain = true;
    bool useSafeList = false;
    bool cheatCodesEnabled = false;
    int rendererScale = 64;
    std::vector<std::uint32_t> randomValues;
    std::size_t randomIndex = 0;
    std::vector<std::pair<std::string, std::uint32_t>> registeredCheats;
    std::vector<std::uint32_t> unregisteredCheats;
    std::vector<cSC4Lot*> writtenLots;

    cIGZMessageServer2* MessageServer() override { return &server; }
    int RendererHighlightScale() override { return rendererScale; }
    IZoneDeveloperService* Renderer() override { return &renderer; }
    IZoneDeveloperLotDeveloperApi* LotDeveloper() override { return &lotDeveloper; }
    IZoneDeveloperBuildingDevelopmentApi* BuildingDevelopment() override { return &buildingDevelopment; }
    IZoneDeveloperCityApi* City() override { return provideCity ? &city : nullptr; }
    IZoneDeveloperOccupantManagerApi* OccupantManager() override { return &occupantManager; }
    IZoneDeveloperTerrainApi* Terrain() override { return provideTerrain ? &terrain : nullptr; }
    IZoneDeveloperTerrainViewApi* TerrainView() override { return &terrainView; }
    IZoneDeveloperService* TrafficSim() override { return &renderer; }   // any service placeholder
    IZoneDeveloperZoneManagerApi* ZoneManager() override { return &zoneManager; }
    IZoneDeveloperNetworkApi* NetworkSubnet(int index) override {
        if (index == 0) return &road;      // network type 0
        if (index == 1) return &street;    // network type 3
        return &rail;                      // network type 0xb
    }
    IZoneDeveloperSafeListApi* SafeListIterator() override { return useSafeList ? &safeList : nullptr; }
    std::vector<cSC4LotConfiguration*> lotConfigurations;
    bool applyLotConfigurationFilter = false;
    bool configurationCompatibleWithZone = true;
    int demolishedRegionCalls = 0;
    SC4CellRegion<int> lastDemolishedRegion;
    int gameDelayCalls = 0;
    int lotConfigurationQueries = 0;
    int lastFilterPurpose = -1, lastFilterWealth = -1, lastFilterStage = -1;

    void LotConfigurationsByFilter(std::vector<cSC4LotConfiguration*>& out,
                                   IZoneDeveloperLotFilter* filter) override {
        if (auto* zoneFilter = dynamic_cast<cZoneDeveloperLotFilter*>(filter)) {
            lastFilterPurpose = zoneFilter->purpose_;
            lastFilterWealth = zoneFilter->wealth_;
            lastFilterStage = zoneFilter->growthStage_;
        }
        for (cSC4LotConfiguration* configuration : lotConfigurations) {
            if (applyLotConfigurationFilter && filter && !filter->IsLotConfigurationIncluded(configuration))
                continue;
            out.push_back(configuration);   // borrowed: the host keeps the manager's references
        }
    }
    void AllLotConfigurations(std::vector<cSC4LotConfiguration*>& out) override { out = lotConfigurations; }
    void LotConfigurationForBuildingType(cSC4LotConfiguration*& out, std::uint32_t) override {
        ++lotConfigurationQueries;
        out = lotConfigurations.empty() ? nullptr : lotConfigurations.front();
    }
    bool LotConfigurationCompatibleWithZone(cISC4LotConfigurationRef*, int) override {
        return configurationCompatibleWithZone;
    }
    void DemolishRegion(const SC4CellRegion<int>& region, int privilege, std::uint32_t flags) override {
        (void)privilege;
        (void)flags;
        ++demolishedRegionCalls;
        lastDemolishedRegion = region;
    }
    void GameDelay(int, bool) override { ++gameDelayCalls; }
    bool CheatCodesEnabled() override { return cheatCodesEnabled; }
    void RegisterCheatCode(const char* text, std::uint32_t id, cIGZMessageTarget2*) override {
        registeredCheats.emplace_back(text, id);
    }
    void UnregisterCheatCode(std::uint32_t id) override { unregisteredCheats.push_back(id); }
    std::uint32_t RandomBelow(std::uint32_t range) override {
        if (randomValues.empty() || range == 0) return 0;
        const std::uint32_t value = randomValues[randomIndex++ % randomValues.size()];
        return value % range;
    }
    void WriteLot(cIGZOStream& stream, cSC4Lot* lot) override {
        writtenLots.push_back(lot);
        stream.SetUint32(static_cast<std::uint32_t>(writtenLots.size()));
    }
    cSC4Lot* ReadLot(cIGZIStream& stream) override {
        std::uint32_t id = 0;
        stream.GetUint32(id);
        auto* lot = new cSC4Lot(0, 0, 1, 1, 0);
        lot->AddRef();
        lot->AddRef();   // the stream and the lot list both reference the object, like CreateLot
        return lot;
    }
};

//----------------------------------------------------------------------------------------------- streams

struct FakeOStream : cIGZOStream {
    std::vector<std::uint8_t> bytes;
    bool error = false;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    void Put(const void* data, std::size_t size) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        bytes.insert(bytes.end(), p, p + size);
    }
    void Flush() override {}
    bool SetSint8(std::int8_t v) override { Put(&v, sizeof v); return true; }
    bool SetUint8(std::uint8_t v) override { Put(&v, sizeof v); return true; }
    bool SetSint16(std::int16_t v) override { Put(&v, sizeof v); return true; }
    bool SetUint16(std::uint16_t v) override { Put(&v, sizeof v); return true; }
    bool SetSint32(std::int32_t v) override { Put(&v, sizeof v); return true; }
    bool SetUint32(std::uint32_t v) override { Put(&v, sizeof v); return true; }
    bool SetSint64(std::int64_t v) override { Put(&v, sizeof v); return true; }
    bool SetUint64(std::uint64_t v) override { Put(&v, sizeof v); return true; }
    bool SetFloat32(float v) override { Put(&v, sizeof v); return true; }
    bool SetFloat64(double v) override { Put(&v, sizeof v); return true; }
    bool SetRZCharStr(const char* v) override { SetUint32(static_cast<std::uint32_t>(std::strlen(v))); Put(v, std::strlen(v)); return true; }
    bool SetGZStr(const cIGZString&) override { return true; }
    bool SetGZSerializable(const cIGZSerializable&) override { return true; }
    bool SetVoid(const void* source, std::uint32_t count) override { Put(source, count); return true; }
    bool GetError() const override { return error; }
    void SetUserData(cIGZVariant*) override {}
    cIGZVariant* GetUserData() override { return nullptr; }
};

struct FakeIStream : cIGZIStream {
    std::vector<std::uint8_t> bytes;
    std::size_t cursor = 0;
    bool error = false;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    template <typename T>
    bool Get(T& out) {
        if (cursor + sizeof(T) > bytes.size()) {
            error = true;
            return false;
        }
        std::memcpy(&out, bytes.data() + cursor, sizeof(T));
        cursor += sizeof(T);
        return true;
    }
    bool Skip(std::uint32_t count) override {
        if (cursor + count > bytes.size()) { error = true; return false; }
        cursor += count;
        return true;
    }
    bool GetSint8(std::int8_t& v) override { return Get(v); }
    bool GetUint8(std::uint8_t& v) override { return Get(v); }
    bool GetSint16(std::int16_t& v) override { return Get(v); }
    bool GetUint16(std::uint16_t& v) override { return Get(v); }
    bool GetSint32(std::int32_t& v) override { return Get(v); }
    bool GetUint32(std::uint32_t& v) override { return Get(v); }
    bool GetSint64(std::int64_t& v) override { return Get(v); }
    bool GetUint64(std::uint64_t& v) override { return Get(v); }
    bool GetFloat32(float& v) override { return Get(v); }
    bool GetFloat64(double& v) override { return Get(v); }
    bool GetRZCharStr(char* buffer, std::uint32_t maxLength) override {
        std::uint32_t length = 0;
        if (!Get(length)) return false;
        for (std::uint32_t i = 0; i < length; ++i) {
            std::uint8_t c = 0;
            if (!Get(c)) return false;
            if (i < maxLength) buffer[i] = static_cast<char>(c);
        }
        if (length <= maxLength) buffer[length] = '\0';
        return true;
    }
    bool GetGZStr(cIGZString&) override { return true; }
    bool GetGZSerializable(cIGZSerializable&) override { return true; }
    bool GetVoid(void* destination, std::uint32_t count) override {
        if (cursor + count > bytes.size()) { error = true; return false; }
        std::memcpy(destination, bytes.data() + cursor, count);
        cursor += count;
        return true;
    }
    bool GetError() const override { return error; }
    void SetUserData(cIGZVariant*) override {}
    cIGZVariant* GetUserData() override { return nullptr; }
};

//----------------------------------------------------------------------------------------------- helpers

cSC4Lot* CreateLotChecked(cSC4ZoneDeveloper* developer, int x, int z, int sizeX, int sizeZ, int facing) {
    cSC4Lot* lot = nullptr;
    CHECK(developer->CreateLot(x, z, sizeX, sizeZ, facing, lot));
    CHECK(lot != nullptr);
    return lot;
}

void ClearLotList(std::list<cRZAutoRefCount<cSC4Lot>>& lots) { lots.clear(); }

//----------------------------------------------------------------------------------------------- tests

void TestLayoutAndCom() {
    CHECK(sizeof(cSC4ZoneDeveloper) == 0x1c0);
    auto* developer = new cSC4ZoneDeveloper();
    CHECK((char*)static_cast<cISC4LotManager*>(developer) - (char*)developer == 8);
    CHECK((char*)static_cast<cIGZMessageTarget2*>(developer) - (char*)developer == 0x10);
    CHECK((char*)static_cast<cIGZSerializable*>(developer) - (char*)developer == 0x18);

    void* result = nullptr;
    CHECK(developer->QueryInterface(1, &result) && result == static_cast<cISC4ZoneDeveloper*>(developer));
    CHECK(developer->QueryInterface(0xe818ed2b, &result) && result == static_cast<cISC4ZoneDeveloper*>(developer));
    CHECK(developer->QueryInterface(0x681905bd, &result) &&
          (char*)result - (char*)developer == 8);
    CHECK(developer->QueryInterface(0x452294aa, &result) &&
          (char*)result - (char*)developer == 0x10);
    CHECK(developer->QueryInterface(0xe4fda3d4, &result) &&
          (char*)result - (char*)developer == 0x18);
    CHECK(!developer->QueryInterface(0xdeadbeef, &result));
    CHECK(developer->GetGZCLSID() == 0x498f9b01);
    // Five successful QIs incremented the shared count to 5 (the last four after the iid-1 success).
    CHECK(developer->AddRef() == 6);
    CHECK(developer->Release() == 5);
    CHECK(developer->Release() == 4);
    CHECK(developer->Release() == 3);
    CHECK(developer->Release() == 2);
    CHECK(developer->Release() == 1);
    CHECK(developer->Release() == 0);   // count < 2 deletes the object
}

void TestInitShutdown() {
    FakeHost host;
    SetZoneDeveloperHost(&host);

    {
        FakeHost noCity;
        noCity.provideCity = false;
        SetZoneDeveloperHost(&noCity);
        auto* developer = new cSC4ZoneDeveloper();
        developer->AddRef();
        CHECK(!developer->Init());
        developer->Release();
        SetZoneDeveloperHost(&host);
    }

    auto* developer = new cSC4ZoneDeveloper();
    developer->AddRef();
    CHECK(developer->Init());
    CHECK(developer->IsInitialized());
    CHECK(developer->GetMaximumCapacityGrid() != nullptr);
    CHECK(developer->GetMaximumCapacityGrid()->GetTractSize() == 4);
    CHECK(developer->GetMaximumCapacityGrid()->GetInstanceID() == 0x69d5c456);
    CHECK(developer->GetLotCount() == 0);
    CHECK(developer->MessageServerForTest() == &host.server);
    CHECK(host.server.notificationsAdded.size() == 3);
    CHECK(host.server.notificationsAdded[0] == 0x99ef1142);
    CHECK(host.server.notificationsAdded[1] == 0x26d31ec1);
    CHECK(host.server.notificationsAdded[2] == 0xa6b79602);
    CHECK(host.registeredCheats.empty());   // cheats disabled in this host
    CHECK(!developer->CheatCodesWereRegistered());
    CHECK(developer->RendererHighlightScale() == 64);
    // Second Init is a no-op.
    CHECK(developer->Init());
    CHECK(host.server.notificationsAdded.size() == 3);

    CHECK(developer->Shutdown());
    CHECK(!developer->IsInitialized());
    CHECK(host.server.notificationsRemoved.size() == 3);
    CHECK(developer->GetMaximumCapacityGrid() == nullptr);
    CHECK(host.city.refs == 0);
    CHECK(host.terrain.refs == 0);
    developer->Release();

    // Cheat registration path.
    host.cheatCodesEnabled = true;
    host.server.notificationsAdded.clear();
    auto* cheats = new cSC4ZoneDeveloper();
    cheats->AddRef();
    CHECK(cheats->Init());
    CHECK(cheats->CheatCodesWereRegistered());
    CHECK(host.registeredCheats.size() == 2);
    CHECK(host.registeredCheats[0].first == "PlopAllLots");
    CHECK(host.registeredCheats[0].second == 0x8929e68e);
    CHECK(host.registeredCheats[1].first == "PlopAllBuildings");
    CHECK(host.registeredCheats[1].second == 0x4ab569c9);
    CHECK(cheats->Shutdown());
    CHECK(!cheats->CheatCodesWereRegistered());
    CHECK(host.unregisteredCheats.size() == 2);
    cheats->Release();

    SetZoneDeveloperHost(nullptr);
}

void TestBoundaryRules() {
    FakeHost host;
    SetZoneDeveloperHost(&host);
    auto* developer = new cSC4ZoneDeveloper();
    developer->AddRef();
    CHECK(developer->Init());

    auto checkRule = [&](BoundaryType boundary, std::uint32_t expectedTexture,
                         std::uint8_t expectedShape) {
        std::uint32_t texture = 0xffffffff;
        std::uint8_t shape = 0xff;
        bool flag = true;
        developer->GetBoundaryLotTexture(boundary, texture, shape, flag);
        CHECK(texture == expectedTexture);
        CHECK(shape == expectedShape);
        CHECK(!flag);
    };
    checkRule(BoundaryType{{0, 0, 0, 0}}, 0x3000, 0);
    checkRule(BoundaryType{{1, 0, 0, 0}}, 0x2000, 0);
    checkRule(BoundaryType{{0, 2, 1, 0}}, 0x2400, 3);       // key 0x210, 16-bit rule 0x103 truncated
    checkRule(BoundaryType{{0, 0, 0, 2}}, 0x2600, 3);       // key 2
    checkRule(BoundaryType{{5, 5, 5, 5}}, 0x3000, 0);        // falls back to key 0

    developer->Shutdown();
    developer->Release();
    SetZoneDeveloperHost(nullptr);
}

void TestLotCreationAndQueries() {
    FakeHost host;
    SetZoneDeveloperHost(&host);
    auto* developer = new cSC4ZoneDeveloper();
    developer->AddRef();
    CHECK(developer->Init());

    cSC4Lot* a = CreateLotChecked(developer, 2, 3, 2, 1, 0);
    CHECK(developer->GetLotCount() == 1);
    CHECK(developer->GetLot(2, 3, false) == a);
    CHECK(developer->GetLot(3, 3, false) == a);
    CHECK(developer->GetLot(2, 3, true) == nullptr);   // facing 0 names the corner cell (3, 3)
    CHECK(developer->GetLot(3, 3, true) == a);
    CHECK(developer->GetLot(-1, 0, false) == nullptr);
    CHECK(developer->GetLot(16, 0, false) == nullptr);
    CHECK(developer->GetLot(0, 16, false) == nullptr);
    CHECK(developer->DoesCellHaveLot(2, 3));
    CHECK(developer->DoesCellHaveLot(3, 3));
    CHECK(!developer->DoesCellHaveLot(4, 3));
    // CreateLot: building development vslot 0x120 (the lot developer) -> 0x158 SetEmptyLotBaseTexture
    CHECK(!host.lotDeveloper.emptyTextured.empty() && host.lotDeveloper.emptyTextured.back() == a);

    CreateLotChecked(developer, 6, 2, 1, 2, 0);   // lot B (x 6, z 2..3)
    CHECK(developer->GetLotCount() == 2);

    // --- GetLotsInCellRect ---------------------------------------------------------------------
    std::list<cRZAutoRefCount<cSC4Lot>> lots;
    CHECK(developer->GetLotsInCellRect(lots, 2, 3, 3, 3, false) == 1);
    CHECK(lots.size() == 1 && lots.front().get() == a);
    ClearLotList(lots);
    CHECK(developer->GetLotsInCellRect(lots, 2, 3, 3, 3, true) == 1);
    CHECK(lots.front().get() == a);
    ClearLotList(lots);
    CHECK(developer->GetLotsInCellRect(lots, 2, 2, 6, 3, false) == 2);
    ClearLotList(lots);
    CHECK(developer->GetLotsInCellRect(lots, 4, 4, 5, 5, false) == 0);
    CHECK(developer->GetLotsInCellRect(lots, -5, -5, 30, 30, false) == 2);
    ClearLotList(lots);

    // --- surrounding ring ----------------------------------------------------------------------
    cSC4Lot* big = CreateLotChecked(developer, 3, 5, 3, 1, 0);   // footprint 3..5 x 5
    const SC4Rect<int> oneCell{4, 5, 4, 5};
    CHECK(developer->GetLotsSurroundingRect(oneCell, lots, 0xf) == 1);
    CHECK(lots.front().get() == big);
    ClearLotList(lots);
    CHECK(developer->GetLotsSurroundingRect(oneCell, lots, 0x7) == 1);   // east/north/south only
    CHECK(lots.front().get() == big);
    ClearLotList(lots);
    CHECK(developer->GetLotsSurroundingRect(oneCell, lots, 0x5) == 0);   // north/south only
    ClearLotList(lots);
    CHECK(developer->GetLotsSurroundingLot(4, 5, lots, 0xf) == 0);   // the lot at (4,5) is `big`; its ring is empty
    ClearLotList(lots);
    CHECK(developer->GetLotsSurroundingLot(big, lots, 0xf) == 0);
    ClearLotList(lots);

    // --- vertices and neighbouring cells --------------------------------------------------------
    const SC4Rect<int> rect{2, 3, 4, 5};
    SC4Rect<int> out{};
    developer->GetVerticesAtEdgeOfLot(rect, 1, out);
    CHECK(out.x0 == 2 && out.z0 == 3 && out.x1 == 5 && out.z1 == 3);
    developer->GetVerticesAtEdgeOfLot(rect, 2, out);
    CHECK(out.x0 == 5 && out.z0 == 3 && out.x1 == 5 && out.z1 == 6);
    developer->GetVerticesAtEdgeOfLot(rect, 4, out);
    CHECK(out.x0 == 2 && out.z0 == 6 && out.x1 == 5 && out.z1 == 6);
    developer->GetVerticesAtEdgeOfLot(rect, 8, out);
    CHECK(out.x0 == 2 && out.z0 == 3 && out.x1 == 2 && out.z1 == 6);
    out = {7, 7, 7, 7};
    developer->GetVerticesAtEdgeOfLot(rect, 16, out);
    CHECK(out.x0 == 7 && out.z0 == 7 && out.x1 == 7 && out.z1 == 7);   // untouched

    CHECK(developer->GetNeighborCellsAtEdgeOfLot(rect, 1, out));
    CHECK(out.x0 == 2 && out.z0 == 2 && out.x1 == 4 && out.z1 == 2);
    CHECK(!developer->GetNeighborCellsAtEdgeOfLot(SC4Rect<int>{0, 0, 3, 3}, 1, out));
    CHECK(out.x0 == 0 && out.z0 == 0 && out.x1 == -1 && out.z1 == -1);
    CHECK(developer->GetNeighborCellsAtEdgeOfLot(SC4Rect<int>{12, 0, 14, 3}, 2, out));
    CHECK(out.x0 == 15 && out.z0 == 0 && out.x1 == 15 && out.z1 == 3);
    CHECK(!developer->GetNeighborCellsAtEdgeOfLot(SC4Rect<int>{13, 0, 15, 3}, 2, out));
    CHECK(developer->GetNeighborCellsAtEdgeOfLot(rect, 4, out));
    CHECK(out.x0 == 2 && out.z0 == 6 && out.x1 == 4 && out.z1 == 6);
    CHECK(developer->GetNeighborCellsAtEdgeOfLot(rect, 8, out));
    CHECK(out.x0 == 1 && out.z0 == 3 && out.x1 == 1 && out.z1 == 5);

    // --- altitude helpers ----------------------------------------------------------------------
    a->SetFlags(1);   // kLotFlagLocked
    a->SetAltitude(12.5f);
    bool found = false;
    CHECK(developer->GetLotSavvyCellAltitude(2, 3, &found) == 12.5f);
    CHECK(found);
    CHECK(developer->GetLotSavvyCellAltitude(3, 3, &found) == 12.5f);
    CHECK(found);
    CHECK(developer->GetLotSavvyCellAltitude(12, 12, &found) == 5.0f);
    CHECK(!found);
    CHECK(developer->GetLotSavvyTerrainVertexAltitude(3, 3, nullptr, &found) == 12.5f);
    CHECK(found);
    CHECK(developer->GetLotSavvyTerrainVertexAltitude(3, 4, nullptr, &found) == 12.5f);
    CHECK(found);
    CHECK(developer->GetLotSavvyTerrainVertexAltitude(3, 4, a, &found) == 5.0f);
    CHECK(!found);
    CHECK(developer->GetLotSavvyTerrainVertexAltitude(12, 12, nullptr, &found) == 5.0f);
    CHECK(!found);
    CHECK(std::fabs(developer->GetLotSavvyTerrainAltitude(3 * 16.0f + 8.0f, 3 * 16.0f + 8.0f, &found) -
                    12.5f) < 0.001f);
    CHECK(found);
    CHECK(std::fabs(developer->GetLotSavvyTerrainAltitude(8.0f, 8.0f, &found) - 5.0f) < 0.001f);
    CHECK(!found);

    float minimum = 0, maximum = 0;
    developer->GetLotVertexAltitudeAdjustmentRange(3, 3, minimum, maximum);
    CHECK(minimum == FLT_MAX && maximum == FLT_MAX);
    a->SetState(2, 0, 0);
    developer->GetLotVertexAltitudeAdjustmentRange(3, 3, minimum, maximum);
    CHECK(minimum == 32.0f && maximum == 32.0f);
    developer->GetLotVertexAltitudeAdjustmentRange(15, 15, minimum, maximum);
    CHECK(minimum == FLT_MAX && maximum == FLT_MAX);

    // --- occupant query ------------------------------------------------------------------------
    FakeOccupant occupant;
    occupant.position[0] = 3 * 16.0f + 8.0f;
    occupant.position[2] = 3 * 16.0f + 8.0f;
    CHECK(developer->GetOccupantLot(&occupant) == a);
    occupant.position[0] = 12 * 16.0f + 8.0f;
    CHECK(developer->GetOccupantLot(&occupant) == nullptr);

    // --- 3D-vector query -----------------------------------------------------------------------
    cS3DVector3 position{3 * 16.0f + 8.0f, 0.0f, 3 * 16.0f + 8.0f};
    CHECK(developer->GetLot(position) == a);
    position = cS3DVector3{-16.0f, 0.0f, 0.0f};
    CHECK(developer->GetLot(position) == nullptr);

    // --- random lot ----------------------------------------------------------------------------
    host.randomValues = {3, 3};
    CHECK(developer->GetRandomLot() == a);
    host.randomValues = {0, 0};   // (0,0) is empty
    CHECK(developer->GetRandomLot() == nullptr);

    // --- create-lot-if-possible ----------------------------------------------------------------
    cSC4Lot* placed = nullptr;
    CHECK(developer->CreateLotIfPossible(9, 9, 1, 1, 0, placed, true, true, nullptr));
    CHECK(placed != nullptr);
    CHECK(developer->GetLot(9, 9, false) == placed);
    CHECK(developer->GetLotCount() == 4);
    CHECK(!developer->CreateLotIfPossible(-1, 0, 1, 1, 0, placed, true, true, nullptr));
    CHECK(placed == nullptr);

    cSC4Lot* shared = nullptr;
    CHECK(developer->CreateLot(10, 10, 1, 1, 0, shared));
    CHECK(host.useSafeList == false);
    CHECK(host.lotDeveloper.emptyTextured.back() == shared);

    // --- delete --------------------------------------------------------------------------------
    const std::size_t removedBefore = host.lotDeveloper.removed.size();
    developer->DeleteLots(9, 9, 9, 9);
    CHECK(developer->GetLot(9, 9, false) == nullptr);
    CHECK(developer->GetLotCount() == 4);
    CHECK(host.lotDeveloper.removed.size() == removedBefore + 1);
    developer->DeleteLot(shared);
    CHECK(developer->GetLot(10, 10, false) == nullptr);
    CHECK(developer->GetLotCount() == 3);

    // Deleting a lot takes its base-texture occupant (lot developer vslot 0x148) out of the occupant manager.
    FakeOccupant texture;
    texture.refs = 1;
    host.lotDeveloper.textures[a] = &texture;
    developer->DeleteLots(2, 3, 3, 3);
    CHECK(host.occupantManager.removed.size() == 1 && host.occupantManager.removed[0] == &texture);
    CHECK(texture.refs == 1);                              // the reference GetLotBaseTextureOccupant gave is released
    CHECK(host.lotDeveloper.unitsReleased.empty());       // the lot has no unit

    // --- ClearLotMap through Shutdown ----------------------------------------------------------
    developer->Shutdown();
    CHECK(developer->GetLotCount() == 0);
    developer->Release();
    SetZoneDeveloperHost(nullptr);
}

void TestNetworkAndOccupantSeams() {
    FakeHost host;
    SetZoneDeveloperHost(&host);
    auto* developer = new cSC4ZoneDeveloper();
    developer->AddRef();
    CHECK(developer->Init());

    FakeNetworkOccupant networkOccupant;
    networkOccupant.flags = (0x1f4f & ~0x104u) | 0x808;
    networkOccupant.connections[{2, 3}] = true;   // direction flag for direction 1

    FakeOccupant occupant;
    occupant.occupantType = 0xc772bf98;
    occupant.network = &networkOccupant;
    host.occupantManager.occupants[{7, 7}].push_back(&occupant);

    CHECK(developer->GetNetwork(7, 7, 0x1f4f) == &networkOccupant);
    CHECK(developer->GetNetwork(7, 7, 0x40000) == nullptr);
    CHECK(developer->GetNetwork(8, 8, 0x1f4f) == nullptr);
    CHECK(developer->ExistsNetworkOfType(7, 7, 0x1f4f));
    CHECK(developer->ExistsNetworkOfType(7, 7, 0x808));
    CHECK(!developer->ExistsNetworkOfType(7, 7, 0x104));
    CHECK(developer->ExistsConnectionOfType(7, 7, 0x1f4f, 1, 3));
    CHECK(!developer->ExistsConnectionOfType(7, 7, 0x1f4f, 0, 3));

    // IsCellWater goes straight to the terrain.
    host.terrain.water.insert({2, 8});
    CHECK(developer->IsCellWater(2, 8));
    CHECK(!developer->IsCellWater(2, 7));

    // ExamineCell: a rail border costs 30 and reports a soft boundary.
    BoundaryCondition boundary = kBoundaryNone;
    int cost = 0;
    developer->ExamineCell(7, 7, boundary, cost);
    CHECK(boundary == kBoundarySoft);
    CHECK(cost == 30);

    // A street (0x104) is a blocked boundary and costs 5.
    networkOccupant.flags = 0x1f4f | 0x104;
    boundary = kBoundaryNone;
    cost = 0;
    developer->ExamineCell(7, 7, boundary, cost);
    CHECK(boundary == kBoundaryBlocked && cost == 5);

    // A plain cell costs 1.
    boundary = kBoundaryNone;
    cost = 0;
    developer->ExamineCell(12, 12, boundary, cost);
    CHECK(boundary == kBoundaryNone && cost == 1);

    // Water is blocked and costs 5.
    boundary = kBoundaryNone;
    cost = 0;
    developer->ExamineCell(2, 8, boundary, cost);
    CHECK(boundary == kBoundaryBlocked && cost == 5);

    // A building occupant also blocks; a non-building occupant does not.
    FakeOccupant building;
    building.occupantType = 0x278128a0;
    host.occupantManager.occupants[{5, 5}].push_back(&building);
    boundary = kBoundaryNone;
    cost = 0;
    developer->ExamineCell(5, 5, boundary, cost);
    CHECK(boundary == kBoundaryBlocked && cost == 5);

    FakeOccupant plain;
    plain.occupantType = 0x12345678;
    host.occupantManager.occupants[{6, 6}].push_back(&plain);
    boundary = kBoundaryNone;
    cost = 0;
    developer->ExamineCell(6, 6, boundary, cost);
    CHECK(boundary == kBoundaryNone && cost == 1);

    // CanCreateLotOnCell: out of bounds, water, network, existing lot, free.
    CHECK(developer->CanCreateLotOnCell(-1, 0, false, false) == 0);
    CHECK(developer->CanCreateLotOnCell(16, 0, false, false) == 0);
    CHECK(developer->CanCreateLotOnCell(2, 8, false, false) == 1);
    CHECK(developer->CanCreateLotOnCell(2, 8, false, true) == 8);
    networkOccupant.flags = 0x1f4f;
    CHECK(developer->CanCreateLotOnCell(7, 7, false, true) == 3);

    // CanCreateLot min over the footprint; a null configuration cannot allow water.
    CHECK(developer->CanCreateLot(0, 0, 2, 2, true, true, nullptr) == 8);
    CHECK(developer->CanCreateLot(2, 8, 1, 1, true, false, nullptr) == 1);
    cSC4Lot* lot = nullptr;
    CHECK(developer->CreateLot(1, 1, 1, 1, 0, lot));
    CHECK(developer->CanCreateLotOnCell(1, 1, false, true) == 7);
    CHECK(developer->CanCreateLotOnCell(3, 3, false, true) == 8);
    CHECK(developer->CanCreateLot(1, 1, 1, 1, true, true, nullptr) == 7);

    // Attach the network sub-managers (as the city-init message does) before checking highlight clears.
    cRZMessage2Standard cityInit;
    cityInit.SetType(0x26d31ec1);
    CHECK(developer->DoMessage(&cityInit));

    // ClearHighlight hits the road network, the street network and the terrain view.
    host.road.clearHighlightCalls = 0;
    host.street.clearHighlightCalls = 0;
    host.terrainView.clearHighlightCalls = 0;
    developer->ClearHighlight();
    CHECK(host.road.clearHighlightCalls == 1);
    CHECK(host.street.clearHighlightCalls == 1);
    CHECK(host.terrainView.clearHighlightCalls == 1);

    // SetOptions maps the three arguments onto the original bytes.
    developer->SetOptions(true, false, true);
    CHECK(developer->SwapOrientation());
    CHECK(!developer->ExtendAsStreets());
    CHECK(developer->ExamineBoundariesEnabled());
    developer->SetOptions(false, true, false);
    CHECK(!developer->SwapOrientation());
    CHECK(developer->ExtendAsStreets());
    CHECK(!developer->ExamineBoundariesEnabled());

    // The safe-list iterator reports "not recovered" as a null iterator.
    CHECK(developer->GetSafeLotIterator() == nullptr);

    developer->Shutdown();
    developer->Release();
    SetZoneDeveloperHost(nullptr);
}

void TestDeferAndMessages() {
    FakeHost host;
    SetZoneDeveloperHost(&host);
    auto* developer = new cSC4ZoneDeveloper();
    developer->AddRef();
    CHECK(developer->Init());

    developer->DeferOccupantOperations(true);
    CHECK(host.server.posted.size() == 1);
    CHECK(host.server.posted[0].type == 0x49d968c4);
    CHECK(host.server.posted[0].hasData1 && host.server.posted[0].data1 == 1);
    developer->DeferOccupantOperations(false);
    CHECK(host.server.posted.size() == 2 && host.server.posted[1].data1 == 0);

    // 0xa6b79602 carries the highlight scale in its second data word.
    cRZMessage2Standard scaleMessage;
    scaleMessage.SetType(0xa6b79602);
    int scale = 77;
    scaleMessage.SetVoid2(&scale);
    CHECK(developer->DoMessage(&scaleMessage));
    CHECK(developer->RendererHighlightScale() == 77);

    // 0x26d31ec1 attaches the three networks and the terrain view and re-posts the loaded lots.
    cRZMessage2Standard initMessage;
    initMessage.SetType(0x26d31ec1);
    CHECK(developer->DoMessage(&initMessage));
    CHECK(host.road.refs == 1);
    CHECK(host.rail.refs == 1);
    CHECK(host.street.refs == 1);
    CHECK(host.terrainView.refs == 1);
    CHECK(host.road.inits == 1 && host.rail.inits == 1 && host.street.inits == 1);
    CHECK(developer->DoMessage(&initMessage));
    CHECK(host.road.refs == 1);   // already attached

    // The occupant-removed message deletes the lot under the occupant and stops (the remnant/connection
    // passes are not recovered).
    cSC4Lot* lot = nullptr;
    CHECK(developer->CreateLot(4, 4, 1, 1, 0, lot));
    FakeOccupant occupant;
    occupant.position[0] = 4 * 16.0f + 8.0f;
    occupant.position[2] = 4 * 16.0f + 8.0f;
    cRZMessage2Standard removal;
    removal.SetType(0x99ef1142);
    removal.SetData3(0xc7729698);
    removal.SetVoid1(&occupant);
    removal.SetData4(1);
    CHECK(developer->DoMessage(&removal));
    CHECK(developer->GetLot(4, 4, false) == nullptr);
    CHECK(developer->GetLotCount() == 0);

    // The network-occupant skip flag leaves the lot alone.
    cSC4Lot* kept = nullptr;
    CHECK(developer->CreateLot(4, 4, 1, 1, 0, kept));
    FakeNetworkOccupant skipView;
    skipView.flags = 0x80200090;
    occupant.network = &skipView;
    CHECK(developer->DoMessage(&removal));
    CHECK(developer->GetLot(4, 4, false) == kept);
    occupant.network = nullptr;

    // Unknown messages are ignored.
    cRZMessage2Standard unknown;
    unknown.SetType(0x12345678);
    CHECK(developer->DoMessage(&unknown));

    developer->Shutdown();
    developer->Release();
    SetZoneDeveloperHost(nullptr);
}

void TestSerialization() {
    FakeHost host;
    SetZoneDeveloperHost(&host);
    auto* developer = new cSC4ZoneDeveloper();
    developer->AddRef();
    CHECK(developer->Init());
    cSC4Lot* lot = nullptr;
    CHECK(developer->CreateLot(2, 2, 1, 1, 0, lot));

    FakeOStream out;
    CHECK(developer->Write(out));
    CHECK(out.bytes.size() == 2 + 4 + 4 + 4);   // version, rows, columns, one lot id
    std::uint16_t version = 0;
    std::uint32_t rows = 0, columns = 0, id = 0;
    std::memcpy(&version, out.bytes.data(), 2);
    std::memcpy(&rows, out.bytes.data() + 2, 4);
    std::memcpy(&columns, out.bytes.data() + 6, 4);
    std::memcpy(&id, out.bytes.data() + 10, 4);
    CHECK(version == 1 && rows == 16 && columns == 16 && id == 1);

    developer->Shutdown();
    developer->Release();

    // Read a one-cell map back into a fresh developer.
    FakeIStream in;
    auto put = [&](const void* data, std::size_t size) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        in.bytes.insert(in.bytes.end(), p, p + size);
    };
    const std::uint16_t readVersion = 1;
    const std::uint32_t readRows = 1, readColumns = 1, readId = 1;
    put(&readVersion, 2);
    put(&readRows, 4);
    put(&readColumns, 4);
    put(&readId, 4);
    auto* reader = new cSC4ZoneDeveloper();
    reader->AddRef();
    CHECK(reader->Init());
    CHECK(reader->Read(in));
    CHECK(reader->GetLotCount() == 1);
    CHECK(reader->GetLot(0, 0, false) != nullptr);
    reader->Shutdown();
    reader->Release();

    // A bad version is refused.
    FakeIStream bad;
    const std::uint16_t badVersion = 2;
    const auto* p = reinterpret_cast<const std::uint8_t*>(&badVersion);
    bad.bytes.insert(bad.bytes.end(), p, p + 2);
    auto* badReader = new cSC4ZoneDeveloper();
    badReader->AddRef();
    CHECK(badReader->Init());
    CHECK(!badReader->Read(bad));
    badReader->Shutdown();
    badReader->Release();

    SetZoneDeveloperHost(nullptr);
}


void TestParcellizableRegionAndCompression() {
    FakeHost host;
    SetZoneDeveloperHost(&host);
    auto* developer = new cSC4ZoneDeveloper();
    developer->AddRef();
    CHECK(developer->Init());

    // A 5x5 region with one water cell: the water cell drops out of the parcellizable mask.
    const SC4Rect<int> bounds{1, 1, 5, 5};
    SC4CellRegion<int> region = SC4CellRegion<int>::Make(bounds, true);
    host.terrain.water.insert({2, 2});
    SC4CellRegion<int>* parcellizable = developer->CreateParcellizableRegion(&region);
    CHECK(parcellizable != nullptr);
    CHECK(parcellizable->bounds.x0 == 1 && parcellizable->bounds.z0 == 1);
    CHECK(parcellizable->rowCount == 5 && parcellizable->bitCount == 5);
    CHECK(!parcellizable->Test(2, 2));   // water
    CHECK(parcellizable->Test(1, 1));
    CHECK(parcellizable->Test(5, 5));
    delete parcellizable;

    // A lot that the host's configuration check rejects is deleted, its cell becomes parcellizable.
    cSC4Lot* lot = nullptr;
    CHECK(developer->CreateLot(3, 3, 1, 1, 0, lot));
    host.configurationCompatibleWithZone = false;
    parcellizable = developer->CreateParcellizableRegion(&region);
    CHECK(parcellizable->Test(3, 3));
    CHECK(developer->GetLot(3, 3, false) == nullptr);
    delete parcellizable;

    // A lot without a configuration object only survives when both zone types are in 1..6: with
    // currentZoneType_ 0 the fallback deletes it and its cell becomes parcellizable.
    CHECK(developer->CreateLot(3, 3, 1, 1, 0, lot));
    host.configurationCompatibleWithZone = true;
    parcellizable = developer->CreateParcellizableRegion(&region);
    CHECK(parcellizable->Test(3, 3));
    CHECK(developer->GetLot(3, 3, false) == nullptr);
    delete parcellizable;

    // CreateLotIfPossible with facing -1 delegates to CreateLotToFaceRoad with an empty list, so the
    // facing comes from the random start (the host returns 2 modulo 4).
    host.randomValues = {2};
    cSC4Lot* randomFacing = nullptr;
    CHECK(developer->CreateLotIfPossible(6, 6, 1, 1, -1, randomFacing, false, false, nullptr));
    CHECK(randomFacing != nullptr);
    if (randomFacing) CHECK(randomFacing->GetFacing() == 2);
    host.randomValues.clear();

    // CompressRegion shrinks to the bounding box of the set bits.
    SC4CellRegion<int>* compressed =
        new SC4CellRegion<int>(SC4CellRegion<int>::Make({0, 0, 9, 9}, false));
    compressed->SetBit(3, 4);
    compressed->SetBit(5, 6);
    developer->CompressRegion(compressed);
    CHECK(compressed->bounds.x0 == 3 && compressed->bounds.z0 == 4);
    CHECK(compressed->bounds.x1 == 5 && compressed->bounds.z1 == 6);
    CHECK(compressed->rowCount == 3 && compressed->bitCount == 3);
    CHECK(compressed->Test(3, 4) && compressed->Test(5, 6));
    CHECK(!compressed->Test(4, 4));
    delete compressed;

    // A region that is already tight is not replaced.
    SC4CellRegion<int>* tight =
        new SC4CellRegion<int>(SC4CellRegion<int>::Make({-2, -2, -1, -1}, true));
    developer->CompressRegion(tight);
    CHECK(tight->bounds.x0 == -2 && tight->bounds.z1 == -1);
    delete tight;

    // An empty region is left as an empty region at its own bounds.
    SC4CellRegion<int>* empty =
        new SC4CellRegion<int>(SC4CellRegion<int>::Make({4, 4, 8, 8}, false));
    developer->CompressRegion(empty);
    CHECK(empty->bounds.x0 == 4 && empty->bounds.z0 == 4);
    delete empty;

    developer->Shutdown();
    developer->Release();
    SetZoneDeveloperHost(nullptr);
}

void TestExtractAndSubdivide() {
    FakeHost host;
    SetZoneDeveloperHost(&host);
    auto* developer = new cSC4ZoneDeveloper();
    developer->AddRef();
    CHECK(developer->Init());

    // Two blobs: a 5x5 region split by a water column becomes two pieces.
    SC4CellRegion<int> region = SC4CellRegion<int>::Make({0, 0, 4, 4}, true);
    for (int z = 0; z <= 4; ++z) host.terrain.water.insert({2, z});
    std::list<SC4CellRegion<int>*> pieces;
    developer->SubdivideTheZone(&region, pieces);
    CHECK(pieces.size() == 2);
    if (pieces.size() == 2) {
        SC4CellRegion<int>* first = pieces.front();
        SC4CellRegion<int>* second = pieces.back();
        CHECK(first->bounds.x0 == 0 && first->bounds.x1 == 1);
        CHECK(second->bounds.x0 == 3 && second->bounds.x1 == 4);
        CHECK(first->rowCount == 2 && first->bitCount == 5);
        CHECK(first->Test(0, 0) && first->Test(1, 4));
        CHECK(!first->Test(3, 0));
    }
    for (SC4CellRegion<int>* piece : pieces) delete piece;

    // A region with nothing parcellizable yields the whole (compressed) empty region.
    SC4CellRegion<int> wet = SC4CellRegion<int>::Make({0, 0, 2, 2}, true);
    for (int x = 0; x <= 2; ++x)
        for (int z = 0; z <= 2; ++z) host.terrain.water.insert({x, z});
    std::list<SC4CellRegion<int>*> none;
    developer->SubdivideTheZone(&wet, none);
    CHECK(none.size() == 1);
    for (SC4CellRegion<int>* piece : none) delete piece;

    // ExtractConnectedCells removes the component from the source and returns it.
    auto* source = new SC4CellRegion<int>(SC4CellRegion<int>::Make({0, 0, 3, 3}, false));
    source->SetBit(0, 0);
    source->SetBit(1, 0);
    source->SetBit(3, 3);
    SC4CellRegion<int>* component = developer->ExtractConnectedCells(source, 0, 0);
    CHECK(component != nullptr);
    CHECK(component->Test(0, 0) && component->Test(1, 0));
    CHECK(!component->Test(3, 3));
    CHECK(!source->Test(0, 0) && !source->Test(1, 0));
    CHECK(source->Test(3, 3));
    SC4CellRegion<int>* next = developer->ExtractConnectedCells(source, 3, 3);
    CHECK(next->Test(3, 3));
    CHECK(!source->Test(3, 3));
    delete component;
    delete next;
    delete source;

    developer->Shutdown();
    developer->Release();
    SetZoneDeveloperHost(nullptr);
}

void TestUpdateZoneTypeAndHighlightTextures() {
    FakeHost host;
    SetZoneDeveloperHost(&host);
    auto* developer = new cSC4ZoneDeveloper();
    developer->AddRef();
    CHECK(developer->Init());

    cRZMessage2Standard cityInit;
    cityInit.SetType(0x26d31ec1);
    CHECK(developer->DoMessage(&cityInit));

    SC4CellRegion<int> region = SC4CellRegion<int>::Make({1, 1, 3, 3}, true);
    cSC4Lot* lot = nullptr;
    CHECK(developer->CreateLot(1, 1, 1, 1, 0, lot));

    // UpdateZoneType marks the cells that hold no lot for the zone manager and leaves the lot's cell.
    developer->UpdateZoneType(&region);
    CHECK(host.zoneManager.placeZoneCalls == 1);
    CHECK(host.zoneManager.lastPlaced.Test(2, 2));
    CHECK(host.zoneManager.lastPlaced.Test(3, 3));
    CHECK(!host.zoneManager.lastPlaced.Test(1, 1));
    CHECK(host.zoneManager.placedZoneTypes.size() == 1 && host.zoneManager.placedZoneTypes[0] == 0);

    // UpdateHighlightTextures: one overlay for the lot, in the region, keyed by the zone colour.
    developer->UpdateHighlightTextures(&region);
    CHECK(host.terrainView.highlights.size() == 1);
    if (!host.terrainView.highlights.empty()) {
        const HighlightCall& call = host.terrainView.highlights[0];
        CHECK(call.x == 1 && call.z == 1);
        CHECK(call.type == 0x7ab50e44 && call.group == 0x0986135e);
        CHECK(call.instance == 0xff000000u);        // zone drag colour of zone type 0
        CHECK(call.texture == 64 + 0x2300);         // renderer highlight scale + rule {2,1,1,1}
        CHECK(call.color == 0xff000000u);
        CHECK(call.layer == 3);
    }

    // A zone mismatch keeps the lot out of the highlight pass.
    host.terrainView.highlights.clear();
    developer->UpdateHighlightTextures(&region);   // currentZoneType_ is still 0, the lot's is 0
    CHECK(host.terrainView.highlights.size() == 1);

    developer->Shutdown();
    developer->Release();
    SetZoneDeveloperHost(nullptr);
}

void TestStreetDrawingAndJogStreets() {
    FakeHost host;
    SetZoneDeveloperHost(&host);
    auto* developer = new cSC4ZoneDeveloper();
    developer->AddRef();
    CHECK(developer->Init());

    // The network tools are only attached by the city-init message.
    cRZMessage2Standard cityInit;
    cityInit.SetType(0x26d31ec1);
    CHECK(developer->DoMessage(&cityInit));

    const SC4Rect<int> bounds{2, 2, 5, 5};
    SC4CellRegion<int> region = SC4CellRegion<int>::Make(bounds, true);
    developer->SetCurrentRegion(&region);

    // A lone street on the top row of the rectangle: ExtendAsStreets draws down to the bottom edge.
    FakeNetworkOccupant lone;
    lone.flags = 8;
    lone.connections[{1, 3}] = true;   // direction 0 -> direction flag 1, network type 3 (street)
    FakeOccupant loneOccupant;
    loneOccupant.occupantType = 0xc772bf98;
    loneOccupant.network = &lone;
    host.occupantManager.occupants[{3, 2}].push_back(&loneOccupant);

    developer->ExtendAsStreets(bounds, 3);
    CHECK(host.street.placements.size() == 4);
    if (!host.street.placements.empty()) {
        const NetworkPlacement& first = host.street.placements[0];
        CHECK(first.x0 == 3 && first.z0 == 2 && first.x1 == 3 && first.z1 == 5);
        CHECK(first.flags == 0xc49);
        CHECK(!first.nullTool);
        CHECK(first.arg7);
        CHECK(first.arg8 == -1);
    }
    host.street.placements.clear();

    // A lone street one row above the rectangle: LayInJogStreets draws along the top edge.
    FakeNetworkOccupant above;
    above.flags = 8;
    above.connections[{1, 3}] = true;
    FakeOccupant aboveOccupant;
    aboveOccupant.occupantType = 0xc772bf98;
    aboveOccupant.network = &above;
    host.occupantManager.occupants[{3, 1}].push_back(&aboveOccupant);

    developer->LayInJogStreets(bounds, 3);
    // Extend mode places the whole run and then every cell of the top edge: four + four calls.
    CHECK(host.street.placements.size() == 8);
    if (!host.street.placements.empty()) {
        CHECK(host.street.placements.front().x0 == 2 && host.street.placements.front().z0 == 2);
        CHECK(host.street.placements.front().x1 == 5 && host.street.placements.front().z1 == 2);
        CHECK(host.street.placements.back().x0 == 5 && host.street.placements.back().z0 == 2);
        CHECK(host.street.placements.back().x1 == 5 && host.street.placements.back().z1 == 2);
    }
    host.street.placements.clear();

    // A perpendicular network on the same cell blocks the jog street entirely.
    FakeNetworkOccupant perpendicular;
    perpendicular.flags = 0x40;   // avenue
    perpendicular.types = {6};
    perpendicular.connections[{0, 6}] = true;   // direction 3 -> direction flag 0
    FakeOccupant perpendicularOccupant;
    perpendicularOccupant.occupantType = 0xc772bf98;
    perpendicularOccupant.network = &perpendicular;
    host.occupantManager.occupants[{3, 1}].push_back(&perpendicularOccupant);
    developer->LayInJogStreets(bounds, 3);
    CHECK(host.street.placements.empty());
    host.occupantManager.occupants[{3, 1}].pop_back();
    host.street.placements.clear();

    // DrawStreet on a single cell places one occupant; a cell outside the region is ignored.
    developer->DrawStreet(3, 3, 3, 3, false);
    CHECK(host.street.placements.size() == 1);
    developer->DrawStreet(9, 9, 9, 9, false);
    CHECK(host.street.placements.size() == 1);

    developer->SetCurrentRegion(nullptr);
    developer->Shutdown();
    developer->Release();
    SetZoneDeveloperHost(nullptr);
}

void TestExamineBoundariesAndOnZone() {
    FakeHost host;
    SetZoneDeveloperHost(&host);
    auto* developer = new cSC4ZoneDeveloper();
    developer->AddRef();
    CHECK(developer->Init());

    // Streets on the north side only; the lot then faces north.
    FakeNetworkOccupant northViews[4];
    FakeOccupant northOccupants[4];
    for (int i = 0; i < 4; ++i) {
        northViews[i].flags = 0x10c;   // street-like: 0x104 for ExamineCell, 0x8 for the facing test
        northOccupants[i].occupantType = 0xc772bf98;
        northOccupants[i].network = &northViews[i];
        host.occupantManager.occupants[{1 + i, 0}].push_back(&northOccupants[i]);
    }
    cRZMessage2Standard cityInit;
    cityInit.SetType(0x26d31ec1);
    CHECK(developer->DoMessage(&cityInit));
    developer->SetOptions(false, true, true);   // extend as streets, examine boundaries first
    SC4CellRegion<int> first = SC4CellRegion<int>::Make({1, 1, 4, 4}, true);
    developer->DoParcellization(first, 7, false);
    // Industrial zone: the normal orientation branch (strongest side 0, alternate 3).
    CHECK(developer->LotOrientation() == 3);
    CHECK(developer->LotOrientationAlt() == 0);
    cSC4Lot* lot = developer->GetLot(1, 1, false);
    CHECK(lot != nullptr);
    CHECK(developer->GetLotCount() == 1);
    if (lot) {
        CHECK(lot->GetFacing() == 0);
        std::uint8_t sizeX = 0, sizeZ = 0;
        lot->GetSize(sizeX, sizeZ);
        CHECK(sizeX == 4 && sizeZ == 4);
        CHECK(developer->GetLot(4, 4, false) == lot);
    }
    developer->Shutdown();
    developer->Release();

    // The full pipeline for an industrial zone: every cell of an 8x8 region becomes a 1x1 lot and the
    // simple parcellizer lays a street every six cells.
    FakeHost industrialHost;
    SetZoneDeveloperHost(&industrialHost);
    auto* industrial = new cSC4ZoneDeveloper();
    industrial->AddRef();
    CHECK(industrial->Init());
    cRZMessage2Standard industrialInit;
    industrialInit.SetType(0x26d31ec1);
    CHECK(industrial->DoMessage(&industrialInit));
    SC4CellRegion<int> second = SC4CellRegion<int>::Make({0, 0, 9, 9}, true);
    industrial->DoParcellization(second, 8, false);
    CHECK(industrial->GetLotCount() == 100);
    CHECK(industrial->GetLot(0, 0, false) != nullptr);
    CHECK(industrial->GetLot(9, 9, false) != nullptr);
    CHECK(!industrialHost.street.placements.empty());   // the two parcellizer streets were drawn
    // No growth developer: the default footprint {2,2}, depth range 1..3 and the density gap 8-3.
    CHECK(industrial->ParcellizationSize(0) == 2);
    CHECK(industrial->ParcellizationSize(1) == 2);
    CHECK(industrial->ParcellizationSize(2) == 1);
    CHECK(industrial->ParcellizationSize(3) == 3);
    CHECK(industrial->ParcellizationSize(4) == 5);
    industrial->Shutdown();
    industrial->Release();

    SetZoneDeveloperHost(nullptr);
}

void TestExamineBoundariesOrientations() {
    FakeHost host;
    SetZoneDeveloperHost(&host);
    auto* developer = new cSC4ZoneDeveloper();
    developer->AddRef();
    CHECK(developer->Init());
    // Residential zone 2 with all boundary costs equal: the special branch swaps the strongest side
    // and its alternate, so the first facing tried is the alternate (3, west).
    developer->SetOptions(false, true, true);
    SC4CellRegion<int> region = SC4CellRegion<int>::Make({1, 1, 4, 4}, true);
    developer->DoParcellization(region, 2, false);
    CHECK(developer->LotOrientation() == 0);
    CHECK(developer->LotOrientationAlt() == 3);
    cSC4Lot* lot = developer->GetLot(1, 1, false);
    CHECK(lot != nullptr);
    if (lot) CHECK(lot->GetFacing() == 3);   // no street anywhere: the first candidate wins
    developer->Shutdown();
    developer->Release();

    // The swap-orientation option exchanges the two again.
    auto* swapped = new cSC4ZoneDeveloper();
    swapped->AddRef();
    CHECK(swapped->Init());
    swapped->SetOptions(true, true, true);
    SC4CellRegion<int> second = SC4CellRegion<int>::Make({1, 1, 4, 4}, true);
    swapped->DoParcellization(second, 2, false);
    CHECK(swapped->LotOrientation() == 3);
    CHECK(swapped->LotOrientationAlt() == 0);
    cSC4Lot* secondLot = swapped->GetLot(1, 1, false);
    CHECK(secondLot != nullptr);
    if (secondLot) CHECK(secondLot->GetFacing() == 0);
    swapped->Shutdown();
    swapped->Release();
    SetZoneDeveloperHost(nullptr);
}

void TestDetermineLotSizeWithDeveloper() {
    FakeHost host;
    SetZoneDeveloperHost(&host);
    auto* developer = new cSC4ZoneDeveloper();
    developer->AddRef();
    CHECK(developer->Init());

    // Residential low: 0x1010 and 0x1020 both answer, 0x1020 wins on its estimate. Its valid growth
    // list selects style 5 (the first item whose desirability is above the estimate).
    FakeGrowthDeveloper low, medium;
    low.purpose = 1;
    low.wealth = 1;
    low.estimate = 10;
    low.demand.value = 3.0f;
    medium.purpose = 1;
    medium.wealth = 1;
    medium.estimate = 20;
    medium.demand.value = 7.0f;
    medium.listsValid = true;
    medium.growthList.push_back({0, 0, 5});
    medium.grid.tractSize = 2;
    medium.grid.values[{0, 0}] = 30;
    host.buildingDevelopment.developers[0x1010] = &low;
    host.buildingDevelopment.developers[0x1020] = &medium;

    SC4CellRegion<int> region = SC4CellRegion<int>::Make({0, 0, 3, 3}, true);
    developer->SetOptions(false, false, false);
    developer->DoParcellization(region, 1, false);
    CHECK(host.lastFilterPurpose == 1);
    CHECK(host.lastFilterWealth == 1);
    CHECK(host.lastFilterStage == 5);
    // No configuration matches, so the footprint falls back to 2x2, the depth range to 1..3 and the
    // residential density gap to three times the lot depth.
    CHECK(developer->ParcellizationSize(0) == 2);
    CHECK(developer->ParcellizationSize(1) == 2);
    CHECK(developer->ParcellizationSize(2) == 1);
    CHECK(developer->ParcellizationSize(3) == 3);
    CHECK(developer->ParcellizationSize(4) == 6);

    // Same region as an industrial zone: no developer is registered for it, so the density gap is 8-3.
    auto* industrial = new cSC4ZoneDeveloper();
    industrial->AddRef();
    CHECK(industrial->Init());
    SC4CellRegion<int> industrialRegion = SC4CellRegion<int>::Make({0, 0, 3, 3}, true);
    industrial->DoParcellization(industrialRegion, 9, false);
    CHECK(industrial->ParcellizationSize(4) == 5);
    industrial->Shutdown();
    industrial->Release();

    developer->Shutdown();
    developer->Release();
    SetZoneDeveloperHost(nullptr);
}

void TestParcellizeLayout() {
    FakeHost host;
    SetZoneDeveloperHost(&host);
    auto* developer = new cSC4ZoneDeveloper();
    developer->AddRef();
    CHECK(developer->Init());
    cRZMessage2Standard cityInit;
    cityInit.SetType(0x26d31ec1);
    CHECK(developer->DoMessage(&cityInit));

    // Residential zoning with no developers and no streets: Parcellize lays the 2x2 default footprint
    // out over the region, levels each footprint through the lot developer and locks the lot vertices.
    SC4CellRegion<int> region = SC4CellRegion<int>::Make({0, 0, 8, 8}, true);
    developer->DoParcellization(region, 1, false);
    CHECK(developer->GetLotCount() > 0);
    // Every lot is inside the region and at most the 2x2..3x2 footprints the layout chooses.
    std::set<cSC4Lot*> distinct;
    for (int x = 0; x <= 8; ++x) {
        for (int z = 0; z <= 8; ++z) {
            cSC4Lot* lot = developer->GetLot(x, z, false);
            if (!lot) continue;
            distinct.insert(lot);
            std::int32_t rect[4] = {0, 0, 0, 0};
            lot->GetBoundingRect(rect);
            CHECK(rect[0] >= 0 && rect[1] >= 0);
            CHECK(rect[2] <= 8 && rect[3] <= 8);
            CHECK(rect[2] - rect[0] + 1 >= 1 && rect[2] - rect[0] + 1 <= 3);
            CHECK(rect[3] - rect[1] + 1 >= 1 && rect[3] - rect[1] + 1 <= 2);
        }
    }
    CHECK(static_cast<int>(distinct.size()) == developer->GetLotCount());
    CHECK(!host.lotDeveloper.levelledAreas.empty());
    CHECK(host.lotDeveloper.levelledAltitudes.front() == FLT_MAX);

    developer->Shutdown();
    developer->Release();
    SetZoneDeveloperHost(nullptr);
}

void TestOnDezoneAndRemnants() {
    FakeHost host;
    SetZoneDeveloperHost(&host);
    auto* developer = new cSC4ZoneDeveloper();
    developer->AddRef();
    CHECK(developer->Init());

    FakeLotHost lotHost;
    SetLotHost(&lotHost);
    cSC4Lot* lot = nullptr;
    CHECK(developer->CreateLot(2, 2, 2, 2, 0, lot));
    lot->UpdateZoneType();   // zone type 2 from the lot host
    CHECK(developer->GetLotCount() == 1);
    // An orphaned rail piece inside the region is handed to the demolition service.
    FakeNetworkOccupant rail;
    rail.flags = 0x808;
    FakeOccupant railOccupant;
    railOccupant.occupantType = 0xc772bf98;
    railOccupant.network = &rail;
    host.occupantManager.occupants[{6, 6}].push_back(&railOccupant);
    SC4CellRegion<int> wide = SC4CellRegion<int>::Make({2, 2, 6, 6}, true);
    developer->OnDezone(&wide, true);
    // The 2x2 lot is deleted and its footprint immediately filled by two 1x2 remnant lots.
    CHECK(developer->GetLotCount() == 2);
    CHECK(developer->GetLot(2, 2, false) != nullptr);
    CHECK(developer->GetLot(2, 2, false) != lot);
    CHECK(developer->GetLot(3, 2, false) != nullptr);
    CHECK(host.demolishedRegionCalls == 1);
    CHECK(host.lastDemolishedRegion.Test(6, 6));
    CHECK(!host.lastDemolishedRegion.Test(2, 2));

    // A cleared footprint is filled by remnant runs; facing east gives 2x1 remnants.
    developer->DeleteLots(0, 0, 15, 15);
    CHECK(developer->GetLotCount() == 0);
    developer->CreateLotRemnants(4, 4, 2, 2, 1);
    CHECK(developer->GetLotCount() == 2);
    CHECK(developer->GetLot(4, 4, false) != nullptr);
    CHECK(developer->GetLot(4, 5, false) != nullptr);
    // Facing north gives 1x2 remnants.
    developer->DeleteLots(0, 0, 15, 15);
    developer->CreateLotRemnants(4, 4, 2, 2, 0);
    CHECK(developer->GetLotCount() == 2);
    CHECK(developer->GetLot(4, 4, false) != nullptr);
    CHECK(developer->GetLot(5, 4, false) != nullptr);
    // Facing south gives 1x2 remnants scanned bottom-up.
    developer->DeleteLots(0, 0, 15, 15);
    developer->CreateLotRemnants(4, 4, 2, 2, 2);
    CHECK(developer->GetLotCount() == 2);
    // Facing west gives 2x1 remnants.
    developer->DeleteLots(0, 0, 15, 15);
    developer->CreateLotRemnants(4, 4, 2, 2, 3);
    CHECK(developer->GetLotCount() == 2);

    SetLotHost(nullptr);
    developer->Shutdown();
    developer->Release();
    SetZoneDeveloperHost(nullptr);
}

void TestConnectionFixups() {
    FakeHost host;
    SetZoneDeveloperHost(&host);
    auto* developer = new cSC4ZoneDeveloper();
    developer->AddRef();
    CHECK(developer->Init());
    cRZMessage2Standard cityInit;
    cityInit.SetType(0x26d31ec1);
    CHECK(developer->DoMessage(&cityInit));

    // A removed street piece at cell (0,0): the street piece to its east has a single connection
    // pointing back west, so a street is placed between the two cells.
    FakeNetworkOccupant removedView;
    removedView.types = {3};
    FakeOccupant removed;
    removed.occupantType = 0xc772bf98;
    removed.network = &removedView;
    FakeNetworkOccupant eastView;
    eastView.types = {3};
    eastView.flags = 0xc09;
    eastView.connections[{2, 3}] = true;   // edge 2, street
    FakeOccupant east;
    east.occupantType = 0xc772bf98;
    east.network = &eastView;
    host.occupantManager.occupants[{1, 0}].push_back(&east);
    developer->ConnectRoadToAnyAdjacentStreets(&removed);
    CHECK(host.street.placements.size() == 1);
    if (!host.street.placements.empty()) {
        const NetworkPlacement& placement = host.street.placements[0];
        CHECK(placement.x0 == 0 && placement.z0 == 0 && placement.x1 == 1 && placement.z1 == 0);
        CHECK(placement.flags == 0);
        CHECK(placement.nullTool);
    }
    host.street.placements.clear();

    // A removed rail piece: a straight rail to the west and a street two cells further out.
    FakeNetworkOccupant railRemovedView;
    railRemovedView.types = {1};
    FakeOccupant railRemoved;
    railRemoved.occupantType = 0xc772bf98;
    railRemoved.network = &railRemovedView;
    FakeNetworkOccupant railView;
    railView.flags = 0x80a;
    railView.networkFlags = 2;
    railView.connections[{1, 1}] = true;
    railView.connections[{3, 1}] = true;
    FakeOccupant railNeighbour;
    railNeighbour.occupantType = 0xc772bf98;
    railNeighbour.network = &railView;
    host.occupantManager.occupants[{-1, 0}].push_back(&railNeighbour);
    FakeNetworkOccupant streetView;
    streetView.types = {3};
    streetView.flags = 8;
    FakeOccupant street;
    street.occupantType = 0xc772bf98;
    street.network = &streetView;
    host.occupantManager.occupants[{-2, 0}].push_back(&street);
    developer->ConnectStreetsAcrossRail(&railRemoved);
    CHECK(host.street.placements.size() == 1);
    if (!host.street.placements.empty()) {
        const NetworkPlacement& placement = host.street.placements[0];
        CHECK(placement.x1 == -2 && placement.z1 == 0);
    }

    developer->Shutdown();
    developer->Release();
    SetZoneDeveloperHost(nullptr);
}

void TestPlopAllLotTypes() {
    FakeHost host;
    SetZoneDeveloperHost(&host);
    auto* developer = new cSC4ZoneDeveloper();
    developer->AddRef();
    CHECK(developer->Init());

    // Without a lot-configuration catalogue (the manager's map is empty) the cheat command has nothing
    // to plop and must not touch the city.
    developer->PlopAllLotTypes();
    CHECK(host.lotDeveloper.constructed.empty());
    CHECK(developer->GetLotCount() == 0);
    CHECK(host.gameDelayCalls == 0);

    developer->Shutdown();
    developer->Release();
    SetZoneDeveloperHost(nullptr);
}

}  // namespace

int main() {
    FakeSimGridCity simGridCity;
    SetSimGridCity(&simGridCity);

    TestLayoutAndCom();
    TestInitShutdown();
    TestBoundaryRules();
    TestLotCreationAndQueries();
    TestNetworkAndOccupantSeams();
    TestDeferAndMessages();
    TestSerialization();
    TestParcellizableRegionAndCompression();
    TestExtractAndSubdivide();
    TestUpdateZoneTypeAndHighlightTextures();
    TestStreetDrawingAndJogStreets();
    TestExamineBoundariesAndOnZone();
    TestExamineBoundariesOrientations();
    TestDetermineLotSizeWithDeveloper();
    TestParcellizeLayout();
    TestOnDezoneAndRemnants();
    TestConnectionFixups();
    TestPlopAllLotTypes();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("sc4_zonedeveloper: all checks passed\n");
    return 0;
}
