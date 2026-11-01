// cSTETerrainView3D — the 3D terrain view: it owns the render-side copy of the terrain (a surface point per vertex,
// a water point per vertex, four city-edge "cliff" strips with three strata layers each, four water-box strips),
// the per-row frustum culling tables, the per-row texture-span lists, the "level cell" quads (cells flattened for
// building lots) and the selection / ripple / wave / overlay helpers. Recovered from Sim City 4 Deluxe Editionsub
// 1.2.1 (class group 0x100aeab80, functions 0x10074607c..0x10075043a).
//
// The original keeps almost everything in class statics (one view per game); the same layout lives in `Shared`
// here so tests can reset it with StaticShutdown().
//
// Vertex layouts (all little endian, byte offsets as in the binary):
//   SurfacePoint (32 B): +0 x, +4 y, +8 z, +0xc diffuse (bytes b,g,r,a), +0x10 u, +0x14 v, +0x18 u2, +0x1c v2.
//   BorderPoint  (24 B): +0 x, +4 y, +8 z, +0xc diffuse, +0x10 u, +0x14 v.
//   WaterBoxPoint(16 B): +0 x, +4 y, +8 z, +0xc diffuse.
//   CellQuad    (128 B): four SurfacePoints (corners (x,z) (x,z+1) (x+1,z+1) (x+1,z)).
// Terrain vertex surface bytes (cSTETerrain::Vertex::surface, offsets from vertex +0x10): +0..2 colour B, +3..5
// colour A, three texture layers at +6/+0xa/+0xe = u16 texture key, +8/+0xc/+0x10 = u8 blend alpha.
//
// Engine services that are not recovered yet (draw context, lighting manager, texture factory, overlay managers,
// plumbing/network queries, tuning exemplars) are reached through ITerrainViewHost / ITerrainDrawContext. Every
// host method has a harmless default so a test only overrides what it needs.
#pragma once
#include <array>
#include <cstdint>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "gz/cIGZSerializable.h"
#include "sc4/cSTETerrain.h"

struct cSC4PackedRenderState;

