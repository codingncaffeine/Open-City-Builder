// cSC4UndergroundNetworkTool — unit test for the whole recovered class (37 symbols): the trivial network-type
// answers, the tuning property load, Init/Shutdown, the occupant lookups, the occupant creation, the solved-cell
// insertion and propagation, the preview/highlight lists, the per-cell placement records, the vertical
// constraints, StartPlacing and SetQuadTrianglesToDraw. Everything the original reaches through an engine global
// or an unrecovered base-class method is a fake here (INetworkToolHost + IUndergroundNetworkToolHost).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <list>
#include <map>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gz/cIGZString.h"
#include "sc4/NetworkTool.h"
#include "sc4/SCProperty.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/PipeTool.h"
#include "sc4/UndergroundNetworkTool.h"

static int failures = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);         \
            ++failures;                                              \
        }                                                            \
    } while (0)
#define CHECK_NEAR(a, b) CHECK(std::fabs((a) - (b)) <= 1e-5f)

using namespace sc4;

namespace sc4 {

// Reaches the private members of the recovered base class (declared friend in NetworkTool.h).
struct NetworkToolTestAccess {
    static std::vector<tDraggedStep>& Steps(cSC4NetworkTool& tool) { return tool.steps_; }
    static std::vector<SC4PointU>& Cells(cSC4NetworkTool& tool) { return tool.cells_; }
    static int& ValidSteps(cSC4NetworkTool& tool) { return tool.validSteps_; }
    static std::vector<tSolvedCell>& SolvedCellList(cSC4NetworkTool& tool) { return tool.solvedCellList_; }
    static std::unordered_set<std::uint32_t>& FixedVertices(cSC4NetworkTool& tool) { return tool.unknown150_; }
    static std::unordered_set<std::uint32_t>& SolvedCells(cSC4NetworkTool& tool) { return tool.solvedCells_; }
    static void SetMode98(cSC4NetworkTool& tool, bool value) { tool.unknown98_ = value; }
    static bool Mode98(const cSC4NetworkTool& tool) { return tool.unknown98_; }
    static cSC4NetworkPlacementParams& Placement(cSC4NetworkTool& tool) { return tool.placement_; }
    static void SetWidthPlusOne(cSC4NetworkTool& tool, int value) { tool.widthPlusOne_ = value; }
    static std::uint32_t NetworkType(const cSC4NetworkTool& tool) { return tool.networkType_; }
    static INetworkConstraintSatisfier* ConstraintA(cSC4NetworkTool& tool) { return tool.constraintA_; }
    static int Unknown458(const cSC4NetworkTool& tool) { return tool.unknown458_; }
};

// Reaches the private members of the underground tool (declared friend in UndergroundNetworkTool.h).
struct UndergroundNetworkToolTestAccess {
    static void SetNetworkDepth(cSC4UndergroundNetworkTool& tool, float value) { tool.networkDepth_ = value; }
    static void SetInited(cSC4UndergroundNetworkTool& tool, bool value) { tool.inited_ = value; }
    static std::list<cSC4UndergroundOccupant*>& PreviewList(cSC4UndergroundNetworkTool& tool) {
        return tool.previewList_;
    }
    static std::list<cSC4UndergroundOccupant*>& HighlightList(cSC4UndergroundNetworkTool& tool) {
        return tool.highlightList_;
    }
};

}  // namespace sc4

namespace {

// ============================================================================================================
// the occupants the fake occupant manager hands out
// ============================================================================================================

// The original's cSC4UndergroundOccupant is abstract (three pure virtuals), so the test instantiates this. The
// reference count is the test's own so the object survives the tool's Release() calls for later inspection.
struct TestOccupant final : cSC4UndergroundOccupant {
    TestOccupant() { fakeRefs = 1; }
    tCustomGeometryInfo info{};
    const tCustomGeometryInfo* GetCustomGeometryInfo() const override { return &info; }
    std::uint32_t GetNetworkType() const override { return 4; }
    std::int32_t GetModelInstanceOffset() const override { return 0x6000000; }
    // cSC4PipeOccupant / cSC4SubwayOccupant answer their network gziid (the tool queries the occupants of a
    // cell with exactly that iid: 0x1001a7d3e). The recovered base only knows the three underground iids.
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == networkGziid) {
            AddRef();
            *out = this;
            return true;
        }
        return cSC4UndergroundOccupant::QueryInterface(iid, out);
    }

    std::uint32_t AddRef() override { return ++fakeRefs; }
    std::uint32_t Release() override {
        ++releaseCalls;
        if (fakeRefs > 0) --fakeRefs;
        return fakeRefs;
    }

    void SetQuadDraw(std::uint32_t pieceId, std::uint8_t rotationFlip) override {
        cSC4UndergroundOccupant::SetQuadDraw(pieceId, rotationFlip);   // pieceId_ / rotationFlip_
        quadDrawPiece = pieceId;
        quadDrawRotation = rotationFlip;
        ++quadDrawCalls;
    }
    void SetTerrainHeights(float* heights) override {
        for (int i = 0; i < 4; ++i) terrainHeights[i] = heights[i];
        ++terrainSetCalls;
    }
    void SetVertexHeights(float* heights) override {
        for (int i = 0; i < 4; ++i) vertexHeights[i] = heights[i];
        ++vertexSetCalls;
    }
    void GetVertexHeights(float* heights) const override {
        for (int i = 0; i < 4; ++i) heights[i] = vertexHeights[i];
        ++vertexGetCalls;
    }
    void CreateWalls(bool update) override {
        ++wallCalls;
        lastWallUpdate = update;
    }
    void CreateFloor(bool update) override {
        ++floorCalls;
        lastFloorUpdate = update;
    }
    void CreateGeometry(bool update) override {
        ++geometryCalls;
        lastGeometryUpdate = update;
    }
    void CreateHighlight(bool highlight) override {
        ++highlightCalls;
        lastHighlightCreate = highlight;
    }
    void SetHighlight(int highlight, bool postNow) override {
        highlightState = highlight;
        ++highlightSetCalls;
        (void)postNow;
    }
    void GetHiddenTriangles(bool& t0, bool& t1, bool& t2) const override {
        t0 = hidden[0];
        t1 = hidden[1];
        t2 = hidden[2];
    }
    bool Shutdown() override {
        ++shutdownCalls;
        return true;
    }

    std::uint32_t networkGziid = kGZIID_cSC4PipeOccupant;
    std::uint32_t fakeRefs = 0;
    int releaseCalls = 0;
    std::uint32_t quadDrawPiece = 0;
    std::uint8_t quadDrawRotation = 0;
    int quadDrawCalls = 0;
    float terrainHeights[4] = {};
    int terrainSetCalls = 0;
    float vertexHeights[4] = {};
    int vertexSetCalls = 0;
    mutable int vertexGetCalls = 0;
    int wallCalls = 0, floorCalls = 0, geometryCalls = 0, highlightCalls = 0, highlightSetCalls = 0;
    int shutdownCalls = 0;
    bool lastWallUpdate = false, lastFloorUpdate = false, lastGeometryUpdate = false, lastHighlightCreate = false;
    int highlightState = 0;
    bool hidden[3] = {false, false, false};
};

// A cSC4UndergroundConnectionOccupant (iid 0x8a139184) of a chosen network, with a counted fake reference.
struct FakeConnectionOccupant final : cSC4UndergroundConnectionOccupant {
    std::uint32_t GetGZIID() const override { return 0xea0a5acf; }
    std::uint32_t GetNetworkOccupantGZIID() const override { return network; }
    std::uint32_t GetNetworkType() const override { return kNetworkPipe; }
    std::uint32_t AddRef() override { return ++fakeRefs; }
    std::uint32_t Release() override {
        ++releaseCalls;
        if (fakeRefs > 0) --fakeRefs;
        return fakeRefs;
    }
    std::uint32_t fakeRefs = 1;
    int releaseCalls = 0;
    std::uint32_t network = kGZIID_cSC4PipeOccupant;
};

// ============================================================================================================
// the seams
// ============================================================================================================

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

