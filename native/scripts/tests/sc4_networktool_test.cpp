// cSC4NetworkTool — unit test for the recovered slice (state, accessors, init/shutdown, failure strings, bridge
// queries, network-type flag helpers, padded rect, the terrain highlight of the valid drag and the
// straight/diagonal splitter). The engine globals the original touches are replaced by INetworkToolHost.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "gz/cIGZMessage2.h"
#include "rz/cRZString.h"
#include "sc4/NetworkTool.h"
#include "sc4/ResourceAsFile.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/TileRules.h"
#include "sc4/SCProperty.h"
#include "rz/cRZVariant.h"

static int failures = 0;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);           \
            ++failures;                                                \
        }                                                              \
    } while (0)

class cIGZPersistResourceManager;
namespace SL {
cIGZPersistResourceManager* spRM = nullptr;   // the bridge type loader's resource manager (none here)
}
// The bridge specialization script loader: no resources here.
namespace sc4 {
bool SC4GetResourceAsFile(const cGZPersistResourceKey&, cIGZFile** out, bool) {
    *out = nullptr;
    return false;
}
}  // namespace sc4

using namespace sc4;

namespace sc4 {
// Reaches the private members the public interface cannot (see the friend declaration in NetworkTool.h).
struct NetworkToolTestAccess {
    static std::vector<tDraggedStep>& Steps(cSC4NetworkTool& t) { return t.steps_; }
    static std::vector<SC4PointU>& Cells(cSC4NetworkTool& t) { return t.cells_; }
    static std::vector<tBridge>& Bridges(cSC4NetworkTool& t) { return t.bridges_; }
    static std::vector<tIntersection>& Intersections(cSC4NetworkTool& t) { return t.intersections_; }
    static std::vector<int>& PossibleBridges(cSC4NetworkTool& t) { return t.possibleBridges_; }
    static std::vector<tNeighborConnection>& NeighborConnections(cSC4NetworkTool& t) {
        return t.neighborConnections_;
    }
    static std::vector<int>& WaterBridgeTypes(cSC4NetworkTool& t) { return t.waterBridgeTypes_; }
    static std::vector<int>& LandBridgeTypes(cSC4NetworkTool& t) { return t.landBridgeTypes_; }
    static cSC4NetworkPlacementParams& Placement(cSC4NetworkTool& t) { return t.placement_; }
    static int TotalSteps(const cSC4NetworkTool& t) { return t.totalSteps_; }
    static void SetTotalSteps(cSC4NetworkTool& t, int value) { t.totalSteps_ = value; }
    static int StraightSteps(const cSC4NetworkTool& t) { return t.straightSteps_; }
    static int DirectionA(const cSC4NetworkTool& t) { return t.directionA_; }
    static int DirectionB(const cSC4NetworkTool& t) { return t.directionB_; }
    static bool StepIsHorizontal(const cSC4NetworkTool& t) { return t.stepIsHorizontal_; }
    static std::uint32_t RendererAltitudeScale(const cSC4NetworkTool& t) { return t.rendererAltitudeScale_; }
    static std::uint32_t PlacementFlagMask(const cSC4NetworkTool& t) { return t.placementFlagMask_; }
    static float SlopeRise(const cSC4NetworkTool& t) { return t.slopeRise_; }
    static float CellSize(const cSC4NetworkTool& t) { return t.cellSize_; }
    static bool HasBridgeTypes(const cSC4NetworkTool& t) { return t.hasBridgeTypes_; }
    static bool Inited(const cSC4NetworkTool& t) { return t.inited_; }
    static void SolveMultiType(cSC4NetworkTool& t, const std::unordered_set<std::uint32_t>& cells, std::vector<std::uint32_t>& solved,
                               std::uint32_t type) {
        t.SolveMultiTypeIntersections(cells, solved, type);
    }
    static std::vector<tSolvedCell>& SolvedCellList(cSC4NetworkTool& t) { return t.solvedCellList_; }
    static void SetDrag(cSC4NetworkTool& t, int valid, int straight, int direction) {
        t.validSteps_ = valid;
        t.straightSteps_ = straight;
        t.directionA_ = direction;
    }
    static void SetPlacing(cSC4NetworkTool& t, bool place) { t.unknown98_ = place; }
    static void ComputeSmoothingSet(cSC4NetworkTool& t, int pad) { t.ComputeInitialSmoothingSet(pad); }
    static const std::unordered_set<std::uint32_t>& SmoothingSet(const cSC4NetworkTool& t) { return t.unknown128_; }
    static const std::unordered_set<std::uint32_t>& FixedVertices(const cSC4NetworkTool& t) { return t.unknown150_; }
    static void ResolveOneWay(cSC4NetworkTool& t, std::uint32_t type) { t.ResolveOneWayConflicts(type); }
    static std::unordered_set<std::uint32_t>& SolveSet(cSC4NetworkTool& t) { return t.solvedCells_; }
};
}  // namespace sc4

namespace {

// ---------------------------------------------------------------------------------------------------------
// counted fake for the seams that are COM-style in the original
// ---------------------------------------------------------------------------------------------------------
class FakeCounted final : public cIGZUnknown {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (--refs == 0) {
            ++destroyed;
            delete this;
            return 0;
        }
        return refs;
    }
    int refs = 0;
    int destroyed = 0;
};

struct FakePreview : INetworkPreview {
    std::uint32_t AddRef() override { return ++refs; }
    // The fake stays allocated so the test can read the counters after the tool released it (the fixture frees it).
    std::uint32_t Release() override {
        if (--refs == 0) {
            ++destroys;
            return 0;
        }
        return refs;
    }
    void Init() override { ++inits; }
    void Shutdown() override { ++shutdowns; }
    void Clear() override { clears++; }
    void Erase(const SC4PointU& cell) override { erased.push_back(cell); }
    void AddUntexturedSelection(const SC4PointU& cell, std::uint32_t color, const float* heights, float elevation,
                                bool lit) override {
        added.push_back(cell);
        colors.push_back(color);
        elevations.push_back(elevation);
        (void)heights;
        lit ? ++litAdds : ++unlitAdds;
    }
    void AdjustCellHeights(const SC4PointU&, const float*, bool) override { ++adjusts; }
    int refs = 0;
    int destroys = 0;
    int inits = 0, shutdowns = 0, clears = 0, litAdds = 0, unlitAdds = 0, adjusts = 0;
    std::vector<SC4PointU> erased, added;
    std::vector<std::uint32_t> colors;
    std::vector<float> elevations;
};

struct FakeWorldCache : INetworkWorldCache {
    void Clear() override {
        ++clears;
        cells.clear();
    }
    // IsTerrainOK reads the tool's own cache (i386 0x...): it answers the host's test cell. Cells made from a world
    // piece (CreateCell) answer first.
    cSC4NetworkCellInfo* GetCell(std::uint32_t key) override {
        auto it = cells.find(key);
        if (it != cells.end()) return &it->second;
        return source ? *source : nullptr;
    }
    cSC4NetworkCellInfo** source = nullptr;
    void GetAllCells(std::vector<cSC4NetworkCellInfo>& out) override { out.clear(); }
    cSC4NetworkCellInfo* CreateCell(std::uint32_t key) override { return &cells[key]; }
    std::map<std::uint32_t, cSC4NetworkCellInfo> cells;
    int clears = 0;
};

struct FakeConstraintSatisfier : INetworkConstraintSatisfier {
    void AddVertexToSolve(int, float) override {}
    bool GetHeight(int, float& out) override {
        out = 0.0f;
        return false;
    }
    void InsertRangeConstraint(int, float, float) override {}
    void InsertPreferredRangeConstraint(int, float, float) override {}
    void InsertPreferredValueConstraint(int, float) override {}
    void InsertFixedVertex(int, float) override {}
    void InsertEqualityConstraint(std::uint32_t, std::uint32_t) override {}
};

struct FakeTypeInfo : INetworkTypeInfo {
    std::uint32_t ViewInputControlId() const override { return 0x11; }
    std::uint32_t ToolViewId() const override { return 0x22; }
    std::uint32_t CursorId() const override { return 0x33; }
    std::uint32_t DirectionalCursorBase() const override { return 0x44; }
    std::uint32_t DefaultExemplarId() const override { return 0x12345678; }
    int PenWidthX() const override { return penX; }
    int PenWidthZ() const override { return penZ; }
    const std::vector<cSC4NetworkBridgeTypeInfo>& BridgeTypes() const override { return bridges; }
    const cSC4NetworkPlacementParams& PlacementParams() const override { return placement; }
    const cSC4NetworkSupportStructInfo* FindSupportStructure(std::uint32_t id) const override {
        return id == 42 ? &support : nullptr;
    }
    std::uint32_t PlacementExemplarId() const override { return 0x5a5a; }
    int penX = 2, penZ = 1;
    std::vector<cSC4NetworkBridgeTypeInfo> bridges;
    cSC4NetworkPlacementParams placement;
    cSC4NetworkSupportStructInfo support;
};