namespace sc4 {

// ---------------------------------------------------------------------------------------------------------------
// Render-side data
// ---------------------------------------------------------------------------------------------------------------
struct TerrainSurfacePoint {
    float x = 0, y = 0, z = 0;
    std::uint8_t diffuse[4] = {0, 0, 0, 0};
    float u = 0, v = 0, u2 = 0, v2 = 0;
};
static_assert(sizeof(TerrainSurfacePoint) == 32);

struct TerrainBorderPoint {
    float x = 0, y = 0, z = 0;
    std::uint8_t diffuse[4] = {0, 0, 0, 0};
    float u = 0, v = 0;
};
static_assert(sizeof(TerrainBorderPoint) == 24);

struct TerrainWaterBoxPoint {
    float x = 0, y = 0, z = 0;
    std::uint8_t diffuse[4] = {0, 0, 0, 0};
};
static_assert(sizeof(TerrainWaterBoxPoint) == 16);

struct TerrainCellQuad { TerrainSurfacePoint corner[4]; };
static_assert(sizeof(TerrainCellQuad) == 128);

struct TerrainLevelCellInfo { std::uint32_t column = 0; float height = 0; };
static_assert(sizeof(TerrainLevelCellInfo) == 8);

struct TerrainSpan { std::int32_t first = 0, last = 0; };                       // inclusive cell columns
struct TerrainTextureSpanInfo {                                                   // all spans of one texture key in a row
    std::uint32_t textureKey = 0;
    std::list<TerrainSpan> spans;
};
struct TerrainRenderTexInfo {                                                     // spans to render for one row
    std::int32_t row = 0;
    std::list<TerrainSpan> spans;
};

// Per-frustum culling tables (the original keeps two copies at +0xe0..0x108 and +0x110..0x138).
struct TerrainFrustumCull {
    std::int32_t rowBegin = 0, rowEnd = 0, rowLast = 0;                           // +0xe0 +0xe4 +0xe8
    std::int32_t terrainRowBegin = 0, terrainRowEnd = 0, terrainRowLast = 0;      // +0xec +0xf0 +0xf4
    std::int32_t waterRowBegin = 0, waterRowEnd = 0, waterRowLast = 0;            // +0xf8 +0xfc +0x100
    std::int32_t colMin = 0, colMax = 0;                                          // +0x104 +0x108
    std::vector<std::int32_t> colStart, colEnd;
    std::vector<std::uint8_t> terrainVisible, waterVisible;
};

// What the terrain code reads from the draw context's view-utilities block (SC4DrawContext +0x28).
struct TerrainFrustumPlanes {
    float minZ = 0, maxZ = 0;                       // world z extent of the frustum (+0x264/+0x270 draw, +0xa0/+0xac view)
    float leftOrigin[2] = {0, 0}, leftDir[2] = {0, 0};     // +0x3c4/+0x3cc draw, +0x210/+0x218 view
    float rightOrigin[2] = {0, 0}, rightDir[2] = {0, 0};   // +0x3d4/+0x3dc draw, +0x220/+0x228 view
    float planeA = 0, planeB = 1, planeC = 0, planeD = 0;  // eye plane used for the minimum visible height
};

// ---------------------------------------------------------------------------------------------------------------
// Host seams
// ---------------------------------------------------------------------------------------------------------------
struct TerrainTextureBinding { std::vector<int> ids; };                          // GL ids per sub texture
struct IPixelBuffer {                                                             // gimex buffer used as gradient / mask
    virtual ~IPixelBuffer() = default;
    virtual int Width() const = 0;
    virtual int Height() const = 0;
    virtual std::uint32_t Pixel(int x, int y) const = 0;                          // 0x00RRGGBB
};
struct TerrainViewMessage {
    std::uint32_t type = 0, data1 = 0, data2 = 0;
};

class ITerrainSelection {                                                         // cSTESelectedTerrainArea
public:
    virtual ~ITerrainSelection() = default;
    virtual void ClearCurrentSelections() {}
    virtual void Shutdown() {}
    virtual bool MarkSelected(const std::vector<std::pair<int, int>>&, int, bool) { return false; }
    virtual bool MarkSelected(const TerrainRect&, int, bool) { return false; }
    virtual bool MarkSelected(std::pair<int, int>, int, bool) { return false; }
    virtual bool MarkSelected(std::pair<int, int>, std::uint64_t, std::uint32_t, std::uint8_t, bool, int, std::uint8_t, bool) { return false; }
};

class ITerrainVertBufExtn;
class ITerrainViewHost;

class ITerrainDrawContext {                                                       // SC4DrawContext
public:
    virtual ~ITerrainDrawContext() = default;
    virtual int Zoom() const { return 5; }                                        // +0x14
    virtual int Rotation() const { return 0; }                                    // +0x18
    virtual int DepthOffset() const { return 0; }                                 // +0x114
    virtual int Pass() const { return 0; }                                        // +0x80 (0 terrain, 1 water)
    virtual bool DrawEnabled() const { return true; }                             // render properties +0x3f8
    virtual bool GridSuppressed() const { return false; }                         // render properties +0x738
    virtual const TerrainFrustumPlanes& DrawFrustum() const = 0;
    virtual const TerrainFrustumPlanes& ViewFrustum() const = 0;
    virtual void GetVisibleEdges(int& edgeA, int& edgeB) const { edgeA = 0; edgeB = 1; }   // SC4ViewUtilities::GetVisibleEdges
    // render state
    virtual void SetRenderState(const cSC4PackedRenderState&) {}
    virtual void SetTexture(int, int /*stage*/) {}
    virtual void SetTexFiltering(int, int, int) {}
    virtual void SetTexWrapModes(int, int, int) {}
    virtual void SetTexEnvMode(int, int) {}
    virtual void SetTexTransform4(const float /*m*/[16], int) {}
    virtual void ClearTexTransform(int) {}
    virtual void ResetModelViewTransform() {}
    virtual void GetModelViewMatrix(float m[16]) const { for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.0f : 0.0f; }
    virtual void SetDepthOffset(int) {}
    virtual void SetDepthFunc(int) {}
    virtual void SetBlendFunc(int, int) {}
    virtual void EnableTextureStateFlag(bool, int) {}
    virtual void EnableBlendStateFlag(bool) {}
    virtual void EnableCullFaceFlag(bool) {}
    virtual void EnableDepthMaskFlag(bool) {}
    // geometry
    virtual void SetVertexBuffer(int /*format*/, int /*count*/, const void*) {}
    virtual void SetIndexBuffer(int /*count*/, const std::uint16_t*) {}
    virtual void DrawPrimsIndexed(int, int /*firstIndex*/, int /*indexCount*/) {}
    virtual void DrawPrims(int /*mode*/, int /*format*/, int /*count*/, const void*) {}
    virtual void DriverSetState(int /*which*/, int /*value*/) {}                  // driver vslot 0x110 (called (0,1) .. (1,1) around the grid)
    virtual ITerrainVertBufExtn* GetVertexBufferExtension() { return nullptr; }   // cSTEVertBufExtn::GetVertexBufferExtension(driver)
    virtual void ReleaseVertexBufferExtension() {}
};

// cSTEVertBufExtn: the OpenGL vertex-buffer extension fast path (CreateBuffer / DrawPrimsIndexedUsingVertBuf ...).
class ITerrainVertBufExtn {
public:
    virtual ~ITerrainVertBufExtn() = default;
    virtual bool CreateBuffer() { return false; }
    virtual void SetSourceVertexData(const void*) {}
    virtual void SetIndexBuffer(const std::uint16_t*, bool) {}
    virtual void DrawPrimsIndexedUsingVertBuf(int /*row*/, int /*firstCell*/, int /*lastCell*/) {}
    virtual void FlushBuffer() {}
};

class ITerrainViewHost {
public:
    virtual ~ITerrainViewHost() = default;
    // lighting manager (SL::spLightingManager)
    virtual bool HasLighting() const { return false; }
    virtual void GetNormalLight(const float /*normal*/[3], float rgba[4]) const { rgba[0] = rgba[1] = rgba[2] = rgba[3] = 1.0f; }
    virtual void GetPointLight(const float /*pos*/[3], const float /*normal*/[3], std::uint8_t out[3]) const { out[0] = out[1] = out[2] = 0xff; }
    virtual void ApplyTerrainLighting(const TerrainRect&) {}                      // cSTETerrain::DoLighting
    // texture binding factory
    virtual std::shared_ptr<TerrainTextureBinding> LoadTerrainTexture(std::uint32_t /*instance*/) { return nullptr; }
    virtual std::shared_ptr<TerrainTextureBinding> LoadWaterTexture(int /*zoom*/, bool /*polluted*/) { return nullptr; }
    virtual std::shared_ptr<TerrainTextureBinding> LoadGridTexture() { return nullptr; }
    virtual std::shared_ptr<TerrainTextureBinding> LoadLotTexture() { return nullptr; }
    virtual const std::vector<std::uint32_t>& TexturesUsedInTerrainType(int /*terrainType*/) const { static const std::vector<std::uint32_t> none; return none; }
    virtual std::shared_ptr<IPixelBuffer> GetBuffer(std::uint32_t /*instance*/, int /*colorType*/) { return nullptr; }
    // tuning exemplar (0x6534284a/0x88cd66e9 group, id 0xa52160f5); returns false when the property is absent
    virtual bool ReadTuningFloat(std::uint32_t /*propertyId*/, float&) { return false; }
    virtual bool ReadTuningBool(std::uint32_t /*propertyId*/, bool&) { return false; }
    virtual bool ReadTuningFloatVector(std::uint32_t /*propertyId*/, std::vector<float>&) { return false; }
    virtual bool ReadPlumbingColor(std::uint32_t /*propertyId*/, std::uint32_t&) { return false; }
    virtual bool HasTuningExemplar() { return false; }
    // services
    virtual void AddRenderLayer(const void* /*object*/, int /*layerType*/, int /*priority*/) {}
    virtual void RemoveRenderLayer(const void* /*object*/) {}
    virtual void RegisterMessage(std::uint32_t) {}
    virtual void UnregisterMessage(std::uint32_t) {}
    virtual bool PreferenceDisplayGrid() const { return false; }
    virtual void StorePreferenceDisplayGrid(bool) {}
    virtual void RedisplayTerrain(const TerrainRect&) {}                          // cSTETerrain::RedisplayTerrain(rect)
    // overlay managers / helpers created in Init (null = skipped)
    virtual std::shared_ptr<ITerrainSelection> CreateSelection(cSTETerrain&) { return nullptr; }
    // plumbing overlay (cSC4PlumbingSimulator / network / occupants)
    virtual std::uint8_t PlumbingCellFlags(int /*x*/, int /*z*/) const { return 0; }               // plumbing slot 0x38
    virtual bool PlumbingQuery58(int, int) const { return false; }                                  // plumbing slot 0x58
    virtual bool PlumbingQuery40(int, int) const { return false; }                                  // plumbing slot 0x40
    virtual bool NetworkHiddenTriangles(int, int, bool& /*flipped*/, bool& /*evenHidden*/, bool& /*oddHidden*/) const { return false; }
    virtual bool CellHasOccupantType(int, int, std::uint32_t /*typeId*/, bool& /*listOk*/) const { return false; }
    // misc
    virtual float ColorToLuminance(float r, float g, float b) const { return 0.299f * r + 0.587f * g + 0.114f * b; }
    virtual bool GetBoolPlumbingDisplay() const { return false; }                 // cSTETerrain::sbDisplayPlumbingInfo
    virtual bool RenderFlag738() const { return false; }                          // render properties +0x738 (also gates the grid)
    virtual bool WaterSurfaceDisplayState() const { return true; }                // cSTETerrain::GetWaterSurfaceDisplayState
};

// ---------------------------------------------------------------------------------------------------------------
class cSTETerrainView3D : public cIGZSerializable {
public:
    static constexpr std::uint32_t kGZCLSID = 0xc9b84e10;
    static constexpr int kZoomLevels = 5;