struct FakePreview final : INetworkPreview {
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (--refs == 0) {
            ++destroys;
            return 0;
        }
        return refs;
    }
    void Init() override { ++inits; }
    void Shutdown() override { ++shutdowns; }
    void Clear() override { ++clears; }
    void Erase(const SC4PointU&) override {}
    void AddUntexturedSelection(const SC4PointU&, std::uint32_t, const float*, float, bool) override {}
    void AdjustCellHeights(const SC4PointU&, const float*, bool) override {}
    int refs = 0, destroys = 0, inits = 0, shutdowns = 0, clears = 0;
};

struct FakeWorldCache final : INetworkWorldCache {
    void Clear() override {
        ++clears;
        cells.clear();
    }
    cSC4NetworkCellInfo* GetCell(std::uint32_t key) override {
        auto it = cells.find(key);
        return it == cells.end() ? nullptr : &it->second;
    }
    void GetAllCells(std::vector<cSC4NetworkCellInfo>& out) override {
        out.clear();
        for (const auto& entry : cells) out.push_back(entry.second);
    }
    cSC4NetworkCellInfo* CreateCell(std::uint32_t key) override { return &cells[key]; }
    std::map<std::uint32_t, cSC4NetworkCellInfo> cells;
    int clears = 0;
};

struct RecordingSatisfier final : INetworkConstraintSatisfier {
    struct RangeCall {
        int kind;   // 0 range, 1 preferred range
        int vertexId;
        float low, high;
    };
    void AddVertexToSolve(int vertexId, float height) override { solved.push_back({vertexId, height}); }
    bool GetHeight(int vertexId, float& out) override {
        auto it = heights.find(vertexId);
        if (it == heights.end()) return false;
        out = it->second;
        return true;
    }
    void InsertRangeConstraint(int vertexId, float low, float high) override {
        ranges.push_back({0, vertexId, low, high});
    }
    void InsertPreferredRangeConstraint(int vertexId, float low, float high) override {
        ranges.push_back({1, vertexId, low, high});
    }
    void InsertPreferredValueConstraint(int vertexId, float value) override {
        preferred.push_back({vertexId, value});
    }
    void InsertFixedVertex(int vertexId, float height) override { fixed.push_back({vertexId, height}); }
    void InsertEqualityConstraint(std::uint32_t a, std::uint32_t b) override {
        equalities.push_back({a, b});
    }
    std::map<int, float> heights;
    std::vector<std::pair<int, float>> solved;
    std::vector<RangeCall> ranges;
    std::vector<std::pair<int, float>> preferred;
    std::vector<std::pair<int, float>> fixed;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> equalities;
};

struct FakeTypeInfo final : INetworkTypeInfo {
    std::uint32_t ViewInputControlId() const override { return 0x11; }
    std::uint32_t ToolViewId() const override { return 0x22; }
    std::uint32_t CursorId() const override { return 0x33; }
    std::uint32_t DirectionalCursorBase() const override { return 0x44; }
    std::uint32_t DefaultExemplarId() const override { return 0x12345678; }
    int PenWidthX() const override { return 2; }
    int PenWidthZ() const override { return 1; }
    const std::vector<cSC4NetworkBridgeTypeInfo>& BridgeTypes() const override { return bridges; }
    const cSC4NetworkPlacementParams& PlacementParams() const override { return placement; }
    const cSC4NetworkSupportStructInfo* FindSupportStructure(std::uint32_t) const override { return nullptr; }
    std::uint32_t PlacementExemplarId() const override { return 0x5a5a; }
    std::vector<cSC4NetworkBridgeTypeInfo> bridges;
    cSC4NetworkPlacementParams placement;
};

// The engine globals of the base cSC4NetworkTool (copied from scripts/tests/sc4_networktool_test.cpp).
struct FakeNetworkToolHost final : INetworkToolHost {
    ~FakeNetworkToolHost() override {
        if (terrain != nullptr) terrain->Release();
        if (occupants != nullptr) occupants->Release();
        if (cityView != nullptr) cityView->Release();
        if (orFilter != nullptr) orFilter->Release();
    }
    void RegisterShutdownCallback(bool (*fn)(void*), void* user) override {
        (void)fn;
        (void)user;
    }
    cIGZUnknown* AcquireTerrain() override {
        if (terrain == nullptr) terrain = new FakeCounted();
        terrain->AddRef();
        return terrain;
    }
    float TerrainCellSize() override { return 16.0f; }
    bool TerrainAllowsNetwork(std::uint32_t, std::uint32_t) override { return true; }
    int CityWidth() override { return 40; }
    int CityHeight() override { return 30; }
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
        layerIdUsed = layerId;
        priorityUsed = priority;
        (void)layer;
        ++addLayerCalls;
    }
    void RemoveRenderLayer(INetworkPreview*) override { ++removeLayerCalls; }
    std::uint32_t RendererAltitudeScale() override { return 77; }
    void RegisterMessage(cIGZMessageTarget2*, std::uint32_t type) override { registeredMessage = type; }
    void UnregisterMessage(cIGZMessageTarget2*, std::uint32_t type) override { unregisteredMessage = type; }
    cIGZUnknown* CreateNetworkOrFilter(std::uint32_t) override {
        if (orFilter == nullptr) orFilter = new FakeCounted();
        orFilter->AddRef();
        return orFilter;
    }
    INetworkPreview* CreatePreview() override {
        preview = new FakePreview();
        return preview;
    }
    INetworkWorldCache* CreateWorldCache() override {
        if (worldCache == nullptr) worldCache = new FakeWorldCache();
        return worldCache;
    }
    INetworkConstraintSatisfier* CreateConstraintSatisfier() override { return satisfier; }
    const INetworkTypeInfo* NetworkTypeInfo(std::uint32_t) override { return &typeInfo; }
    cSC4NetworkCellInfo* GetCellInfo(std::uint32_t key) override {
        return worldCache != nullptr ? worldCache->GetCell(key) : nullptr;
    }
    cIGZUnknown* GetTuningProperty(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) override {
        return nullptr;
    }
    bool LoadString(std::uint32_t, std::uint32_t, cIGZString&) override { return false; }
    // Occupant manager InsertOccupant: the piece lands on `insertKey` of the underground fake's cell map.
    int InsertNetworkOccupant(cSC4NetworkOccupant* occupant, std::uint32_t) override {
        ++insertCalls;
        if (insertedCells == nullptr) return 0;
        occupant->AddRef();
        (*insertedCells)[insertKey].push_back(occupant);
        return 0;
    }

    std::map<std::uint32_t, std::vector<cISC4Occupant*>>* insertedCells = nullptr;
    std::uint32_t insertKey = 0;
    int insertCalls = 0;
    FakeTypeInfo typeInfo;
    FakePreview* preview = nullptr;
    FakeWorldCache* worldCache = nullptr;
    FakeCounted* terrain = nullptr;
    FakeCounted* occupants = nullptr;
    FakeCounted* cityView = nullptr;
    FakeCounted* orFilter = nullptr;
    INetworkConstraintSatisfier* satisfier = nullptr;
    std::uint32_t layerIdUsed = 0;
    std::int32_t priorityUsed = 0;
    std::uint32_t registeredMessage = 0, unregisteredMessage = 0;
    int addLayerCalls = 0, removeLayerCalls = 0;
};

