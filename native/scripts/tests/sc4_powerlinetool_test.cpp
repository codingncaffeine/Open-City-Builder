// cSC4PowerLineTool — unit test for the recovered slice: the pole info bookkeeping, the placement tests, the
// pole creation/hookup, the cost/failure strings, the init/read-ini/shutdown path, the preview highlight and
// the PARTIAL solver/placement passes. The engine globals and the unrecovered occupants are replaced by
// ICityOccupant / IPowerPoleOccupant / IPowerLineToolHost fakes.
#include <cstdio>
#include <cstring>
#include <list>
#include <string>
#include <unistd.h>
#include <vector>

#include "gz/cIGZMessage2.h"
#include "rz/cRZString.h"
#include "sc4/PowerLineOccupant.h"
#include "sc4/PowerLineTool.h"

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
// Access to the cSC4NetworkTool base state (the base declares NetworkToolTestAccess a friend).
struct NetworkToolTestAccess {
    static std::vector<tDraggedStep>& Steps(cSC4NetworkTool& t) { return t.steps_; }
    static std::vector<SC4PointU>& Cells(cSC4NetworkTool& t) { return t.cells_; }
    static std::vector<SC4PointU>& Anchors(cSC4NetworkTool& t) { return t.anchors_; }
    static int& TotalSteps(cSC4NetworkTool& t) { return t.totalSteps_; }
    static int& ValidSteps(cSC4NetworkTool& t) { return t.validSteps_; }
    static int& StraightSteps(cSC4NetworkTool& t) { return t.straightSteps_; }
    static bool& Unknown98(cSC4NetworkTool& t) { return t.unknown98_; }
    static int& FailureReason(cSC4NetworkTool& t) { return t.failureReason_; }
};

// Access to the derived members the public interface cannot reach (PowerLineTool.h friend).
struct PowerLineToolTestAccess {
    static std::vector<tPoleInfo>& PoleInfos(cSC4PowerLineTool& t) { return t.poleInfos_; }
    static std::vector<std::vector<tLineInfo>>& LineInfos(cSC4PowerLineTool& t) { return t.lineInfos_; }
    static int& WireCellCount(cSC4PowerLineTool& t) { return t.wireCellCount_; }
    static int& PoleCount(cSC4PowerLineTool& t) { return t.poleCount_; }
    static std::list<IPowerPoleOccupant*>& CreatedPoles(cSC4PowerLineTool& t) { return t.createdPoles_; }
    static std::list<IPowerPoleOccupant*>& PreviewPoles(cSC4PowerLineTool& t) { return t.previewPoles_; }
    static std::list<tIntersectionInfo>& ConnectionList(cSC4PowerLineTool& t) { return t.connectionList_; }
    static std::vector<std::vector<int>>& ConnectionGroups(cSC4PowerLineTool& t) { return t.connectionGroups_; }
    static int& PoleSpacing(cSC4PowerLineTool& t) { return t.poleSpacing_; }
    static float& PoleHeightOffset(cSC4PowerLineTool& t) { return t.poleHeightOffset_; }
    static int& PoleCost(cSC4PowerLineTool& t) { return t.poleCost_; }
    static float& WireCost(cSC4PowerLineTool& t) { return t.wireCost_; }
    static bool& PowerLineInited(cSC4PowerLineTool& t) { return t.powerLineInited_; }
    static std::uint32_t* Directions() { return cSC4PowerLineTool::sPowerPoleForDirectionsFlag; }
    static bool& SbInited() { return cSC4PowerLineTool::sbInited_; }
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
        if (refs > 0) --refs;
        return refs;
    }
    int refs = 0;
};

// The exemplar the tuning / pole lookups hand out (the tool Releases it without having AddRef'd it; the real
// resource manager keeps it alive, so this fake never dies).
class FakeExemplar final : public cIGZUnknown {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return refs; }
    int refs = 1;
};

struct FakePreview : INetworkPreview {
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (refs > 0) --refs;
        return refs;
    }
    void Init() override { ++inits; }
    void Shutdown() override { ++shutdowns; }
    void Clear() override { ++clears; }
    void Erase(const SC4PointU&) override {}
    void AddUntexturedSelection(const SC4PointU& cell, std::uint32_t color, const float* heights, float elevation,
                                bool lit) override {
        added.push_back(cell);
        colors.push_back(color);
        (void)heights;
        (void)elevation;
        lit ? ++litAdds : ++unlitAdds;
    }
    void AdjustCellHeights(const SC4PointU&, const float*, bool) override {}
    int refs = 0;
    int inits = 0, shutdowns = 0, clears = 0, litAdds = 0, unlitAdds = 0;
    std::vector<SC4PointU> added;
    std::vector<std::uint32_t> colors;
};

struct FakeWorldCache : INetworkWorldCache {
    void Clear() override {}
    cSC4NetworkCellInfo* GetCell(std::uint32_t) override { return nullptr; }
    void GetAllCells(std::vector<cSC4NetworkCellInfo>& out) override { out.clear(); }
    cSC4NetworkCellInfo* CreateCell(std::uint32_t) override { return nullptr; }
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
    int PenWidthX() const override { return 2; }
    int PenWidthZ() const override { return 1; }
    const std::vector<cSC4NetworkBridgeTypeInfo>& BridgeTypes() const override { return bridges; }
    const cSC4NetworkPlacementParams& PlacementParams() const override { return placement; }
    const cSC4NetworkSupportStructInfo* FindSupportStructure(std::uint32_t) const override { return nullptr; }
    std::uint32_t PlacementExemplarId() const override { return 0x5a5a; }
    std::vector<cSC4NetworkBridgeTypeInfo> bridges;
    cSC4NetworkPlacementParams placement;
};