    // Statics of the original class.
    struct Shared {
        bool inited = false;
        bool displayGrid = false;
        int curZoom = 5, curRotation = 4, curTerrainType = -1;
        int textureZoom = 5, textureRotation = 4;
        std::uint32_t cellCountX = 0, cellCountZ = 0, vertexCountX = 0, vertexCountZ = 0, numVertices = 0;
        float citySizeX = 0, citySizeZ = 0;
        int vertStart[4] = {0, 0, 0, 0};          // first vertex of each city-edge strip
        int vertIncrement[4] = {0, 0, 0, 0};
        std::vector<TerrainSurfacePoint> points, waterPoints;
        std::vector<std::uint16_t> triangleIndices, flippedTriangleIndices, levelCellQuadTriIndices, allTriangleIndices;
        std::vector<std::uint16_t> allLevelCellIndices;
        std::uint32_t numTriangleIndicesPerRow = 0;
        std::vector<std::vector<TerrainCellQuad>> levelCellQuads;
        std::vector<std::vector<TerrainLevelCellInfo>> levelCellInfos;
        std::uint16_t triQuadIndices[6] = {0, 2, 3, 0, 3, 1};
        std::uint16_t flippedTriQuadIndices[6] = {0, 2, 1, 1, 2, 3};
        std::vector<TerrainBorderPoint> borderPoints[4][3];
        std::vector<TerrainWaterBoxPoint> waterBoxPoints[4];
        TerrainFrustumCull draw, view;             // the original keeps the row tables in statics (sDrawColStart ...)
        std::vector<std::list<TerrainTextureSpanInfo>> textureSpansForRows;
        std::vector<bool> texSpanInfoDirty;
        std::array<std::vector<std::shared_ptr<TerrainTextureBinding>>, 5> terrainTextureBinding;
        std::array<bool, 5> terrainTexturesLoaded = {false, false, false, false, false};
        std::array<std::shared_ptr<TerrainTextureBinding>, 4> waterSurfaceTexture, pollutedWaterSurfaceTexture;
        std::shared_ptr<TerrainTextureBinding> lotTextureBinding;
        std::shared_ptr<IPixelBuffer> waterBoxGradient, waterColorDepthGradient, waterGlareMaskBuf;
        std::uint32_t waterBoxGradientHt = 0, waterColorDepthGradientHt = 0;
        // tuning
        float terrainTexTilingFactor = 1.0f;
        float waterDepthForMaxAlpha = 0, waterMinAlpha = 0;
        float waterDepthToColorGradientFactor = 0.72f, cityEdgeBrightnessKludgeFactor = 0.7f;
        bool displayWaterSurface = true, displayTerrainSurface = true;
        float waterNightColorAdjustment[3] = {1, 1, 1};
        std::vector<float> meanCityEdgeLayerHeights = {0, 0};
        float cityEdgeTexTilingFactors[2] = {1, 1};
        float cityEdgeLayerHeights[2][0x101] = {};
        float cityEdgeV = 0.015625f;                // DAT_100b729e0
        ITerrainViewHost* host = nullptr;
    };
    static Shared& S();

