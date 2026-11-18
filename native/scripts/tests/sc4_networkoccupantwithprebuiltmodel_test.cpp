// cSC4NetworkOccupantWithPreBuiltModel — the pre-built-model network occupant base: constructor state, the shear
// fill rows and their altitude / normal / node transform, the model and support-model instances, the two shadow
// overlays (land / water, including the -2 pending state and the flat-terrain skip), translucency, the version 4
// record, the path info resolution and the IPathNetworkOccupant forwarding.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "sc4/Exemplar.h"
#include "sc4/NetworkOccupantWithPreBuiltModel.h"
#include "sc4/SCProperty.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECK_NEAR(a, b) do { const double va = (a), vb = (b); if (std::fabs(va - vb) > 1e-4) { \
    std::printf("FAIL line %d: %s == %f, expected %f\n", __LINE__, #a, va, vb); ++failures; } } while (0)

using namespace sc4;

// ---------------------------------------------------------------------------------------------------------------
// fakes
// ---------------------------------------------------------------------------------------------------------------
namespace {

struct FakeModel;

// The pre-built model instance: a cIS3DModelInstance with the counters the test inspects.
struct FakeModel final : s3d::cIS3DModelInstance {
    int refs = 0;
    int releaseAtZero = 0;
    int setOwnerCalls = 0;
    int setTransformCalls = 0;
    int setAttributesCalls = 0;
    cIGZUnknown* owner = nullptr;
    cS3DTransform transform;
    cS3DBoundingBox bounds;
    s3d::SC4DrawAttributes attributes;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 0x26a68449) return false;
        *out = this;
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override {
        --refs;
        if (refs <= 0) ++releaseAtZero;
        return refs > 0 ? static_cast<std::uint32_t>(refs) : 0u;
    }
    cIGZUnknown* GetOwner() override { return owner; }
    void SetOwner(cIGZUnknown* value) override { owner = value; ++setOwnerCalls; }
    void* GetCustomData() override { return nullptr; }
    void SetCustomData(void*) override {}
    void GetBounds(cS3DBoundingBox& out) override { out = bounds; }
    void GetWorldBounds(cS3DBoundingBox& out) override { out = bounds; }
    void GetTransform(cS3DTransform& out) override { out = transform; ++out.changeCounter; }
    void SetTransform(const cS3DTransform& value) override { transform = value; ++setTransformCalls; }
    bool Pick(cSC4PickContext*) override { return false; }
    int Draw(SC4DrawContext*, bool) override { return 1; }
    int GetProperties() override { return 0; }
    void SetProperties(int) override {}
    s3d::SC4DrawAttributes GetDrawAttributes() const override { return attributes; }
    void SetDrawAttributes(const s3d::SC4DrawAttributes& value) override { attributes = value; ++setAttributesCalls; }
    void GetSortValue(std::uint32_t& out) const override { out = 0; }
    s3d::cIS3DResModel* GetResModel() const override { return nullptr; }
};

struct FakeOccupantModel final : INetworkOccupantModelInstance {
    FakeModel model;
    bool initResult = true;
    int initCalls = 0, prelightCalls = 0, deformCalls = 0, flipCalls = 0, clipCalls = 0;
    int lastDeformRotation = -1;
    bool lastDeformFlip = false;
    cS3DVector3 lastOffset{}, lastFillA{}, lastFillB{};
    bool lastClipFlag = false;
    cS3DVector3 lastClipLo{}, lastClipHi{};
    s3d::cIS3DModelInstance* ModelInstance() override { return &model; }
    bool Init(s3d::cIS3DResModel*, std::uint32_t) override { ++initCalls; return initResult; }
    void Prelight() override { ++prelightCalls; }
    void Deform(int rotation, bool flip, const cS3DVector3& fillA, const cS3DVector3& fillB,
                const cS3DVector3& offset, bool) override {
        ++deformCalls;
        lastDeformRotation = rotation;
        lastDeformFlip = flip;
        lastFillA = fillA;
        lastFillB = fillB;
        lastOffset = offset;
    }
    void FlipAndLight(bool) override { ++flipCalls; }
    void Clip(const cS3DVector3& lo, const cS3DVector3& hi, bool flag) override {
        ++clipCalls;
        lastClipLo = lo;
        lastClipHi = hi;
        lastClipFlag = flag;
    }
};

struct FakeResModel final : s3d::cIS3DResModel {
    int refs = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { --refs; return refs > 0 ? static_cast<std::uint32_t>(refs) : 0u; }
    void Slot3() override {}
    bool CreateModelInstance(std::uint32_t, s3d::cIS3DModelInstance** out) override { *out = nullptr; return false; }
    void Slot6() override {}
    void ComputeBounds(cS3DBoundingBox&) override {}
};

struct FakeOverlay final : IShadowOverlay {
    int addCalls = 0, removeCalls = 0;
    std::uint32_t nextHandle = 7;
    cGZPersistResourceKey lastKey;
    cS3DVector2 lastPosition;
    float lastHeight = 0, lastRotation = 0;
    bool lastFlip = false;
    std::vector<std::uint32_t> removed;
    std::uint32_t AddShadowDecal(const cGZPersistResourceKey& key, const cS3DVector2& position, float height,
                                 float rotation, bool flip) override {
        ++addCalls;
        lastKey = key;
        lastPosition = position;
        lastHeight = height;
        lastRotation = rotation;
        lastFlip = flip;
        return nextHandle++;
    }
    void RemoveOverlay(std::uint32_t overlayId) override { ++removeCalls; removed.push_back(overlayId); }
};

struct FakePathInfo final : cISC4PathInfo {
    int refs = 0;
    int releaseAtZero = 0;
    bool useElevation = false;
    int cloneCalls = 0, setElevationCalls = 0, createCopyCalls = 0, removeWrongWayCalls = 0;
    int verifyEdgesCalls = 0, initBitmapCalls = 0, offsetCalls = 0;
    cS3DVector3 lastOffset;
    SC4RectFloat32 lastRect;
    IPathNetworkOccupant* lastOccupant = nullptr;
    FakePathInfo* cloneResult = nullptr;
    FakePathInfo* copyResult = nullptr;
    FakePathInfo* offsetResult = nullptr;

    bool QueryInterface(std::uint32_t, void** out) override { *out = this; AddRef(); return true; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override {
        --refs;
        if (refs <= 0) ++releaseAtZero;
        return refs > 0 ? static_cast<std::uint32_t>(refs) : 0u;
    }
    const tPath* GetPath(std::uint32_t, std::uint8_t, std::uint8_t, std::uint8_t, float*) const override { return nullptr; }
    const tPath* GetPath(std::uint32_t, float*) const override { return nullptr; }
    const tPath* GetBestPath(std::uint32_t, std::uint8_t, std::uint8_t, const cS3DVector3&, std::uint32_t*,
                             float*) const override { return nullptr; }
    std::vector<std::uint32_t> GetPathsNearStartPoint(std::uint32_t, const cS3DVector3&, std::uint8_t, std::uint8_t,
                                                      float) const override { return {}; }
    std::vector<std::uint32_t> GetPathsNearEndPoint(std::uint32_t, const cS3DVector3&, std::uint8_t, std::uint8_t,
                                                    float) const override { return {}; }
    const tPath* GetRandomPath(std::uint32_t, std::uint8_t, std::uint8_t, cRZRandomSuper*, float*) const override {
        return nullptr;
    }
    const tPath* GetRandomPathWildcard(std::uint32_t, std::uint8_t, std::uint8_t, cRZRandomSuper*, std::uint32_t*,
                                       float*) const override { return nullptr; }
    float GetPathLength(std::uint32_t, std::uint8_t, std::uint8_t, std::uint8_t) const override { return 0; }
    float GetPathLength(std::uint32_t) const override { return 0; }
    int GetLaneCount(std::uint32_t, std::uint8_t, std::uint8_t) const override { return 0; }
    bool IsIntersection(std::uint32_t) const override { return false; }
    int GetPathsFromEntryEdge(std::uint32_t, std::uint8_t, std::vector<std::uint32_t>&) const override { return 0; }
    int GetPathsToExitEdge(std::uint32_t, std::uint8_t, std::vector<std::uint32_t>&) const override { return 0; }
    bool UseElevation() const override { return useElevation; }
    void SetUseElevationFlag(bool on) override { useElevation = on; ++setElevationCalls; }
    bool GetDestinationPoint(const std::vector<cS3DVector3>*, const cS3DVector3&, cS3DVector3&, int&) const override {
        return false;
    }
    int SnapPointToPath(cS3DVector3&, const std::vector<cS3DVector3>*) const override { return 0; }
    int SnapPointToAnyPath(cS3DVector3&, std::uint32_t, std::uint32_t*) const override { return 0; }
    bool GetDispatchDestination(std::uint32_t, const cS3DVector3&, cS3DVector3&, std::uint32_t*) const override {
        return false;
    }
    bool GetPathMidpoint(const std::vector<cS3DVector3>*, cS3DVector3&) const override { return false; }
    bool GetRandomPathPoint(std::uint32_t, cS3DVector3&) const override { return false; }
    cISC4PathInfo* Clone() override { ++cloneCalls; return cloneResult; }
    cISC4PathInfo* CreateTransformedCopy(IPathNetworkOccupant* occupant) override {
        ++createCopyCalls;
        lastOccupant = occupant;
        return copyResult;
    }
    void RemoveWrongWayPaths(IPathNetworkOccupant*) override { ++removeWrongWayCalls; }
    void InitializeNetworkConnectionBitmap(IPathNetworkOccupant*) override { ++initBitmapCalls; }
    void OffsetBy(float, float, float) override {}
    cISC4PathInfo* OffsetAndScissor(float x, float, float z, const SC4RectFloat32& rect) override {
        ++offsetCalls;
        lastOffset = cS3DVector3{x, 0.0f, z};
        lastRect = rect;
        return offsetResult;
    }
    PathMap* GetPathMap() override { return nullptr; }
    int CopyPaths(std::uint8_t, std::uint32_t, std::uint32_t, bool) override { return 0; }
    void MakeTunnelPaths(IPathNetworkOccupant*, IPathNetworkOccupant*) override {}
    int GetStopPointCount(std::uint32_t, std::uint8_t, std::uint8_t, std::uint8_t) const override { return 0; }
    int GetStopPointCount(std::uint32_t) const override { return 0; }
    bool GetStopPoint(cS3DVector3&, std::uint32_t, std::uint8_t, std::uint8_t, std::uint8_t) const override {
        return false;
    }
    bool GetStopPoint(cS3DVector3&, std::uint32_t) const override { return false; }
    std::uint32_t GetCenterLane(std::uint32_t, std::uint8_t, std::uint8_t) const override { return 0; }
    bool GetJunctionKeys(std::uint32_t, std::uint8_t, std::vector<std::uint32_t>&) const override { return false; }
    bool GetPathIntersection(std::uint32_t, const cS3DVector3&, const cS3DVector3&, float, cS3DVector3&, std::uint32_t&,
                             int&) const override { return false; }
    bool VerifyPathEdges(IPathNetworkOccupant*) override { ++verifyEdgesCalls; return true; }
};

struct FakeNetworkHost final : INetworkOccupantHost {
    void PostMessage(std::uint32_t, cISC4Occupant*, std::uint32_t) override {}
    void RegisterWithNetworkManager(cISC4Occupant*) override {}
    bool IntersectionPieceId(std::uint32_t) override { return false; }
    void ReadPersistRef(cIGZIStream&, std::uint32_t, cIGZUnknown*& out) override { out = nullptr; }
    void WritePersistRef(cIGZOStream&, cIGZUnknown*) override {}
};

struct FakeHost final : IPreBuiltModelOccupantHost {
    // base occupant half
    int drawCalls = 0;
    int baseInstanceCalls = 0;
    FakeModel* baseInstance = nullptr;
    int lightingCalls = 0;
    // lighting / view / render properties
    bool night = false;
    cS3DVector3 sun{0, 1, 0};
    int zoom = 2, rotation = 0;
    float objectRotationAngle = 0.0f;
    float viewRotationAngle = 0.0f;
    std::int32_t shadowViewMode = 3;
    // terrain
    bool cellWater = false;
    float terrainAltitude = 0.0f;
    float waterLevel = 0.0f;
    bool waterSurfaceDisplayed = false;
    bool overlaysAvailable = false;
    FakeOverlay land, water;
    // network manager
    std::uint32_t mappedShadow = 0;
    int mapShadowCalls = 0;
    std::uint32_t lastShadowInstance = 0;
    std::uint8_t lastShadowRotation = 0;
    std::uint32_t lastShadowFlags = 0;
    std::uint32_t lastShadowRaised = 0;
    std::uint32_t mappedPath = 0;
    std::uint32_t lastPathId = 0;
    // model maker / resources
    std::uint32_t modelId = 0;
    int modelIdCalls = 0;
    bool lastModelIdFlag = false;
    int resourceCalls = 0;
    std::vector<FakeOccupantModel*> created;
    bool createInstance = true;
    bool initResult = true;
    FakePathInfo* pathResource = nullptr;
    cGZPersistResourceKey lastPathKey;
    // renderer
    bool hasModelInstance = true;
    int addModelCalls = 0;
    std::vector<int> addedLayers;

    void DrawOccupant(cSC4NetworkOccupant&, SC4DrawContext*, bool) override { ++drawCalls; }
    void GetOccupantModelInstances(cSC4NetworkOccupant&, int, int, s3d::cIS3DModelInstance** out, int,
                                   int& count) override {
        ++baseInstanceCalls;
        if (baseInstance) {
            baseInstance->AddRef();
            out[count] = baseInstance;
            ++count;
        }
    }
    void UpdateOccupantLighting(cSC4NetworkOccupant&) override { ++lightingCalls; }
    bool IsNight() override { return night; }
    void GetSunDirection(cS3DVector3& out) override { out = sun; }
    void GetZoomAndRotation(int& z, int& r) override { z = zoom; r = rotation; }
    float GetObjectYRotationAngle(int rotation) override { return objectRotationAngle * rotation; }
    float ViewRotationAngle() override { return viewRotationAngle; }
    std::int32_t ShadowViewMode() override { return shadowViewMode; }
    bool CellHasWater(std::uint32_t, std::uint32_t) override { return cellWater; }
    float TerrainAltitudeAtVertex(int, int) override { return terrainAltitude; }
    float WaterLevel(float, float) override { return waterLevel; }
    bool WaterSurfaceDisplayed() override { return waterSurfaceDisplayed; }
    bool GetShadowOverlays(IShadowOverlay** l, IShadowOverlay** w) override {
        if (!overlaysAvailable) return false;
        *l = &land;
        *w = &water;
        return true;
    }
    std::uint32_t MapShadowTexture(std::uint32_t instance, std::uint8_t& rotation, std::uint32_t flags,
                                   std::uint32_t raised) override {
        ++mapShadowCalls;
        lastShadowInstance = instance;
        lastShadowRotation = rotation;
        lastShadowFlags = flags;
        lastShadowRaised = raised;
        return mappedShadow;
    }
    std::uint32_t GetModelInstanceID(cISC4Occupant*, int, int, int, bool flag, std::uint32_t*,
                                     std::uint32_t*) override {
        ++modelIdCalls;
        lastModelIdFlag = flag;
        return modelId;
    }
    std::vector<FakeResModel*> resources;
    s3d::cIS3DResModel* CreateModelResource(const cGZPersistResourceKey& key) override {
        ++resourceCalls;
        lastResourceKey = key;
        auto* model = new FakeResModel();
        model->AddRef();
        resources.push_back(model);
        return model;
    }
    INetworkOccupantModelInstance* CreateOccupantModelInstance() override {
        if (!createInstance) return nullptr;
        auto* instance = new FakeOccupantModel();
        instance->initResult = initResult;
        instance->model.transform.scale = 2.5f;          // GetTransform must carry it into the built matrix
        created.push_back(instance);
        return instance;
    }
    bool HasModelInstance(s3d::cIS3DModelInstance*) override { return hasModelInstance; }
    void AddModelInstanceToRenderer(s3d::cIS3DModelInstance*, int layer) override {
        ++addModelCalls;
        addedLayers.push_back(layer);
    }
    cISC4PathInfo* GetPathInfoResource(const cGZPersistResourceKey& key) override {
        lastPathKey = key;
        if (!pathResource) return nullptr;
        pathResource->AddRef();
        return pathResource;
    }
    std::uint32_t MapPath(std::uint32_t pathId) override {
        lastPathId = pathId;
        return mappedPath;
    }
    cGZPersistResourceKey lastResourceKey;
};

}  // namespace

int main() {
    FakeNetworkHost networkHost;
    SetNetworkOccupantHost(&networkHost);
    FakeHost host;
    SetPreBuiltModelOccupantHost(&host);

    auto* occ = new cSC4NetworkOccupantWithPreBuiltModel();
    occ->AddRef();

    // ---- constructor / identity -------------------------------------------------------------------------------
    CHECK(occ->GetGZCLSID() == 0x49c1a034);
    CHECK(occ->GetNetworkFlag() == 0x06400000);          // base 0x06000000 | the constructor's 0x00400000
    CHECK(occ->IsManagedGlobally());
    CHECK(occ->HasViewDependentModel());
    CHECK(occ->ShadowLand() == -1 && occ->ShadowWater() == -1);
    CHECK(occ->LastZoom() == 0xff && occ->LastShadowView() == 0xff);
    CHECK(occ->SupportModelId() == 0 && occ->PreBuiltModel() == nullptr && occ->SupportModel() == nullptr);
    CHECK(occ->ModelOffset().x == 0.0f && occ->ModelOffset().y == 0.0f && occ->ModelOffset().z == 0.0f);

    // ---- QueryInterface -------------------------------------------------------------------------------------
    void* out = nullptr;
    CHECK(occ->QueryInterface(0x49c1a034, &out) && out == occ);
    out = nullptr;
    CHECK(occ->QueryInterface(0xa821ef94, &out) && out == occ);
    out = nullptr;
    CHECK(occ->QueryInterface(0xa9764c8c, &out) && out == occ);
    out = nullptr;
    CHECK(!occ->QueryInterface(0x12345678, &out));       // no +0x68 model instance yet
    FakeModel occupantModel;
    occ->SetModelInstance(&occupantModel);
    CHECK(!occ->QueryInterface(0x12345678, &out));
    out = nullptr;
    CHECK(occ->QueryInterface(0x26a68449, &out) && out == &occupantModel);

    // ---- Init / shear rows ------------------------------------------------------------------------------------
    const float corners[4][3] = {{16, 1, 32}, {32, 2, 32}, {32, 3, 48}, {16, 4, 48}};
    CHECK(occ->Init(corners));
    CHECK(occ->IsInitialized());
    CHECK(occ->ModelStateFlags() == 0x40);
    CHECK(occ->Init());                                  // second Init returns true without redoing the work
    occ->RecomputeShearTransform();
    // mean of corners 0 / 2 = 2, position y = 4 (max corner)
    CHECK_NEAR(occ->FillA().x, 1.0f / 16.0f);
    CHECK_NEAR(occ->FillA().y, 1.0f / 16.0f);
    CHECK_NEAR(occ->FillA().z, -2.0f);
    CHECK_NEAR(occ->FillB().x, 3.0f / 16.0f);
    CHECK_NEAR(occ->FillB().y, -1.0f / 16.0f);
    CHECK_NEAR(occ->FillB().z, -2.0f);
    // the fill plane reproduces the two corners of the diagonal it was built from
    CHECK_NEAR(occ->GetAltitude(24.0f, 40.0f), 2.0f);
    CHECK_NEAR(occ->GetAltitude(16.0f, 32.0f), 1.0f);
    CHECK_NEAR(occ->GetAltitude(32.0f, 48.0f), 3.0f);
    float normal[3];
    occ->GetNormal(24.0f, 40.0f, normal);
    CHECK_NEAR(normal[0], -3.0f / 16.0f);
    CHECK_NEAR(normal[1], 1.0f);
    CHECK_NEAR(normal[2], 1.0f / 16.0f);
    // the diagonal flag swaps the fill rows (0x1000000)
    occ->SetNetworkFlag(0x01000000);
    occ->RecomputeShearTransform();
    CHECK_NEAR(occ->FillA().x, 3.0f / 16.0f);
    CHECK_NEAR(occ->FillA().y, 1.0f / 16.0f);
    CHECK_NEAR(occ->FillB().x, 1.0f / 16.0f);
    CHECK_NEAR(occ->FillB().y, -1.0f / 16.0f);
    occ->ClearNetworkFlag(0x01000000);
    occ->RecomputeShearTransform();

    // ---- TransformNode (path nodes are tile-local, so the point is relative to the piece) ---------------------
    cS3DVector3 node{8.0f, 10.0f, 0.0f};
    occ->TransformNode(node);                            // rotation 0, no flip, no path override
    CHECK_NEAR(node.y, 9.5f);                            // row B: 10 + 8(3/16) - 2
    node = cS3DVector3{-8.0f, 10.0f, 0.0f};
    occ->TransformNode(node);
    CHECK_NEAR(node.y, 7.5f);                            // row A: 10 - 8(1/16) - 2
    occ->SetRotationAndFlip(1);                          // quarter turn: (x, z) -> (-z, x)
    node = cS3DVector3{8.0f, 10.0f, 0.0f};
    occ->TransformNode(node);
    CHECK_NEAR(node.y, 8.5f);                            // rx = 0, rz = 8; row A: 10 + 8/16 - 2
    occ->SetRotationAndFlip(0x81);                       // quarter turn + flip
    node = cS3DVector3{0.0f, 10.0f, 8.0f};
    occ->TransformNode(node);
    CHECK_NEAR(node.y, 9.5f);                            // rx = 8, rz = 0; row B: 10 + 8(3/16) - 2
    occ->SetRotationAndFlip(0);
    occ->SetPathOffset(3.0f, 5.0f);
    occ->SetConnectionPathIDOverride(77);
    CHECK(occ->GetPathIDOverride() == 77);
    node = cS3DVector3{0.0f, 0.0f, 0.0f};
    occ->TransformNode(node);
    CHECK_NEAR(node.y, -1.5f);                           // rx 3, rz 5; row A: 5/16 + 3/16 - 2
    occ->SetConnectionPathIDOverride(0);
    occ->SetPathOffset(0.0f, 0.0f);
    CHECK(occ->GetPathIDOverride() == 0);

    // ---- support model ---------------------------------------------------------------------------------------
    CHECK(!occ->IsSupportPylonPresent());
    occ->AddSupportModel(123, 1.5f, cS3DVector3{1.0f, 2.0f, 3.0f});
    CHECK(occ->IsSupportPylonPresent());
    CHECK(occ->SupportModelId() == 123);
    CHECK_NEAR(occ->SupportRotation(), 1.5f);
    CHECK_NEAR(occ->SupportOffset().y, 2.0f);

    // ---- AddModel ---------------------------------------------------------------------------------------------
    auto* exemplar = new cSCResExemplar();
    exemplar->AddRef();
    exemplar->SetKey(cGZPersistResourceKey{0x496678fe, 0x296678f7, 0x2c});
    const float modelPosition[3] = {4.0f, 5.0f, 6.0f};
    // the base class hands its own +0x68 instance to the union; ComputeBoundingBox must skip it but release it
    host.baseInstance = &occupantModel;
    const int baseRefsBefore = occupantModel.refs;
    occ->AddModel(exemplar, modelPosition, 2);
    CHECK(occ->GetRotation() == 2);
    CHECK_NEAR(occ->ModelOffset().x, 4.0f);
    CHECK_NEAR(occ->ModelOffset().z, 6.0f);
    CHECK(occ->AsPropertyHolder()->HasProperty(0x4835c6a6));
    CHECK(occupantModel.setOwnerCalls == 0);             // excluded instance: released without clearing the owner
    CHECK(occupantModel.refs == baseRefsBefore);         // added by the base list, released by the union
    float lo[3], hi[3];
    occ->GetBoundingBox(lo, hi);
    CHECK_NEAR(lo[0], 16.0f);
    CHECK_NEAR(hi[2], 48.0f);
    // quirk (0x1001baa63): with no model instance at all the output box is left untouched
    host.baseInstance = nullptr;
    float untouchedLo[3] = {123.0f, 123.0f, 123.0f};
    float untouchedHi[3] = {-123.0f, -123.0f, -123.0f};
    occ->ComputeBoundingBox(untouchedLo, untouchedHi);
    CHECK(untouchedLo[0] == 123.0f && untouchedLo[2] == 123.0f);
    CHECK(untouchedHi[0] == -123.0f && untouchedHi[2] == -123.0f);

    // ---- shadows: land ---------------------------------------------------------------------------------------
    occ->SetRotationAndFlip(0);
    occ->cSC4Occupant::SetAllFlags(5);                   // placed (1) + visible (4), without a shadow refresh
    host.overlaysAvailable = true;
    host.night = false;
    host.shadowViewMode = 3;
    host.mappedShadow = 0x1234;
    host.cellWater = false;
    host.sun = cS3DVector3{1.0f, 2.0f, 3.0f};
    host.zoom = 2;
    occ->UpdateShadow(true);
    CHECK(host.mapShadowCalls == 1);
    CHECK(host.lastShadowInstance == 2);                 // piece id 0 + zoom 2
    CHECK(occ->ShadowLand() == 7);
    CHECK(occ->ShadowWater() == -2);
    CHECK(host.land.addCalls == 1 && host.water.addCalls == 0);
    CHECK(host.land.lastKey.type == 0x7ab50e44 && host.land.lastKey.group == 0x2bc2759a);
    CHECK(host.land.lastKey.instance == 0x1234);
    // quirk: the sun direction is multiplied by -0.0f, so the both offsets (and the division by sun.y) are zero
    CHECK_NEAR(host.land.lastPosition.x, 24.0f);         // 16 + 8
    CHECK_NEAR(host.land.lastPosition.y, 40.0f);         // 32 + 8
    CHECK_NEAR(host.land.lastHeight, 16.0f);
    CHECK_NEAR(host.land.lastRotation, 0.0f);
    CHECK(!host.land.lastFlip);
    // hiding the occupant removes the overlay, the pending water handle resolves to -1
    occ->SetAllFlags(0);
    CHECK(occ->ShadowLand() == -1 && occ->ShadowWater() == -1);
    CHECK(host.land.removeCalls == 1 && host.land.removed[0] == 7);

    // ---- shadows: water --------------------------------------------------------------------------------------
    host.cellWater = true;
    occ->cSC4Occupant::SetAllFlags(5);
    occ->UpdateShadow(true);
    CHECK(occ->ShadowWater() == 7 && occ->ShadowLand() == -2);   // the water layer has its own handle counter
    CHECK(host.water.addCalls == 1);
    CHECK(host.water.lastKey.instance == 0x1234);

    // ---- shadows: flat terrain skip (flag 0x40000) -----------------------------------------------------------
    occ->SetNetworkFlag(0x00040000);
    host.cellWater = false;
    host.terrainAltitude = 1.0f;                          // corner deltas 0, 1, 2, 3 -> flat enough
    const int landAdds = host.land.addCalls, waterAdds = host.water.addCalls;
    occ->UpdateShadow(true);
    CHECK(occ->ShadowLand() == -2 && occ->ShadowWater() == -2);
    CHECK(host.land.addCalls == landAdds && host.water.addCalls == waterAdds);
    CHECK(host.water.removeCalls == 1);                   // the water overlay of the previous check is gone
    occ->ClearNetworkFlag(0x00040000);

    // ---- translucency ----------------------------------------------------------------------------------------
    auto* model = static_cast<FakeModel*>(occ->PreBuiltModel());
    CHECK(model == nullptr);
    // put a model in place through GetModelInstances below; translucency is checked after it

    // ---- GetModelInstances -----------------------------------------------------------------------------------
    host.baseInstance = nullptr;
    host.modelId = 0x999;
    host.createInstance = true;
    host.initResult = true;
    host.objectRotationAngle = -1.5707964f;
    host.mappedShadow = 0;
    host.overlaysAvailable = false;                       // keep UpdateShadow quiet
    s3d::cIS3DModelInstance* instances[10] = {};
    int count = 0;
    occ->SetRotationAndFlip(1);
    occ->GetModelInstances(0, 0, instances, 10, count);
    CHECK(count == 2);                                    // pre-built model + support model
    CHECK(host.created.size() == 2);
    CHECK(host.modelIdCalls >= 2 && host.lastModelIdFlag);   // AddModel's bounds pass already asked once
    CHECK(host.lastResourceKey.type == 0x5ad0e817 && host.lastResourceKey.group == 0xbadb57f1);
    CHECK(host.created[0]->initCalls == 1);
    CHECK(host.created[0]->prelightCalls == 1 && host.created[0]->deformCalls == 1);
    CHECK(host.created[0]->flipCalls == 1 && host.created[0]->clipCalls == 0);
    CHECK(host.created[0]->lastDeformRotation == 1);
    CHECK_NEAR(host.created[0]->lastFillA.x, 1.0f / 16.0f);
    CHECK(host.created[0]->model.setTransformCalls == 1);
    CHECK_NEAR(host.created[0]->model.transform.translation[0], 4.0f + 24.0f);
    CHECK_NEAR(host.created[0]->model.transform.translation[1], 5.0f + 4.0f);
    CHECK_NEAR(host.created[0]->model.transform.translation[2], 6.0f + 40.0f);
    CHECK_NEAR(host.created[0]->model.transform.rotation[0][0], 0.0f);
    CHECK_NEAR(host.created[0]->model.transform.rotation[0][2], -1.0f);
    CHECK_NEAR(host.created[0]->model.transform.scale, 2.5f);   // copied from GetTransform, not forced to 1
    CHECK(host.created[0]->model.setOwnerCalls >= 1);
    CHECK(occ->PreBuiltModel() == &host.created[0]->model);
    CHECK(occ->SupportModel() == &host.created[1]->model);
    CHECK(host.created[0]->model.refs == 2);             // held by +0x220 and by the output array
    CHECK(host.created[1]->model.refs == 2);
    CHECK(occ->LastZoom() == 0);
    // the array entries are counted references
    instances[0]->Release();
    instances[1]->Release();
    CHECK(host.created[0]->model.refs == 1);
    CHECK(host.created[1]->model.refs == 1);

    // ---- translucency with the model in place ----------------------------------------------------------------
    occ->SetTranslucency(true, 128);
    CHECK(host.created[0]->model.attributes.flags & 2u);
    CHECK_NEAR(host.created[0]->model.attributes.shade[3], 128.0f / 255.0f);
    CHECK(host.addModelCalls == 2 && host.addedLayers[0] == 5 && host.addedLayers[1] == 5);
    occ->SetTranslucency(false, 255);
    CHECK(!(host.created[0]->model.attributes.flags & 2u));
    CHECK_NEAR(host.created[0]->model.attributes.shade[3], 1.0f);

    // ---- Draw ------------------------------------------------------------------------------------------------
    host.shadowViewMode = 3;
    occ->Draw(nullptr, false);
    CHECK(host.drawCalls == 1);
    CHECK(occ->LastShadowView() == 3);
    occ->Draw(nullptr, false);
    CHECK(host.drawCalls == 2);

    // ---- record round trip -----------------------------------------------------------------------------------
    occ->SetShadowOverride(0x4321, 0x81);
    PortMemoryOStream record;
    CHECK(occ->Write(record));
    PortMemoryIStream input(record.bytes().data(), record.bytes().size());
    auto* loaded = new cSC4NetworkOccupantWithPreBuiltModel();
    loaded->AddRef();
    CHECK(loaded->Read(input));
    CHECK(loaded->ShadowPieceId() == 0x4321 && loaded->ShadowRotation() == 0x81);
    CHECK(loaded->SupportModelId() == 123);
    CHECK_NEAR(loaded->SupportRotation(), 1.5f);
    CHECK_NEAR(loaded->ModelOffset().x, 4.0f);
    CHECK_NEAR(loaded->SupportOffset().z, 3.0f);
    // a version-2 record is rejected before the base record is read
    {
        PortMemoryOStream bad;
        bad.SetUint16(2);
        PortMemoryIStream badInput(bad.bytes().data(), bad.bytes().size());
        CHECK(!loaded->Read(badInput));
    }
    loaded->Release();

    // ---- path info -------------------------------------------------------------------------------------------
    FakePathInfo resource, clone, copy, offsetInfo;
    resource.AddRef();
    clone.AddRef();
    copy.AddRef();
    offsetInfo.AddRef();
    resource.cloneResult = &clone;
    resource.copyResult = &copy;
    resource.offsetResult = &offsetInfo;
    clone.copyResult = &copy;
    clone.offsetResult = &offsetInfo;
    copy.offsetResult = &offsetInfo;
    host.pathResource = &resource;
    host.mappedPath = 0xBEEF;
    CHECK(occ->GetPathInfo() == &copy);
    CHECK(resource.cloneCalls == 1);
    CHECK(clone.setElevationCalls == 1);
    CHECK(clone.releaseAtZero == 1);
    CHECK(clone.createCopyCalls == 1);
    CHECK(copy.verifyEdgesCalls == 1 && copy.initBitmapCalls == 1);
    CHECK(host.lastPathId == 0x2c);                       // no override: the exemplar key's instance part
    CHECK(host.lastPathKey.type == 0x296678f7 && host.lastPathKey.group == 0xa966883f);
    CHECK_NEAR(static_cast<double>(host.lastPathKey.instance), 0xBEEF);
    occ->AddPathConnection(1, 0, 2);
    CHECK(occ->PathConnections(1, 0) != 0);
    occ->ClearCachedPathInfo();
    CHECK(occ->CachedPathInfo() == nullptr);
    host.pathResource = nullptr;
    CHECK(occ->GetPathInfo() == nullptr);

    // path override: OffsetAndScissor gets the path origin and the corner rectangle
    occ->SetPathOffset(1.5f, -2.5f);
    occ->SetConnectionPathIDOverride(99);
    resource.useElevation = true;                         // skip the clone on the second resolution
    resource.refs = 1;
    copy.refs = 1;
    offsetInfo.refs = 1;
    host.pathResource = &resource;
    CHECK(occ->GetPathInfo() == &offsetInfo);
    CHECK(copy.offsetCalls == 1);
    CHECK_NEAR(copy.lastOffset.x, 1.5f);
    CHECK_NEAR(copy.lastOffset.z, -2.5f);
    CHECK_NEAR(copy.lastRect.x0, 16.0f);
    CHECK_NEAR(copy.lastRect.z0, 32.0f);
    CHECK_NEAR(copy.lastRect.x1, 32.0f);
    CHECK_NEAR(copy.lastRect.z1, 48.0f);
    CHECK_NEAR(static_cast<double>(host.lastPathId), 99.0);
    CHECK(occ->PathConnections(1, 0) == 0);               // the verified path info clears the connection bitmap
    occ->SetConnectionPathIDOverride(0);
    occ->SetPathOffset(0.0f, 0.0f);
    occ->ClearCachedPathInfo();

    // ---- SetVisibility / SetAllFlags / UpdateLighting --------------------------------------------------------
    occ->SetVisibility(true, false);
    CHECK(occ->IsFlagSet(4));
    occ->UpdateLighting(nullptr);
    CHECK(host.lightingCalls == 1);

    // ---- Shutdown --------------------------------------------------------------------------------------------
    CHECK(occ->Shutdown());
    CHECK(!occ->IsInitialized());
    CHECK(occ->PreBuiltModel() == nullptr && occ->SupportModel() == nullptr);
    CHECK(host.created[0]->model.refs == 0);
    CHECK(host.created[1]->model.refs == 0);
    // clear the exemplar reference before the objects go away
    static_cast<cSCExemplarPropertyHolder*>(occ->AsPropertyHolder())->SetDefaultExemplar(nullptr);
    exemplar->Release();
    occ->Release();
    for (FakeOccupantModel* created : host.created) delete created;
    for (FakeResModel* resource : host.resources) delete resource;
    SetPreBuiltModelOccupantHost(nullptr);
    SetNetworkOccupantHost(nullptr);
    std::puts(failures ? "sc4_networkoccupantwithprebuiltmodel: FAILED"
                       : "sc4_networkoccupantwithprebuiltmodel ok");
    return failures != 0;
}