// cISC4PowerPoleOccupant / cSC4PowerPoleOccupant seam fake.
struct FakePole : IPowerPoleOccupant {
    ~FakePole() override = default;
    int GetNumberOfConnections() override { return static_cast<int>(connected.size()); }
    IPowerPoleOccupant* GetConnectionOccupant(int index) override { return connected[static_cast<std::size_t>(index)]; }
    std::vector<IPowerPoleOccupant*> connected;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 1 || iid == kGZCLSID_cSC4PowerPoleOccupant) {
            *out = static_cast<IPowerPoleOccupant*>(this);
            AddRef();
            return true;
        }
        return false;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (refs > 0) --refs;
        ++releases;
        return refs;
    }
    void GetCell(std::uint32_t& x, std::uint32_t& z) override {
        x = cellX;
        z = cellZ;
    }
    void InitFromExemplar(cIGZUnknown* exemplar, std::uint32_t directionMask) override {
        exemplarInits.push_back(exemplar);
        masks.push_back(directionMask);
    }
    void SetExemplar(cIGZUnknown* exemplar) override {
        exemplars.push_back(exemplar);
        ++setExemplarCalls;
    }
    void SetPosition(const float* xyz) override {
        position[0] = xyz[0];
        position[1] = xyz[1];
        position[2] = xyz[2];
        ++positionCalls;
    }
    void SetPreview(bool value) override {
        preview = value;
        ++previewCalls;
    }
    void SetModelState(int state, bool immediate) override {
        modelState = state;
        ++modelCalls;
        (void)immediate;
    }
    void OnInserted() override { ++insertedCalls; }
    std::uint32_t DirectionFlags() const override { return directionFlags; }
    void SetDirectionFlags(std::uint32_t flags) override {
        directionFlags = flags;
        ++directionCalls;
    }

    int refs = 0;
    int releases = 0;
    std::uint32_t cellX = 0, cellZ = 0;
    std::uint32_t directionFlags = 0;
    float position[3] = {0.0f, 0.0f, 0.0f};
    bool preview = false;
    int modelState = -1;
    int positionCalls = 0, previewCalls = 0, modelCalls = 0, insertedCalls = 0, directionCalls = 0;
    int setExemplarCalls = 0;
    std::vector<cIGZUnknown*> exemplarInits;
    std::vector<cIGZUnknown*> exemplars;
    std::vector<std::uint32_t> masks;
};

// cISC4PowerLineOccupant seam fake.
struct FakeWire : IPowerLineOccupant {
    ~FakeWire() override = default;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 1 || iid == kGZIID_cISC4PowerLineOccupant) {
            *out = static_cast<IPowerLineOccupant*>(this);
            AddRef();
            return true;
        }
        return false;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (refs > 0) --refs;
        ++releases;
        return refs;
    }
    void GetConnectedPoles(IPowerPoleOccupant*& first, IPowerPoleOccupant*& second, bool& connected) override {
        first = nullptr;
        second = nullptr;
        connected = false;
    }
    int refs = 1;
    int releases = 0;
};

// cISC4Occupant seam fake: one city occupant with an optional pole / wire occupant attached.
struct FakeOccupant : ICityOccupant {
    ~FakeOccupant() override = default;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 1 || iid == kGZIID_cISC4Occupant) {
            *out = static_cast<ICityOccupant*>(this);
            AddRef();
            return true;
        }
        return false;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (refs > 0) --refs;
        ++releases;
        return refs;
    }
    std::uint32_t GetType() override { return type; }
    void GetPosition(float* xyz) override {
        xyz[0] = position[0];
        xyz[1] = position[1];
        xyz[2] = position[2];
    }
    IPowerPoleOccupant* QueryPole() override {
        if (pole != nullptr) pole->AddRef();
        return pole;
    }
    IPowerLineOccupant* QueryPowerLine() override {
        if (wire != nullptr) wire->AddRef();
        return wire;
    }
    bool QueriesAs(std::uint32_t iid) override {
        queried.push_back(iid);
        return answers;
    }

    int refs = 1;
    int releases = 0;
    std::uint32_t type = 0;
    float position[3] = {0.0f, 0.0f, 0.0f};
    IPowerPoleOccupant* pole = nullptr;
    IPowerLineOccupant* wire = nullptr;
    bool answers = false;
    std::vector<std::uint32_t> queried;
};

// Both hosts in one object: the base tool's engine globals and the power-line tool's neighbours.
struct FakeHost : INetworkToolHost, IPowerLineToolHost {
    FakeHost() {
        char path[] = "/tmp/sc4_powerlinetool_testXXXXXX";
        const int fd = ::mkstemp(path);
        const char* text = "[PowerPoles]\r\n0 = 0x100\r\n1 = 0x101\r\n3 = 0x103\r\n5 = 0x105\r\n";
        if (fd >= 0) {
            (void)::write(fd, text, std::strlen(text));
            ::close(fd);
        }
        iniPath = path;
        tuningHolder.AddRef();
        tuningHolderProps();
    }
    ~FakeHost() override {
        for (FakePole* pole : ownedPoles) delete pole;
        for (FakeWire* wire : ownedWires) delete wire;
        for (FakeOccupant* occupant : ownedOccupants) delete occupant;
        if (!iniPath.empty()) ::unlink(iniPath.c_str());
        delete preview;
        delete worldCache;
        delete terrain;
        delete occupantsManager;
        delete cityViewModel;
        delete orFilter;
    }

    // --- INetworkToolHost ---
    void RegisterShutdownCallback(bool (*fn)(void*), void* user) override {
        shutdownFn = fn;
        shutdownUser = user;
    }
    cIGZUnknown* AcquireTerrain() override {
        terrain->AddRef();
        return terrain;
    }
    float TerrainCellSize() override { return cellSize; }
    bool TerrainAllowsNetwork(std::uint32_t x, std::uint32_t z) override {
        lastTerrainCell = x | (z << 16);
        return terrainAllows;
    }
    int CityWidth() override { return 40; }
    int CityHeight() override { return 30; }
    cIGZUnknown* AcquireOccupantManager() override {
        occupantsManager->AddRef();
        return occupantsManager;
    }
    cIGZUnknown* AcquireCityViewModel() override {
        cityViewModel->AddRef();
        return cityViewModel;
    }
    void AddRenderLayer(INetworkPreview* layer, std::uint32_t layerId, std::int32_t priority) override {
        addedLayer = layer;
        layerIdUsed = layerId;
        priorityUsed = priority;
    }
    void RemoveRenderLayer(INetworkPreview* layer) override { removedLayer = layer; }
    std::uint32_t RendererAltitudeScale() override { return 77; }
    void RegisterMessage(cIGZMessageTarget2*, std::uint32_t messageType) override {
        registeredMessage = messageType;
    }
    void UnregisterMessage(cIGZMessageTarget2*, std::uint32_t messageType) override {
        unregisteredMessage = messageType;
    }
    cIGZUnknown* CreateNetworkOrFilter(std::uint32_t flags) override {
        orFilterFlags = flags;
        orFilter->AddRef();
        return orFilter;
    }
    INetworkPreview* CreatePreview() override {
        if (preview == nullptr) preview = new FakePreview();
        preview->AddRef();
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
        return nullptr;
    }
    cIGZUnknown* GetTuningProperty(std::uint32_t type, std::uint32_t group, std::uint32_t instance,
                                   std::uint32_t propertyId) override {
        tuningType = type;
        tuningGroup = group;
        tuningInstance = instance;
        tuningProperty = propertyId;
        return &exemplar;
    }
    bool LoadString(std::uint32_t group, std::uint32_t instance, cIGZString& out) override {
        loadStringGroup = group;
        loadStringInstance = instance;
        ++loadStringCalls;
        out.AssignFromCString("You cannot place a power line there.");
        return true;
    }