    explicit cSTETerrainView3D(ITerrainViewHost* host = nullptr);
    virtual ~cSTETerrainView3D();

    // ---- lifetime -----------------------------------------------------------------------------------------
    static bool StaticInit(ITerrainViewHost* host);
    static bool StaticShutdown();
    bool Init(cSTETerrain* terrain);
    void Shutdown();
    bool IsInitialized() const { return initialized_; }
    std::uint32_t GetGZCLSID() const override { return kGZCLSID; }
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    // QueryInterface ids: 0xc989f960 / 0xc9b84e10 this, 0x6771477d / 0x452294aa / 0xe4fda3d4 secondary bases.
    bool QueryInterface(std::uint32_t iid, void** out) override;

    // ---- terrain size / data structures ----------------------------------------------------------------------
    static void SetTerrainSize(std::uint32_t cellsX, std::uint32_t cellsZ);
    static void CreateTerrainSizeSpecificDataStructures();
    static void DeleteTerrainSizeSpecificDataStructures();
    static void InitTerrainSurfaceItems();
    static void InitWaterSurfaceItems();
    void ObtainHeightFieldInfo(const TerrainRect& r);
    void ObtainWaterSurfaceInfo(const TerrainRect& r);
    static void UpdateBorderHeight(int side, int index, float height, float seaLevel);
    static void ReadTuningParameters();
    static void InitializeCityEdgeLayerHeights();
    static void ReadPlumbingTuningData(std::uint32_t colors[4], bool& showAll);
    static std::shared_ptr<IPixelBuffer> GetBuffer(std::uint32_t instance, int colorType);