struct FakeUndergroundHost final : IUndergroundNetworkToolHost {
    float TerrainHeight(std::uint32_t vertexId) override {
        auto it = terrainHeights.find(vertexId);
        return it == terrainHeights.end() ? 0.0f : it->second;
    }
    void TerrainCellHeights(int x, int z, float* heights) override {
        (void)x;
        (void)z;
        for (int i = 0; i < 4; ++i) heights[i] = cellHeights[i];
    }
    void QueryCellOccupants(int x, int z, std::list<cISC4Occupant*>& out) override {
        ++queryCalls;
        lastQueryKey = static_cast<std::uint32_t>(x) + (static_cast<std::uint32_t>(z) << 16);
        auto it = cellOccupants.find(lastQueryKey);
        if (it == cellOccupants.end()) return;
        for (cISC4Occupant* occupant : it->second) {
            occupant->AddRef();
            out.push_back(occupant);
        }
    }
    std::uint32_t AddOccupantToManager(cISC4Occupant* occupant, int param458) override {
        ++addCalls;
        addedOccupants.push_back(occupant);
        lastAddParam = param458;
        return ++nextHandle;
    }
    void RemoveOccupantFromManager(cISC4Occupant* occupant, bool keep, int param458) override {
        ++removeCalls;
        removedOccupants.push_back(occupant);
        lastRemoveKeep = keep;
        lastRemoveParam = param458;
    }
    cISCPropertyHolder* AcquireNetworkTuningProperties(std::uint32_t exemplarId) override {
        ++tuningCalls;
        tuningExemplar = exemplarId;
        auto* holder = new cSCPropertyHolder();
        holder->AddRef();
        cSCPropertyHelp::SetPropertyValue(holder, kUndergroundPropNetworkDepth, 2.5f);
        cSCPropertyHelp::SetPropertyValue(holder, kUndergroundPropTuningA, 1.25f);
        cSCPropertyHelp::SetPropertyValue(holder, kUndergroundPropTuningB, 3.75f);
        cSCPropertyHelp::SetPropertyValue(holder, kUndergroundPropTuningFlag, true);
        return holder;
    }
    void InsertRangeConstraintsForCellBase(std::uint32_t cellKey) override {
        ++rangeBaseCalls;
        lastRangeBaseKey = cellKey;
    }

    std::map<std::uint32_t, std::vector<cISC4Occupant*>> cellOccupants;
    std::map<std::uint32_t, float> terrainHeights;
    float cellHeights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    std::vector<cISC4Occupant*> addedOccupants, removedOccupants;
    std::uint32_t nextHandle = 0x40;
    std::uint32_t lastQueryKey = 0;
    std::uint32_t tuningExemplar = 0;
    std::uint32_t lastRangeBaseKey = 0;
    int queryCalls = 0, addCalls = 0, removeCalls = 0, tuningCalls = 0;
    int rangeBaseCalls = 0;
    bool lastRemoveKeep = true;
    int lastAddParam = 0, lastRemoveParam = 0;
};

// ============================================================================================================
// the concrete tool (the original's slots 0x298 / 0x2a0 are pure virtual; cSC4PipeTool / cSC4SubwayTool)
// ============================================================================================================
struct TestTool final : cSC4UndergroundNetworkTool {
    TestTool(std::uint32_t networkType, std::uint32_t gziid)
        : cSC4UndergroundNetworkTool(networkType), gziid_(gziid) {}
    std::uint32_t NetworkGZIID() override { return gziid_; }
    void CreateNetworkOccupant(cSC4NetworkCellInfo* cell, cSC4UndergroundOccupant*& out, float (*heights)[3],
                               std::uint32_t networkType) override {
        (void)networkType;
        ++createCalls;
        auto* occupant = new TestOccupant();
        occupant->hidden[0] = hideFirstTriangle;
        for (int v = 0; v < 4; ++v) heights[v][0] = 0.0f;
        created.push_back(occupant);
        if (assignCell) cell->occupant = occupant;   // as cSC4PipeTool does (the port's cell keeps a bare pointer)
        out = occupant;
    }
    std::uint32_t gziid_ = 0;
    bool assignCell = false;
    bool hideFirstTriangle = false;
    int createCalls = 0;
    std::vector<TestOccupant*> created;
};