    // --- IPowerLineToolHost ---
    void GetOccupantsInCellRect(std::vector<ICityOccupant*>& out, int x0, int z0, int x1, int z1) override {
        ++cellQueries;
        for (FakeOccupant* occupant : cityOccupants) {
            const int cx = cellOf(occupant->position[0]);
            const int cz = cellOf(occupant->position[2]);
            if (cx < x0 || cx > x1 || cz < z0 || cz > z1) continue;
            occupant->AddRef();
            out.push_back(occupant);
        }
    }
    void PositionToCityCell(float x, float z, int& cx, int& cz) override {
        cx = cellOf(x);
        cz = cellOf(z);
    }
    void InsertPoleOccupant(IPowerPoleOccupant* pole) override {
        inserted.push_back(pole);
        pole->AddRef();
    }
    void RemovePoleOccupant(IPowerPoleOccupant* pole, bool shutdown) override {
        removed.push_back(pole);
        (void)shutdown;
        pole->Release();
    }
    int ZoneAt(int x, int z) override {
        zoneCalls.push_back({x, z});
        return zoneValue;
    }
    bool HasLotAt(int x, int z) override {
        lotCalls.push_back({x, z});
        return hasLot;
    }
    void TerrainCornerHeights(int x, int z, float out[4]) override {
        (void)x;
        (void)z;
        for (int i = 0; i < 4; ++i) out[i] = corners[i];
    }
    float TerrainHeightAt(float x, float z) override {
        (void)x;
        (void)z;
        return terrainHeight;
    }
    bool RendererRayTest(const float* start, const float* direction, float* end) override {
        (void)start;
        (void)direction;
        ++rayCalls;
        end[0] = rayEnd[0];
        end[1] = rayEnd[1];
        end[2] = rayEnd[2];
        return rayHit;
    }
    const cISCPropertyHolder* TuningPropertyHolder() override { return &tuningHolder; }
    cRZIniFile* OpenPowerPoleIni() override { ++iniOpens; return new cRZIniFile(cRZString(iniPath.c_str())); }
    int Get0To3Direction(int x1, int z1, int x2, int z2) override {
        ++directionCalls;
        const int dx = x2 > x1 ? 1 : (x2 < x1 ? -1 : 0);
        const int dz = z2 > z1 ? 1 : (z2 < z1 ? -1 : 0);
        if (dx == 0 && dz == 0) return 0;
        if (dx == 0) return 2;
        if (dz == 0) return 0;
        return dx == dz ? 1 : 3;
    }
    void DrawNetworkLine(SC4PointU& a, SC4PointU& b, bool preview, std::uint32_t networkType) override {
        drawnLines.push_back({a, b});
        (void)preview;
        (void)networkType;
    }
    bool AttemptNeighborConnections(bool first, std::uint32_t networkType) override {
        (void)first;
        (void)networkType;
        ++attemptCalls;
        return attemptResult;
    }
    void PlaceNeighborConnections() override { ++placeNeighborsCalls; }
    IPowerPoleOccupant* CreatePoleOccupant() override {
        auto* pole = new FakePole();
        ownedPoles.push_back(pole);
        return pole;
    }
    void ConnectPoles(IPowerPoleOccupant* from, IPowerPoleOccupant* to,
                      const std::vector<tLineInfo>* lineInfos) override {
        connects.push_back({from, to});
        connectLines.push_back(lineInfos ? static_cast<int>(lineInfos->size()) : -1);
        connectLineVectors.push_back(lineInfos);
        if (auto* pole = dynamic_cast<FakePole*>(from)) pole->connected.push_back(to);   // AddConnection
    }
    void RemovePoleConnection(IPowerPoleOccupant* from, IPowerPoleOccupant* to, bool preview) override {
        removals.push_back({from, to});
        (void)preview;
    }
    void DrawPoleConnection(IPowerPoleOccupant* from, IPowerPoleOccupant* to, bool preview) override {
        draws.push_back({from, to});
        (void)preview;
    }

    // --- helpers ---
    FakeOccupant* AddOccupant(float x, float z, std::uint32_t type) {
        auto* occupant = new FakeOccupant();
        occupant->position[0] = x;
        occupant->position[2] = z;
        occupant->type = type;
        ownedOccupants.push_back(occupant);
        cityOccupants.push_back(occupant);
        return occupant;
    }
    static int cellOf(float value) { return static_cast<int>(value / 16.0f); }
    void tuningHolderProps() {
        cSCPropertyHelp::SetPropertyValue(&tuningHolder, 0x098b25c8, static_cast<std::uint32_t>(25));
        cSCPropertyHelp::SetPropertyValue(&tuningHolder, 0x098b27d0, 3.5f);
        cSCPropertyHelp::SetPropertyValue(&tuningHolder, 0x4a5f5953, static_cast<std::int32_t>(100));
        cSCPropertyHelp::SetPropertyValue(&tuningHolder, 0x0a5f596b, 0.5f);
    }

    // INetworkToolHost state
    FakeCounted* terrain = new FakeCounted();
    FakeCounted* occupantsManager = new FakeCounted();
    FakeCounted* cityViewModel = new FakeCounted();
    FakeCounted* orFilter = new FakeCounted();
    FakeConstraintSatisfier satisfier;
    FakeTypeInfo typeInfo;
    FakeExemplar exemplar;
    FakePreview* preview = nullptr;
    FakeWorldCache* worldCache = nullptr;
    cSCPropertyHolder tuningHolder;
    std::string iniPath;
    bool (*shutdownFn)(void*) = nullptr;
    void* shutdownUser = nullptr;
    INetworkPreview* addedLayer = nullptr;
    INetworkPreview* removedLayer = nullptr;
    std::uint32_t layerIdUsed = 0;
    std::int32_t priorityUsed = 0;
    std::uint32_t registeredMessage = 0, unregisteredMessage = 0, orFilterFlags = 0;
    std::uint32_t lastNetworkType = 0xffffffffu, lastTerrainCell = 0xffffffffu, cellKey = 0xffffffffu;
    std::uint32_t tuningType = 0, tuningGroup = 0, tuningInstance = 0, tuningProperty = 0;
    std::uint32_t loadStringGroup = 0, loadStringInstance = 0;
    int loadStringCalls = 0;

