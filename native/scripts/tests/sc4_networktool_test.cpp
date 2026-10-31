// cSC4NetworkTool — unit test for the recovered slice (state, accessors, init/shutdown, failure strings, bridge
// queries, network-type flag helpers, padded rect, the terrain highlight of the valid drag and the
// straight/diagonal splitter). The engine globals the original touches are replaced by INetworkToolHost.
#include <cstdio>
#include <cstring>
#include <vector>

#include "gz/cIGZMessage2.h"
#include "rz/cRZString.h"
#include "sc4/NetworkTool.h"

static int failures = 0;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);           \
            ++failures;                                                \
        }                                                              \
    } while (0)

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
    void Clear() override { ++clears; }
    cSC4NetworkCellInfo* GetCell(std::uint32_t) override { return nullptr; }
    void GetAllCells(std::vector<cSC4NetworkCellInfo>& out) override { out.clear(); }
    cSC4NetworkCellInfo* CreateCell(std::uint32_t) override { return nullptr; }
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
        return terrainAllows;
    }
    int CityWidth() override { return width; }
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
        return worldCache;
    }
    INetworkConstraintSatisfier* CreateConstraintSatisfier() override { return &satisfier; }
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
        bridge.type = 3;
        bridge.altitude = 12.5f;
        bridge.referenceAltitude = 10.0f;
        bridge.landBridge = true;
        NetworkToolTestAccess::Bridges(*f.tool).push_back(bridge);
        tBridge second{};
        second.type = 5;
        second.altitude = 20.0f;
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

    // ---- Unregister the host and finish ---------------------------------------------------------------------
    SetNetworkToolHost(nullptr);
    if (failures == 0) {
        std::printf("sc4_networktool: ok\n");
        return 0;
    }
    std::printf("sc4_networktool: %d failure(s)\n", failures);
    return 1;
}