struct FakeHost : INetworkToolHost {
    ~FakeHost() override {
        if (terrain != nullptr) terrain->Release();
        if (occupants != nullptr) occupants->Release();
        if (cityView != nullptr) cityView->Release();
        if (orFilter != nullptr) orFilter->Release();
    }
    void RegisterShutdownCallback(bool (*fn)(void*), void* user) override {
        shutdownFn = fn;
        shutdownUser = user;
        ++registerShutdownCalls;
    }
    cIGZUnknown* AcquireTerrain() override {
        if (terrain == nullptr) terrain = new FakeCounted();
        terrain->AddRef();
        return terrain;
    }
    float TerrainCellSize() override { return cellSize; }
    bool TerrainAllowsNetwork(std::uint32_t x, std::uint32_t z) override {
        lastTerrainCell = x | (z << 16);
        return terrainAllows || water.count(x | (z << 16)) != 0;
    }
    // The bridge queries: water cells have their surface at waterSurface over a bed at waterBed, land is flat at landLevel.
    std::set<std::uint32_t> water;
    float landLevel = 300.0f, waterSurface = 250.0f, waterBed = 200.0f;
    bool occupiedRect = false;
    float CellMaxSurfaceLevel(std::uint32_t x, std::uint32_t z) override { return water.count(x | (z << 16)) ? waterSurface : landLevel; }
    void CellCornerAltitudes(std::uint32_t x, std::uint32_t z, float out[4]) override {
        for (int i = 0; i != 4; ++i) out[i] = water.count(x | (z << 16)) ? waterBed : landLevel;
    }
    bool NetworkOccupantInRect(int, int, int, int, std::uint32_t) override { return occupiedRect; }
    bool anyOccupant = false;
    bool OccupantInRect(int, int, int, int) override { return anyOccupant; }
    std::map<std::int32_t, float> vertexHeights;   // the terrain the land bridge scan reads (others 100)
    float TerrainVertexHeight(std::int32_t vertex) override {
        auto it = vertexHeights.find(vertex);
        return it == vertexHeights.end() ? 100.0f : it->second;
    }
    int inserted = 0;
    int InsertNetworkOccupant(cSC4NetworkOccupant*, std::uint32_t) override { return ++inserted; }
    int CityWidth() override { return width; }
    std::int32_t TerrainVertexIndex(std::int32_t x, std::int32_t z) override { return z * 1000 + x; }
    int CityHeight() override { return height; }
    cIGZUnknown* AcquireOccupantManager() override {
        if (occupants == nullptr) occupants = new FakeCounted();
        occupants->AddRef();
        return occupants;
    }
    cIGZUnknown* AcquireCityViewModel() override {
        if (cityView == nullptr) cityView = new FakeCounted();
        cityView->AddRef();
        return cityView;
    }
    void AddRenderLayer(INetworkPreview* layer, std::uint32_t layerId, std::int32_t priority) override {
        addedLayer = layer;
        layerIdUsed = layerId;
        priorityUsed = priority;
        ++addLayerCalls;
    }
    void RemoveRenderLayer(INetworkPreview* layer) override {
        removedLayer = layer;
        ++removeLayerCalls;
    }
    std::uint32_t RendererAltitudeScale() override { return altitudeScale; }
    void RegisterMessage(cIGZMessageTarget2* target, std::uint32_t messageType) override {
        registeredTarget = target;
        registeredMessage = messageType;
        ++registerMessageCalls;
    }
    void UnregisterMessage(cIGZMessageTarget2* target, std::uint32_t messageType) override {
        unregisteredTarget = target;
        unregisteredMessage = messageType;
        ++unregisterMessageCalls;
    }
    cIGZUnknown* CreateNetworkOrFilter(std::uint32_t flags) override {
        orFilterFlags = flags;
        if (orFilter == nullptr) orFilter = new FakeCounted();
        orFilter->AddRef();
        return orFilter;
    }
    INetworkPreview* CreatePreview() override {
        preview = new FakePreview();
        ++createPreviewCalls;
        return preview;
    }
    INetworkWorldCache* CreateWorldCache() override {
        if (worldCache == nullptr) worldCache = new FakeWorldCache();
        worldCache->source = &cell;
        return worldCache;
    }
    INetworkConstraintSatisfier* CreateConstraintSatisfier() override { return &satisfier; }
    // The pieces of a small world for DestroyOccupantsAndResolve: cell key -> piece (one reference each).
    std::map<std::uint32_t, cSC4NetworkOccupant*> pieces;
    std::vector<cSC4NetworkOccupant*> removedPieces, demolishedPieces;   // identities only
    cSC4NetworkOccupant* NetworkOccupantAt(std::uint32_t x, std::uint32_t z, std::uint32_t) override {
        auto it = pieces.find(x | (z << 16));
        if (it == pieces.end()) return nullptr;
        it->second->AddRef();
        return it->second;
    }
    void SendOccupantDemolished(cSC4NetworkOccupant* occupant) override { demolishedPieces.push_back(occupant); }
    bool RemoveNetworkOccupant(cSC4NetworkOccupant* occupant) override {
        for (auto it = pieces.begin(); it != pieces.end(); ++it) {
            if (it->second != occupant) continue;
            removedPieces.push_back(occupant);
            pieces.erase(it);
            occupant->Release();
            return true;
        }
        return false;
    }
    const INetworkTypeInfo* NetworkTypeInfo(std::uint32_t type) override {
        lastNetworkType = type;
        return &typeInfo;
    }
    cSC4NetworkCellInfo* GetCellInfo(std::uint32_t key) override {
        cellKey = key;
        return cell;
    }
    cIGZUnknown* GetTuningProperty(std::uint32_t type, std::uint32_t group, std::uint32_t instance,
                                   std::uint32_t propertyId) override {
        tuningType = type;
        tuningGroup = group;
        tuningInstance = instance;
        tuningProperty = propertyId;
        return &tuning;
    }
    bool LoadString(std::uint32_t group, std::uint32_t instance, cIGZString& out) override {
        loadStringGroup = group;
        loadStringInstance = instance;
        if (!loadStringsOk) return false;
        out.AssignFromCString("You cannot build there.");
        return true;
    }

    FakeTypeInfo typeInfo;
    FakeCounted tuning;
    FakePreview* preview = nullptr;
    FakeWorldCache* worldCache = nullptr;
    FakeCounted* terrain = nullptr;
    FakeCounted* occupants = nullptr;
    FakeCounted* cityView = nullptr;
    FakeCounted* orFilter = nullptr;
    FakeConstraintSatisfier satisfier;
    cSC4NetworkCellInfo* cell = nullptr;
    int width = 40, height = 30;
    float cellSize = 16.0f;
    bool terrainAllows = true;
    std::uint32_t altitudeScale = 77;
    bool loadStringsOk = false;
    bool (*shutdownFn)(void*) = nullptr;
    void* shutdownUser = nullptr;
    int registerShutdownCalls = 0, addLayerCalls = 0, removeLayerCalls = 0, registerMessageCalls = 0;
    int unregisterMessageCalls = 0, createPreviewCalls = 0;
    INetworkPreview* addedLayer = nullptr;
    INetworkPreview* removedLayer = nullptr;
    std::uint32_t layerIdUsed = 0;
    std::int32_t priorityUsed = 0;
    cIGZMessageTarget2* registeredTarget = nullptr;
    cIGZMessageTarget2* unregisteredTarget = nullptr;
    std::uint32_t registeredMessage = 0, unregisteredMessage = 0;
    std::uint32_t orFilterFlags = 0;
    std::uint32_t lastNetworkType = 0xffffffffu, lastTerrainCell = 0xffffffffu, cellKey = 0xffffffffu;
    std::uint32_t tuningType = 0, tuningGroup = 0, tuningInstance = 0, tuningProperty = 0;
    std::uint32_t loadStringGroup = 0, loadStringInstance = 0;
};

// cIGZMessage2Standard fake: DoMessage reads payload 2 without a QueryInterface, exactly like the original.
struct FakeMessage : cIGZMessage2Standard {
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Create(std::uint32_t, void**) const override { return false; }
    std::uint32_t GetType() const override { return type; }
    void SetType(std::uint32_t value) override { type = value; }
    bool IsEqual(const cIGZMessage2&) const override { return false; }
    bool IsLess(const cIGZMessage2&) const override { return false; }
    bool GetHasData1() const override { return false; }
    std::uint64_t GetData1() const override { return 0; }
    void* GetVoid1() const override { return nullptr; }
    void SetData1(std::uint64_t) override {}
    void SetVoid1(void*) override {}
    bool GetHasData2() const override { return void2 != nullptr; }
    std::uint64_t GetData2() const override { return 0; }
    void* GetVoid2() const override { return void2; }
    void SetData2(std::uint64_t) override {}
    void SetVoid2(void* value) override { void2 = value; }
    bool GetHasData3() const override { return false; }
    std::uint64_t GetData3() const override { return 0; }
    void* GetVoid3() const override { return nullptr; }
    void SetData3(std::uint64_t) override {}
    void SetVoid3(void*) override {}
    bool GetHasData4() const override { return false; }
    std::uint64_t GetData4() const override { return 0; }
    void* GetVoid4() const override { return nullptr; }
    void SetData4(std::uint64_t) override {}
    void SetVoid4(void*) override {}
    bool GetHasString() const override { return false; }
    bool GetString(std::uint32_t, void**) override { return false; }
    void SetString(cIGZString*) override {}
    bool GetHasIGZUnknown() const override { return false; }
    cIGZUnknown* GetIGZUnknown() override { return nullptr; }
    void SetIGZUnknown(cIGZUnknown*) override {}
    bool GetHasExtra() const override { return false; }
    bool GetExtra(std::uint32_t, void**) override { return false; }
    void SetExtra(cIGZSerializable*) override {}
    std::uint32_t type = 0;
    void* void2 = nullptr;
    int refs = 0;
};

cSC4NetworkBridgeTypeInfo MakeBridge(bool land, bool water, std::uint32_t keyA, std::uint32_t keyB,
                                     const char* name) {
    cSC4NetworkBridgeTypeInfo bridge;
    bridge.isLand = land;
    bridge.isWater = water;
    bridge.bitmapKeyA = keyA;
    bridge.bitmapKeyB = keyB;
    bridge.name = name;
    return bridge;
}

// A tool with a completed Init over a fake host; destroyed on scope exit.
struct Fixture {
    FakeHost host;
    cSC4NetworkTool* tool = nullptr;
    FakePreview* preview = nullptr;

    explicit Fixture(std::uint32_t type = kNetworkRoad) {
        host.typeInfo.bridges.push_back(MakeBridge(true, false, 0, 0, "Land Bridge"));
        host.typeInfo.bridges.push_back(MakeBridge(false, true, 0x1111, 0x2222, "Water Bridge"));
        host.typeInfo.placement.unknown10[0] = 0xaaaa;
        host.typeInfo.placement.maxSlope = 0.25f;
        host.typeInfo.placement.unknown24 = 7;
        host.typeInfo.placement.straightOrDiagonalOnly = 0;
        host.typeInfo.placement.list28 = {7, 8, 9};
        host.typeInfo.placement.list60 = {60, 61};
        host.typeInfo.placement.list78 = {78};
        host.typeInfo.placement.list90 = {1.5f, 2.5f};
        host.typeInfo.placement.unknownA8[3] = 0x1234;
        host.typeInfo.placement.listD0 = {0xd0};
        SetNetworkToolHost(&host);
        tool = new cSC4NetworkTool(type);
        tool->AddRef();
    }
    ~Fixture() {
        tool->Release();
        delete host.worldCache;
        host.worldCache = nullptr;
        delete host.preview;
        host.preview = nullptr;
        SetNetworkToolHost(nullptr);
    }
    void Init() {
        CHECK(tool->Init());
        preview = host.preview;
    }
};
}  // namespace