    // IPowerLineToolHost state
    float cellSize = 16.0f;
    bool terrainAllows = true;
    float terrainHeight = 5.0f;
    float corners[4] = {1.0f, 3.0f, 2.0f, 0.0f};
    bool rayHit = false;
    float rayEnd[3] = {0.0f, 0.0f, 0.0f};
    int zoneValue = 0;
    bool hasLot = false;
    std::vector<std::pair<int, int>> zoneCalls, lotCalls;
    std::vector<FakeOccupant*> cityOccupants, ownedOccupants;
    std::vector<FakePole*> ownedPoles;
    std::vector<FakeWire*> ownedWires;
    std::vector<IPowerPoleOccupant*> inserted, removed;
    std::vector<std::pair<IPowerPoleOccupant*, IPowerPoleOccupant*>> connects, removals, draws;
    std::vector<int> connectLines;
    std::vector<const std::vector<tLineInfo>*> connectLineVectors;
    std::vector<std::pair<SC4PointU, SC4PointU>> drawnLines;
    int cellQueries = 0, rayCalls = 0, directionCalls = 0, attemptCalls = 0;
    int placeNeighborsCalls = 0, iniOpens = 0;
    bool attemptResult = false;
};

struct Fixture {
    FakeHost host;
    cSC4PowerLineTool* tool = nullptr;

    explicit Fixture(bool init = true) {
        SetNetworkToolHost(&host);
        SetPowerLineToolHost(&host);
        tool = new cSC4PowerLineTool();
        tool->AddRef();
        if (init) CHECK(tool->Init());
    }
    ~Fixture() {
        tool->Shutdown();
        tool->Release();
        SetNetworkToolHost(nullptr);
        SetPowerLineToolHost(nullptr);
    }
    FakeHost& operator->() { return host; }
};

SC4PointU Point(std::uint32_t x, std::uint32_t z) { return SC4PointU{x, z}; }

}  // namespace