    // ---- texture spans ------------------------------------------------------------------------------------
    static bool AddSpansCorrespondingToTexture(int row, std::uint32_t key, std::vector<TerrainRenderTexInfo>& out);
    static bool AddSpansThatNeedToBeRendered(int row, const TerrainTextureSpanInfo& info, std::vector<TerrainRenderTexInfo>& out);
    static void AddTextureSpan(std::list<TerrainTextureSpanInfo>& row, std::uint32_t key, int firstCell, int lastCell);
    static void MergeTextureSpan(TerrainTextureSpanInfo& info, const TerrainSpan& span);
    void ComputeTextureSpansForRow(int row);
    static void MarkTextureSpanInfoDirty(int firstRow, int lastRow);
    static void GetNextTextureToDraw(const std::vector<int>& rows, int& index, std::set<std::uint32_t>& drawn,
                                     std::uint32_t& key, std::vector<TerrainRenderTexInfo>& out);

    // ---- zoom / rotation / terrain type ------------------------------------------------------------------------
    void AssignTerrainAndWaterTextureCoordinates(const ITerrainDrawContext& dc);
    void ChangeRotation(const ITerrainDrawContext& dc);
    void ChangeZoomLevel(const ITerrainDrawContext& dc);
    static void ChangeTerrainType(int terrainType);
    static void ReleaseLoadedTerrainTextures();
    static void SetUpTexturesForCurrentTerrainTypeAndZoom(int zoom);
    void SetWaterColorUpdateRect(const TerrainRect* r);
    void SetLightingUpdateRect(const TerrainRect* r);
    void SetWaterGlareStatus(bool on);
    bool IsWaterGlareEnabled() const { return waterGlare_; }

    // ---- culling -----------------------------------------------------------------------------------------
    void ComputeDrawFrustumOptimizationInfo(const ITerrainDrawContext& dc);
    void ComputeDrawRectsForDrawFrustum(const ITerrainDrawContext& dc);
    void ComputeHeightBasedCullingInfoForDrawFrustum(const ITerrainDrawContext& dc);
    void ComputeViewFrustumOptimizationInfo(const ITerrainDrawContext& dc);
    void ComputeDrawRectsForViewFrustum(const ITerrainDrawContext& dc);
    void ComputeHeightBasedCullingInfoForViewFrustum(const ITerrainDrawContext& dc);
    void ComputeMinimumVisibleHtAtDrawRows(const ITerrainDrawContext& dc, float* out) const;
    void ComputeMinimumVisibleHtAtViewRows(const ITerrainDrawContext& dc, float* out) const;
    void GetCullingInfo(int* rowBegin, int* rowEnd, const std::int32_t** colStart, const std::int32_t** colEnd) const;

    // ---- lighting ----------------------------------------------------------------------------------------
    void DoLighting();
    static void LightBorderCliffs();
    void UpdateLevelCellLighting(int row, int firstCell, int lastCell);