// ============================================================================================================
// fixture
// ============================================================================================================
struct Fixture {
    explicit Fixture(std::uint32_t networkType = kNetworkPipe, std::uint32_t gziid = kGZIID_cSC4PipeOccupant) {
        baseHost.worldCache = &cache;
        baseHost.satisfier = &satisfier;
        baseHost.typeInfo.placement.float10[1] = 1.5f;
        baseHost.typeInfo.placement.float40[0] = 3.0f;
        SetNetworkToolHost(&baseHost);
        SetUndergroundNetworkToolHost(&host);
        tool = new TestTool(networkType, gziid);
    }
    ~Fixture() {
        SetUndergroundNetworkToolHost(nullptr);
        SetNetworkToolHost(nullptr);
        if (baseHost.preview != nullptr) delete baseHost.preview;
        // The tool and the fake world cache are deliberately leaked: its destructor is the original's ud2 trap by design.
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    FakeNetworkToolHost baseHost;
    FakeUndergroundHost host;
    FakeWorldCache cache;
    RecordingSatisfier satisfier;
    TestTool* tool = nullptr;
};

// A solved cell with the given piece type / rotation for the given key.
tSolvedCell Solved(std::uint32_t cellKey, std::uint32_t pieceType = 0x300, std::uint8_t rotation = 7) {
    tSolvedCell solved;
    solved.pieceType = pieceType;
    solved.rotationFlip = rotation;
    solved.cellKey = cellKey;
    return solved;
}

std::uint32_t Key(int x, int z) { return static_cast<std::uint32_t>(x) + (static_cast<std::uint32_t>(z) << 16); }

// ============================================================================================================
// tests
// ============================================================================================================

void TestTrivialAnswersAndTuning() {
    Fixture f;
    CHECK(!f.tool->StopPickAtWater());
    CHECK(!f.tool->AllowBridges());
    CHECK(!f.tool->AllowTunnels());
    CHECK(!f.tool->IsStoppedByWater());
    CHECK(f.tool->ShouldAutoConnect());
    int failureReason = 42;
    CHECK(f.tool->CheckForGoodTerrain(failureReason));
    CHECK(failureReason == 42);   // untouched
    CHECK(f.tool->CanTilesBeSupportedOnTerrain(kNetworkSubway));
    CHECK(f.tool->UpdateOccupantHighlights(-3) == -3);
    f.tool->DestroyHilightedOccupants();
    f.tool->ClearOccupantHighlights();
    std::vector<std::uint32_t> heights{1, 2, 3};
    f.tool->UpdateTerrainHeights(heights);
    CHECK(heights.size() == 3);
    int cell = 5;
    f.tool->CheckForReservedTiles(cell, kNetworkPipe, true);
    CHECK(cell == 5);
    std::vector<tImmovablePortion> portions(2);
    f.tool->ApplyPlaneConstraintToRegularCells(portions);
    CHECK(portions.size() == 2);
    CHECK_NEAR(f.tool->NetworkDepth(), 0.0f);
    CHECK_NEAR(f.tool->GetNetworkHeightForTerrainHeight(12.0f), 12.0f);

    // 0x1001a7c7e: the tuning properties and the release of the holder.
    f.tool->ReloadNetworkSpecificInfo(0x0a367ca7);
    CHECK(f.host.tuningCalls == 1);
    CHECK(f.host.tuningExemplar == 0x0a367ca7u);
    CHECK_NEAR(f.tool->NetworkDepth(), 2.5f);
    CHECK_NEAR(f.tool->TuningA(), 1.25f);
    CHECK_NEAR(f.tool->TuningB(), 3.75f);
    CHECK(f.tool->TuningFlag());
    CHECK_NEAR(f.tool->GetNetworkHeightForTerrainHeight(12.0f), 9.5f);
}

void TestInitAndShutdown() {
    Fixture f;
    CHECK(f.tool->Init());
    CHECK(f.tool->Inited());
    CHECK(f.baseHost.preview != nullptr);
    CHECK(f.baseHost.preview->inits == 1);
    CHECK(f.baseHost.registeredMessage == kNetworkToolMessageAltitudes);
    CHECK(f.baseHost.layerIdUsed == 5);
    CHECK(f.baseHost.priorityUsed == 1000);
    CHECK_NEAR(f.tool->NetworkDepth(), 2.5f);   // base Init called ReloadNetworkSpecificInfo
    CHECK(NetworkToolTestAccess::NetworkType(*f.tool) == kNetworkPipe);
    CHECK(NetworkToolTestAccess::ConstraintA(*f.tool) != nullptr);

    // A second Init keeps the state as it is (the guard is already set).
    CHECK(f.tool->Init());
    CHECK(f.baseHost.preview->inits == 1);

    CHECK(f.tool->Shutdown());
    CHECK(!f.tool->Inited());
    CHECK(f.baseHost.preview->shutdowns == 1);
    CHECK(f.baseHost.unregisteredMessage == kNetworkToolMessageAltitudes);
    // Shutdown twice is a no-op.
    CHECK(f.tool->Shutdown());
    CHECK(f.baseHost.preview->shutdowns == 1);
}

void TestOccupantLookups() {
    Fixture f;
    // 0x1001a7d3e
    auto* occupant = new TestOccupant();
    f.host.cellOccupants[Key(3, 4)].push_back(occupant);
    const std::uint32_t refsBefore = occupant->fakeRefs;
    cSC4UndergroundOccupant* found = nullptr;
    CHECK(f.tool->FindUndergroundOccupant(3, 4, found, kGZIID_cSC4PipeOccupant));
    CHECK(found == occupant);
    CHECK(occupant->fakeRefs == refsBefore + 1);   // the QI reference the caller owns
    found->Release();
    CHECK(occupant->fakeRefs == refsBefore);
    // A different iid is not answered.
    found = nullptr;
    CHECK(!f.tool->FindUndergroundOccupant(3, 4, found, 0xdeadbeefu));
    CHECK(found == nullptr);
    // Empty cell.
    CHECK(!f.tool->FindUndergroundOccupant(9, 9, found, kGZIID_cSC4PipeOccupant));
    CHECK(f.host.queryCalls == 3);
    CHECK(f.host.lastQueryKey == Key(9, 9));

    // 0x1001a81d6
    auto* connection = new FakeConnectionOccupant();
    connection->network = kGZIID_cSC4PipeOccupant;
    f.host.cellOccupants[Key(5, 6)].push_back(connection);
    cSC4UndergroundConnectionOccupant* foundConnection = nullptr;
    CHECK(f.tool->FindUndergroundConnectionOccupant(5, 6, kGZIID_cSC4PipeOccupant, foundConnection));
    CHECK(foundConnection == connection);
    foundConnection->Release();
    // Network mismatch: the query reference is dropped and nothing is returned.
    const int releasesBefore = connection->releaseCalls;
    foundConnection = nullptr;
    CHECK(!f.tool->FindUndergroundConnectionOccupant(5, 6, kGZIID_cSC4SubwayOccupant, foundConnection));
    CHECK(foundConnection == nullptr);
    CHECK(connection->releaseCalls == releasesBefore + 2);   // the mismatch reference + the list reference

    // 0x1001a93a6
    CHECK(f.tool->TestForAutoConnectOccupant(3, 4));
    CHECK(f.tool->TestForAutoConnectOccupant(5, 6));
    CHECK(!f.tool->TestForAutoConnectOccupant(7, 7));
}

void TestCreateNewOccupant() {
    Fixture f;
    f.tool->ReloadNetworkSpecificInfo(1);   // depth 2.5
    cSC4NetworkCellInfo cell;
    cell.x = 3;
    cell.z = 4;
    cell.vertexBaseHeights[0] = 10;
    cell.vertexBaseHeights[1] = 11;
    cell.vertexBaseHeights[2] = 12;
    cell.vertexBaseHeights[3] = 13;
    f.host.terrainHeights[10] = 1.0f;
    f.host.terrainHeights[11] = 2.0f;
    f.host.terrainHeights[12] = 3.0f;
    f.host.terrainHeights[13] = 4.0f;

    cSC4UndergroundOccupant* out = nullptr;
    bool existing = true;
    CHECK(f.tool->CreateNewUndergroundOccupant(&cell, Solved(Key(3, 4), 0xaaa, 9), out, existing));
    CHECK(!existing);   // a new occupant was built
    CHECK(out != nullptr);
    CHECK(f.tool->createCalls == 1);
    auto* created = static_cast<TestOccupant*>(out);
    CHECK((created->GetNetworkFlag() & kUndergroundToolFlagBuilt) != 0);
    CHECK(created->quadDrawPiece == 0xaaa);
    CHECK(created->quadDrawRotation == 9);
    // Quirk: the terrain heights are stored raw, without subtracting the network depth.
    CHECK_NEAR(created->terrainHeights[0], 1.0f);
    CHECK_NEAR(created->terrainHeights[1], 2.0f);
    CHECK_NEAR(created->terrainHeights[2], 3.0f);
    CHECK_NEAR(created->terrainHeights[3], 4.0f);
    CHECK(created->wallCalls == 1 && !created->lastWallUpdate);
    CHECK(created->floorCalls == 1 && !created->lastFloorUpdate);
    created->Release();

    // The cell now holds an occupant: the same call only refreshes the flag and reports "existing".
    f.host.cellOccupants[Key(3, 4)].push_back(created);
    out = nullptr;
    existing = false;
    CHECK(f.tool->CreateNewUndergroundOccupant(&cell, Solved(Key(3, 4)), out, existing));
    CHECK(existing);
    CHECK(out == created);
    CHECK((created->GetNetworkFlag() & kUndergroundToolFlagBuilt) != 0);

    // With the base's +0x98 mode byte set the flag is cleared instead.
    NetworkToolTestAccess::SetMode98(*f.tool, true);
    created->ClearNetworkFlag(kUndergroundToolFlagBuilt);
    out = nullptr;
    CHECK(f.tool->CreateNewUndergroundOccupant(&cell, Solved(Key(3, 4)), out, existing));
    CHECK((created->GetNetworkFlag() & kUndergroundToolFlagBuilt) == 0);
    out->Release();
}

void TestCreateNetworkOccupantWrapper() {
    Fixture f;
    cSC4NetworkCellInfo cell;
    cell.x = 8;
    cell.z = 9;
    float heights[4][3] = {};
    // 0x1001a71e6: the non-virtual wrapper only guards the output slot and forwards to the concrete tool.
    cSC4NetworkOccupant* out = nullptr;
    f.tool->cSC4UndergroundNetworkTool::CreateNetworkOccupant(&cell, &out, heights, kNetworkPipe);
    CHECK(f.tool->createCalls == 1);
    CHECK(out != nullptr);
    CHECK(out == static_cast<cSC4NetworkOccupant*>(f.tool->created.back()));
    out->Release();
    // A null out slot does not build anything.
    f.tool->cSC4UndergroundNetworkTool::CreateNetworkOccupant(&cell, nullptr, heights, kNetworkPipe);
    CHECK(f.tool->createCalls == 1);
}

void TestInsertOccupantsForSolvedCells() {
    Fixture f;
    f.tool->Init();
    const std::uint32_t key = Key(2, 2);
    cSC4NetworkCellInfo& cell = f.cache.cells[key];
    cell.x = 2;
    cell.z = 2;
    cell.status = 1;
    cell.vertexBaseHeights[0] = 20;
    cell.vertexBaseHeights[1] = 21;
    cell.vertexBaseHeights[2] = 22;
    cell.vertexBaseHeights[3] = 23;
    cell.vertexHeights[0] = 7.0f;
    cell.vertexHeights[1] = 8.0f;
    cell.vertexHeights[2] = 9.0f;
    cell.vertexHeights[3] = 10.0f;
    cell.tileNeighborhoods[kNetworkPipe] = 0xbeef;
    std::vector<tSolvedCell> solved{Solved(key)};
    std::list<cSC4NetworkOccupant*> occupants;

    f.host.terrainHeights[20] = 12.0f;
    f.host.terrainHeights[21] = 13.0f;
    f.host.terrainHeights[22] = 14.0f;
    f.host.terrainHeights[23] = 15.0f;

    // The base's +0x98 byte is clear and the call is not a preview: the base insertion (0x10018a13c) previews the cell:
    // its heights through vslot 0x12c (the underground's: terrain minus the 2.5 depth) and the underground
    // AddCellPreview (vslot 0xf0), which builds a preview occupant. The underground pass itself does not run.
    CHECK(!NetworkToolTestAccess::Mode98(*f.tool));
    f.tool->InsertOccupantsForSolvedCells(solved, &occupants, false);
    CHECK_NEAR(cell.vertexHeights[0], 9.5f);
    CHECK_NEAR(cell.vertexHeights[3], 12.5f);
    CHECK(f.tool->createCalls == 1);
    CHECK(f.tool->PreviewList().size() == 1);
    CHECK(occupants.empty());
    f.tool->ClearCellPreviews();
    f.host.cellOccupants.erase(key);
    const int addsBefore = f.host.addCalls;

    // With the mode byte set the base inserts the piece (its occupant manager puts it on the cell) and the
    // underground pass finds it again: geometry for an existing piece, the network's edges, usable, not re-added.
    f.baseHost.insertedCells = &f.host.cellOccupants;
    f.baseHost.insertKey = key;
    f.tool->hideFirstTriangle = true;
    NetworkToolTestAccess::SetMode98(*f.tool, true);
    f.tool->InsertOccupantsForSolvedCells(solved, &occupants, false);
    CHECK(f.tool->createCalls == 2);
    auto* created = f.tool->created.back();
    CHECK(f.baseHost.insertCalls == 1);
    CHECK(occupants.size() == 1 && occupants.front() == created);
    CHECK(created->vertexSetCalls == 1);
    CHECK_NEAR(created->vertexHeights[0], 9.5f);
    CHECK_NEAR(created->vertexHeights[3], 12.5f);
    CHECK(created->geometryCalls == 1);
    CHECK(created->lastGeometryUpdate);    // CreateGeometry(existing = true)
    CHECK(created->GetEdgeTypesFlag(kNetworkPipe) == 0xbeef);
    // The base insertion reaches the underground triangle slot: the piece's hidden triangle 0 becomes its flag.
    CHECK((created->GetNetworkFlag() & kNetworkFlagHideTriangle0) != 0);
    CHECK((created->GetNetworkFlag() & kUndergroundToolFlagUsable) != 0);
    CHECK((created->GetNetworkFlag() & kUndergroundToolFlagTranslucent) == 0);
    CHECK(f.host.addCalls == addsBefore);  // the manager already holds it
    for (cSC4NetworkOccupant* occupant : occupants) occupant->Release();
    occupants.clear();

    // A cell the manager does not know yet: the underground pass adds its own piece (handle in the cell's byte).
    f.baseHost.insertedCells = nullptr;
    f.host.cellOccupants.erase(key);
    f.tool->InsertOccupantsForSolvedCells(solved, nullptr, false);
    CHECK(f.tool->createCalls == 4);       // the base's piece and the underground's own
    CHECK(f.host.addCalls == addsBefore + 1);
    CHECK(f.host.lastAddParam == NetworkToolTestAccess::Unknown458(*f.tool));
    CHECK(cell.occupantId == static_cast<std::uint8_t>(f.host.nextHandle));
    CHECK(!f.tool->created.back()->lastGeometryUpdate);
}

// A concrete tool that puts the new piece into the cell record (cSC4PipeTool): the preview keeps its one reference. The stack copy
// AddCellPreview makes gives back only the reference it took on the cell's previous occupant.
void TestCellPreviewReferences() {
    Fixture f;
    f.tool->Init();
    f.tool->assignCell = true;
    cSC4NetworkCellInfo cell;
    cell.x = 5;
    cell.z = 5;
    cell.status = 1;
    auto* previous = new TestOccupant();
    previous->AddRef();
    cell.occupant = previous;
    f.tool->AddCellPreview(cell, Solved(Key(5, 5)));
    CHECK(f.tool->PreviewList().size() == 1);
    if (!f.tool->created.empty()) {
        TestOccupant* made = f.tool->created.back();
        CHECK(made->AddRef() == 2);   // the preview list's reference (the one CreateNetworkOccupant handed back), plus this one
        made->Release();
    }
    CHECK(previous->AddRef() == 3);   // (test occupants start at one) the cell's previous occupant got its reference back
    previous->Release();
    previous->Release();
}

void TestCellPreviews() {
    Fixture f;
    f.tool->Init();
    cSC4NetworkCellInfo cell;
    cell.x = 5;
    cell.z = 5;
    cell.status = 1;
    cell.vertexBaseHeights[0] = 30;
    cell.vertexBaseHeights[1] = 31;
    cell.vertexBaseHeights[2] = 32;
    cell.vertexBaseHeights[3] = 33;
    f.host.terrainHeights[30] = 10.0f;
    f.host.terrainHeights[31] = 10.0f;
    f.host.terrainHeights[32] = 10.0f;
    f.host.terrainHeights[33] = 10.0f;

    // 0x1001a7e56: a new occupant goes to the preview list and to the manager.
    f.tool->AddCellPreview(cell, Solved(Key(5, 5)));
    CHECK(f.tool->PreviewList().size() == 1);
    CHECK(f.tool->HighlightList().empty());
    CHECK(f.host.addCalls == 1);
    CHECK(cell.occupantId == static_cast<std::uint8_t>(f.host.nextHandle));
    auto* created = f.tool->created.back();
    CHECK(created->highlightCalls == 1);
    CHECK(created->lastHighlightCreate);
    CHECK(created->highlightState == 1);
    CHECK(created->highlightSetCalls == 1);

    // An occupied cell goes to the highlight list and is not built again.
    auto* old = new TestOccupant();
    f.host.cellOccupants[Key(6, 6)].push_back(old);
    cSC4NetworkCellInfo cell2;
    cell2.x = 6;
    cell2.z = 6;
    cell2.status = 1;
    const int createCallsBefore = f.tool->createCalls;
    f.tool->AddCellPreview(cell2, Solved(Key(6, 6)));
    CHECK(f.tool->PreviewList().size() == 1);
    CHECK(f.tool->HighlightList().size() == 1);
    CHECK(f.tool->createCalls == createCallsBefore);
    CHECK(old->highlightState == 1);

    // status == 0: nothing happens.
    cSC4NetworkCellInfo cell3;
    cell3.x = 7;
    cell3.z = 7;
    f.tool->AddCellPreview(cell3, Solved(Key(7, 7)));
    CHECK(f.tool->PreviewList().size() == 1);
    CHECK(f.tool->HighlightList().size() == 1);

    // 0x1001a80ec: both lists are unhighlighted; only the preview occupants are shut down and removed from the
    // manager (the original's highlight loop is just SetHighlight(0, 0) + Release -- checked against Ghidra).
    static_cast<cISC4Occupant*>(created)->SetFlag(1);
    static_cast<cISC4Occupant*>(old)->SetFlag(1);
    const int clearsBefore = f.baseHost.preview->clears;
    f.tool->ClearCellPreviews();
    CHECK(f.tool->PreviewList().empty());
    CHECK(f.tool->HighlightList().empty());
    CHECK(f.host.removeCalls == 1);   // the preview occupant; a highlighted one stays registered
    CHECK((static_cast<cISC4Occupant*>(old)->GetFlags() & 1u) != 0);
    CHECK(f.host.lastRemoveKeep == false);
    CHECK(f.host.lastRemoveParam == NetworkToolTestAccess::Unknown458(*f.tool));
    CHECK(created->highlightState == 0);
    CHECK(old->highlightState == 0);
    CHECK(created->shutdownCalls == 1);
    CHECK(f.baseHost.preview->clears == clearsBefore + 1);   // the base ClearCellPreviews clears the preview layer
}

void TestCreateCellInfos() {
    Fixture f;
    f.tool->Init();
    NetworkToolTestAccess::SetWidthPlusOne(*f.tool, 11);
    NetworkToolTestAccess::Placement(*f.tool).float40[0] = 3.0f;
    NetworkToolTestAccess::Placement(*f.tool).float10[1] = 1.5f;
    f.host.terrainHeights[0] = 100.0f;
    // width 11, x=1, z=2 -> vertex ids 2*11+1=23, 34, 35, 24
    f.host.terrainHeights[23] = 10.0f;
    f.host.terrainHeights[34] = 11.0f;
    f.host.terrainHeights[35] = 12.0f;
    f.host.terrainHeights[24] = 13.0f;
    NetworkToolTestAccess::Steps(*f.tool) = {tDraggedStep{0, 0, 1}};
    NetworkToolTestAccess::Cells(*f.tool) = {SC4PointU{1, 2}};
    NetworkToolTestAccess::ValidSteps(*f.tool) = 1;

    // 0x1001a78ee: a new cell record.
    f.tool->CreateCellInfosForCurrentPlacementTiles(kNetworkPipe);
    cSC4NetworkCellInfo* cell = f.cache.GetCell(Key(1, 2));
    CHECK(cell != nullptr);
    if (cell != nullptr) {
        CHECK(cell->x == 1 && cell->z == 2);
        CHECK(cell->networkFlags == kFlagForNetworkType[kNetworkPipe]);
        CHECK(cell->status == 1);
        CHECK(cell->stepIndex == 0);
        CHECK(cell->solvedOrImmovable);   // direction 1
        CHECK(!cell->shortStep);
        CHECK(cell->vertexBaseHeights[0] == 23);
        CHECK(cell->vertexBaseHeights[1] == 34);
        CHECK(cell->vertexBaseHeights[2] == 35);
        CHECK(cell->vertexBaseHeights[3] == 24);
        CHECK_NEAR(cell->vertexHeights[0], 7.5f);    // 10 - 2.5
        CHECK_NEAR(cell->vertexHeights[3], 10.5f);   // 13 - 2.5
        CHECK_NEAR(cell->range70[0].low, 4.5f);      // 7.5 - 3.0 (placement +0x40)
        CHECK_NEAR(cell->range70[0].high, 7.5f);
        CHECK_NEAR(cell->preferred90[0].low, 7.5f);
        CHECK_NEAR(cell->preferred90[0].high, 7.5f);
    }

    // An existing cell: flags are OR-ed in, and the ranges use placement +0x14.
    cSC4NetworkCellInfo& existing = f.cache.cells[Key(1, 2)];
    existing.vertexHeights[0] = 4.0f;
    existing.vertexHeights[1] = 5.0f;
    existing.vertexHeights[2] = 6.0f;
    existing.vertexHeights[3] = 7.0f;
    existing.networkFlags = 0x8000;
    f.host.terrainHeights[23] = 20.0f;
    f.tool->CreateCellInfosForCurrentPlacementTiles(kNetworkPipe);
    CHECK(existing.networkFlags == (0x8000u | kFlagForNetworkType[kNetworkPipe]));
    CHECK_NEAR(existing.vertexHeights[0], 4.0f);   // untouched
    CHECK_NEAR(existing.range70[0].low, 16.0f);    // 17.5 - 1.5
    CHECK_NEAR(existing.range70[0].high, 17.5f);
    CHECK_NEAR(existing.preferred90[0].low, 4.0f); // min(4.0, 17.5)
    CHECK_NEAR(existing.preferred90[0].high, 4.0f);

    // No valid step: nothing happens.
    NetworkToolTestAccess::ValidSteps(*f.tool) = 0;
    const std::size_t cellsBefore = f.cache.cells.size();
    f.tool->CreateCellInfosForCurrentPlacementTiles(kNetworkPipe);
    CHECK(f.cache.cells.size() == cellsBefore);
}

void TestConstraints() {
    Fixture f;
    f.tool->Init();
    f.tool->ReloadNetworkSpecificInfo(1);
    NetworkToolTestAccess::FixedVertices(*f.tool).insert(2);

    cSC4NetworkCellInfo& cell = f.cache.cells[Key(1, 1)];
    cell.x = 1;
    cell.z = 1;
    cell.vertexBaseHeights[0] = 1;
    cell.vertexBaseHeights[1] = 2;
    cell.vertexBaseHeights[2] = 3;
    cell.vertexBaseHeights[3] = 4;
    cell.vertexHeights[0] = 1.0f;
    cell.vertexHeights[1] = 2.0f;
    cell.vertexHeights[2] = 3.0f;
    cell.vertexHeights[3] = 4.0f;
    cell.range70[0] = {0.5f, 9.0f};
    cell.range70[1] = {1.5f, 9.0f};
    cell.range70[2] = {2.5f, 9.0f};
    cell.range70[3] = {3.5f, 9.0f};
    cell.preferred90[0] = {0.25f, 8.0f};
    cell.preferred90[1] = {1.25f, 8.0f};
    cell.preferred90[2] = {2.25f, 8.0f};
    cell.preferred90[3] = {3.25f, 8.0f};
    cell.hasOccupant = true;
    f.host.terrainHeights[1] = 5.5f;   // -2.5 depth -> 3.0
    f.host.terrainHeights[2] = 100.0f;
    f.host.terrainHeights[3] = 100.0f;
    f.host.terrainHeights[4] = 100.0f;

    // 0x1001a7616: four AddVertexToSolve calls, vertex 2 is already pinned.
    f.tool->InsertRangeConstraintsForCell(Key(1, 1));
    CHECK(f.satisfier.solved.size() == 4);
    CHECK(f.satisfier.fixed.size() == 1);
    CHECK(f.satisfier.fixed[0].first == 2);
    CHECK_NEAR(f.satisfier.fixed[0].second, 2.0f);
    // vertex 0: range min(0.5, 3.0)=0.5 .. min(9,3)=3, preferred min(0.25,3)=0.25 .. min(8,3)=3
    CHECK(f.satisfier.ranges.size() == 6);   // three vertices x (range + preferred range)
    CHECK(f.satisfier.ranges[0].kind == 0);
    CHECK(f.satisfier.ranges[0].vertexId == 1);
    CHECK_NEAR(f.satisfier.ranges[0].low, 0.5f);
    CHECK_NEAR(f.satisfier.ranges[0].high, 3.0f);
    CHECK(f.satisfier.ranges[1].kind == 1);
    CHECK_NEAR(f.satisfier.ranges[1].low, 0.25f);
    CHECK_NEAR(f.satisfier.ranges[1].high, 3.0f);
    CHECK(f.satisfier.preferred.size() == 3);   // hasOccupant

    // A buildable cell takes the base implementation instead.
    cell.buildable = true;
    f.tool->InsertRangeConstraintsForCell(Key(1, 1));
    CHECK(f.host.rangeBaseCalls == 1);
    CHECK(f.host.lastRangeBaseKey == Key(1, 1));
    CHECK(f.satisfier.solved.size() == 4);   // unchanged
    cell.buildable = false;

    // A solved/immovable cell (+0x5b) only gets four fixed vertices: no AddVertexToSolve, no ranges.
    const std::size_t solvedBefore = f.satisfier.solved.size();
    const std::size_t rangesBefore = f.satisfier.ranges.size();
    const std::size_t fixedBefore = f.satisfier.fixed.size();
    cell.solvedOrImmovable = true;
    f.tool->InsertRangeConstraintsForCell(Key(1, 1));
    CHECK(f.satisfier.solved.size() == solvedBefore);
    CHECK(f.satisfier.ranges.size() == rangesBefore);
    CHECK(f.satisfier.fixed.size() == fixedBefore + 4);
    CHECK(f.satisfier.fixed.back().first == 4);
    CHECK_NEAR(f.satisfier.fixed.back().second, 4.0f);
    cell.solvedOrImmovable = false;

    // 0x1001a779a: solvedOrImmovable returns immediately.
    cell.solvedOrImmovable = true;
    f.tool->InsertNetworkSpecificConstraintsForCell(Key(1, 1));
    CHECK(f.satisfier.equalities.empty());
    cell.solvedOrImmovable = false;

    // A plain piece id returns immediately.
    NetworkToolTestAccess::SolvedCellList(*f.tool).push_back(Solved(Key(1, 1), kUndergroundPiecePipe));
    cell.pieceFromSolvedList = true;
    cell.solvedCellIndex = 0;
    f.tool->InsertNetworkSpecificConstraintsForCell(Key(1, 1));
    CHECK(f.satisfier.equalities.empty());

    // A complex piece (not one of the eight plain ones) pins adjacent vertex ids.
    NetworkToolTestAccess::SolvedCellList(*f.tool)[0].pieceType = 0x1234;
    cell.vertexBaseHeights[0] = 5;
    cell.vertexBaseHeights[1] = 5;   // equal -> skipped
    cell.vertexBaseHeights[2] = 7;
    cell.vertexBaseHeights[3] = 8;
    f.tool->InsertNetworkSpecificConstraintsForCell(Key(1, 1));
    CHECK(f.satisfier.equalities.size() == 2);
    CHECK(f.satisfier.equalities[0].first == 5);
    CHECK(f.satisfier.equalities[0].second == 7);
    CHECK(f.satisfier.equalities[1].first == 7);
    CHECK(f.satisfier.equalities[1].second == 8);

    // Buildable cells shift the ids by 100000; a negative-looking sum is skipped.
    f.satisfier.equalities.clear();
    cell.buildable = true;
    cell.vertexBaseHeights[0] = 5;
    cell.vertexBaseHeights[1] = 5;
    cell.vertexBaseHeights[2] = 7;
    cell.vertexBaseHeights[3] = static_cast<std::int32_t>(0x80000000u);   // 0x80000000+100000 < 0
    f.tool->InsertNetworkSpecificConstraintsForCell(Key(1, 1));
    CHECK(f.satisfier.equalities.size() == 1);
    CHECK(f.satisfier.equalities[0].first == 100005u);
    CHECK(f.satisfier.equalities[0].second == 100007u);

    // The occupant path: piece id from the occupant (0x1001a77ef).
    f.satisfier.equalities.clear();
    cell.buildable = false;
    cell.pieceFromSolvedList = false;
    auto* occupant = new TestOccupant();
    occupant->SetQuadDraw(0x2222, 0);
    cell.occupant = occupant;
    cell.vertexBaseHeights[0] = 1;
    cell.vertexBaseHeights[1] = 2;
    cell.vertexBaseHeights[2] = 3;
    cell.vertexBaseHeights[3] = 4;
    f.tool->InsertNetworkSpecificConstraintsForCell(Key(1, 1));
    CHECK(f.satisfier.equalities.size() == 3);
    cell.occupant = nullptr;

    // 0x1001a7c30: the satisfier's height wins, the terrain fallback subtracts the depth.
    f.satisfier.heights[1] = 42.0f;
    CHECK_NEAR(f.tool->GetNetworkVertexHt(1), 42.0f);
    f.satisfier.heights.erase(1);
    CHECK_NEAR(f.tool->GetNetworkVertexHt(1), 3.0f);   // 5.5 - 2.5
}

void TestPropagation() {
    Fixture f;
    f.tool->Init();   // the tool's world cache holds the cells
    // (0,0) connects to -x (edge 0) and +z (edge 3).
    const std::uint32_t originKey = Key(0, 0);
    cSC4NetworkCellInfo& origin = f.cache.cells[originKey];
    origin.x = 0;
    origin.z = 0;
    origin.tileNeighborhoods[kNetworkPipe] = 0x03000003u;   // edges 0 and 3 have both bits
    auto* west = new TestOccupant();
    west->SetQuadDraw(0x111, 1);
    cSC4NetworkCellInfo& westCell = f.cache.cells[Key(-1, 0)];
    westCell.x = 0xffffffffu;
    westCell.z = 0;
    westCell.occupant = west;
    auto* south = new TestOccupant();
    south->SetQuadDraw(0x222, 2);
    cSC4NetworkCellInfo& southCell = f.cache.cells[Key(0, 1)];
    southCell.x = 0;
    southCell.z = 1;
    southCell.occupant = south;

    auto& solvedList = NetworkToolTestAccess::SolvedCellList(*f.tool);
    f.tool->PropagateFromCell(&origin, 0);
    CHECK(solvedList.size() == 2);
    CHECK(solvedList[0].cellKey == Key(-1, 0));
    CHECK(solvedList[0].pieceType == 0x111);
    CHECK(solvedList[0].rotationFlip == 1);
    CHECK(solvedList[1].cellKey == Key(0, 1));
    CHECK(solvedList[1].pieceType == 0x222);
    CHECK(southCell.solvedCellIndex == 1);

    // A neighbour that is already solved is not added again.
    f.tool->PropagateFromCell(&origin, 0);
    CHECK(solvedList.size() == 2);

    // A cell without an occupant is not pushed.
    origin.tileNeighborhoods[kNetworkPipe] = 0x03000003u;   // edges 0 and 3 again
    f.cache.cells.erase(Key(0, 1));
    solvedList.clear();
    f.tool->PropagateFromCell(&origin, 0);
    CHECK(solvedList.size() == 1);   // only the west neighbour is still there
    CHECK(solvedList[0].cellKey == Key(-1, 0));

    // 0x1001a932a walks the solved list and propagates from each cell; the pass reaches the new entries too.
    solvedList.clear();
    solvedList.push_back(Solved(originKey, 0x300));
    f.tool->PropagateSolvedCellsForSmoothing();
    CHECK(solvedList.size() == 2);   // the origin plus the west cell
}

void TestStartPlacing() {
    Fixture f;
    f.tool->Init();
    f.tool->ReloadNetworkSpecificInfo(1);
    // Vertex heights the solver reports: 5 for every id.
    for (int id = 0; id < 8; ++id) f.satisfier.heights[id] = 5.0f;

    // A cell whose occupant already has the right heights: no geometry rebuild.
    cSC4NetworkCellInfo& cellA = f.cache.cells[Key(1, 1)];
    cellA.x = 1;
    cellA.z = 1;
    cellA.status = 1;
    cellA.vertexBaseHeights[0] = 0;
    cellA.vertexBaseHeights[1] = 1;
    cellA.vertexBaseHeights[2] = 2;
    cellA.vertexBaseHeights[3] = 3;
    auto* occupantA = new TestOccupant();
    for (int v = 0; v < 4; ++v) occupantA->vertexHeights[v] = 5.0f;
    f.host.cellOccupants[Key(1, 1)].push_back(occupantA);
    NetworkToolTestAccess::SolvedCells(*f.tool).insert(Key(1, 1));

    // A cell whose occupant has different heights: SetVertexHeights + CreateGeometry(true).
    cSC4NetworkCellInfo& cellB = f.cache.cells[Key(2, 2)];
    cellB.x = 2;
    cellB.z = 2;
    cellB.status = 1;
    cellB.vertexBaseHeights[0] = 4;
    cellB.vertexBaseHeights[1] = 5;
    cellB.vertexBaseHeights[2] = 6;
    cellB.vertexBaseHeights[3] = 7;
    auto* occupantB = new TestOccupant();
    for (int v = 0; v < 4; ++v) occupantB->vertexHeights[v] = 1.0f;
    f.host.cellOccupants[Key(2, 2)].push_back(occupantB);
    NetworkToolTestAccess::SolvedCells(*f.tool).insert(Key(2, 2));

    // A cell with no piece flag is left alone.
    cSC4NetworkCellInfo& cellC = f.cache.cells[Key(3, 3)];
    cellC.x = 3;
    cellC.z = 3;
    cellC.status = 1;
    cellC.vertexBaseHeights[0] = 0;
    cellC.vertexBaseHeights[1] = 1;
    cellC.vertexBaseHeights[2] = 2;
    cellC.vertexBaseHeights[3] = 3;
    auto* occupantC = new TestOccupant();
    for (int v = 0; v < 4; ++v) occupantC->vertexHeights[v] = 1.0f;
    occupantC->SetNetworkFlag(kUndergroundToolFlagNoPiece);
    f.host.cellOccupants[Key(3, 3)].push_back(occupantC);
    NetworkToolTestAccess::SolvedCells(*f.tool).insert(Key(3, 3));

    // Not in the solved set: ignored.
    cSC4NetworkCellInfo& cellD = f.cache.cells[Key(4, 4)];
    cellD.x = 4;
    cellD.z = 4;
    cellD.status = 1;
    auto* occupantD = new TestOccupant();
    f.host.cellOccupants[Key(4, 4)].push_back(occupantD);

    // occupantId != 0 and status == 0: the quirk skip.
    cSC4NetworkCellInfo& cellE = f.cache.cells[Key(5, 5)];
    cellE.x = 5;
    cellE.z = 5;
    cellE.status = 0;
    cellE.occupantId = 3;
    auto* occupantE = new TestOccupant();
    f.host.cellOccupants[Key(5, 5)].push_back(occupantE);
    NetworkToolTestAccess::SolvedCells(*f.tool).insert(Key(5, 5));

    f.tool->StartPlacing();
    CHECK(occupantA->vertexSetCalls == 0);
    CHECK(occupantA->geometryCalls == 0);
    CHECK(occupantB->vertexSetCalls == 1);
    CHECK_NEAR(occupantB->vertexHeights[0], 5.0f);
    CHECK(occupantB->geometryCalls == 1);
    CHECK(occupantB->lastGeometryUpdate);
    CHECK(occupantC->vertexSetCalls == 0);
    CHECK(occupantC->geometryCalls == 0);
    CHECK(occupantD->vertexSetCalls == 0);
    CHECK(occupantE->vertexSetCalls == 0);
}

void TestSetQuadTrianglesToDraw() {
    Fixture f;
    auto* occupant = new TestOccupant();
    occupant->SetNetworkFlag(kNetworkFlagHideTriangle0 | kNetworkFlagHideTriangle1 | kNetworkFlagHideTriangle2);
    occupant->hidden[0] = true;
    occupant->hidden[1] = false;
    occupant->hidden[2] = false;
    cSC4TileNeighborhoodCompact compact(0);
    f.tool->SetQuadTrianglesToDraw(occupant, compact);
    CHECK((occupant->GetNetworkFlag() & kNetworkFlagHideTriangle0) != 0);   // hidden -> set
    CHECK((occupant->GetNetworkFlag() & kNetworkFlagHideTriangle1) != 0);   // visible -> also set (quirk)
    CHECK((occupant->GetNetworkFlag() & kNetworkFlagHideTriangle2) != 0);

    // All visible: bit 0 clear, bits 1 and 2 set (the inverted ones).
    occupant->hidden[0] = false;
    occupant->SetNetworkFlag(kNetworkFlagHideTriangle0 | kNetworkFlagHideTriangle1 | kNetworkFlagHideTriangle2);
    f.tool->SetQuadTrianglesToDraw(occupant, compact);
    CHECK((occupant->GetNetworkFlag() & kNetworkFlagHideTriangle0) == 0);
    CHECK((occupant->GetNetworkFlag() & kNetworkFlagHideTriangle1) != 0);
    CHECK((occupant->GetNetworkFlag() & kNetworkFlagHideTriangle2) != 0);

    // All hidden: bit 0 set, bits 1 and 2 clear.
    occupant->hidden[0] = true;
    occupant->hidden[1] = true;
    occupant->hidden[2] = true;
    occupant->SetNetworkFlag(kNetworkFlagHideTriangle0 | kNetworkFlagHideTriangle1 | kNetworkFlagHideTriangle2);
    f.tool->SetQuadTrianglesToDraw(occupant, compact);
    CHECK((occupant->GetNetworkFlag() & kNetworkFlagHideTriangle0) != 0);
    CHECK((occupant->GetNetworkFlag() & kNetworkFlagHideTriangle1) == 0);
    CHECK((occupant->GetNetworkFlag() & kNetworkFlagHideTriangle2) == 0);

    // A plain network occupant does not answer the underground iid: nothing is touched.
    auto* plain = new cSC4NetworkOccupant();
    plain->SetNetworkFlag(kNetworkFlagHideTriangle0);
    f.tool->SetQuadTrianglesToDraw(plain, compact);
    CHECK((plain->GetNetworkFlag() & kNetworkFlagHideTriangle0) != 0);
    f.tool->SetQuadTrianglesToDraw(nullptr, compact);   // no crash
}

void TestUpdateTerrainHeightsPartial() {
    Fixture f;
    f.tool->Init();
    f.cache.cells[Key(1, 1)] = cSC4NetworkCellInfo{};
    NetworkToolTestAccess::SolvedCellList(*f.tool).push_back(Solved(Key(1, 1)));
    std::list<cSC4NetworkOccupant*> occupants;
    f.tool->UpdateUndergroundOccupantTerrainHeights({SC4PointU{1, 1}}, occupants, kNetworkPipe);
    CHECK(f.cache.cells.empty());   // the world cache was cleared
    CHECK(NetworkToolTestAccess::SolvedCellList(*f.tool).empty());
    CHECK(f.cache.clears == 1);
}

// cSC4PipeTool::CreateNetworkOccupant (i386 0x0012c8a0): a pipe on the cell's corners, the cell's occupant, its network
// flags, the terrain under its vertices, the pipe edges and the type's demolition cost.
void TestPipeToolCreatesPipe() {
    Fixture f;
    f.baseHost.typeInfo.placement.unknownA8[9] = 125;
    auto* tool = new cSC4PipeTool();
    CHECK(tool->Init());
    CHECK(tool->NetworkGZIID() == kGZCLSID_cSC4PipeOccupant);
    cSC4NetworkCellInfo cell;
    cell.x = 3;
    cell.z = 5;
    cell.networkFlags = 0x10;
    cell.tileNeighborhoods[kNetworkPipe] = 0x02000200;
    cell.tileNeighborhoods[0] = 0x0303;
    for (int v = 0; v < 4; ++v) {
        cell.vertexHeights[v] = 100.0f + static_cast<float>(v);
        cell.vertexBaseHeights[v] = 40 + v;
        f.host.terrainHeights[static_cast<std::uint32_t>(40 + v)] = 110.0f + static_cast<float>(v);
    }
    float corners[4][3] = {};
    cSC4UndergroundOccupant* made = nullptr;
    tool->CreateNetworkOccupant(&cell, made, corners, 7);
    CHECK(made != nullptr);
    auto* pipe = dynamic_cast<cSC4PipeOccupant*>(made);
    CHECK(pipe != nullptr);
    CHECK(cell.occupant == made);
    CHECK_NEAR(corners[0][0], 48.0f);
    CHECK_NEAR(corners[0][1], 100.0f);
    CHECK_NEAR(corners[0][2], 80.0f);
    CHECK_NEAR(corners[1][2], 96.0f);
    CHECK_NEAR(corners[1][1], 101.0f);
    CHECK_NEAR(corners[2][0], 64.0f);
    CHECK_NEAR(corners[2][2], 96.0f);
    CHECK_NEAR(corners[3][0], 64.0f);
    CHECK_NEAR(corners[3][1], 103.0f);
    CHECK_NEAR(corners[3][2], 80.0f);
    std::uint32_t cx = 0, cz = 0;
    made->GetOccupiedCell(cx, cz);
    CHECK(cx == 3 && cz == 5);
    CHECK((made->GetNetworkFlag() & 0x10) != 0);
    CHECK(made->GetEdgeTypesFlag(kNetworkPipe) == 0x02000200);
    CHECK(made->GetEdgeTypesFlag(0) == 0);   // only the tool's network
    CHECK(made->GetCostOfDemolition() == 125);
    float terrain[4] = {};
    made->GetTerrainHeights(terrain);
    CHECK_NEAR(terrain[0], 110.0f);
    CHECK_NEAR(terrain[3], 113.0f);
    made->Release();
    tool->Shutdown();
    // The tool is never destroyed (its destructor is the original's trap).
}

}  // namespace

int main() {
    TestTrivialAnswersAndTuning();
    TestInitAndShutdown();
    TestOccupantLookups();
    TestCreateNewOccupant();
    TestCreateNetworkOccupantWrapper();
    TestInsertOccupantsForSolvedCells();
    TestCellPreviews();
    TestCellPreviewReferences();
    TestCreateCellInfos();
    TestConstraints();
    TestPropagation();
    TestStartPlacing();
    TestSetQuadTrianglesToDraw();
    TestUpdateTerrainHeightsPartial();
    TestPipeToolCreatesPipe();
    if (failures == 0) {
        std::printf("all cSC4UndergroundNetworkTool tests passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", failures);
    return 1;
}