int main() {
    // ---- static state / offsets -----------------------------------------------------------------------------
    {
        CHECK(sizeof(tConnectionInfo) == 0x08);
        CHECK(sizeof(tPoleInfo) == 0x50);
        CHECK(sizeof(tIntersectionInfo) == 0x18);
        CHECK(sizeof(tLineInfo) == 0x18);
        CHECK(offsetof(tLineInfo, first) == 0x10);
    }

    // ---- AddPowerPoleInfo + ReadPowerPoleInfos --------------------------------------------------------------
    {
        FakeHost host;
        SetNetworkToolHost(&host);
        SetPowerLineToolHost(&host);
        cSC4PowerLineTool::AddPowerPoleInfo(cRZString("7"), cRZString("0x707"), nullptr);
        CHECK(PowerLineToolTestAccess::Directions()[7] == 0x707);
        cSC4PowerLineTool::ReadPowerPoleInfos();
        CHECK(host.iniOpens == 1);
        for (auto& entry : {std::pair<std::uint32_t, std::uint32_t>{0, 0x100}, {1, 0x101}, {3, 0x103}, {5, 0x105}}) {
            CHECK(PowerLineToolTestAccess::Directions()[entry.first] == entry.second);
        }
        cSC4PowerLineTool::sbInited_ = false;
        CHECK(cSC4PowerLineTool::StaticInit());
        CHECK(host.iniOpens == 2);           // StaticInit -> ReadPowerPoleInfos
        CHECK(cSC4PowerLineTool::StaticInit());  // guarded
        CHECK(host.iniOpens == 2);
        cSC4PowerLineTool::sbInited_ = false;
        SetNetworkToolHost(nullptr);
        SetPowerLineToolHost(nullptr);
    }

    // ---- Init / tuning values / shutdown --------------------------------------------------------------------
    {
        Fixture f;
        CHECK(f.tool->powerLineInited_);
        CHECK(PowerLineToolTestAccess::PoleSpacing(*f.tool) == 25);
        CHECK(PowerLineToolTestAccess::PoleHeightOffset(*f.tool) == 3.5f);
        CHECK(PowerLineToolTestAccess::PoleCost(*f.tool) == 100);
        CHECK(PowerLineToolTestAccess::WireCost(*f.tool) == 0.5f);
        CHECK(f.host.registeredMessage == kNetworkToolMessageAltitudes);  // base Init registers it
        CHECK(f.host.lastNetworkType == kNetworkPowerLine);
        CHECK(f.tool->CursorId() == 0xa16f1463);
        CHECK(f.tool->DirectionalCursorBase() == 0x69e13770);
        CHECK(f.tool->GetCostOfSolution() == 0);  // poleCount_ <= 1
        CHECK(f.tool->Shutdown());
        CHECK(!f.tool->powerLineInited_);
        CHECK(f.tool->Shutdown());  // guarded
    }

    // ---- ComputePolePosition --------------------------------------------------------------------------------
    {
        Fixture f;
        float out[3] = {0.0f, 0.0f, 0.0f};
        f.tool->ComputePolePosition(2, 3, out);
        CHECK(out[0] == 2.0f * 16.0f + 8.0f);
        CHECK(out[2] == 3.0f * 16.0f + 8.0f);
        CHECK(out[1] == 5.5f);  // terrain height + 0.5
        f.host.terrainAllows = false;
        f.tool->ComputePolePosition(0, 0, out);
        CHECK(out[1] == 3.5f);  // max corner = 3.0
        f.host.corners[0] = -4.0f;
        f.host.terrainAllows = true;
        f.tool->ComputePolePosition(1, 1, out);
        CHECK(out[1] == 5.5f);
    }

    // ---- PlacePole / AddConnection --------------------------------------------------------------------------
    {
        Fixture f;
        auto& cells = NetworkToolTestAccess::Cells(*f.tool);
        cells = {Point(4, 4), Point(5, 4)};
        tIntersectionInfo info;
        f.tool->PlacePole(0, false, 0, info);
        f.tool->PlacePole(1, false, 0, info);
        auto& poles = PowerLineToolTestAccess::PoleInfos(*f.tool);
        CHECK(poles.size() == 2);
        CHECK(poles[0].index == 0 && poles[0].x == 4 && poles[0].z == 4);
        CHECK(poles[1].index == 1 && poles[1].x == 5 && poles[1].z == 4);
        CHECK(poles[0].connectionList.empty() && poles[1].connectionList.empty());
        cSC4PowerLineTool::AddConnection(poles[0], poles[1], -1);
        CHECK(poles[0].connectionList.size() == 1);
        CHECK(poles[0].connectionList[0].poleIndex == 1 && poles[0].connectionList[0].value == -1);
        CHECK(poles[1].connectionList[0].poleIndex == 0);
        CHECK(poles[0].connections == 0x01 && poles[1].connections == 0x01);  // direction 0 (+x)
        // diagonal same sign -> direction 1, the other diagonal -> 3
        CHECK(f.host.Get0To3Direction(0, 0, 2, 2) == 1);
        CHECK(f.host.Get0To3Direction(0, 0, 2, -2) == 3);
        CHECK(f.host.Get0To3Direction(0, 0, 0, 2) == 2);
    }

    // ---- GetPrimaryCell / PoleIsInList / IntersectionIsInList ------------------------------------------------
    {
        Fixture f;
        auto& steps = NetworkToolTestAccess::Steps(*f.tool);
        auto& cells = NetworkToolTestAccess::Cells(*f.tool);
        auto& anchors = NetworkToolTestAccess::Anchors(*f.tool);
        steps = {tDraggedStep{1, 4, 0}};
        cells = {Point(0, 8), Point(2, 2), Point(2, 2), Point(3, 3), Point(9, 9)};
        anchors = {Point(2, 2)};
        CHECK(f.tool->GetPrimaryCell(0, 0) == 1);  // cells_[1] == (2,2): the scan runs from the step's first cell
        anchors[0] = Point(5, 5);
        CHECK(f.tool->GetPrimaryCell(0, 0) == 1);  // |5-2| == |5-2|
        anchors[0] = Point(9, 0);
        CHECK(f.tool->GetPrimaryCell(0, 0) == -1);  // no diagonal cell
        NetworkToolTestAccess::StraightSteps(*f.tool) = 1;
        CHECK(f.tool->GetPrimaryCell(0, 0) == 1);  // straight part: first cell, no scan
        NetworkToolTestAccess::StraightSteps(*f.tool) = 0;

        auto& poles = PowerLineToolTestAccess::PoleInfos(*f.tool);
        poles.clear();
        tPoleInfo info;
        info.x = 5;
        info.z = 1;
        poles.push_back(info);
        FakePole pole;
        pole.cellX = 5;
        pole.cellZ = 99;
        CHECK(f.tool->PoleIsInList(&pole));  // quirk: x matches, z does not
        pole.cellX = 99;
        pole.cellZ = 1;
        CHECK(f.tool->PoleIsInList(&pole));  // quirk: z matches
        pole.cellX = 99;
        pole.cellZ = 99;
        CHECK(!f.tool->PoleIsInList(&pole));
        CHECK(!f.tool->PoleIsInList(nullptr));

        poles[0].poleA = &pole;
        poles[0].poleB = nullptr;
        poles[0].flagA = true;
        tIntersectionInfo intersection{&pole, nullptr, true, false, {}};
        CHECK(f.tool->IntersectionIsInList(intersection));
        intersection.secondNew = true;
        CHECK(!f.tool->IntersectionIsInList(intersection));
    }

    // ---- CellIsOKForPlacement (zone query quirk, occupant types, lots) ----------------------------------------
    {
        Fixture f;
        // unzoned cell, no occupant, no lot -> 0
        CHECK(f.tool->CellIsOKForPlacement(2, 7) == 0);
        CHECK(f.host.zoneCalls.size() == 1);  // short-circuits on the first zone == 0
        f.host.zoneCalls.clear();
        // zoned cell: the original queries (x, z), (x, z) again and (z, z) -> 1
        f.host.zoneValue = 2;
        CHECK(f.tool->CellIsOKForPlacement(2, 7) == 1);
        CHECK(f.host.zoneCalls.size() == 3);
        CHECK(f.host.zoneCalls[0] == std::make_pair(2, 7));
        CHECK(f.host.zoneCalls[1] == std::make_pair(2, 7));
        CHECK(f.host.zoneCalls[2] == std::make_pair(7, 7));  // quirk: the third query is (z, z)
        f.host.zoneValue = 0;
        // building occupant -> 2
        FakeOccupant* building = f.host.AddOccupant(2 * 16.0f + 8.0f, 7 * 16.0f + 8.0f, 0x278128a0);
        CHECK(f.tool->CellIsOKForPlacement(2, 7) == 2);
        building->type = 0xc772bf98;
        building->answers = true;  // underground network occupant: ignored
        CHECK(f.tool->CellIsOKForPlacement(2, 7) == 0);
        building->answers = false;  // other network occupant -> 3
        CHECK(f.tool->CellIsOKForPlacement(2, 7) == 3);
        CHECK(building->queried.size() == 2 && building->queried[0] == 0x4a10afc2);
        building->type = 0x1234;  // unrelated occupant: ignored
        f.host.hasLot = true;
        CHECK(f.tool->CellIsOKForPlacement(2, 7) == 2);
        CHECK(f.host.lotCalls.back() == std::make_pair(2, 7));
    }

    // ---- FindPoleOccupant / CellIsOccupiedByPowerLine -------------------------------------------------------
    {
        Fixture f;
        FakePole pole;
        pole.cellX = 3;
        pole.cellZ = 4;
        FakeOccupant* occupant = f.host.AddOccupant(3 * 16.0f + 8.0f, 4 * 16.0f + 8.0f, 0xc772bf98);
        occupant->pole = &pole;
        IPowerPoleOccupant* found = nullptr;
        CHECK(f.tool->FindPoleOccupant(3, 4, found));
        CHECK(found == &pole);
        CHECK(pole.refs == 1);  // QueryPole AddRef
        found->Release();
        CHECK(!f.tool->FindPoleOccupant(5, 5, found));

        auto* wire = new FakeWire();
        wire->refs = 1;
        f.host.ownedWires.push_back(wire);
        FakeOccupant* wireCell = f.host.AddOccupant(6 * 16.0f + 8.0f, 7 * 16.0f + 8.0f, 0xc772bf98);
        wireCell->wire = wire;
        IPowerLineOccupant* foundWire = nullptr;
        CHECK(f.tool->CellIsOccupiedByPowerLine(6, 7, foundWire));
        CHECK(foundWire == wire);
        CHECK(wire->refs == 2);  // the host's reference plus the one QueryPowerLine returned
        foundWire->Release();
        CHECK(wire->refs == 1);
    }

    // ---- CreatePowerPole: new pole and existing pole -------------------------------------------------------
    {
        Fixture f;
        // new pole: exemplar id comes from sPowerPoleForDirectionsFlag[mask]
        PowerLineToolTestAccess::Directions()[1] = 0x5151;
        tPoleInfo info;
        info.x = 2;
        info.z = 3;
        info.connections = 0x01;
        f.tool->CreatePowerPole(info);
        CHECK(info.created);
        CHECK(info.occupant != nullptr);
        auto* created = dynamic_cast<FakePole*>(info.occupant);
        CHECK(created != nullptr && created->refs == 1);  // the factory returns 0 references, CreatePowerPole adds one
        CHECK(created->masks.size() == 1 && created->masks[0] == 0x01);
        CHECK(f.host.tuningInstance == 0x5151);
        CHECK(created->positionCalls == 1);
        CHECK(created->position[1] == f.host.terrainHeight + 0.5f);   // on the cell: the height offset is only the solver's sight line
        CHECK(created->previewCalls == 1 && created->preview);
        CHECK(created->directionFlags == 0x01);
        info.occupant->Release();

        // existing pole: direction flags merge into the mask, SetExemplar on the existing occupant
        Fixture g;
        FakePole existing;
        existing.cellX = 9;
        existing.cellZ = 9;
        existing.directionFlags = 0x02;
        FakeOccupant* cityPole = g.host.AddOccupant(9 * 16.0f + 8.0f, 9 * 16.0f + 8.0f, 0xc772bf98);
        cityPole->pole = &existing;
        PowerLineToolTestAccess::Directions()[3] = 0x5353;
        tPoleInfo reused;
        reused.x = 9;
        reused.z = 9;
        reused.connections = 0x01;
        g.tool->CreatePowerPole(reused);
        CHECK(!reused.created);
        CHECK(reused.occupant == &existing);
        CHECK(existing.directionFlags == 0x03);
        CHECK(existing.setExemplarCalls == 1);
        CHECK(g.host.tuningInstance == 0x5353);
        CHECK(existing.preview);  // SetPreview(unknown98_ == 0) == true in the normal placement mode
        reused.occupant->Release();
    }

    // ---- CheckForReservedTiles ------------------------------------------------------------------------------
    {
        Fixture f;
        auto& steps = NetworkToolTestAccess::Steps(*f.tool);
        auto& cells = NetworkToolTestAccess::Cells(*f.tool);
        steps = {tDraggedStep{0, 1, 0}, tDraggedStep{2, 3, 0}};
        cells = {Point(0, 0), Point(1, 0), Point(2, 0), Point(3, 0)};
        NetworkToolTestAccess::ValidSteps(*f.tool) = 2;
        // zoned cells (result 1) only set the failure reason and keep the drag
        f.host.zoneValue = 2;
        int step = 2;
        f.tool->CheckForReservedTiles(step);
        CHECK(step == 2);  // valid = total - 1 -> step unchanged
        CHECK(NetworkToolTestAccess::FailureReason(*f.tool) == static_cast<int>(0xf0000011u));
        // a building occupant in the second step (result 2) trims to the previous step
        f.host.zoneValue = 0;
        FakeOccupant* building = f.host.AddOccupant(3 * 16.0f + 8.0f, 0.0f + 8.0f, 0x278128a0);
        step = 2;
        f.tool->CheckForReservedTiles(step);
        CHECK(step == 1);
        CHECK(NetworkToolTestAccess::FailureReason(*f.tool) == static_cast<int>(0xf0000012u));
        building->type = 0x1234;
        f.host.zoneValue = 0;
        // a network occupant (result 3): no trim on the last step when the previous cell was not placeable
        building->type = 0xc772bf98;
        building->answers = false;
        step = 2;
        f.tool->CheckForReservedTiles(step);
        CHECK(step == 1);
        CHECK(NetworkToolTestAccess::FailureReason(*f.tool) == static_cast<int>(0xf0000013u));
    }

    // ---- SetTerrainHilight ----------------------------------------------------------------------------------
    {
        Fixture f;
        auto& steps = NetworkToolTestAccess::Steps(*f.tool);
        auto& cells = NetworkToolTestAccess::Cells(*f.tool);
        auto& poles = PowerLineToolTestAccess::PoleInfos(*f.tool);
        steps = {tDraggedStep{0, 1, 0}, tDraggedStep{2, 2, 0}};
        cells = {Point(0, 0), Point(1, 0), Point(2, 0)};
        NetworkToolTestAccess::ValidSteps(*f.tool) = 1;
        NetworkToolTestAccess::TotalSteps(*f.tool) = 2;
        tPoleInfo pole;
        pole.x = 1;
        pole.z = 0;
        poles.push_back(pole);
        f.tool->SetTerrainHilight(0);
        // valid step 0: cell (0,0) blue, cell (1,0) has a pole -> skipped; invalid step 1: (2,0) red
        CHECK(f.host.preview->added.size() == 2);
        CHECK(f.host.preview->colors[0] == 0x800000ffu);
        CHECK(f.host.preview->colors[1] == 0x80ff0000u);
        CHECK(f.host.preview->added[0] == Point(0, 0));
        CHECK(f.host.preview->added[1] == Point(2, 0));
        CHECK(f.host.preview->unlitAdds == 2 && f.host.preview->litAdds == 0);
    }

    // ---- GetCostOfSolution / GetFailureReasonString --------------------------------------------------------
    {
        Fixture f;
        PowerLineToolTestAccess::WireCellCount(*f.tool) = 10;
        PowerLineToolTestAccess::PoleCount(*f.tool) = 3;
        CHECK(f.tool->GetCostOfSolution() == static_cast<int>(10 * 0.5f) + 3 * 100);
        PowerLineToolTestAccess::PoleCount(*f.tool) = 1;
        CHECK(f.tool->GetCostOfSolution() == 0);
        auto& anchors = NetworkToolTestAccess::Anchors(*f.tool);
        anchors = {Point(4, 4)};
        cRZString text;
        f.host.zoneValue = 3;  // > 0 -> "cannot build here"
        f.tool->GetFailureReasonString(0, text);
        CHECK(f.host.loadStringCalls == 1);
        CHECK(f.host.loadStringGroup == 0x2a592fd1u && f.host.loadStringInstance == 0x8aadd2b5u);
        f.host.loadStringCalls = 0;
        f.host.zoneValue = 0;
        f.tool->GetFailureReasonString(0, text);
        CHECK(f.host.loadStringCalls == 0);
        // a non-zero reason goes to the base tool's string loader (## Missing failure reason string ##)
        f.tool->GetFailureReasonString(0x5, text);
        CHECK(f.host.loadStringCalls == 1 && f.host.loadStringInstance == 0x5);
    }

    // ---- DeterminePolePositions: anchors force poles, the cells between them are the segment's wire cells --------
    {
        Fixture f;
        auto& steps = NetworkToolTestAccess::Steps(*f.tool);
        auto& cells = NetworkToolTestAccess::Cells(*f.tool);
        steps = {tDraggedStep{0, 2, 0}};
        cells = {Point(0, 0), Point(1, 0), Point(2, 0)};
        NetworkToolTestAccess::Anchors(*f.tool) = {Point(0, 0), Point(2, 0)};
        NetworkToolTestAccess::ValidSteps(*f.tool) = 1;
        NetworkToolTestAccess::TotalSteps(*f.tool) = 1;
        f.host.zoneValue = 0;  // unzoned: CellIsOKForPlacement == 0
        f.tool->DeterminePolePositions();
        auto& poles = PowerLineToolTestAccess::PoleInfos(*f.tool);
        auto& lines = PowerLineToolTestAccess::LineInfos(*f.tool);
        CHECK(poles.size() == 2);
        CHECK(poles[0].x == 0 && poles[1].x == 2);
        CHECK(poles[0].connectionList.size() == 1 && poles[0].connectionList[0].poleIndex == 1);
        CHECK(poles[0].connectionList[0].value == 1);   // the segment list of the second pole
        CHECK(lines.size() == 2 && lines[0].empty() && lines[1].size() == 1 && lines[1][0].x == 1);
        CHECK(PowerLineToolTestAccess::PoleCount(*f.tool) == 2);
        CHECK(PowerLineToolTestAccess::WireCellCount(*f.tool) == 1);
        CHECK(NetworkToolTestAccess::ValidSteps(*f.tool) == 1);
        // a second call releases the old pole occupants and rebuilds
        f.tool->DeterminePolePositions();
        CHECK(poles.size() == 2);
        CHECK(PowerLineToolTestAccess::PoleCount(*f.tool) == 2);
    }

    // ---- DeterminePolePositions: a pole every poleSpacing steps, the anchors at both ends ------------------------
    {
        Fixture f;
        auto& steps = NetworkToolTestAccess::Steps(*f.tool);
        auto& cells = NetworkToolTestAccess::Cells(*f.tool);
        steps.clear();
        cells.clear();
        for (std::uint32_t i = 0; i < 25; ++i) {
            steps.push_back(tDraggedStep{i, i, 0});
            cells.push_back(Point(i, 4));
        }
        NetworkToolTestAccess::Anchors(*f.tool) = {Point(0, 4), Point(24, 4)};
        NetworkToolTestAccess::ValidSteps(*f.tool) = 25;
        NetworkToolTestAccess::TotalSteps(*f.tool) = 25;
        NetworkToolTestAccess::StraightSteps(*f.tool) = 25;
        PowerLineToolTestAccess::PoleSpacing(*f.tool) = 10;
        f.host.zoneValue = 0;
        f.host.rayHit = false;  // nothing between the poles: no sight-line pole
        f.tool->DeterminePolePositions();
        auto& poles = PowerLineToolTestAccess::PoleInfos(*f.tool);
        auto& lines = PowerLineToolTestAccess::LineInfos(*f.tool);
        CHECK(poles.size() == 4);
        CHECK(poles.size() == 4 && poles[0].x == 0 && poles[1].x == 10 && poles[2].x == 20 && poles[3].x == 24);
        CHECK(lines.size() == 4 && lines[1].size() == 9 && lines[2].size() == 9 && lines[3].size() == 3);
        CHECK(PowerLineToolTestAccess::WireCellCount(*f.tool) == 21);
        CHECK(f.host.rayCalls > 0);  // the primary cells between poles asked for the sight line
        // a hill (the ray stops 1 m from the last pole) puts a pole on the next primary cell
        f.host.rayHit = true;
        f.host.rayEnd[0] = 0.5f * 16.0f + 1.0f;
        f.host.rayEnd[1] = f.host.terrainHeight;
        f.host.rayEnd[2] = 4.5f * 16.0f;
        f.tool->DeterminePolePositions();
        CHECK(poles.size() >= 2 && poles[1].x == 1);
    }

    // ---- PlacePoles: occupants, the wire occupants of each new connection, the city and the lists ---------------
    {
        Fixture f;
        auto& cells = NetworkToolTestAccess::Cells(*f.tool);
        cells = {Point(0, 0), Point(1, 0), Point(2, 0)};
        auto& lines = PowerLineToolTestAccess::LineInfos(*f.tool);
        tIntersectionInfo none;
        f.tool->PlacePole(0, false, 0, none);
        lines.push_back({});
        f.tool->PlacePole(2, true, 1, none);
        tLineInfo wireCell;
        wireCell.x = 1;
        wireCell.z = 0;
        wireCell.first = true;
        lines.push_back({wireCell});
        auto& poles = PowerLineToolTestAccess::PoleInfos(*f.tool);
        NetworkToolTestAccess::Unknown98(*f.tool) = true;  // placing for good
        f.tool->PlacePoles(true);
        CHECK(poles.size() == 2);
        CHECK(poles[0].occupant == nullptr && poles[1].occupant == nullptr);  // the city holds them now
        CHECK(f.host.inserted.size() == 2);  // both were created -> inserted into the city
        // both ends connected; the lower pole (0, 0) owns the segment's one wire occupant
        CHECK(f.host.connects.size() == 2);
        CHECK(f.host.connectLines.size() == 2 && f.host.connectLines[0] == 1 && f.host.connectLines[1] == -1);
        CHECK(lines[1][0].line == nullptr);  // the segment dropped its reference after the connection took one
        auto& created = PowerLineToolTestAccess::CreatedPoles(*f.tool);
        CHECK(created.empty());  // previews only
        CHECK(f.host.ownedPoles.size() == 2 && f.host.ownedPoles[0]->insertedCalls == 1 && f.host.ownedPoles[1]->insertedCalls == 1);

        // a preview: the poles go to the created list highlighted, nothing is wired
        Fixture g;
        auto& gcells = NetworkToolTestAccess::Cells(*g.tool);
        gcells = {Point(0, 0), Point(1, 0)};
        g.tool->PlacePole(0, false, 0, none);
        g.tool->PlacePole(1, false, 0, none);
        auto& groups = PowerLineToolTestAccess::ConnectionGroups(*g.tool);
        groups = {{0, 1}};  // one connection group listing both drag cell indices
        NetworkToolTestAccess::Unknown98(*g.tool) = false;
        g.tool->PlacePoles(true);
        auto& gpoles = PowerLineToolTestAccess::PoleInfos(*g.tool);
        CHECK(gpoles[0].connectionList.size() == 2);  // pass A connects every ordered pair one cell apart
        CHECK(groups.empty());
        auto& gcreated = PowerLineToolTestAccess::CreatedPoles(*g.tool);
        CHECK(gcreated.size() == 2);
        CHECK(g.host.connects.empty());
        for (IPowerPoleOccupant* pole : gcreated) pole->Release();
        gcreated.clear();
    }

    // ---- FindNewPolePosition --------------------------------------------------------------------------------
    {
        Fixture f;
        std::vector<SC4PointU> cells = {Point(0, 0), Point(1, 0), Point(2, 0)};
        SC4CellRect rect{0, 0, 1, 1};  // cells (0,0) and (1,0) are inside, (2,0) outside
        FakePole existing;
        existing.cellX = 7;
        existing.cellZ = 3;
        f.tool->FindNewPolePosition(rect, 0, 3, 1, cells, &existing, nullptr);
        CHECK(f.host.inserted.size() == 1);  // a new pole was created and inserted at (2, 0)
        auto& anchors = NetworkToolTestAccess::Anchors(*f.tool);
        CHECK(anchors.size() == 2);
        CHECK(anchors[0] == Point(2, 0) && anchors[1] == Point(7, 3));
        CHECK(f.host.drawnLines.size() == 1);
        // the pole the search created was connected against the existing pole's direction
        CHECK(f.host.directionCalls > 0);
    }

    // ---- DoNetworkSpecificPlacement / UpdateOnZoneChange ----------------------------------------------------
    {
        Fixture f;
        auto& steps = NetworkToolTestAccess::Steps(*f.tool);
        auto& cells = NetworkToolTestAccess::Cells(*f.tool);
        steps = {tDraggedStep{0, 2, 0}, tDraggedStep{3, 3, 0}};
        cells = {Point(0, 0), Point(1, 0), Point(2, 0), Point(3, 0)};
        NetworkToolTestAccess::TotalSteps(*f.tool) = 2;
        NetworkToolTestAccess::Anchors(*f.tool) = {Point(0, 0), Point(2, 0)};
        CHECK(f.tool->DoNetworkSpecificPlacement(false, true));
        // the anchors (0,0) and (2,0) get poles; the wire cell (1,0) and the unanchored last cell (3,0) are highlighted
        CHECK(f.host.preview->added.size() == 2);
        CHECK(f.host.preview->added.size() == 2 && f.host.preview->added[0] == Point(1, 0) && f.host.preview->added[1] == Point(3, 0));
        CHECK(f.host.preview->colors[0] == 0x800000ffu);
        CHECK(NetworkToolTestAccess::Unknown98(*f.tool) == false);
        NetworkToolTestAccess::Unknown98(*f.tool) = false;
        f.host.attemptResult = true;
        NetworkToolTestAccess::Unknown98(*f.tool) = true;  // only the update path auto-connects
        CHECK(f.tool->DoNetworkSpecificPlacement(true, true));
        CHECK(f.host.attemptCalls == 2 && f.host.placeNeighborsCalls == 1);  // Attempt runs before the flag test
        NetworkToolTestAccess::Unknown98(*f.tool) = false;

        f.tool->UpdateOnZoneChange(SC4RectU{0, 0, 1, 1}, 0);
        CHECK(NetworkToolTestAccess::Unknown98(*f.tool) == false);
        f.tool->UpdateOnZoneChange(SC4RectU{0, 0, 1, 1}, kNetworkPowerLine);
        CHECK(NetworkToolTestAccess::Unknown98(*f.tool) == true);  // PARTIAL: the flag is all that is recovered
        NetworkToolTestAccess::Unknown98(*f.tool) = false;
    }

    // ---- ClearCellPreviews ----------------------------------------------------------------------------------
    {
        Fixture f;
        FakePole* createdA = f.host.ownedPoles.empty() ? nullptr : f.host.ownedPoles.back();
        auto* a = new FakePole();
        auto* b = new FakePole();
        f.host.ownedPoles.push_back(a);
        f.host.ownedPoles.push_back(b);
        (void)createdA;
        PowerLineToolTestAccess::CreatedPoles(*f.tool).push_back(a);
        PowerLineToolTestAccess::PreviewPoles(*f.tool).push_back(b);
        a->AddRef();  // the occupant manager's reference
        a->AddRef();  // the list's reference
        b->AddRef();  // the list's reference
        auto& list = PowerLineToolTestAccess::ConnectionList(*f.tool);
        auto* c = new FakePole();
        auto* d = new FakePole();
        f.host.ownedPoles.push_back(c);
        f.host.ownedPoles.push_back(d);
        c->AddRef();  // the list's reference
        d->AddRef();
        list.push_back(tIntersectionInfo{c, d, true, true, {}});
        const int clearsBefore = f.host.preview ? f.host.preview->clears : -1;
        f.tool->ClearCellPreviews();
        CHECK(PowerLineToolTestAccess::CreatedPoles(*f.tool).empty());
        CHECK(PowerLineToolTestAccess::PreviewPoles(*f.tool).empty());
        CHECK(PowerLineToolTestAccess::ConnectionList(*f.tool).empty());
        CHECK(a->modelCalls == 1 && b->modelCalls == 1);
        CHECK(a->refs == 0 && b->refs == 0);
        CHECK(f.host.removed.size() == 1 && f.host.removed[0] == a);  // manager remove for created poles
        CHECK(b->previewCalls >= 1);                                  // the second list is marked as preview
        CHECK(f.host.draws.size() == 2);                              // both ends of the connection
        CHECK(f.host.preview && f.host.preview->clears == clearsBefore + 1);   // the base tool's preview is cleared
    }

    // ---- destructor keeps the original's leak of the pole references ----------------------------------------
    {
        FakeHost host;
        SetNetworkToolHost(&host);
        SetPowerLineToolHost(&host);
        auto* tool = new cSC4PowerLineTool();
        tool->AddRef();
        CHECK(tool->Init());
        auto* pole = new FakePole();
        host.ownedPoles.push_back(pole);
        pole->AddRef();  // the list's reference
        PowerLineToolTestAccess::CreatedPoles(*tool).push_back(pole);
        tool->Release();  // deletes the tool
        CHECK(pole->refs == 1);  // quirk: the destructor drops the list without releasing the pole
        SetNetworkToolHost(nullptr);
        SetPowerLineToolHost(nullptr);
    }

    SetNetworkToolHost(nullptr);
    SetPowerLineToolHost(nullptr);
    std::printf(failures == 0 ? "sc4_powerlinetool_test: ok\n" : "sc4_powerlinetool_test: %d failure(s)\n",
                failures);
    return failures == 0 ? 0 : 1;
}