    // ---- messages / persistence -----------------------------------------------------------------------------
    bool DoMessage(const TerrainViewMessage& msg);
    // cIGZSerializable view (version u16 (1) + bool displayGrid); the byte-buffer pair below is the test hook.
    bool Read(cIGZIStream& stream) override;
    bool Write(cIGZOStream& stream) const override;
    bool Read(const std::vector<std::uint8_t>& stream);       // version u16 (1) + bool displayGrid
    std::vector<std::uint8_t> Write() const;
    bool GetDisplayGridFlag() const { return S().displayGrid; }
    void SetDisplayGridFlag(bool on);
    bool SetAutomaticCellCursorUpdateStatus(bool on) { bool old = cursorAutoUpdate_; cursorAutoUpdate_ = on; return old; }
    bool HandleCheatCommand(const std::vector<std::string>& args);

    // ---- selection forwarding -------------------------------------------------------------------------------
    void ClearCurrentSelections();
    bool MarkSelected(const std::vector<std::pair<int, int>>& pts, int color, bool on);
    bool MarkSelected(const TerrainRect& r, int color, bool on);
    bool MarkSelected(std::pair<int, int> p, int color, bool on);
    bool MarkSelected(std::pair<int, int> p, std::uint64_t key, std::uint32_t keyInstance, std::uint8_t a, bool b, int color, std::uint8_t c, bool d);

    // ---- level cells --------------------------------------------------------------------------------------
    void LevelCell(int x, int z, float height);
    void UpdateLevelCellsFromTerrain(int row, int firstCell, int lastCell);
    void RemoveCellLevelling(int x, int z);
    void SetTriangleIndicesForCell(int z, int x);
    void SetTriangleIndicesForAllRows();

    // ---- picking -----------------------------------------------------------------------------------------
    bool Pick(const float origin[3], const float dir[3], float* outPoint /*vec3*/, bool includeWater = false);

    // ---- drawing ------------------------------------------------------------------------------------------
    bool Update(ITerrainDrawContext& dc);
    void DrawTerrainLayers(ITerrainDrawContext& dc, ITerrainVertBufExtn* vb);
    void DrawWaterLayers(ITerrainDrawContext& dc, ITerrainVertBufExtn* vb);
    bool DrawGrid(ITerrainDrawContext& dc, ITerrainVertBufExtn* vb);
    bool DrawTerrainSurface(ITerrainDrawContext& dc, ITerrainVertBufExtn* vb);
    bool DrawTextureLessTerrainSurfaceWithVertBufExtension(ITerrainVertBufExtn* vb);
    bool DrawTextureLessTerrainSurface(ITerrainDrawContext& dc);
    bool DrawTexturedTerrainSurface(ITerrainDrawContext& dc, ITerrainVertBufExtn* vb);
    void DrawTerrainSurfaceRows(ITerrainDrawContext& dc, const std::vector<int>& rows);
    void DrawTerrainSurfaceRowsWithVertBufExtension(ITerrainDrawContext& dc, ITerrainVertBufExtn* vb, const std::vector<int>& rows);
    void DrawTerrainMeshSubsetInDrawFrustum(TerrainRect r, ITerrainDrawContext& dc);
    void DrawTerrainMeshSubsetInViewFrustum(TerrainRect r, ITerrainDrawContext& dc);
    void DrawTerrainMeshSubsetWithVertBufExtensionInDrawFrustum(ITerrainDrawContext& dc, ITerrainVertBufExtn* vb, TerrainRect r);
    void DrawTerrainMeshSubsetWithVertBufExtensionInViewFrustum(ITerrainDrawContext& dc, ITerrainVertBufExtn* vb, TerrainRect r);
    void DrawMixedMeshSubsetInDrawFrustum(const TerrainRect& r, ITerrainDrawContext& dc);
    void DrawMixedMeshSubsetInViewFrustum(const TerrainRect& r, ITerrainDrawContext& dc);
    bool DrawSideCliffs(ITerrainDrawContext& dc);
    void DrawWaterBox(ITerrainDrawContext& dc);
    bool DrawWaterSurface(ITerrainDrawContext& dc, ITerrainVertBufExtn* vb);
    bool DrawWaterSurfaceSubsetInDrawFrustum(TerrainRect r, ITerrainDrawContext& dc);
    bool DrawWaterSurfaceSubsetInViewFrustum(TerrainRect r, ITerrainDrawContext& dc);
    void DrawPlumbingInfo(ITerrainDrawContext& dc);
    void DrawTerrainQuad(ITerrainDrawContext& dc, std::uint32_t x, std::uint32_t z);
    void DrawTexturedTerrainQuad(ITerrainDrawContext& dc, std::uint32_t x, std::uint32_t z, const void* quadVertices, bool force);
    void SetColorOfWaterSurface();