int main() {
    // ---- construction: the network flags and the default bridge state, for every network type -----------------
    {
        FakeHost host;
        SetNetworkToolHost(&host);
        for (std::uint32_t type = 0; type < kNetworkTypeCount; ++type) {
            auto* tool = new cSC4NetworkTool(type);
            tool->AddRef();
            CHECK(tool->GetPreferredBridge() == -1);
            CHECK(tool->GetBridgeAltitudeOverride() == -1.0f);
            CHECK(!tool->GetFailureState());
            std::vector<int> noBridges;
            CHECK(!tool->GetPossibleBridges(noBridges) && noBridges.empty());
            CHECK(tool->GetMajorBridge() == -1);
            float altitude = 0.0f;
            CHECK(!tool->GetMajorBridgeAltitude(altitude));
            CHECK(!tool->GetMajorBridgeReferenceAltitude(altitude));
            CHECK(!tool->IsMajorBridgeLandBridge());
            NeighborConnectionInfo info;
            CHECK(!tool->GetPossibleNeighborConnection(info));
            std::vector<SC4PointU> cells;
            tool->GetDraggedCells(cells);
            CHECK(cells.empty());
            CHECK(tool->GetValidStepCount() == 0 && tool->GetTotalStepCount() == 0 &&
                  tool->GetStraightStepCount() == 0);
            CHECK(tool->AllowBridges() && tool->AllowTunnels() && tool->IsStoppedByWater());
            CHECK(!tool->ShouldAutoConnect() && tool->StopPickAtWater());
            // networkFlags = ~kFlagForNetworkType[type] & 0x1f4f
            const std::uint32_t expected = ~kFlagForNetworkType[type] & 0x1f4f;
            CHECK(expected == (~(1u << type) & 0x1f4f));
            // (the private member is checked in the accessor round below; here we pin the table itself)
            CHECK(kFlagForNetworkType[type] == (1u << type));
            // the private member: ~flag & 0x1f4f
            CHECK(NetworkToolTestAccess::PlacementFlagMask(*tool) == 0);  // not Initialised yet
            tool->Release();
        }
        SetNetworkToolHost(nullptr);
    }

    // ---- COM identity ---------------------------------------------------------------------------------------
    {
        FakeHost host;
        SetNetworkToolHost(&host);
        auto* tool = new cSC4NetworkTool(kNetworkRoad);
        tool->AddRef();
        CHECK(tool->AddRef() == 2);
        void* out = nullptr;
        CHECK(tool->QueryInterface(1, &out) && out == static_cast<cISC4NetworkTool*>(tool));
        CHECK(tool->QueryInterface(kGZIID_cISC4NetworkTool, &out) && out == static_cast<cISC4NetworkTool*>(tool));
        CHECK(tool->QueryInterface(kGZIID_cIGZMessageTarget2, &out) &&
              out == static_cast<cIGZMessageTarget2*>(tool));
        // the message-target sub-object really is the +8 sub-object of the vtable layout
        CHECK(reinterpret_cast<char*>(out) == reinterpret_cast<char*>(tool) + 8);
        CHECK(!tool->QueryInterface(0xdeadbeefu, &out));
        // two AddRefs + three successful queries = five references
        CHECK(tool->Release() == 4);
        CHECK(tool->Release() == 3);
        CHECK(tool->Release() == 2);
        CHECK(tool->Release() == 1);
        CHECK(tool->Release() == 0);  // deletes; ASan checks the destructor
        SetNetworkToolHost(nullptr);
    }

    // ---- static init / shutdown callback --------------------------------------------------------------------
    {
        FakeHost host;
        SetNetworkToolHost(&host);
        CHECK(cSC4NetworkTool::StaticInit());
        CHECK(host.registerShutdownCalls == 1 && host.shutdownFn != nullptr && host.shutdownUser == nullptr);
        CHECK(cSC4NetworkTool::StaticInit());  // guarded
        CHECK(host.registerShutdownCalls == 1);
        CHECK(host.shutdownFn(nullptr));       // StaticShutdown
        CHECK(cSC4NetworkTool::StaticInit());  // re-arms after the shutdown callback
        CHECK(host.registerShutdownCalls == 2);
        host.shutdownFn(nullptr);
        cSC4NetworkTool::ReloadCheck();
        SetNetworkToolHost(nullptr);
    }

    // ---- Init / Shutdown ------------------------------------------------------------------------------------
    {
        Fixture f(kNetworkRoad);
        CHECK(f.tool->GetToolViewId() == 0);  // no type info before Init
        f.Init();
        CHECK(NetworkToolTestAccess::Inited(*f.tool));
        CHECK(f.host.lastNetworkType == kNetworkRoad);
        CHECK(f.preview->inits == 1);
        CHECK(f.host.addLayerCalls == 1 && f.host.addedLayer == f.preview && f.host.layerIdUsed == 5 &&
              f.host.priorityUsed == 1000);
        CHECK(f.host.registerMessageCalls == 1 &&
              f.host.registeredTarget == static_cast<cIGZMessageTarget2*>(f.tool) &&
              f.host.registeredMessage == kNetworkToolMessageAltitudes);
        CHECK(f.host.orFilterFlags == kNetworkFlagMask);  // road keeps 0x1f4f
        CHECK(NetworkToolTestAccess::PlacementFlagMask(*f.tool) == kNetworkFlagMask);
        CHECK(f.host.terrain != nullptr && f.host.terrain->refs == 2);  // Acquire + AddRef
        CHECK(f.host.occupants != nullptr && f.host.occupants->refs == 2);
        CHECK(f.host.cityView != nullptr && f.host.cityView->refs == 2);
        CHECK(NetworkToolTestAccess::CellSize(*f.tool) == 16.0f);
        // tan(0.25) * 16
        CHECK(NetworkToolTestAccess::SlopeRise(*f.tool) > 4.0f && NetworkToolTestAccess::SlopeRise(*f.tool) < 4.1f);
        CHECK(NetworkToolTestAccess::RendererAltitudeScale(*f.tool) == 77);
        CHECK(NetworkToolTestAccess::HasBridgeTypes(*f.tool));
        CHECK(NetworkToolTestAccess::WaterBridgeTypes(*f.tool).size() == 1);
        CHECK(NetworkToolTestAccess::WaterBridgeTypes(*f.tool)[0] == 1);
        CHECK(NetworkToolTestAccess::LandBridgeTypes(*f.tool).size() == 1);
        CHECK(NetworkToolTestAccess::LandBridgeTypes(*f.tool)[0] == 0);
        // the placement block was copied from the type info
        cSC4NetworkPlacementParams params;
        f.tool->GetPlacementParams(params);
        CHECK(params.unknown10[0] == 0xaaaa);
        CHECK(params.maxSlope == 0.25f);
        CHECK(params.unknown24 == 7 && params.straightOrDiagonalOnly == 0);
        CHECK(params.list28.size() == 3 && params.list28[2] == 9);
        CHECK(params.list60.size() == 2 && params.list78.size() == 1 && params.list90.size() == 2);
        CHECK(params.unknownA8[3] == 0x1234 && params.listD0.size() == 1 && params.listD0[0] == 0xd0);
        // the tool's own block passed back as the destination: vectors must not be touched
        cSC4NetworkPlacementParams& self = NetworkToolTestAccess::Placement(*f.tool);
        self.listD0.clear();
        f.tool->GetPlacementParams(self);
        CHECK(self.listD0.empty());
        CHECK(self.maxSlope == 0.25f);
        // cursors / pen widths / view id come from the network type info
        CHECK(f.tool->ViewInputControlId() == 0x11);
        CHECK(f.tool->GetToolViewId() == 0x22);
        CHECK(f.tool->CursorId() == 0x33);
        CHECK(f.tool->DirectionalCursorBase() == 0x44);
        int penX = 0, penZ = 0;
        f.tool->PenWidth(penX, penZ);
        CHECK(penX == 2 && penZ == 1);
        // a second Init is a no-op
        CHECK(f.tool->Init());
        CHECK(f.host.addLayerCalls == 1 && f.host.registerMessageCalls == 1);
        // Shutdown releases everything once
        CHECK(f.tool->Shutdown());
        CHECK(!NetworkToolTestAccess::Inited(*f.tool));
        CHECK(f.host.unregisterMessageCalls == 1 &&
              f.host.unregisteredTarget == static_cast<cIGZMessageTarget2*>(f.tool) &&
              f.host.unregisteredMessage == kNetworkToolMessageAltitudes);
        CHECK(f.host.removeLayerCalls == 1 && f.host.removedLayer == f.preview);
        CHECK(f.preview->shutdowns == 1 && f.preview->destroys == 1);
        // the fake host holds the one reference its Acquire*/Create* call took
        CHECK(f.host.terrain->refs == 1 && f.host.terrain->destroyed == 0);
        CHECK(f.host.occupants->refs == 1 && f.host.occupants->destroyed == 0);
        CHECK(f.host.cityView->refs == 1 && f.host.cityView->destroyed == 0);
        CHECK(f.host.orFilter->refs == 1 && f.host.orFilter->destroyed == 0);
        CHECK(f.tool->Shutdown());  // idempotent
        CHECK(f.host.unregisterMessageCalls == 1);
    }

    // ---- Init without a host --------------------------------------------------------------------------------
    {
        SetNetworkToolHost(nullptr);
        auto* tool = new cSC4NetworkTool(kNetworkRoad);
        tool->AddRef();
        CHECK(!tool->Init());
        CHECK(!NetworkToolTestAccess::Inited(*tool));
        tool->Release();
    }

    // ---- the OR-filter mask per network type (0x100187752) --------------------------------------------------
    {
        for (std::uint32_t type = 0; type < kNetworkTypeCount; ++type) {
            Fixture f(type);
            f.Init();
            const std::uint32_t expected =
                ((kNetworkFlagMask >> (type & 0x3f)) & 1) ? kNetworkFlagMask : kFlagForNetworkType[type];
            CHECK(NetworkToolTestAccess::PlacementFlagMask(*f.tool) == expected);
            CHECK(f.host.orFilterFlags == expected);
            CHECK(f.host.lastNetworkType == type);
        }
    }

    // ---- network-type flag helpers --------------------------------------------------------------------------
    {
        CHECK(cSC4NetworkTool::GetNetworkTypeFromFlag(0x0001) == 0);
        CHECK(cSC4NetworkTool::GetNetworkTypeFromFlag(0x0008) == 3);
        CHECK(cSC4NetworkTool::GetNetworkTypeFromFlag(0x0800) == 11);
        CHECK(cSC4NetworkTool::GetNetworkTypeFromFlag(0x1000) == 12);  // ground highway: not scanned
        CHECK(cSC4NetworkTool::GetNetworkTypeFromFlag(0) == 12);
        CHECK(cSC4NetworkTool::GetNetworkTypeFromFlag(0x0003) == 0);   // first hit wins
        // lowest set bit only
        CHECK(cSC4NetworkTool::GetFirstNetworkTypeFromFlags(0x0006) == 1);  // 0x2
        CHECK(cSC4NetworkTool::GetFirstNetworkTypeFromFlags(0x0c00) == 10);  // 0x400
        CHECK(cSC4NetworkTool::GetFirstNetworkTypeFromFlags(0x1000) == 12);
        CHECK(cSC4NetworkTool::GetFirstNetworkTypeFromFlags(0) == 12);
    }

    // ---- padded rect ----------------------------------------------------------------------------------------
    {
        Fixture f;
        f.Init();  // 40 x 30 cells
        SC4RectU rect{};
        f.tool->GetPaddedRectAroundCell(10, 10, rect, 5);
        CHECK(rect.x0 == 5 && rect.x1 == 15 && rect.z0 == 5 && rect.z1 == 15);
        f.tool->GetPaddedRectAroundCell(2, 1, rect, 5);
        CHECK(rect.x0 == 0 && rect.x1 == 7 && rect.z0 == 0 && rect.z1 == 6);
        f.tool->GetPaddedRectAroundCell(38, 28, rect, 5);
        CHECK(rect.x0 == 33 && rect.x1 == 39 && rect.z0 == 23 && rect.z1 == 29);
        f.tool->GetPaddedRectAroundCell(0, 0, rect, 0);
        CHECK(rect.x0 == 0 && rect.x1 == 0 && rect.z0 == 0 && rect.z1 == 0);
        f.tool->GetPaddedRectAroundCell(39, 29, rect, 10);
        CHECK(rect.x0 == 29 && rect.x1 == 39 && rect.z0 == 19 && rect.z1 == 29);
    }

    // ---- failure reason strings -----------------------------------------------------------------------------
    {
        Fixture f;
        f.Init();
        cRZString text;
        text.AssignFromCString("stale");
        f.tool->GetFailureReasonString(text);
        CHECK(std::strcmp(text.ToChar(), "") == 0);  // reason 0 leaves it empty
        CHECK(!f.tool->GetFailureState());
        cRZString reason;
        f.tool->GetFailureReasonString(7u, reason);  // no localized string in the fake
        CHECK(std::strcmp(reason.ToChar(), "## Missing failure reason string ##") == 0);
        f.host.loadStringsOk = true;
        f.tool->GetFailureReasonString(7u, reason);
        CHECK(f.host.loadStringGroup == kFailureReasonStringGroup && f.host.loadStringInstance == 7);
        CHECK(std::strcmp(reason.ToChar(), "You cannot build there.") == 0);
        CHECK(!f.tool->GetFailureState());
        f.tool->ClearFailureReason();
    }

    // ---- bridge queries -------------------------------------------------------------------------------------
    {
        Fixture f;
        f.Init();
        CHECK(f.tool->GetMajorBridge() == -1);
        tBridge bridge{};
        bridge.bridgeType = 3;
        bridge.elevation = 12.5f;
        bridge.referenceAltitude = 10.0f;
        bridge.landBridge = true;
        NetworkToolTestAccess::Bridges(*f.tool).push_back(bridge);
        tBridge second{};
        second.bridgeType = 5;
        second.elevation = 20.0f;
        second.referenceAltitude = 18.0f;
        NetworkToolTestAccess::Bridges(*f.tool).push_back(second);
        CHECK(f.tool->GetMajorBridge() == 5);  // the last element
        float altitude = 0.0f;
        CHECK(f.tool->GetMajorBridgeAltitude(altitude) && altitude == 20.0f);
        CHECK(f.tool->GetMajorBridgeReferenceAltitude(altitude) && altitude == 18.0f);
        CHECK(!f.tool->IsMajorBridgeLandBridge());

        std::vector<int> none;
        CHECK(!f.tool->GetPossibleBridges(none));
        NetworkToolTestAccess::PossibleBridges(*f.tool) = {4, 9};
        std::vector<int> bridges;
        CHECK(f.tool->GetPossibleBridges(bridges) && bridges.size() == 2 && bridges[1] == 9);
        f.tool->SetPreferredBridge(9);
        CHECK(f.tool->GetPreferredBridge() == 9);
        f.tool->SetBridgeAltitudeOverride(4.5f);
        CHECK(f.tool->GetBridgeAltitudeOverride() == 4.5f);

        cRZString name;
        f.tool->GetBridgeName(name, 1);
        CHECK(std::strcmp(name.ToChar(), "Water Bridge") == 0);
        f.tool->GetBridgeName(name, 0);
        CHECK(std::strcmp(name.ToChar(), "Land Bridge") == 0);
        std::uint32_t keyA = 0, keyB = 0;
        CHECK(!f.tool->GetBridgeBitmapKey(0, keyA, keyB));  // no bitmap keys on the land bridge
        CHECK(f.tool->GetBridgeBitmapKey(1, keyA, keyB) && keyA == 0x1111 && keyB == 0x2222);

        tNeighborConnection connection{};
        connection.info.x = 4;
        connection.info.z = 9;
        connection.info.unknown8 = 0x55;
        connection.info.unknownC = 1;
        NetworkToolTestAccess::NeighborConnections(*f.tool).push_back(connection);
        NeighborConnectionInfo info;
        CHECK(f.tool->GetPossibleNeighborConnection(info));
        CHECK(info.x == 4 && info.z == 9 && info.unknown8 == 0x55 && info.unknownC == 1);

        CHECK(f.tool->GetSupportStructureInfo(42) == &f.host.typeInfo.support);
        CHECK(f.tool->GetSupportStructureInfo(41) == nullptr);

        // GetOccupantExemplar: the resource key is {0x6534284a, group, instance} with property 0xa52160f5.
        CHECK(f.tool->GetOccupantExemplar(0x99, 0) == &f.host.tuning);
        CHECK(f.host.tuningType == kNetworkTuningType && f.host.tuningProperty == kNetworkTuningProperty);
        CHECK(f.host.tuningGroup == 0x12345678 && f.host.tuningInstance == 0x99);  // default group
        CHECK(f.tool->GetOccupantExemplar(0x99, 0xab) == &f.host.tuning);
        CHECK(f.host.tuningGroup == 0xab);
    }

    // ---- dragged-cell queries / steps -----------------------------------------------------------------------
    {
        Fixture f;
        f.Init();
        auto& cells = NetworkToolTestAccess::Cells(*f.tool);
        auto& steps = NetworkToolTestAccess::Steps(*f.tool);
        cells = {{10, 10}, {10, 11}, {11, 11}, {11, 12}};
        steps = {{0, 1, 0}, {2, 3, 3}};
        f.tool->SetValidStepCount(1);
        CHECK(f.tool->GetValidStepCount() == 1);
        CHECK(f.tool->GetTotalStepCount() == 0);  // Init does not set it; the drag does
        CHECK(f.tool->IsRegularStep(0));
        CHECK(!f.tool->IsRegularStep(1));  // direction 3 = stepped/short step
        SC4PointU last{};
        f.tool->GetLastValidPoint(last);  // steps_[0].lastCell -> cells_[1]
        CHECK(last.x == 10 && last.z == 11);
        std::vector<SC4PointU> out;
        f.tool->GetDraggedCells(out);
        CHECK(out.size() == 4 && out[3].x == 11 && out[3].z == 12);
        std::vector<SC4PointU> anchors = {{1, 2}, {3, 4}};
        f.tool->SetAnchorPoints(anchors);
        anchors.clear();
        f.tool->SetAnchorPoints(anchors);
    }

    // ---- SetTerrainHilight / SetValidStepCount --------------------------------------------------------------
    {
        Fixture f(kNetworkHighway);
        f.Init();
        auto& cells = NetworkToolTestAccess::Cells(*f.tool);
        auto& steps = NetworkToolTestAccess::Steps(*f.tool);
        cells = {{1, 1}, {1, 2}, {2, 1}, {2, 2}};
        steps = {{0, 1, 0}, {2, 3, 0}};
        NetworkToolTestAccess::SetTotalSteps(*f.tool, 2);
        // validSteps 0, total 2: the skip flag is false, so both steps are highlighted with the highway
        // elevation 15.5 and the 0x80ff0000 colour
        f.tool->SetValidStepCount(0);
        CHECK(f.preview->erased.size() == 4 && f.preview->added.size() == 4);
        CHECK(f.preview->colors.size() == 4 && f.preview->colors[0] == 0x80ff0000);
        CHECK(f.preview->elevations[3] == 15.5f);
        CHECK(f.preview->unlitAdds == 4 && f.preview->litAdds == 0);
        CHECK(f.preview->erased[0].x == 1 && f.preview->erased[0].z == 1);
        CHECK(f.preview->erased[3].x == 2 && f.preview->erased[3].z == 2);
        // validSteps == total - 1: the first (and only remaining) step is not skipped
        f.preview->erased.clear();
        f.preview->added.clear();
        f.tool->SetValidStepCount(1);
        CHECK(f.preview->erased.size() == 2 && f.preview->erased[0].x == 2 && f.preview->erased[0].z == 1);
        // validSteps == total: nothing to highlight
        f.preview->erased.clear();
        f.tool->SetValidStepCount(2);
        CHECK(f.preview->erased.empty());
        // a step in the middle: the quirk skips the first iteration, then highlights the rest
        f.preview->erased.clear();
        NetworkToolTestAccess::Steps(*f.tool) = {{0, 1, 0}, {2, 3, 0}, {0, 1, 0}, {2, 3, 0}};
        NetworkToolTestAccess::SetTotalSteps(*f.tool, 4);
        f.tool->SetValidStepCount(2);
        CHECK(f.preview->erased.size() == 2);  // only step 3 (cells 0..1), step 2 was skipped
        // a non-highway tool uses a flat elevation
        Fixture road(kNetworkRoad);
        road.Init();
        NetworkToolTestAccess::Cells(*road.tool) = {{5, 5}};
        NetworkToolTestAccess::Steps(*road.tool) = {{0, 0, 0}};
        NetworkToolTestAccess::SetTotalSteps(*road.tool, 1);
        road.tool->SetValidStepCount(0);
        CHECK(road.preview->elevations.size() == 1 && road.preview->elevations[0] == 0.0f);
    }

    // ---- terrain / cell queries -----------------------------------------------------------------------------
    {
        Fixture f;
        f.Init();
        cSC4NetworkCellInfo cell{};
        f.host.cell = &cell;
        // no occupant
        std::uint32_t id = 0;
        CHECK(!f.tool->OccupantExists(3, 4, 0xffff, id));
        CHECK(f.host.cellKey == (3u | (4u << 16)));
        cell.hasOccupant = true;
        cell.networkFlags = 0x0002;
        cell.tileNeighborhoods[kNetworkRail] = 0xbeef;
        CHECK(!f.tool->OccupantExists(3, 4, 0x0001, id));  // mask does not match
        CHECK(f.tool->OccupantExists(3, 4, 0x0002, id));
        CHECK(id == cell.tileNeighborhoods[kNetworkRoad]);  // the tool's own network type indexes the array
        {
            Fixture rail(kNetworkRail);
            rail.Init();
            rail.host.cell = &cell;
            CHECK(rail.tool->OccupantExists(3, 4, 0x0002, id) && id == 0xbeef);
        }
        SetNetworkToolHost(&f.host);  // the rail fixture unregistered itself

        // IsTerrainOK: terrain refusal answers true (quirk), a missing/blocked cell answers false
        f.host.terrainAllows = false;
        CHECK(f.tool->IsTerrainOK(1, 2));
        f.host.terrainAllows = true;
        cell.buildable = false;
        CHECK(!f.tool->IsTerrainOK(1, 2));
        cell.buildable = true;
        CHECK(f.tool->IsTerrainOK(1, 2));
        f.host.cell = nullptr;
        CHECK(!f.tool->IsTerrainOK(1, 2));

        // IdentifyStartingConditions: cells_ is empty -> false (the original would dereference element 0)
        int reason = 5;
        CHECK(!f.tool->IdentifyStartingConditions(reason));
        auto& cells = NetworkToolTestAccess::Cells(*f.tool);
        cells = {{1, 2}};
        f.host.cell = &cell;
        cell.buildable = true;
        CHECK(f.tool->IdentifyStartingConditions(reason));
        cell.buildable = false;
        CHECK(!f.tool->IdentifyStartingConditions(reason) && reason == 0);
    }

    // ---- DoMessage ------------------------------------------------------------------------------------------
    {
        Fixture f;
        f.Init();
        FakeMessage message;
        std::uint32_t payload = 4242;
        message.SetVoid2(&payload);
        message.type = kNetworkToolMessageAltitudes;
        CHECK(f.tool->DoMessage(&message));
        CHECK(NetworkToolTestAccess::RendererAltitudeScale(*f.tool) == 4242);
        message.type = 0x12345678;
        message.SetVoid2(nullptr);
        CHECK(f.tool->DoMessage(&message));  // other messages are ignored
        CHECK(NetworkToolTestAccess::RendererAltitudeScale(*f.tool) == 4242);
    }

    // ---- Reset ----------------------------------------------------------------------------------------------
    {
        Fixture f;
        f.Init();
        NetworkToolTestAccess::PossibleBridges(*f.tool) = {1, 2};
        NetworkToolTestAccess::Cells(*f.tool) = {{1, 1}};
        f.tool->SetPreferredBridge(3);
        f.tool->SetBridgeAltitudeOverride(2.0f);
        f.tool->Reset();
        CHECK(f.host.worldCache->clears == 1);
        std::vector<int> none;
        CHECK(!f.tool->GetPossibleBridges(none));
        CHECK(f.tool->GetPreferredBridge() == -1 && f.tool->GetBridgeAltitudeOverride() == -1.0f);
        CHECK(NetworkToolTestAccess::Cells(*f.tool).empty());
    }

    // ---- IsSimpleDiagonalNetwork ----------------------------------------------------------------------------
    {
        cSC4NetworkCellInfo cell{};
        cell.tileNeighborhoods[kNetworkRoad] = 0x00000003;  // edge 0: diagonal kind 3 -> one diagonal
        CHECK(cSC4NetworkTool::IsSimpleDiagonalNetwork(cell, kNetworkRoad));
        cell.tileNeighborhoods[kNetworkRoad] = 0x00000303;  // two diagonals
        CHECK(cSC4NetworkTool::IsSimpleDiagonalNetwork(cell, kNetworkRoad));
        cell.tileNeighborhoods[kNetworkRoad] = 0x00030303;  // three: not a plain diagonal tile
        CHECK(!cSC4NetworkTool::IsSimpleDiagonalNetwork(cell, kNetworkRoad));
        cell.tileNeighborhoods[kNetworkRoad] = 0x00000203;  // one straight + one diagonal
        CHECK(!cSC4NetworkTool::IsSimpleDiagonalNetwork(cell, kNetworkRoad));
        cell.tileNeighborhoods[kNetworkRoad] = 0x00000002;  // straight only
        CHECK(!cSC4NetworkTool::IsSimpleDiagonalNetwork(cell, kNetworkRoad));
        cell.tileNeighborhoods[kNetworkRoad] = 0;  // no edges
        CHECK(!cSC4NetworkTool::IsSimpleDiagonalNetwork(cell, kNetworkRoad));
        cell.tileNeighborhoods[kNetworkRail] = 0x00000001;  // kind 1 is diagonal too
        CHECK(cSC4NetworkTool::IsSimpleDiagonalNetwork(cell, kNetworkRail));
        CHECK(!cSC4NetworkTool::IsSimpleDiagonalNetwork(cell, kNetworkRoad));
    }

    // ---- IdentifyWaterAndLandBridgeTypes --------------------------------------------------------------------
    {
        std::vector<cSC4NetworkBridgeTypeInfo> types;
        types.push_back(MakeBridge(true, false, 0, 0, "a"));
        types.push_back(MakeBridge(false, true, 0, 0, "b"));
        types.push_back(MakeBridge(true, true, 0, 0, "c"));
        std::vector<int> water, land;
        cSC4NetworkTool::IdentifyWaterAndLandBridgeTypes(&types, water, land);
        CHECK(land.size() == 2 && land[0] == 0 && land[1] == 2);
        CHECK(water.size() == 2 && water[0] == 1 && water[1] == 2);
        cSC4NetworkTool::IdentifyWaterAndLandBridgeTypes(nullptr, water, land);  // clears both
        CHECK(water.empty() && land.empty());
    }

    // ---- BreakIntoStraightAndDiagSegments -------------------------------------------------------------------
    {
        Fixture f;
        f.Init();
        // city-independent helper: (start, end, allowDiagonals, penX, penZ)
        struct Case {
            SC4PointU start, end, expected;
            bool allowDiagonals;
            int penX, penZ;
            int total, straight, dirA, dirB;
            bool vertical, horizontal;
        };
        const Case cases[] = {
            // horizontal drag: direction A = east(4), B = 3 (the 8-way code of a zero-length north step)
            {{10, 10}, {20, 10}, {20, 10}, true, 2, 1, 11, 11, 4, 3, false, true},
            // vertical drag: direction A = north(6), B = north-east(7) because the end x is not past the start
            {{10, 10}, {10, 20}, {10, 20}, true, 2, 1, 11, 11, 6, 7, true, false},
            // perfect diagonal: straightSteps collapses to 0 (|dx| - |dz| == 0)
            {{10, 10}, {14, 14}, {14, 14}, true, 2, 1, 5, 0, 6, 5, true, false},
            // one cell off the vertical axis: the end x is snapped onto the start column
            {{10, 10}, {11, 20}, {10, 20}, true, 2, 1, 11, 11, 6, 5, true, false},
            // too shallow for a diagonal (2 * |dz| < |dx|): pure horizontal
            {{10, 10}, {20, 12}, {20, 10}, true, 2, 1, 11, 11, 4, 5, false, true},
            {{10, 10}, {30, 14}, {30, 10}, true, 2, 1, 21, 21, 4, 5, false, true},
            // too steep for a diagonal: pure vertical
            {{10, 10}, {14, 30}, {10, 30}, true, 2, 1, 21, 21, 6, 5, true, false},
            // pen width 1 on a near diagonal: |dx| is clamped to |dz| + 1, so the straight part is 1 -> 0
            {{10, 10}, {20, 17}, {18, 17}, true, 1, 1, 9, 0, 4, 5, false, true},
            // same drag with a wider pen: snapped onto the next perfect diagonal
            {{10, 10}, {20, 17}, {17, 17}, true, 2, 1, 8, 0, 6, 5, true, false},
        };
        for (const Case& c : cases) {
            SC4PointU end = c.end;
            int total = 0, straight = 0, dirA = 0, dirB = 0, vertical = 0, horizontal = 0;
            f.tool->BreakIntoStraightAndDiagSegments(c.start, end, total, straight, dirA, dirB, vertical, horizontal,
                                                     c.allowDiagonals, c.penX, c.penZ);
            CHECK(end.x == c.expected.x && end.z == c.expected.z);
            CHECK(total == c.total);
            CHECK(straight == c.straight);
            CHECK(dirA == c.dirA && dirB == c.dirB);
            CHECK((vertical != 0) == c.vertical);
            CHECK((horizontal != 0) == c.horizontal);
        }
        // no diagonals allowed: only the pre-snap applies
        {
            SC4PointU end{20, 18};
            SC4PointU start{10, 10};
            int total = 0, straight = 0, dirA = 0, dirB = 0, vertical = 0, horizontal = 0;
            f.tool->BreakIntoStraightAndDiagSegments(start, end, total, straight, dirA, dirB, vertical, horizontal,
                                                     false, 2, 1);
            CHECK(end.x == 19 && end.z == 18);
        }
        // straight-or-diagonal-only (network type info block +0x25 -> this[0x525])
        {
            NetworkToolTestAccess::Placement(*f.tool).straightOrDiagonalOnly = 1;
            SC4PointU start{10, 10};
            SC4PointU end{20, 18};
            int total = 0, straight = 0, dirA = 0, dirB = 0, vertical = 0, horizontal = 0;
            f.tool->BreakIntoStraightAndDiagSegments(start, end, total, straight, dirA, dirB, vertical, horizontal,
                                                     true, 2, 1);
            CHECK(end.x == 19 && end.z == 10);  // horizontal (|dz| < |dx| after the pre-snap)
            CHECK(horizontal != 0 && vertical == 0);
            SC4PointU end2{18, 20};
            SC4PointU start2{10, 10};
            f.tool->BreakIntoStraightAndDiagSegments(start2, end2, total, straight, dirA, dirB, vertical, horizontal,
                                                     true, 2, 1);
            CHECK(end2.x == 10 && end2.z == 19);  // vertical
            CHECK(vertical != 0 && horizontal == 0);
            NetworkToolTestAccess::Placement(*f.tool).straightOrDiagonalOnly = 0;
        }
        // |dx| == 1 with a long |dz|: the end x is snapped before anything else (0x10071aa64)
        {
            SC4PointU start{10, 10};
            SC4PointU end{9, 30};
            int total = 0, straight = 0, dirA = 0, dirB = 0, vertical = 0, horizontal = 0;
            f.tool->BreakIntoStraightAndDiagSegments(start, end, total, straight, dirA, dirB, vertical, horizontal,
                                                     true, 2, 1);
            CHECK(end.x == 10 && end.z == 30);
            CHECK(total == 21 && straight == 21);
        }
        // |dz| == 1 with a long |dx|: the end z is snapped (0x10071aa64, the third arm)
        {
            SC4PointU start{10, 10};
            SC4PointU end{30, 11};
            int total = 0, straight = 0, dirA = 0, dirB = 0, vertical = 0, horizontal = 0;
            f.tool->BreakIntoStraightAndDiagSegments(start, end, total, straight, dirA, dirB, vertical, horizontal,
                                                     true, 2, 1);
            CHECK(end.x == 30 && end.z == 10);
            CHECK(total == 21 && straight == 21);
        }
        // penZ == 1 turns a one-cell straight part of a diagonal into 0 (0x10071aa64 tail)
        {
            SC4PointU start{10, 10};
            for (int penZ = 1; penZ <= 2; ++penZ) {
                SC4PointU end{20, 17};
                int total = 0, straight = 0, dirA = 0, dirB = 0, vertical = 0, horizontal = 0;
                f.tool->BreakIntoStraightAndDiagSegments(start, end, total, straight, dirA, dirB, vertical,
                                                         horizontal, true, 1, penZ);
                CHECK(end.x == 18 && end.z == 17);  // |dx| clamped to |dz| + 1
                CHECK(total == 9);
                CHECK(straight == (penZ == 1 ? 0 : 1));
            }
        }
    }

    // ---- DrawNetworkLine / ComputeDraggedCells (i386 0x004cdd0a / 0x004cc96a), pen 1 x 1 ----------------------
    {
        Fixture f(0);
        f.host.typeInfo.penX = 1;
        f.host.typeInfo.penZ = 1;
        f.Init();
        using A = NetworkToolTestAccess;
        // A straight x drag: one step per x, one cell per step.
        SC4PointU start{2, 5}, end{7, 5};
        CHECK(f.tool->DrawNetworkLine(start, end, true, 0));
        CHECK(end.x == 7 && end.z == 5);
        CHECK(A::TotalSteps(*f.tool) == 6 && f.tool->GetStraightStepCount() == 6);
        CHECK(A::Cells(*f.tool).size() == 6);
        for (std::uint32_t i = 0; i < 6; ++i) {
            CHECK(A::Cells(*f.tool)[i] == (SC4PointU{2 + i, 5}));
            CHECK(A::Steps(*f.tool)[i].firstCell == i && A::Steps(*f.tool)[i].lastCell == i);
        }
        // A 45-degree drag: the walk moves z first on a tie, so every step after the first spans two cells (a staircase).
        start = {0, 0};
        end = {3, 3};
        CHECK(f.tool->DrawNetworkLine(start, end, true, 0));
        CHECK(A::TotalSteps(*f.tool) == 4);
        const std::vector<SC4PointU> stair = {{0, 0}, {0, 1}, {1, 1}, {1, 2}, {2, 2}, {2, 3}, {3, 3}};
        CHECK(A::Cells(*f.tool) == stair);
        const std::uint32_t firsts[4] = {0, 1, 3, 5}, lasts[4] = {0, 2, 4, 6};
        for (int i = 0; i < 4; ++i) CHECK(A::Steps(*f.tool)[i].firstCell == firsts[i] && A::Steps(*f.tool)[i].lastCell == lasts[i]);
        // Backwards along z: the steps run from the start towards smaller z.
        start = {4, 9};
        end = {4, 6};
        CHECK(f.tool->DrawNetworkLine(start, end, true, 0));
        const std::vector<SC4PointU> down = {{4, 9}, {4, 8}, {4, 7}, {4, 6}};
        CHECK(A::Cells(*f.tool) == down);
    }

    // ---- the multitype rules: a crossing takes its piece, a type running along an allowed overrider gives way ----
    {
        using A = NetworkToolTestAccess;
        Fixture f;
        f.Init();
        SetTileRuleResourceLoader([](const cGZPersistResourceKey& key, std::string& out) {
            if (key.instance != 0x10000001) return false;
            out = "[RoadStreetIntersectionSolutions]\n; road edges, street edges, piece, rotation, flip\n"
                  "RNSS=0X00000200,0X02000000,0X00AA0100,0,0\nRWSN=0x00000002,0x00000200,0x00AA0200,0,1\n"
                  "[AllowedOverrides]\n1 = Road , Street\n";
            return true;
        });
        cSC4NetworkTool::ReloadRules();
        cSC4NetworkCellInfo cell;
        f.host.cell = &cell;
        const std::uint32_t key = 5 | (7u << 16);
        std::vector<std::uint32_t> solved;
        // A street ending on the side of a road: the road (priority 3) keeps its edges, the pair has a piece.
        cell.networkFlags = 0x1 | 0x8;
        cell.tileNeighborhoods[0] = 0x00000200;
        cell.tileNeighborhoods[3] = 0x02000000;
        A::SolveMultiType(*f.tool, {key}, solved, 3);
        CHECK(solved.size() == 1 && solved[0] == key);
        const std::vector<tSolvedCell>& pieces = A::SolvedCellList(*f.tool);
        CHECK(pieces.size() == 1 && pieces[0].pieceType == 0x00aa0100 && pieces[0].rotationFlip == 0 && pieces[0].cellKey == key);
        CHECK(cell.solvedCellIndex == 0);
        // The flip column sets 0x80 on the rotation.
        solved.clear();
        cell.tileNeighborhoods[0] = 0x00000002;
        cell.tileNeighborhoods[3] = 0x00000200;
        A::SolveMultiType(*f.tool, {key}, solved, 3);
        CHECK(solved.size() == 1 && pieces.size() == 2 && pieces[1].pieceType == 0x00aa0200 && pieces[1].rotationFlip == 0x80);
        // A street lying along the road: AllowedOverrides hands the cell to the road, no crossing piece.
        solved.clear();
        cell.networkFlags = 0x1 | 0x8;
        cell.tileNeighborhoods[0] = 0x02000200;
        cell.tileNeighborhoods[3] = 0x02000200;
        A::SolveMultiType(*f.tool, {key}, solved, 3);
        CHECK(solved.empty() && cell.networkFlags == 0x1 && cell.tileNeighborhoods[3] == 0);
        // A pair with no override and no rules (rail 1 + street 8) is left as it was.
        cell.networkFlags = 0x2 | 0x8;
        cell.tileNeighborhoods[1] = 0x02000200;
        cell.tileNeighborhoods[3] = 0x02000200;
        A::SolveMultiType(*f.tool, {key}, solved, 3);
        CHECK(solved.empty() && cell.networkFlags == (0x2 | 0x8) && cell.tileNeighborhoods[3] == 0x02000200);
        f.host.cell = nullptr;
        SetTileRuleResourceLoader(nullptr);
        cSC4NetworkTool::ReloadRules();
    }

    // ---- DestroyOccupantsAndResolve (network-tools.md §10) ------------------------------------------------------
    {
        // three road pieces in a row at (5..7, 5); edge bytes per side (0 -x, 1 -z, 2 +x, 3 +z)
        auto makePiece = [](FakeHost& host, std::uint32_t x, std::uint32_t z, std::uint32_t edges, std::uint32_t extraFlags) {
            auto* piece = new cSC4NetworkOccupant();
            piece->AddRef();
            const float fx = static_cast<float>(x) * 16.0f, fz = static_cast<float>(z) * 16.0f;
            const float corners[4][3] = {{fx, 0, fz}, {fx, 0, fz + 16}, {fx + 16, 0, fz + 16}, {fx + 16, 0, fz}};
            piece->Init(corners);
            piece->SetNetworkFlag(kFlagForNetworkType[kNetworkRoad] | extraFlags);
            piece->SetEdgeTypesFlag(kNetworkRoad, edges);
            piece->SetUsable(true);
            host.pieces[x | (z << 16)] = piece;
            return piece;
        };
        // (a) plain edges: the middle goes; the lot connection nodes beside it lose the facing edge and stay, unusable
        {
            Fixture f;
            SetNetworkToolHost(&f.host);
            CHECK(f.tool->Init());
            cSC4NetworkOccupant* west = makePiece(f.host, 5, 5, 0x00010000, 0x200000);
            cSC4NetworkOccupant* middle = makePiece(f.host, 6, 5, 0x00010001, 0);
            cSC4NetworkOccupant* east = makePiece(f.host, 7, 5, 0x00000001, 0x200000);
            f.tool->DestroyOccupantsAndResolve({middle});
            CHECK(f.host.removedPieces.size() == 1 && f.host.removedPieces[0] == middle);
            CHECK(f.host.demolishedPieces.size() == 1 && f.host.demolishedPieces[0] == middle);
            CHECK(f.host.pieces.size() == 2 && f.host.pieces.count(5 | (5 << 16)) && f.host.pieces.count(7 | (5 << 16)));
            CHECK(west->GetEdgeTypesFlag(kNetworkRoad) == 0 && east->GetEdgeTypesFlag(kNetworkRoad) == 0);
            CHECK(!west->IsUsable() && !east->IsUsable());
            CHECK(west->HasNetworkFlag(kFlagForNetworkType[kNetworkRoad]));   // a single type is never cleared
            for (auto& entry : f.host.pieces) entry.second->Release();
            f.host.pieces.clear();
            SetNetworkToolHost(nullptr);
        }
        // (b) a grouped edge (bit 2) pulls the neighbour in: both go, and only the caller's piece was asked for
        {
            Fixture f;
            SetNetworkToolHost(&f.host);
            CHECK(f.tool->Init());
            cSC4NetworkOccupant* a = makePiece(f.host, 10, 8, 0x00040000, 0);
            cSC4NetworkOccupant* b = makePiece(f.host, 11, 8, 0x00000004, 0);
            f.tool->DestroyOccupantsAndResolve({a});
            CHECK(f.host.removedPieces.size() == 2 && f.host.pieces.empty());
            CHECK(f.host.demolishedPieces.size() == 2);
            (void)b;
            SetNetworkToolHost(nullptr);
        }
    }

    // ---- bridges: the bridge type record read from an exemplar's properties ---------------------------------
    {
        sc4::cSCPropertyHolder holder;
        const auto addU32 = [&](std::uint32_t id, std::vector<std::uint32_t> values) {
            cRZVariant v;
            v.RefUint32(values.data(), static_cast<std::uint32_t>(values.size()));
            holder.AddProperty(id, &v, false);
        };
        const auto addF32 = [&](std::uint32_t id, std::vector<float> values) {
            cRZVariant v;
            v.RefFloat32(values.data(), static_cast<std::uint32_t>(values.size()));
            holder.AddProperty(id, &v, false);
        };
        const auto addU8 = [&](std::uint32_t id, std::vector<std::uint8_t> values) {
            cRZVariant v;
            v.RefUint8(values.data(), static_cast<std::uint32_t>(values.size()));
            holder.AddProperty(id, &v, false);
        };
        holder.AddProperty(0x482cb442, 2u, false);                 // two ramp steps
        addU32(0x482cb443, {0x10, 0x11});
        addU8(0x482cb444, {1, 2});
        addF32(0x482cb44d, {5.0f, -1.0f});
        addF32(0x482cb44b, {0.0f, 4.0f, 1.0f, 3.0f});              // the ramp pairs, near end first
        holder.AddProperty(0x482cb448, 1u, false);                 // one pylon step
        holder.AddProperty(0x482cb447, 4u, false);
        addF32(0x482cb44e, {1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, -1.0f});
        cSC4NetworkBridgeTypeInfo info;
        CHECK(ReadNetworkBridgeTypeInfo(&holder, info));
        CHECK(info.width == 1 && info.rampLength == 2);
        CHECK(info.rampModels.size() == 2 && info.rampModels[1].id == 0x11 && info.rampModels[1].rotation == 2);
        CHECK(info.rampRise.size() == 3 && info.rampRise[2] == 0.0f && info.rampRise[1] == 2.0f && info.rampRise[0] == 6.0f);
        CHECK(info.rampDrop.size() == 2 && info.rampDrop[1] == 3.0f && info.rampDrop[0] == 6.0f);
        CHECK(info.landClearance == 6.0f);
        CHECK(info.lastSupportedRamp == 0);                         // the second ramp step has no support
        CHECK(info.minSpan == 6 && info.maxSpan == 100000);         // the defaults: rampLength * 2 + 2
        CHECK(info.pylonLength == 1 && info.pylonSpacing == 4 && info.pylonInset == 4);
        CHECK(info.maxWaterDepth == 10000.0f && info.maxPylons == 100 && !info.hasCables);
        CHECK(info.rampFootprint.size() == 32);                     // four corners and their three quarter turns
        CHECK(info.rampFootprint[8] > -0.001f && info.rampFootprint[8] < 0.001f && info.rampFootprint[9] > 0.999f);
        CHECK(info.pylonFootprint.size() == 32);                    // 0x482cb45e missing: the ramp footprint
    }

    // ---- bridges: a straight drag over water becomes a bridge, its pieces laid out and inserted -------------
    {
        Fixture f;
        cSC4NetworkBridgeTypeInfo bridge;
        bridge.isWater = true;
        bridge.minSpan = 4;
        bridge.maxSpan = 100000;
        bridge.rampLength = 1;
        bridge.rampModels = {cSC4NetworkModelInfo{0x20, 1}};
        bridge.rampSupportHeights = {-1.0f};
        bridge.rampRise = {4.0f, 0.0f};
        bridge.rampDrop = {3.0f};
        bridge.landClearance = 4.0f;
        bridge.waterClearance = 20.0f;
        bridge.deckModel = 0x30;
        bridge.deckRotation = 1;
        bridge.pylonLength = 1;
        bridge.pylonSpacing = 3;
        bridge.pylonInset = 2;
        bridge.pylonModels = {cSC4NetworkModelInfo{0x40, 0}};
        bridge.pylonSupportHeights = {12.0f};
        bridge.pylonDeckOffset = 1.5f;
        bridge.name = "Test Bridge";
        f.host.typeInfo.bridges = {bridge};
        f.Init();
        f.host.terrainAllows = false;
        for (std::uint32_t x = 8; x <= 12; ++x) f.host.water.insert(x | (5u << 16));
        auto& steps = NetworkToolTestAccess::Steps(*f.tool);
        auto& cells = NetworkToolTestAccess::Cells(*f.tool);
        steps.clear();
        cells.clear();
        for (std::uint32_t i = 0; i < 20; ++i) {
            cells.push_back(SC4PointU{i, 5});
            steps.push_back(tDraggedStep{i, i, 0});
        }
        NetworkToolTestAccess::SetDrag(*f.tool, 20, 20, 0);
        int count = 20;
        f.tool->IdentifyBridgeCells(count, kNetworkRoad);
        CHECK(count == 20);
        const std::vector<tBridge>& bridges = f.tool->Bridges();
        CHECK(bridges.size() == 1);
        if (bridges.size() == 1) {
            const tBridge& b = bridges[0];
            CHECK(b.startStep == 7 && b.endStep == 13 && b.bridgeType == 0 && !b.landBridge);
            CHECK(b.referenceAltitude == 300.0f);                   // no bank rises: the start bank's surface
            CHECK(b.elevation == 304.0f);                           // land under the span + the ramp rise
            for (int s = 0; s < 20; ++s) CHECK(steps[static_cast<std::size_t>(s)].direction == ((s >= 7 && s <= 13) ? 1u : 0u));
            std::vector<int> possible;
            CHECK(f.tool->GetPossibleBridges(possible) && possible.size() == 1 && possible[0] == 0);
            CHECK(f.tool->GetMajorBridge() == 0);
            // ramp, span, pylon (the first pylon step is the inset from the inner ramp end), spans, ramp
            static const int kKinds[7] = {0, 2, 1, 2, 2, 2, 0};
            CHECK(b.pieces.size() == 7);
            for (std::size_t i = 0; i < b.pieces.size() && i < 7; ++i) {
                CHECK(b.pieces[i].kind == kKinds[i]);
                CHECK(b.pieces[i].x == 7 + i && b.pieces[i].z == 5);
            }
            if (b.pieces.size() == 7) {
                // the drag direction 0 turns every piece by 3 quarter turns
                CHECK(b.pieces[0].model == 0x20 && b.pieces[0].rotation == ((1 + 3) & 3));
                CHECK(b.pieces[6].model == 0x20 && b.pieces[6].rotation == ((1 + 2 + 3) & 3));   // the far ramp is mirrored
                CHECK(b.pieces[1].model == 0x30 && b.pieces[1].rotation == ((1 + 3) & 3));
                CHECK(b.pieces[2].model == 0x40 && b.pieces[2].modelHeight == 305.5f && b.pieces[2].supportHeight == 12.0f);
                CHECK(b.pieces[1].modelHeight == 304.0f && b.pieces[1].corners[0] == 304.0f && b.pieces[1].corners[3] == 304.0f);
                // the near ramp: its low edge (corners 2, 3 for this direction) at the deck minus the whole rise
                CHECK(b.pieces[0].modelHeight == 301.0f);
                CHECK(b.pieces[0].corners[2] == 300.0f && b.pieces[0].corners[3] == 300.0f);
                CHECK(b.pieces[0].corners[0] == 304.0f && b.pieces[0].corners[1] == 304.0f);
            }

            // the bridge cells leave the set of cells the regular solve places
            std::unordered_set<std::uint32_t> solve = {6u | (5u << 16), 9u | (5u << 16), 14u | (5u << 16)};
            f.tool->RemoveBridgePiecesFromSetOfCellsToBeSolved(solve);
            CHECK(solve.size() == 2 && solve.count(9u | (5u << 16)) == 0);

            // the pieces become immovable bridge occupants with their models
            for (const tBridgePiece& piece : b.pieces) f.host.worldCache->cells[piece.x | (piece.z << 16)].x = piece.x;
            NetworkToolTestAccess::SetPlacing(*f.tool, true);
            std::list<cSC4NetworkOccupant*> placed;
            f.tool->InsertBridgePieces(&placed);
            CHECK(placed.size() == 7 && f.host.inserted == 7);
            if (!placed.empty()) {
                cSC4NetworkOccupant* first = placed.front();
                CHECK(first->GetGZCLSID() == 0x49cc1bcd);
                CHECK(first->PieceId() == 0x20 && first->IsImmovable());
            }
            for (cSC4NetworkOccupant* occupant : placed) occupant->Release();
        }

        // control: a step whose surroundings hold another network piece takes no bridge, and the drag stops there
        Fixture g;
        g.host.typeInfo.bridges = {bridge};
        g.Init();
        g.host.terrainAllows = false;
        g.host.water = f.host.water;
        g.host.occupiedRect = true;
        NetworkToolTestAccess::Steps(*g.tool) = steps;
        for (tDraggedStep& step : NetworkToolTestAccess::Steps(*g.tool)) step.direction = 0;
        NetworkToolTestAccess::Cells(*g.tool) = cells;
        NetworkToolTestAccess::SetDrag(*g.tool, 20, 20, 0);
        count = 20;
        g.tool->IdentifyBridgeCells(count, kNetworkRoad);
        CHECK(g.tool->Bridges().empty() && count == 20);
    }
    // ---- the height solve: LoadNetworkSlopeLimits and ComputeInitialSmoothingSet (i386 0x00152598 / 0x00142c92) ------
    {
        cSC4NetworkPlacementParams params{};
        const float slopeAngle = std::atan(0.5f), smoothAngle = std::atan(0.25f), otherAngle = std::atan(1.0f);
        std::memcpy(&params.unknown00[0], &slopeAngle, 4);
        std::memcpy(&params.unknown00[1], &otherAngle, 4);
        std::memcpy(&params.unknown00[2], &smoothAngle, 4);
        cSC4NetworkSlopeLimits limits;
        CHECK(limits.maxSlope == 10.0f && limits.unknown110 == 20.0f);   // the record's constructor defaults
        LoadNetworkSlopeLimits(params, limits);
        CHECK(std::fabs(limits.maxSlope - 8.0f) < 1e-4f);                // tan * 16
        CHECK(std::fabs(limits.maxSlopeDiagonal - 8.0f * 0.70710677f) < 1e-4f);
        CHECK(std::fabs(limits.maxSmoothness - 4.0f) < 1e-4f);
        CHECK(std::fabs(limits.maxSmoothnessDiagonal - 4.0f * 0.70710677f) < 1e-4f);
        CHECK(std::fabs(limits.unknown110 - 16.0f) < 1e-4f);
    }
    {
        Fixture f(0);
        f.host.typeInfo.penX = 1;
        f.host.typeInfo.penZ = 1;
        f.Init();
        using A = NetworkToolTestAccess;
        SC4PointU start{5, 5}, end{9, 5};
        CHECK(f.tool->DrawNetworkLine(start, end, true, 0));
        f.tool->SetValidStepCount(A::TotalSteps(*f.tool));
        // The straight branch: one solved piece per step. Only cells with a record join the set, a short step does not.
        A::SolvedCellList(*f.tool).resize(static_cast<std::size_t>(A::TotalSteps(*f.tool)));
        for (std::uint32_t x = 5; x <= 9; ++x) f.host.worldCache->CreateCell(x | (5u << 16))->shortStep = (x == 7);
        f.host.worldCache->CreateCell(10u | (6u << 16));   // inside the padded rectangle
        f.host.worldCache->CreateCell(13u | (6u << 16));   // outside it
        A::ComputeSmoothingSet(*f.tool, 1);
        const auto& set = A::SmoothingSet(*f.tool);
        CHECK(set.size() == 5);
        CHECK(set.count(7u | (5u << 16)) == 0 && set.count(10u | (6u << 16)) == 1 && set.count(13u | (6u << 16)) == 0);
        // The outline of the vertex rectangle x 4..11, z 4..7 (the drag's cells grown by 1, plus one vertex).
        const auto& fixed = A::FixedVertices(*f.tool);
        CHECK(fixed.size() == 20);
        CHECK(fixed.count(4 + 4 * 1000) && fixed.count(11 + 7 * 1000) && fixed.count(4 + 6 * 1000) && fixed.count(11 + 5 * 1000));
        CHECK(!fixed.count(5 + 5 * 1000) && !fixed.count(10 + 6 * 1000));
        // Pad 0: the drag's own vertex rows are the outline, so every corner of the drag is fixed.
        A::ComputeSmoothingSet(*f.tool, 0);
        CHECK(A::FixedVertices(*f.tool).size() == 12 && A::FixedVertices(*f.tool).count(7 + 5 * 1000));
        SetNetworkToolHost(nullptr);
    }

    // ---- one-way roads: ResolveOneWayConflicts -------------------------------------------------------------
    {
        // An existing eastbound one-way road on x 0..9, z 5 (direction 2 on its x edges); a new drag from its end cell
        // (9,5) to (15,5) carries direction 1. The walk from the drag's first step turns the whole old road around and
        // takes its cells into the solve set; the old road's cell beyond an intersection (x 3 has three edges) stops it.
        Fixture f(kNetworkOneWayRoad);
        f.Init();
        using A = NetworkToolTestAccess;
        auto key = [](std::uint32_t x, std::uint32_t z) { return x | (z << 16); };
        auto cellAt = [&](std::uint32_t x, std::uint32_t z) { return f.host.worldCache->CreateCell(key(x, z)); };
        // A connection on edge e: byte e of the packed neighbourhood (kind 2 = a straight x connection here).
        auto edge = [](int e, std::uint32_t kind) { return kind << (e * 8); };
        for (std::uint32_t x = 0; x <= 15; ++x) {
            cSC4NetworkCellInfo* c = cellAt(x, 5);
            c->x = x;
            c->z = 5;
            const bool drag = x >= 9;
            std::uint32_t edges = 0;
            if (x > 0) edges |= edge(0, 2);
            if (x < 15) edges |= edge(2, 2);
            if (x == 3) edges |= edge(1, 2);                 // a side street joins x 3 from z 4
            c->tileNeighborhoods[kNetworkOneWayRoad] = edges;
            std::uint8_t bits = 0;
            const std::uint8_t dir = (x < 9) ? 2 : 1;
            if (x > 0) bits |= static_cast<std::uint8_t>(dir << 0);
            if (x < 15) bits |= static_cast<std::uint8_t>(dir << 4);
            if (x == 9) bits = static_cast<std::uint8_t>((2u << 0) | (1u << 4));   // the old end piece's west edge, the drag's east edge
            c->unknown58 = bits;
            c->status = drag ? 1 : 0;
            c->pieceFromSolvedList = drag;
            if (drag) A::SolveSet(*f.tool).insert(key(x, 5));
        }
        auto& steps = A::Steps(*f.tool);
        auto& cells = A::Cells(*f.tool);
        steps.clear();
        cells.clear();
        for (std::uint32_t x = 9; x <= 15; ++x) {
            cells.push_back(SC4PointU{x, 5});
            tDraggedStep step;
            step.firstCell = step.lastCell = x - 9;
            steps.push_back(step);
        }
        A::SetTotalSteps(*f.tool, 7);
        A::ResolveOneWay(*f.tool, kNetworkOneWayRoad);
        for (std::uint32_t x = 4; x <= 9; ++x) {
            const cSC4NetworkCellInfo* c = f.host.worldCache->GetCell(key(x, 5));
            CHECK((c->unknown58 & 3u) == 1 && ((c->unknown58 >> 4) & 3u) == 1);   // west and east edges now direction 1
            CHECK(c->pieceFromSolvedList && A::SolveSet(*f.tool).count(key(x, 5)) == 1 && c->status == 1);
        }
        // x 3 (three edges): its east edge took the direction from x 4, its west edge and x 0..2 keep theirs
        const cSC4NetworkCellInfo* junction = f.host.worldCache->GetCell(key(3, 5));
        CHECK(((junction->unknown58 >> 4) & 3u) == 1 && (junction->unknown58 & 3u) == 2);
        for (std::uint32_t x = 0; x <= 2; ++x) {
            const cSC4NetworkCellInfo* c = f.host.worldCache->GetCell(key(x, 5));
            CHECK(((c->unknown58 >> 4) & 3u) == 2 && !c->pieceFromSolvedList);
        }
        // the drag's own cells keep direction 1
        for (std::uint32_t x = 10; x <= 15; ++x) CHECK((f.host.worldCache->GetCell(key(x, 5))->unknown58 & 3u) == 1);
    }
    {
        // The conflict pass: edges 0 and 2 of (20,8) share connection slot 2 ((byte + edge) & 3); direction 1 on edge 0
        // reads 1, on edge 2 it reads 2 (flipped: the edge and the slot agree in bit 1), so edge 2 loses its bits and
        // the neighbour (21,8) loses its edge 0 bits. A cell without a contradiction keeps its bits.
        Fixture f(kNetworkOneWayRoad);
        f.Init();
        using A = NetworkToolTestAccess;
        auto key = [](std::uint32_t x, std::uint32_t z) { return x | (z << 16); };
        cSC4NetworkCellInfo* c = f.host.worldCache->CreateCell(key(20, 8));
        c->x = 20;
        c->z = 8;
        c->tileNeighborhoods[kNetworkOneWayRoad] = 0x00040002u;
        c->unknown58 = 0x11;                            // direction 1 on edges 0 and 2
        c->pieceFromSolvedList = true;
        cSC4NetworkCellInfo* n = f.host.worldCache->CreateCell(key(21, 8));
        n->x = 21;
        n->z = 8;
        n->unknown58 = 0x13;                            // edge 0 direction 3, edge 2 direction 1
        cSC4NetworkCellInfo* calm = f.host.worldCache->CreateCell(key(25, 8));
        calm->x = 25;
        calm->z = 8;
        calm->tileNeighborhoods[kNetworkOneWayRoad] = 0x00020002u;   // slots 2 and 0: no shared slot
        calm->unknown58 = 0x11;
        calm->pieceFromSolvedList = true;
        A::SolveSet(*f.tool) = {key(20, 8), key(25, 8)};
        auto& steps = A::Steps(*f.tool);
        auto& cells = A::Cells(*f.tool);
        steps.assign(1, tDraggedStep{});
        cells.assign(1, SC4PointU{30, 20});             // a seed cell without one-way bits
        f.host.worldCache->CreateCell(key(30, 20))->x = 30;
        A::SetTotalSteps(*f.tool, 1);
        A::ResolveOneWay(*f.tool, kNetworkOneWayRoad);
        CHECK(c->unknown58 == 0x01);
        CHECK(n->unknown58 == 0x10 && n->pieceFromSolvedList);
        CHECK(calm->unknown58 == 0x11);
        // the original queues the walked cell (not the neighbour) for another round: the set still holds (20,8)
        CHECK(A::SolveSet(*f.tool).count(key(20, 8)) == 1 && A::SolveSet(*f.tool).count(key(21, 8)) == 0);
    }

    // ---- bridges: a straight drag over a valley gets a land bridge ------------------------------------------
    {
        cSC4NetworkBridgeTypeInfo land;
        land.isLand = true;
        land.minSpan = 4;
        land.maxSpan = 100000;
        land.deckModel = 0x50;
        land.name = "Land Test Bridge";
        // a V-shaped valley under the leading edge (vertex x + 1) of the cells x = 5..11 along z = 5
        static const float kValley[] = {95.0f, 85.0f, 75.0f, 70.0f, 75.0f, 85.0f, 95.0f};   // vertex x = 6..12
        const auto run = [&](bool occupied, std::vector<tBridge>& out, std::vector<tDraggedStep>& stepsOut) {
            Fixture f;
            f.host.typeInfo.bridges = {land};
            f.host.typeInfo.placement.float40[2] = 10.0f;   // the dip must sink more than this below the deck
            f.host.typeInfo.placement.float40[3] = 5.0f;    // the far bank within this of the deck
            f.Init();
            f.host.anyOccupant = occupied;
            const std::int32_t row = 41;                    // the fixture city is 40 cells wide
            for (int i = 0; i < 7; ++i) {
                f.host.vertexHeights[5 * row + 6 + i] = kValley[i];
                f.host.vertexHeights[6 * row + 6 + i] = kValley[i];
            }
            auto& steps = NetworkToolTestAccess::Steps(*f.tool);
            auto& cells = NetworkToolTestAccess::Cells(*f.tool);
            steps.clear();
            cells.clear();
            for (std::uint32_t i = 0; i < 20; ++i) {
                cells.push_back(SC4PointU{i, 5});
                steps.push_back(tDraggedStep{i, i, 0});
            }
            NetworkToolTestAccess::SetDrag(*f.tool, 20, 20, 0);
            f.tool->IdentifyLandBridgeCells(20, kNetworkRoad);
            out = f.tool->Bridges();
            stepsOut = steps;
        };
        std::vector<tBridge> bridges;
        std::vector<tDraggedStep> steps;
        run(false, bridges, steps);
        CHECK(bridges.size() == 1);
        if (bridges.size() == 1) {
            const tBridge& b = bridges[0];
            // from the first step down (at its edge height) to the step that climbs back to it
            CHECK(b.startStep == 5 && b.endStep == 11 && b.landBridge && b.elevation == 95.0f && b.referenceAltitude == 95.0f);
            CHECK(b.pieces.size() == 7 && b.pieces[3].model == 0x50);
            for (int s = 0; s < 20; ++s) CHECK(steps[static_cast<std::size_t>(s)].direction == ((s >= 5 && s <= 11) ? 1u : 0u));
        }
        // control: anything standing in the span's rectangle prevents the land bridge
        run(true, bridges, steps);
        CHECK(bridges.empty());
        for (const tDraggedStep& step : steps) CHECK(step.direction == 0);
    }

    // ---- Unregister the host and finish ---------------------------------------------------------------------
    SetNetworkToolHost(nullptr);
    if (failures == 0) {
        std::printf("sc4_networktool: ok\n");
        return 0;
    }
    std::printf("sc4_networktool: %d failure(s)\n", failures);
    return 1;
}