    // state accessors for tests / the render integration
    cSTETerrain* Terrain() const { return terrain_; }
    const TerrainFrustumCull& DrawCull() const { return S().draw; }
    const TerrainFrustumCull& ViewCull() const { return S().view; }
    TerrainRect LightRect() const { return lightRect_; }
    bool LightingDone() const { return lightingDone_; }
    TerrainRect WaterColorRect() const { return waterRect_; }
    bool WaterColorDone() const { return waterColorDone_; }
    float NightFactor() const { return nightFactor_; }
    float DisplayAltitudeHigh() const { return heightHi_; }   // +0x94
    float DisplayAltitudeLow() const { return heightLo_; }    // +0x98
    int VisibleEdgeA() const { return visibleEdge_[0]; }
    int VisibleEdgeB() const { return visibleEdge_[1]; }
    ITerrainSelection* Selection() const { return selection_.get(); }
    ITerrainViewHost* Host() const { return host_; }

private:
    static void ComputeRects(const TerrainFrustumPlanes& fp, TerrainFrustumCull& c);
    static void ComputeMinHt(const TerrainFrustumPlanes& fp, const TerrainFrustumCull& c, float* out);
    void ComputeHeightCull(TerrainFrustumCull& c, const float* minVis);
    static void UnionRect(TerrainRect& dst, const TerrainRect& r);
    const cSTETerrain::Vertex& TV(int idx) const { return terrain_->Vertices()[static_cast<std::size_t>(idx)]; }
    void DrawSubsetCommon(const TerrainFrustumCull& c, const std::vector<std::int32_t>& cs, const std::vector<std::int32_t>& ce,
                          const std::vector<std::uint8_t>& vis, TerrainRect r, ITerrainDrawContext& dc, bool rowsFromView);
    void DrawMixedCommon(const TerrainFrustumCull& c, const std::vector<std::int32_t>& cs, const std::vector<std::int32_t>& ce,
                         const std::vector<std::uint8_t>& terrainVis, const std::vector<std::uint8_t>& waterVis,
                         const TerrainRect& r, ITerrainDrawContext& dc);
    bool DrawWaterSubsetCommon(const TerrainFrustumCull& c, const std::vector<std::int32_t>& cs, const std::vector<std::int32_t>& ce,
                               const std::vector<std::uint8_t>& vis, TerrainRect r, ITerrainDrawContext& dc);
    void DrawVertBufSubsetCommon(const TerrainFrustumCull& c, const std::vector<std::int32_t>& cs, const std::vector<std::int32_t>& ce,
                                 const std::vector<std::uint8_t>& vis, ITerrainDrawContext& dc, ITerrainVertBufExtn* vb, TerrainRect r);

    ITerrainViewHost* host_ = nullptr;
    int refCount_ = 0;
    bool initialized_ = false;
    bool restoredFromStream_ = false;           // +0x31
    cSTETerrain* terrain_ = nullptr;
    bool cursorAutoUpdate_ = true;              // +0x60 (initialised to 0 then 1 by ctor: 0x100 at +0x60)
    bool lightingDone_ = false;                 // +0x61
    TerrainRect lightRect_{};                   // +0x64
    float nightFactor_ = 0;                     // +0x74
    bool waterColorDone_ = true;                // +0x78
    bool waterGlare_ = true;                    // +0x79
    TerrainRect waterRect_{};                   // +0x7c
    int visibleEdge_[2] = {0, 0};               // +0x8c/+0x90
    float heightHi_ = 3.4028235e38f, heightLo_ = -3.4028235e38f;   // +0x94/+0x98 (refreshed by Update)
    std::shared_ptr<ITerrainSelection> selection_;                  // +0xa0
    std::shared_ptr<TerrainTextureBinding> gridTexture_;            // +0xd8
    bool viewDirty_ = true;                     // +0x10c
};

}  // namespace sc4
