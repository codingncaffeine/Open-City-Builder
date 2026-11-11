// cSTETerrainView3D — data structures, texture spans, culling, lighting, level cells, picking, messages.
// The drawing half lives in cSTETerrainView3DDraw.cpp. See the header for the memory layouts.
#include <cstdio>
#include <cstdlib>

#include "sc4/cSTETerrainView3D.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "sc4/GeometryUtilities.h"

namespace sc4 {
namespace {
constexpr float kFloatMax = 3.4028235e38f;
// kTextureScale @0x100a29e20 (indexed by zoom level), kXStart/kZStart/kXIncrement/kZIncrement @0x100a29e40..e70.
constexpr float kTextureScale[6] = {1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 0.0f};
constexpr float kXStart[4] = {0, 1, 1, 0}, kZStart[4] = {1, 1, 0, 0};
constexpr float kXIncrement[4] = {16, 0, -16, 0}, kZIncrement[4] = {0, -16, 0, 16};
constexpr std::uint32_t kDisplacementMapInstanceId[2] = {0xca5cc9c0, 0xca5cc9c1};   // @0x100a29eec

// The original rounds with the 1.5 * 2^23 trick; these are exactly floor() and ceil() (NaN keeps the rounded value).
inline int FloorToInt(float v) { return static_cast<int>(std::floor(v)); }
inline int CeilToInt(float v) { return static_cast<int>(std::ceil(v)); }
}  // namespace

cSTETerrainView3D::Shared& cSTETerrainView3D::S() {
    static Shared shared;
    return shared;
}

cSTETerrainView3D::cSTETerrainView3D(ITerrainViewHost* host) : host_(host ? host : S().host) {}   // 0x100748284

cSTETerrainView3D::~cSTETerrainView3D() {                                                         // 0x1007483b2
    if (initialized_) Shutdown();
    gridTexture_.reset();
}

std::uint32_t cSTETerrainView3D::Release() {                                                     // 0x10074e460
    // the original thunks to cRZUnknown::Release (this + 8), which destroys at the last reference.
    if (refCount_ < 2) {
        delete this;
        return 0;
    }
    return --refCount_;
}

bool cSTETerrainView3D::QueryInterface(std::uint32_t iid, void** out) {                           // 0x10074e056
    // The original answers +0 (0xc989f960 / 0xc9b84e10), +0x18 (0x6771477d), +0x20 (0x452294aa) and
    // +0x28 (0xe4fda3d4). The port keeps a single cIGZSerializable base, so every accepted id resolves
    // to `this`; the serializable id is what the city-load serializer (SC4ReadGZSerializable) asks for.
    if (iid != 0xc989f960 && iid != kGZCLSID && iid != 0x6771477d && iid != 0x452294aa && iid != 0xe4fda3d4 && iid != 1)
        return false;
    if (out) *out = static_cast<cIGZSerializable*>(this);
    AddRef();
    return true;
}

void cSTETerrainView3D::UnionRect(TerrainRect& dst, const TerrainRect& r) {                       // SC4Rect<int>::UnionRect
    dst.left = std::min(dst.left, r.left);
    dst.top = std::min(dst.top, r.top);
    dst.right = std::max(dst.right, r.right);
    dst.bottom = std::max(dst.bottom, r.bottom);
}

// ===============================================================================================================
// Static lifetime
// ===============================================================================================================
bool cSTETerrainView3D::StaticInit(ITerrainViewHost* host) {                                      // 0x10074d478
    Shared& s = S();
    if (s.inited) return false;
    s.host = host;
    ReadTuningParameters();
    for (auto& v : s.terrainTextureBinding) v.clear();
    if (!host) return false;
    for (int z = 0; z < 4; ++z) {
        s.waterSurfaceTexture[z] = host->LoadWaterTexture(z, false);
        s.pollutedWaterSurfaceTexture[z] = host->LoadWaterTexture(z, true);
    }
    s.waterBoxGradient = host->GetBuffer(0xc9efb727, 8);
    s.waterColorDepthGradient = host->GetBuffer(0x69efb6f8, 8);
    if (s.waterBoxGradient) s.waterBoxGradientHt = static_cast<std::uint32_t>(s.waterBoxGradient->Height() - 1);
    // Original quirk (confirmed in the disassembly at 0x10074d666): it tests the *height* static, which is still 0
    // on the first start, so the colour-depth gradient height is never initialised and the clamp below stays at 0.
    if (s.waterColorDepthGradientHt != 0 && s.waterColorDepthGradient)
        s.waterColorDepthGradientHt = static_cast<std::uint32_t>(s.waterColorDepthGradient->Height() - 1);
    s.lotTextureBinding = host->LoadLotTexture();
    s.textureSpansForRows.assign(0x100, {});
    s.texSpanInfoDirty.assign(0x100, false);
    s.inited = true;
    return true;
}

bool cSTETerrainView3D::StaticShutdown() {                                                         // 0x10074fef8
    Shared& s = S();
    if (!s.inited) return true;
    s.inited = false;
    DeleteTerrainSizeSpecificDataStructures();
    s.waterColorDepthGradient.reset();
    s.waterBoxGradient.reset();
    s.waterGlareMaskBuf.reset();
    ReleaseLoadedTerrainTextures();
    for (int i = 0; i < 4; ++i) {
        s.waterSurfaceTexture[i].reset();
        s.pollutedWaterSurfaceTexture[i].reset();
    }
    for (auto& v : s.terrainTextureBinding) v.clear();
    s.lotTextureBinding.reset();
    s.cellCountX = s.cellCountZ = s.vertexCountX = s.vertexCountZ = s.numVertices = 0;
    s.citySizeX = s.citySizeZ = 0;
    s.host = nullptr;
    return true;
}

std::shared_ptr<IPixelBuffer> cSTETerrainView3D::GetBuffer(std::uint32_t instance, int colorType) {   // 0x10074cd9c
    return S().host ? S().host->GetBuffer(instance, colorType) : nullptr;
}

// ===============================================================================================================
// Tuning
// ===============================================================================================================
void cSTETerrainView3D::ReadTuningParameters() {                                                   // 0x10074e18c
    Shared& s = S();
    ITerrainViewHost* h = s.host;
    if (!h || !h->HasTuningExemplar()) return;
    h->ReadTuningFloat(0x89a312c0, s.terrainTexTilingFactor);
    s.terrainTexTilingFactor /= 100.0f;
    h->ReadTuningFloat(0x29ab15c0, s.waterDepthForMaxAlpha);
    h->ReadTuningFloat(0x29ab15c1, s.waterMinAlpha);
    h->ReadTuningFloat(0x29ab15c2, s.waterDepthToColorGradientFactor);
    h->ReadTuningBool(0x29ab15c3, s.displayWaterSurface);
    h->ReadTuningBool(0x29ab15c4, s.displayTerrainSurface);
    std::vector<float> night;
    h->ReadTuningFloatVector(0x29ab15c5, night);
    for (std::size_t i = 0; i < 3 && i < night.size(); ++i) s.waterNightColorAdjustment[i] = night[i];
    h->ReadTuningFloatVector(0x29ab15c6, s.meanCityEdgeLayerHeights);
    for (std::size_t i = 0; i < 2 && i < s.meanCityEdgeLayerHeights.size(); ++i)
        s.cityEdgeTexTilingFactors[i] = 1.0f / s.meanCityEdgeLayerHeights[i];
    s.cityEdgeV = 0.015625f;
    InitializeCityEdgeLayerHeights();
    h->ReadTuningFloat(0x29ab15c7, s.cityEdgeBrightnessKludgeFactor);
}

void cSTETerrainView3D::InitializeCityEdgeLayerHeights() {                                         // 0x10074e354
    Shared& s = S();
    if (!s.host) return;
    for (int layer = 0; layer < 2; ++layer) {
        std::shared_ptr<IPixelBuffer> buf = s.host->GetBuffer(kDisplacementMapInstanceId[layer], 2);
        if (!buf) continue;
        float mean = layer < static_cast<int>(s.meanCityEdgeLayerHeights.size()) ? s.meanCityEdgeLayerHeights[layer] : 0.0f;
        for (int i = 0; i < 0x101; ++i)
            s.cityEdgeLayerHeights[layer][i] = (static_cast<float>(buf->Pixel(i, 0) & 0xff) - 127.0f) * 0.5f + mean;
    }
}

void cSTETerrainView3D::ReadPlumbingTuningData(std::uint32_t colors[4], bool& showAll) {            // 0x10074cbf0
    ITerrainViewHost* h = S().host;
    if (!h || !h->HasTuningExemplar()) return;
    static const std::uint32_t ids[4] = {0x497f42a4, 0xe97f42b0, 0xa97f42ce, 0x097f4c4e};
    for (int i = 0; i < 4; ++i) h->ReadPlumbingColor(ids[i], colors[i]);
    h->ReadTuningBool(0x297f42b7, showAll);
}

// ===============================================================================================================
// Terrain size dependent data
// ===============================================================================================================
void cSTETerrainView3D::SetTerrainSize(std::uint32_t cellsX, std::uint32_t cellsZ) {               // 0x10074d716
    Shared& s = S();
    if (s.cellCountX == cellsX && s.cellCountZ == cellsZ) return;
    s.vertexCountX = cellsX + 1;
    s.vertexCountZ = cellsZ + 1;
    s.numVertices = s.vertexCountZ * s.vertexCountX;
    s.citySizeX = static_cast<float>(cellsX) * 16.0f;
    s.citySizeZ = static_cast<float>(cellsZ) * 16.0f;
    // The four city-edge strips walk the border: top (z = max), right, bottom, left.
    s.vertStart[0] = static_cast<int>(cellsZ * s.vertexCountX);
    s.vertStart[1] = static_cast<int>(s.numVertices) - 1;
    s.vertStart[2] = static_cast<int>(cellsX);
    s.vertStart[3] = 0;
    s.vertIncrement[0] = 1;
    s.vertIncrement[1] = -static_cast<int>(s.vertexCountX);
    s.vertIncrement[2] = -1;
    s.vertIncrement[3] = static_cast<int>(s.vertexCountX);
    s.cellCountX = cellsX;
    s.cellCountZ = cellsZ;
    DeleteTerrainSizeSpecificDataStructures();
    CreateTerrainSizeSpecificDataStructures();
}

void cSTETerrainView3D::InitTerrainSurfaceItems() {                                                // 0x1007480ec
    Shared& s = S();
    for (std::uint32_t i = 0; i < s.numVertices; ++i) {
        TerrainSurfacePoint& p = s.points[i];
        p.x = static_cast<float>(i % s.vertexCountX) * 16.0f;
        p.z = static_cast<float>(i / s.vertexCountX) * 16.0f;
        p.diffuse[3] = 0xff;
        p.u2 = p.v2 = 0;
    }
}

void cSTETerrainView3D::InitWaterSurfaceItems() {                                                  // 0x1007481be
    Shared& s = S();
    for (std::uint32_t i = 0; i < s.numVertices; ++i) {
        TerrainSurfacePoint& p = s.waterPoints[i];
        p.x = static_cast<float>(i % s.vertexCountX) * 16.0f;
        p.z = static_cast<float>(i / s.vertexCountX) * 16.0f;
        p.diffuse[0] = 0x00; p.diffuse[1] = 0x00; p.diffuse[2] = 0xff; p.diffuse[3] = 0x00;   // 0x00ff0000 as a dword
        p.u = p.x * 0.0125f;
        p.v = p.z * 0.0125f;
        p.u2 = p.v2 = 0;
    }
    S().waterGlareMaskBuf = GetBuffer(0x69efb7da, 8);
}

void cSTETerrainView3D::CreateTerrainSizeSpecificDataStructures() {                                // 0x100747a80
    Shared& s = S();
    const std::uint32_t vx = s.vertexCountX, vz = s.vertexCountZ, cx = s.cellCountX, cz = s.cellCountZ;
    s.points.assign(s.numVertices, {});
    InitTerrainSurfaceItems();
    s.waterPoints.assign(s.numVertices, {});
    InitWaterSurfaceItems();
    s.numTriangleIndicesPerRow = cx * 6;
    s.triangleIndices.assign(cx * 6, 0);
    s.flippedTriangleIndices.assign(cx * 6, 0);
    s.levelCellQuadTriIndices.assign(cx * 6, 0);
    for (std::uint32_t c = 0; c < cx; ++c) {
        const std::uint16_t i = static_cast<std::uint16_t>(c), w = static_cast<std::uint16_t>(vx);
        std::uint16_t* t = &s.triangleIndices[c * 6];
        t[0] = i; t[1] = static_cast<std::uint16_t>(w + i); t[2] = static_cast<std::uint16_t>(i + 1 + w);
        t[3] = i; t[4] = static_cast<std::uint16_t>(i + 1 + w); t[5] = static_cast<std::uint16_t>(i + 1);
        std::uint16_t* f = &s.flippedTriangleIndices[c * 6];
        f[0] = i; f[1] = static_cast<std::uint16_t>(w + i); f[2] = static_cast<std::uint16_t>(i + 1);
        f[3] = static_cast<std::uint16_t>(i + 1); f[4] = static_cast<std::uint16_t>(i + w); f[5] = static_cast<std::uint16_t>(i + 1 + w);
        const std::uint16_t q = static_cast<std::uint16_t>(c * 4);
        std::uint16_t* l = &s.levelCellQuadTriIndices[c * 6];
        l[0] = q; l[1] = static_cast<std::uint16_t>(q + 1); l[2] = static_cast<std::uint16_t>(q + 2);
        l[3] = static_cast<std::uint16_t>(q + 2); l[4] = static_cast<std::uint16_t>(q + 3); l[5] = q;
    }
    s.allTriangleIndices.assign(static_cast<std::size_t>(s.numTriangleIndicesPerRow) * cz, 0);
    s.allLevelCellIndices.assign(static_cast<std::size_t>(cx + 1) * cz, 0);
    s.levelCellQuads.assign(cz, {});
    s.levelCellInfos.assign(cz, {});
    const std::uint16_t triQuad[6] = {0, 2, 3, 0, 3, 1}, flipped[6] = {0, 2, 1, 1, 2, 3};
    std::memcpy(s.triQuadIndices, triQuad, sizeof triQuad);
    std::memcpy(s.flippedTriQuadIndices, flipped, sizeof flipped);

    for (int side = 0; side < 4; ++side) {
        const std::uint32_t n = (side & 1) ? vz : vx;
        for (int layer = 0; layer < 3; ++layer) s.borderPoints[side][layer].assign(n * 2, {});
        s.waterBoxPoints[side].assign(n * 2, {});
    }
    std::uint32_t boxColor = s.waterBoxGradient ? s.waterBoxGradient->Pixel(0, 0) : 0;
    for (int side = 0; side < 4; ++side) {
        const std::uint32_t n = (side & 1) ? vz : vx;
        float zc = kZStart[side] * s.citySizeZ, xc = kXStart[side] * s.citySizeX;
        const float dx = kXIncrement[side], dz = kZIncrement[side];
        for (std::uint32_t i = 0; i < n; ++i) {
            const float along = (side & 1) ? zc : xc;
            const std::uint32_t j = i + n;
            for (int layer = 0; layer < 3; ++layer) {
                std::vector<TerrainBorderPoint>& arr = s.borderPoints[side][layer];
                arr[j].x = xc; arr[i].x = xc;
                const float u = (layer < 2 ? s.cityEdgeTexTilingFactors[layer] : s.cityEdgeTexTilingFactors[1]) * along;
                arr[j].u = u; arr[i].u = u;
                arr[j].z = zc; arr[i].z = zc;
                arr[j].diffuse[3] = 0xff; arr[i].diffuse[3] = 0xff;
                if (layer == 2) {
                    arr[j].v = 0; arr[j].y = 0;               // the lower strata row of the last layer sits at height 0
                    break;
                }
                arr[i].v = 0.0f;
                arr[j].v = 1.0f;
            }
            TerrainWaterBoxPoint& top = s.waterBoxPoints[side][i];
            TerrainWaterBoxPoint& bottom = s.waterBoxPoints[side][j];
            top.x = xc; bottom.x = xc;
            top.z = zc; bottom.z = zc;
            top.diffuse[0] = static_cast<std::uint8_t>(boxColor >> 16);
            top.diffuse[1] = static_cast<std::uint8_t>(boxColor >> 8);
            top.diffuse[2] = static_cast<std::uint8_t>(boxColor);
            top.diffuse[3] = 0xff; bottom.diffuse[3] = 0xff;
            xc += dx;
            zc += dz;
        }
    }
    s.textureZoom = 5;
    s.textureRotation = 4;
}

void cSTETerrainView3D::DeleteTerrainSizeSpecificDataStructures() {                                // 0x10074888a
    Shared& s = S();
    s.points.clear(); s.points.shrink_to_fit();
    s.waterPoints.clear(); s.waterPoints.shrink_to_fit();
    s.triangleIndices.clear(); s.flippedTriangleIndices.clear(); s.allTriangleIndices.clear();
    s.levelCellQuadTriIndices.clear(); s.allLevelCellIndices.clear();
    s.levelCellQuads.clear(); s.levelCellInfos.clear();
    for (int side = 0; side < 4; ++side) {
        for (int layer = 0; layer < 3; ++layer) s.borderPoints[side][layer].clear();
        s.waterBoxPoints[side].clear();
    }
}

// ===============================================================================================================
// Init / Shutdown
// ===============================================================================================================
bool cSTETerrainView3D::Init(cSTETerrain* terrain) {                                               // 0x10074ce30
    Shared& s = S();
    if (!s.inited) StaticInit(host_);
    if (initialized_) return false;
    initialized_ = true;
    if (!restoredFromStream_ && host_) s.displayGrid = host_->PreferenceDisplayGrid();
    if (!terrain_) terrain_ = terrain;
    SetTerrainSize(terrain_->CellCountX(), terrain_->CellCountZ());
    ChangeTerrainType(terrain_->TerrainType());
    s.curRotation = 4;
    s.curZoom = 5;
    if (!selection_ && host_) selection_ = host_->CreateSelection(*terrain_);
    TerrainRect all{0, 0, static_cast<std::int32_t>(s.cellCountX), static_cast<std::int32_t>(s.cellCountZ)};
    ObtainHeightFieldInfo(all);
    ObtainWaterSurfaceInfo(all);
    SetLightingUpdateRect(nullptr);
    if (s.texSpanInfoDirty.size() < s.cellCountZ) s.texSpanInfoDirty.resize(std::max<std::size_t>(0x100, s.cellCountZ), false);
    if (s.textureSpansForRows.size() < s.cellCountZ) s.textureSpansForRows.resize(std::max<std::size_t>(0x100, s.cellCountZ));
    for (std::uint32_t z = 0; z < s.cellCountZ; ++z) s.texSpanInfoDirty[z] = true;
    SetTriangleIndicesForAllRows();
    std::fill(s.allLevelCellIndices.begin(), s.allLevelCellIndices.end(), static_cast<std::uint16_t>(0));
    for (auto& q : s.levelCellQuads) q.clear();
    for (auto& i : s.levelCellInfos) i.clear();
    if (host_) {
        static const std::uint32_t msgs[6] = {0x277a9364, 0x278e6003, 0x678e6e3a, 0xa6b79601, 0xa6b79611, 0xc9da96ea};
        for (std::uint32_t m : msgs) host_->RegisterMessage(m);
        host_->AddRenderLayer(this, 0, 0);
        host_->AddRenderLayer(selection_.get(), 5, 1000);
    }
    for (auto& p : s.waterPoints) p.diffuse[3] = 0;
    if (host_) gridTexture_ = host_->LoadGridTexture();
    return true;
}

void cSTETerrainView3D::Shutdown() {                                                                // 0x1007484de
    if (!initialized_) return;
    initialized_ = false;
    if (host_) {
        host_->RemoveRenderLayer(this);
        host_->RemoveRenderLayer(selection_.get());
        static const std::uint32_t msgs[6] = {0x678e6e3a, 0x278e6003, 0x277a9364, 0xa6b79601, 0xa6b79611, 0xc9da96ea};
        for (std::uint32_t m : msgs) host_->UnregisterMessage(m);
    }
    if (selection_) selection_->Shutdown();
    selection_.reset();
    if (host_) host_->StorePreferenceDisplayGrid(S().displayGrid);
    gridTexture_.reset();
    terrain_ = nullptr;
}

// ===============================================================================================================
// Terrain height / water info
// ===============================================================================================================
void cSTETerrainView3D::ObtainHeightFieldInfo(const TerrainRect& r) {                              // 0x100748eca
    Shared& s = S();
    const float sea = terrain_->GetSeaLevel();
    const std::int32_t vx = static_cast<std::int32_t>(s.vertexCountX), cz = static_cast<std::int32_t>(s.cellCountZ),
                       cx = static_cast<std::int32_t>(s.cellCountX);
    for (std::int32_t z = r.top; z <= r.bottom; ++z) {
        for (std::int32_t x = r.left; x <= r.right; ++x) {
            const std::int32_t idx = vx * z + x;
            const float h = TV(idx).altitude;
            s.points[static_cast<std::size_t>(idx)].y = h;
            if (z == cz) UpdateBorderHeight(0, x, h, sea);
            if (x == cx) UpdateBorderHeight(1, cz - z, h, sea);
            if (z == 0) UpdateBorderHeight(2, cx - x, h, sea);
            if (x == 0) UpdateBorderHeight(3, z, h, sea);
        }
    }
}

void cSTETerrainView3D::ObtainWaterSurfaceInfo(const TerrainRect& r) {                             // 0x10074902c
    Shared& s = S();
    const float sea = terrain_->GetSeaLevel();
    const std::int32_t vx = static_cast<std::int32_t>(s.vertexCountX);
    const std::vector<float>& depth = terrain_->WaterDepth();
    for (std::int32_t z = r.top; z <= r.bottom; ++z) {
        for (std::int32_t x = r.left; x <= r.right; ++x) {
            const std::int32_t idx = vx * z + x;
            const float d = depth[static_cast<std::size_t>(idx)];
            s.waterPoints[static_cast<std::size_t>(idx)].y = d > 0.0f ? d + TV(idx).altitude : sea;
        }
    }
    for (int side = 0; side < 4; ++side) {
        const std::int32_t n = static_cast<std::int32_t>((side & 1) ? s.vertexCountZ : s.vertexCountX);
        std::int32_t vert = s.vertStart[side];
        for (std::int32_t i = 0; i < n; ++i, vert += s.vertIncrement[side]) {
            TerrainWaterBoxPoint& top = s.waterBoxPoints[side][static_cast<std::size_t>(i)];
            TerrainWaterBoxPoint& bottom = s.waterBoxPoints[side][static_cast<std::size_t>(i + n)];
            top.y = s.waterPoints[static_cast<std::size_t>(vert)].y;
            const float diff = top.y - bottom.y;
            if (diff > 0.0f) {
                std::uint32_t idx = static_cast<std::uint32_t>(diff * s.waterDepthToColorGradientFactor);
                idx = std::min(idx, s.waterBoxGradientHt);
                const std::uint32_t c = s.waterColorDepthGradient ? s.waterColorDepthGradient->Pixel(0, static_cast<int>(idx)) : 0;
                bottom.diffuse[0] = static_cast<std::uint8_t>(c >> 16);
                bottom.diffuse[1] = static_cast<std::uint8_t>(c >> 8);
                bottom.diffuse[2] = static_cast<std::uint8_t>(c);
            }
        }
    }
    if (waterColorDone_) {
        waterColorDone_ = false;
        waterRect_ = r;
    } else {
        UnionRect(waterRect_, r);
    }
}

void cSTETerrainView3D::UpdateBorderHeight(int side, int index, float height, float seaLevel) {    // 0x10074d9ae
    Shared& s = S();
    const std::uint32_t n = (side & 1) ? s.vertexCountZ : s.vertexCountX;
    S().waterBoxPoints[side][n + static_cast<std::uint32_t>(index)].y = std::min(height, seaLevel);
    float h = height;
    for (int layer = 0; layer < 2; ++layer) {
        s.borderPoints[side][layer][static_cast<std::size_t>(index)].y = h;
        const float next = h - s.cityEdgeLayerHeights[layer][index];
        h = next >= 0.0f ? next : 0.0f;
        s.borderPoints[side][layer][n + static_cast<std::uint32_t>(index)].y = h;
    }
    s.borderPoints[side][2][static_cast<std::size_t>(index)].y = h;
    s.borderPoints[side][2][static_cast<std::size_t>(index)].v = h * s.cityEdgeV;
}

// ===============================================================================================================
// Texture spans
// ===============================================================================================================
void cSTETerrainView3D::MergeTextureSpan(TerrainTextureSpanInfo& info, const TerrainSpan& span) {   // 0x100746402
    int first = span.first, last = span.last;
    auto it = info.spans.begin();
    while (it != info.spans.end() && it->first <= last) {
        if (it->last < first) {
            ++it;
        } else {
            first = std::min(first, it->first);
            last = std::max(last, it->last);
            it = info.spans.erase(it);
        }
    }
    info.spans.insert(it, TerrainSpan{first, last});
}

void cSTETerrainView3D::AddTextureSpan(std::list<TerrainTextureSpanInfo>& row, std::uint32_t key, int firstCell, int lastCell) {   // 0x10074629c
    Shared& s = S();
    TerrainSpan span{firstCell < 1 ? 0 : firstCell - 1, lastCell + 1};
    if (static_cast<std::uint32_t>(span.last) > s.cellCountX) span.last = static_cast<int>(s.cellCountX);   // unsigned compare as in the binary
    auto it = row.begin();
    for (; it != row.end() && it->textureKey < key; ++it) {}
    if (it != row.end() && it->textureKey == key) {
        MergeTextureSpan(*it, span);
        return;
    }
    it = row.insert(it, TerrainTextureSpanInfo{});
    it->textureKey = key;
    it->spans.push_back(span);
}

void cSTETerrainView3D::ComputeTextureSpansForRow(int row) {                                       // 0x100747928
    Shared& s = S();
    std::list<TerrainTextureSpanInfo>& spans = s.textureSpansForRows[static_cast<std::size_t>(row)];
    spans.clear();
    s.texSpanInfoDirty[static_cast<std::size_t>(row)] = false;
    const std::int32_t vx = static_cast<std::int32_t>(s.vertexCountX), ccx = static_cast<std::int32_t>(s.cellCountX);
    auto keyAt = [&](std::int32_t vert, int layer) -> std::uint32_t {
        std::uint16_t k;
        std::memcpy(&k, &TV(vert).surface[6 + layer * 4], 2);
        return k;
    };
    for (int layer = 0; layer < 3; ++layer) {
        if (vx == 0) continue;
        std::int32_t vert = vx * row;
        std::uint32_t cur = 0, runStart = static_cast<std::uint32_t>(row), x = 0;
        for (x = 0; x < static_cast<std::uint32_t>(vx); ++x) {
            for (int pass = 0; pass < 2; ++pass) {
                const std::uint32_t k = keyAt(vert, layer);
                if (cur == 0) {
                    cur = k;
                    if (k != 0) runStart = x;
                } else if (cur != k) {
                    AddTextureSpan(spans, cur, static_cast<int>(runStart), static_cast<int>(x) - (pass == 0 ? 1 : 0));
                    cur = keyAt(vert, layer);
                    if (cur != 0) runStart = x;
                }
                vert += pass == 0 ? vx : -ccx;                // the second row of the cell pair, then back and one right
            }
        }
        if (cur != 0) AddTextureSpan(spans, cur, static_cast<int>(runStart), ccx);
    }
}

void cSTETerrainView3D::MarkTextureSpanInfoDirty(int firstRow, int lastRow) {                      // 0x10074d7d4
    Shared& s = S();
    for (int r = firstRow; r <= lastRow; ++r)
        if (r >= 0 && static_cast<std::size_t>(r) < s.texSpanInfoDirty.size()) s.texSpanInfoDirty[static_cast<std::size_t>(r)] = true;
}

bool cSTETerrainView3D::AddSpansThatNeedToBeRendered(int row, const TerrainTextureSpanInfo& info,
                                                     std::vector<TerrainRenderTexInfo>& out) {      // 0x10074613c
    Shared& s = S();
    const std::int32_t drawStart = s.draw.colStart[static_cast<std::size_t>(row)];
    const std::int32_t drawEnd = s.draw.colEnd[static_cast<std::size_t>(row)] - 1;
    TerrainRenderTexInfo* target = nullptr;
    for (const TerrainSpan& span : info.spans) {
        const std::int32_t first = std::max(drawStart, span.first);
        const std::int32_t last = std::min(drawEnd, span.last);
        if (first > last) continue;
        if (!target) {
            out.push_back(TerrainRenderTexInfo{row, {}});
            target = &out.back();
        }
        target->spans.push_back(TerrainSpan{first, last});
    }
    return target != nullptr;
}

bool cSTETerrainView3D::AddSpansCorrespondingToTexture(int row, std::uint32_t key, std::vector<TerrainRenderTexInfo>& out) {   // 0x1007460fe
    const std::list<TerrainTextureSpanInfo>& spans = S().textureSpansForRows[static_cast<std::size_t>(row)];
    for (const TerrainTextureSpanInfo& info : spans) {
        if (info.textureKey == key) return AddSpansThatNeedToBeRendered(row, info, out);
        if (key < info.textureKey) return false;
    }
    return false;
}

void cSTETerrainView3D::GetNextTextureToDraw(const std::vector<int>& rows, int& index, std::set<std::uint32_t>& drawn,
                                             std::uint32_t& key, std::vector<TerrainRenderTexInfo>& out) {   // 0x10074b858
    out.clear();
    const int count = static_cast<int>(rows.size());
    if (index >= count) return;
    while (true) {
        const int row = rows[static_cast<std::size_t>(index)];
        for (const TerrainTextureSpanInfo& info : S().textureSpansForRows[static_cast<std::size_t>(row)]) {
            if (drawn.count(info.textureKey)) continue;
            if (!AddSpansThatNeedToBeRendered(row, info, out)) continue;
            key = info.textureKey;
            for (int j = index + 1; j < count; ++j) AddSpansCorrespondingToTexture(rows[static_cast<std::size_t>(j)], key, out);
            return;
        }
        ++index;
        if (index >= count) return;
    }
}

// ===============================================================================================================
// Zoom / rotation / terrain type
// ===============================================================================================================
void cSTETerrainView3D::AssignTerrainAndWaterTextureCoordinates(const ITerrainDrawContext& dc) {    // 0x1007464b8
    Shared& s = S();
    const int zoom = dc.Zoom();
    const int rot = dc.Rotation();
    if (s.textureZoom == zoom && s.textureRotation == rot) return;
    s.textureZoom = zoom;
    s.textureRotation = rot;
    if (rot < 0 || rot >= 4) return;
    const float k = kTextureScale[std::clamp(s.curZoom, 0, 5)] * s.terrainTexTilingFactor;
    const float sx = s.citySizeX, sz = s.citySizeZ;
    auto assign = [&](std::vector<TerrainSurfacePoint>& pts) {
        for (std::uint32_t i = 0; i < s.numVertices; ++i) {
            TerrainSurfacePoint& p = pts[i];
            switch (rot) {
                case 0: p.u = p.x * k; p.v = p.z * k; break;
                case 1: p.u = p.z * k; p.v = (sx - p.x) * k; break;
                case 2: p.u = (sx - p.x) * k; p.v = (sz - p.z) * k; break;
                case 3: p.u = (sz - p.z) * k; p.v = p.x * k; break;
            }
        }
    };
    assign(s.points);
    assign(s.waterPoints);
}

void cSTETerrainView3D::ChangeRotation(const ITerrainDrawContext& dc) {                            // 0x100746724
    S().curRotation = dc.Rotation();
    SetLightingUpdateRect(nullptr);
    SetWaterColorUpdateRect(nullptr);
    AssignTerrainAndWaterTextureCoordinates(dc);
}

void cSTETerrainView3D::ChangeZoomLevel(const ITerrainDrawContext& dc) {                           // 0x100746970
    Shared& s = S();
    if (dc.Zoom() == s.curZoom) return;
    s.curZoom = dc.Zoom();
    SetUpTexturesForCurrentTerrainTypeAndZoom(s.curZoom);
    TerrainRect all{0, 0, static_cast<std::int32_t>(s.cellCountX), static_cast<std::int32_t>(s.cellCountZ)};
    SetLightingUpdateRect(&all);
    SetWaterColorUpdateRect(&all);
    AssignTerrainAndWaterTextureCoordinates(dc);
}

void cSTETerrainView3D::ChangeTerrainType(int terrainType) {                                       // 0x1007467ca
    Shared& s = S();
    if (s.curTerrainType == terrainType) return;
    ReleaseLoadedTerrainTextures();
    s.curTerrainType = terrainType;
    SetUpTexturesForCurrentTerrainTypeAndZoom(s.curZoom);
}

void cSTETerrainView3D::ReleaseLoadedTerrainTextures() {                                            // 0x1007467fe
    Shared& s = S();
    for (int z = 0; z < 5; ++z) {
        if (!s.terrainTexturesLoaded[static_cast<std::size_t>(z)]) continue;
        if (s.host)
            for (std::uint32_t id : s.host->TexturesUsedInTerrainType(s.curTerrainType))
                if (id < s.terrainTextureBinding[static_cast<std::size_t>(z)].size()) s.terrainTextureBinding[static_cast<std::size_t>(z)][id].reset();
        s.terrainTexturesLoaded[static_cast<std::size_t>(z)] = false;
    }
}

void cSTETerrainView3D::SetUpTexturesForCurrentTerrainTypeAndZoom(int zoom) {                      // 0x10074688c
    Shared& s = S();
    if (zoom < 0 || zoom > 4 || s.terrainTexturesLoaded[static_cast<std::size_t>(zoom)] || !s.host) return;
    std::vector<std::shared_ptr<TerrainTextureBinding>>& bindings = s.terrainTextureBinding[static_cast<std::size_t>(zoom)];
    for (std::uint32_t id : s.host->TexturesUsedInTerrainType(s.curTerrainType)) {
        if (id >= bindings.size()) bindings.resize(id + 1);
        std::shared_ptr<TerrainTextureBinding> b = s.host->LoadTerrainTexture(id + static_cast<std::uint32_t>(zoom) * 0x100);
        if (!b) b = s.host->LoadTerrainTexture(id);      // fall back to the base-zoom resource
        bindings[id] = b;
    }
    s.terrainTexturesLoaded[static_cast<std::size_t>(zoom)] = true;
}

void cSTETerrainView3D::SetWaterColorUpdateRect(const TerrainRect* r) {                            // 0x10074675c
    Shared& s = S();
    TerrainRect rect = r ? *r : TerrainRect{0, 0, static_cast<std::int32_t>(s.cellCountX), static_cast<std::int32_t>(s.cellCountZ)};
    if (waterColorDone_) {
        waterColorDone_ = false;
        waterRect_ = rect;
    } else {
        UnionRect(waterRect_, rect);
    }
}

void cSTETerrainView3D::SetLightingUpdateRect(const TerrainRect* r) {                              // 0x10074fe34
    Shared& s = S();
    TerrainRect rect = r ? *r : TerrainRect{0, 0, static_cast<std::int32_t>(s.cellCountX), static_cast<std::int32_t>(s.cellCountZ)};
    if (lightingDone_) {
        lightingDone_ = false;
        lightRect_ = rect;
    } else {
        UnionRect(lightRect_, rect);
    }
}

void cSTETerrainView3D::SetWaterGlareStatus(bool on) {                                              // 0x10074feb8
    if (on == waterGlare_) return;
    waterGlare_ = on;
    SetWaterColorUpdateRect(nullptr);
}

// ===============================================================================================================
// Frustum culling
// ===============================================================================================================
void cSTETerrainView3D::ComputeRects(const TerrainFrustumPlanes& fp, TerrainFrustumCull& c) {      // 0x100746ad4 / 0x10074713a
    Shared& s = S();
    const std::int32_t vx = static_cast<std::int32_t>(s.vertexCountX);
    c.colStart.resize(s.vertexCountZ);
    c.colEnd.resize(s.vertexCountZ);
    const float origin[2] = {0.0f, 0.0f}, dirX[2] = {1.0f, 0.0f};
    std::int32_t begin = FloorToInt(fp.minZ * 0.0625f);
    if (begin < 0) begin = 0;
    c.rowBegin = begin;
    const std::uint32_t endCell = static_cast<std::uint32_t>(CeilToInt(fp.maxZ * 0.0625f)) + 1u;   // unsigned clamp as in the binary
    const std::uint32_t end = std::min(endCell, s.vertexCountZ);
    c.rowEnd = static_cast<std::int32_t>(end);
    c.rowLast = c.rowEnd - 1;
    (void)origin;
    for (std::int32_t row = c.rowBegin; row < c.rowEnd; ++row) {
        const float rowOrigin[2] = {0.0f, static_cast<float>(row) * 16.0f};
        float a[2] = {0, 0}, b[2] = {0, 0};
        SC4GeometryUtilities::RaysIntersect2D(rowOrigin, dirX, fp.leftOrigin, fp.leftDir, a);
        SC4GeometryUtilities::RaysIntersect2D(rowOrigin, dirX, fp.rightOrigin, fp.rightDir, b);
        const float lo = std::min(a[0], b[0]), hi = std::max(a[0], b[0]);
        c.colStart[static_cast<std::size_t>(row)] = FloorToInt(lo * 0.0625f);
        c.colEnd[static_cast<std::size_t>(row)] = CeilToInt(hi * 0.0625f) + 1;
    }
    c.colMin = vx;
    c.colMax = 0;
    for (std::int32_t row = c.rowBegin; row < c.rowEnd; ++row) {
        std::int32_t& st = c.colStart[static_cast<std::size_t>(row)];
        std::int32_t& en = c.colEnd[static_cast<std::size_t>(row)];
        st = std::clamp(st, 0, vx);
        en = std::clamp(en, 0, vx);
        c.colMin = std::min(c.colMin, st);
        c.colMax = std::max(c.colMax, en);
    }
    for (std::int32_t row = c.rowBegin; row + 1 < c.rowEnd; ++row) {
        c.colStart[static_cast<std::size_t>(row)] = std::min(c.colStart[static_cast<std::size_t>(row)], c.colStart[static_cast<std::size_t>(row) + 1]);
        c.colEnd[static_cast<std::size_t>(row)] = std::max(c.colEnd[static_cast<std::size_t>(row)], c.colEnd[static_cast<std::size_t>(row) + 1]);
    }
}

void cSTETerrainView3D::ComputeMinHt(const TerrainFrustumPlanes& fp, const TerrainFrustumCull& c, float* out) {   // 0x100747764 / 0x100747846
    const float a = fp.planeA, cc = fp.planeC, d = fp.planeD, inv = -1.0f / fp.planeB;
    for (std::int32_t row = c.rowBegin; row < c.rowEnd; ++row) {
        out[row] = kFloatMax;
        const std::int32_t st = c.colStart[static_cast<std::size_t>(row)], en = c.colEnd[static_cast<std::size_t>(row)];
        if (st < en) {
            const float rowTerm = static_cast<float>(row) * 16.0f * cc;
            const float v = (static_cast<float>(st) * 16.0f * a + d + rowTerm) * inv;
            out[row] = v;
            if (en > 0) {
                const float v2 = (rowTerm + static_cast<float>(en - 1) * 16.0f * a + d) * inv;
                if (v2 < v) out[row] = v2;
            }
        }
    }
}

void cSTETerrainView3D::ComputeMinimumVisibleHtAtDrawRows(const ITerrainDrawContext& dc, float* out) const {
    ComputeMinHt(dc.DrawFrustum(), S().draw, out);
}
void cSTETerrainView3D::ComputeMinimumVisibleHtAtViewRows(const ITerrainDrawContext& dc, float* out) const {
    ComputeMinHt(dc.ViewFrustum(), S().view, out);
}

void cSTETerrainView3D::ComputeHeightCull(TerrainFrustumCull& c, const float* minVis) {            // 0x100746e0c / 0x100747472
    Shared& s = S();
    c.terrainVisible.assign(s.vertexCountZ + 1, 0);
    c.waterVisible.assign(s.vertexCountZ + 1, 0);
    std::int32_t newBegin = c.rowEnd, newLast = c.rowLast;
    c.terrainRowBegin = c.terrainRowEnd = c.waterRowBegin = c.waterRowEnd = c.rowEnd;
    bool anyFirst = false, terrainFirst = false, waterFirst = false;
    const std::int32_t origLast = c.rowLast;
    for (std::int32_t row = c.rowBegin; row < c.rowEnd; ++row) {
        const std::int32_t st = c.colStart[static_cast<std::size_t>(row)], en = c.colEnd[static_cast<std::size_t>(row)];
        if (st < en) {
            float minTerrain = kFloatMax, maxTerrain = -kFloatMax, maxWater = -kFloatMax;
            const std::size_t base = static_cast<std::size_t>(s.vertexCountX) * static_cast<std::size_t>(row) + static_cast<std::size_t>(st);
            for (std::int32_t i = 0; i < en - st; ++i) {
                const float y = s.points[base + static_cast<std::size_t>(i)].y;
                maxTerrain = std::max(maxTerrain, y);
                minTerrain = std::min(minTerrain, y);
                maxWater = std::max(maxWater, s.waterPoints[base + static_cast<std::size_t>(i)].y);
            }
            const float threshold = minVis[row];
            if (std::getenv("SC4_CULLDUMP") && (row % 4) == 0) { static int dumps = 0; if (dumps++ < 40) std::fprintf(stderr, "cull row %d cols %d..%d maxTerrain %.1f minTerrain %.1f maxWater %.1f minVis %.1f\n", row, st, en, maxTerrain, minTerrain, maxWater, threshold); }
            c.terrainVisible[static_cast<std::size_t>(row)] = threshold <= maxTerrain;
            c.waterVisible[static_cast<std::size_t>(row)] = minTerrain < maxWater && threshold <= maxWater;
            if (c.terrainVisible[static_cast<std::size_t>(row)] || c.waterVisible[static_cast<std::size_t>(row)]) {
                if (!anyFirst) { newBegin = row > 0 ? row - 1 : 0; anyFirst = true; }
                if (c.terrainVisible[static_cast<std::size_t>(row)]) {
                    if (!terrainFirst) { c.terrainRowBegin = row > 0 ? row - 1 : 0; terrainFirst = true; }
                    c.terrainRowEnd = row + 2;
                }
                if (c.waterVisible[static_cast<std::size_t>(row)]) {
                    if (!waterFirst) { c.waterRowBegin = row > 0 ? row - 1 : 0; waterFirst = true; }
                    c.waterRowEnd = row + 2;
                }
                if (row < origLast) newLast = row + 1;
            }
        } else {
            c.terrainVisible[static_cast<std::size_t>(row)] = 0;
            c.waterVisible[static_cast<std::size_t>(row)] = 0;
        }
    }
    std::int32_t rb = std::max(newBegin, c.rowBegin);
    c.rowBegin = rb;
    std::int32_t last = std::min(newLast, c.rowLast);
    c.rowLast = last;
    c.rowEnd = last + 1;
    c.terrainRowBegin = std::max(c.terrainRowBegin, rb);
    c.terrainRowEnd = std::min(c.terrainRowEnd, c.rowEnd);
    c.waterRowBegin = std::max(rb, c.waterRowBegin);
    c.waterRowEnd = std::min(c.waterRowEnd, c.rowEnd);
    c.terrainRowLast = c.terrainRowEnd - 1;
    c.waterRowLast = c.waterRowEnd - 1;
}

void cSTETerrainView3D::ComputeDrawRectsForDrawFrustum(const ITerrainDrawContext& dc) { ComputeRects(dc.DrawFrustum(), S().draw); }
void cSTETerrainView3D::ComputeDrawRectsForViewFrustum(const ITerrainDrawContext& dc) { ComputeRects(dc.ViewFrustum(), S().view); }

void cSTETerrainView3D::ComputeHeightBasedCullingInfoForDrawFrustum(const ITerrainDrawContext& dc) {
    std::vector<float> minVis(S().vertexCountZ + 1, kFloatMax);
    ComputeMinimumVisibleHtAtDrawRows(dc, minVis.data());
    ComputeHeightCull(S().draw, minVis.data());
}
void cSTETerrainView3D::ComputeHeightBasedCullingInfoForViewFrustum(const ITerrainDrawContext& dc) {
    std::vector<float> minVis(S().vertexCountZ + 1, kFloatMax);
    ComputeMinimumVisibleHtAtViewRows(dc, minVis.data());
    ComputeHeightCull(S().view, minVis.data());
}

void cSTETerrainView3D::ComputeDrawFrustumOptimizationInfo(const ITerrainDrawContext& dc) {         // 0x100746a50
    ComputeDrawRectsForDrawFrustum(dc);
    dc.GetVisibleEdges(visibleEdge_[0], visibleEdge_[1]);
    visibleEdge_[0] = (6 - visibleEdge_[0]) % 4;
    visibleEdge_[1] = (6 - visibleEdge_[1]) % 4;
    ComputeHeightBasedCullingInfoForDrawFrustum(dc);
}

void cSTETerrainView3D::ComputeViewFrustumOptimizationInfo(const ITerrainDrawContext& dc) {         // 0x1007470fe
    if (!viewDirty_) return;
    viewDirty_ = false;
    ComputeDrawRectsForViewFrustum(dc);
    ComputeHeightBasedCullingInfoForViewFrustum(dc);
}

void cSTETerrainView3D::GetCullingInfo(int* rowBegin, int* rowEnd, const std::int32_t** colStart, const std::int32_t** colEnd) const {   // 0x10074e162
    *rowBegin = S().draw.rowBegin;
    *rowEnd = S().draw.rowEnd;
    *colStart = S().draw.colStart.data();
    *colEnd = S().draw.colEnd.data();
}

void cSTETerrainView3D::GetViewCullingInfo(int* rowBegin, int* rowEnd, const std::int32_t** colStart, const std::int32_t** colEnd) const {
    *rowBegin = S().view.rowBegin;
    *rowEnd = S().view.rowEnd;
    *colStart = S().view.colStart.data();
    *colEnd = S().view.colEnd.data();
}

// ===============================================================================================================
// Lighting
// ===============================================================================================================
void cSTETerrainView3D::DoLighting() {                                                             // 0x100748b34
    if (lightingDone_) return;
    if (host_) host_->ApplyTerrainLighting(lightRect_);
    LightBorderCliffs();
    lightingDone_ = true;
}

void cSTETerrainView3D::LightBorderCliffs() {                                                      // 0x100748b64
    Shared& s = S();
    ITerrainViewHost* h = s.host;
    if (!h) return;
    // sideNormal: the outward-facing normals of the four city edges.
    static const float sideNormal[4][3] = {{0, 0, 1}, {1, 0, 0}, {0, 0, -1}, {-1, 0, 0}};
    for (int side = 0; side < 4; ++side) {
        float rgba[4] = {1, 1, 1, 1};
        h->GetNormalLight(sideNormal[side], rgba);
        const std::uint32_t n = (side & 1) ? s.vertexCountZ : s.vertexCountX;
        if (static_cast<std::int32_t>(n) <= 0) continue;
        // luminance rounded with the 1.5 * 2^15 trick, then n - (n >> 8) and masked to a byte
        const float lumf = h->ColorToLuminance(rgba[0], rgba[1], rgba[2]);
        const std::uint32_t rounded = static_cast<std::uint32_t>(static_cast<std::int32_t>(std::nearbyint(lumf * 256.0f)));   // (int)(x + 49152.0) bit trick
        const float base = static_cast<float>((rounded - (rounded >> 8)) & 0xff) * s.cityEdgeBrightnessKludgeFactor;
        const float top = std::min(base, 254.0f);
        const float bottom = std::min(top + top, 254.0f);
        for (std::uint32_t i = 0; i < n; ++i) {
            for (int layer = 0; layer < 3; ++layer) {
                TerrainBorderPoint& p = s.borderPoints[side][layer][i];
                const float yv = p.y;
                const float shade = yv < 70.0f ? bottom : ((bottom - top) / 70.0f) * yv + top;   // below 70: fVar16; above: ramp
                const std::uint8_t c = static_cast<std::uint8_t>(static_cast<int>(shade));
                p.diffuse[0] = p.diffuse[1] = p.diffuse[2] = c;
            }
            for (int layer = 0; layer < 2; ++layer) {
                const std::uint8_t c = s.borderPoints[side][layer + 1][i].diffuse[0];
                TerrainBorderPoint& lower = s.borderPoints[side][layer][i + n];
                lower.diffuse[0] = lower.diffuse[1] = lower.diffuse[2] = c;
            }
            const std::uint8_t c = static_cast<std::uint8_t>(static_cast<int>(top));
            TerrainBorderPoint& last = s.borderPoints[side][2][i + n];
            last.diffuse[0] = last.diffuse[1] = last.diffuse[2] = c;
        }
    }
}

void cSTETerrainView3D::UpdateLevelCellLighting(int row, int firstCell, int lastCell) {            // 0x10074f272
    Shared& s = S();
    if (!host_ || !host_->HasLighting()) return;
    const float normal[3] = {0, 1, 0};
    for (int c = firstCell; c < lastCell; ++c) {
        TerrainCellQuad& q = s.levelCellQuads[static_cast<std::size_t>(row)][static_cast<std::size_t>(c)];
        const float pos[3] = {(q.corner[0].x + q.corner[2].x) * 0.5f, (q.corner[0].y + q.corner[2].y) * 0.5f,
                              (q.corner[0].z + q.corner[2].z) * 0.5f};
        std::uint8_t rgb[3] = {0, 0, 0};
        host_->GetPointLight(pos, normal, rgb);
        for (int k = 0; k < 4; ++k) {
            q.corner[k].diffuse[0] = rgb[0]; q.corner[k].diffuse[1] = rgb[1]; q.corner[k].diffuse[2] = rgb[2];
            q.corner[k].diffuse[3] = 0xff;
        }
    }
}

// ===============================================================================================================
// Messages, persistence, display grid, selection forwarding
// ===============================================================================================================
bool cSTETerrainView3D::DoMessage(const TerrainViewMessage& msg) {                                 // 0x100748d8c
    auto rectOf = [&]() {
        TerrainRect r;
        r.left = static_cast<std::int32_t>(msg.data1 >> 16);
        r.right = static_cast<std::int32_t>(msg.data1 & 0xffff);
        r.top = static_cast<std::int32_t>(msg.data2 >> 16);
        r.bottom = static_cast<std::int32_t>(msg.data2 & 0xffff);
        return r;
    };
    switch (static_cast<std::int32_t>(msg.type)) {
        case static_cast<std::int32_t>(0xc9da96ea): {
            SetLightingUpdateRect(nullptr);
            SetWaterColorUpdateRect(nullptr);
            std::memcpy(&nightFactor_, &msg.data2, 4);
            return true;
        }
        case static_cast<std::int32_t>(0xa6b79601):
        case static_cast<std::int32_t>(0xa6b79611):
            break;
        case 0x277a9364: SetDisplayGridFlag(!S().displayGrid); return true;
        case 0x678e6e3a: ObtainWaterSurfaceInfo(rectOf()); break;
        case 0x278e6003: ObtainHeightFieldInfo(rectOf()); break;
        default: return true;
    }
    viewDirty_ = true;
    return true;
}

bool cSTETerrainView3D::Read(cIGZIStream& stream) {                                                  // 0x10074e0ee
    restoredFromStream_ = true;
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 1) return false;
    std::uint8_t grid = 0;
    stream.GetUint8(grid);
    S().displayGrid = grid != 0;
    return !stream.GetError();
}

bool cSTETerrainView3D::Write(cIGZOStream& stream) const {                                            // 0x1007503de
    stream.SetUint16(1);
    stream.SetUint8(S().displayGrid ? 1 : 0);
    return !stream.GetError();
}

bool cSTETerrainView3D::Read(const std::vector<std::uint8_t>& stream) {                            // byte-buffer test hook
    restoredFromStream_ = true;
    if (stream.size() < 3) return false;
    std::uint16_t version;
    std::memcpy(&version, stream.data(), 2);
    if (version != 1) return false;
    S().displayGrid = stream[2] != 0;
    return true;
}

std::vector<std::uint8_t> cSTETerrainView3D::Write() const {                                       // byte-buffer test hook
    return {1, 0, static_cast<std::uint8_t>(S().displayGrid ? 1 : 0)};
}

void cSTETerrainView3D::SetDisplayGridFlag(bool on) {                                              // 0x10074f6c8
    Shared& s = S();
    if (s.displayGrid == on) return;
    TerrainRect all{0, 0, static_cast<std::int32_t>(s.cellCountX), static_cast<std::int32_t>(s.cellCountZ)};
    s.displayGrid = on;
    if (host_) host_->RedisplayTerrain(all);
}

bool cSTETerrainView3D::HandleCheatCommand(const std::vector<std::string>& args) {                 // 0x10074f730
    if (args.empty()) return false;
    if (args[0] == "level" && args.size() == 4) {
        LevelCell(std::atoi(args[1].c_str()), std::atoi(args[2].c_str()), static_cast<float>(std::atof(args[3].c_str())));
        return true;
    }
    if (args[0] == "unlevel" && args.size() == 3) {
        RemoveCellLevelling(std::atoi(args[1].c_str()), std::atoi(args[2].c_str()));
        return true;
    }
    return false;
}

void cSTETerrainView3D::ClearCurrentSelections() { if (selection_) selection_->ClearCurrentSelections(); }   // 0x100746a20
bool cSTETerrainView3D::MarkSelected(const std::vector<std::pair<int, int>>& pts, int color, bool on) {      // 0x10074d872
    return selection_ ? selection_->MarkSelected(pts, color, on) : false;
}
bool cSTETerrainView3D::MarkSelected(const TerrainRect& r, int color, bool on) {                             // 0x10074d8ae
    return selection_ ? selection_->MarkSelected(r, color, on) : false;
}
bool cSTETerrainView3D::MarkSelected(std::pair<int, int> p, int color, bool on) {                            // 0x10074d8ea
    return selection_ ? selection_->MarkSelected(p, color, on) : false;
}
bool cSTETerrainView3D::MarkSelected(std::pair<int, int> p, std::uint64_t key, std::uint32_t keyInstance, std::uint8_t a,
                                     bool b, int color, std::uint8_t c, bool d) {                             // 0x10074d926 / 0x10074d96a
    return selection_ ? selection_->MarkSelected(p, key, keyInstance, a, b, color, c, d) : false;
}

// ===============================================================================================================
// Level cells
// ===============================================================================================================
void cSTETerrainView3D::SetTriangleIndicesForCell(int z, int x) {                                  // 0x10074f376
    Shared& s = S();
    const std::uint16_t f = TV(static_cast<int>(s.vertexCountX) * z + x).flags;
    std::uint16_t* t = &s.allTriangleIndices[static_cast<std::size_t>(z) * s.numTriangleIndicesPerRow + static_cast<std::size_t>(x) * 6];
    const std::uint16_t cx = static_cast<std::uint16_t>(x), w = static_cast<std::uint16_t>(s.vertexCountX);
    const std::uint16_t cx1 = static_cast<std::uint16_t>(cx + 1), rowNext = static_cast<std::uint16_t>(cx + w),
                        diag = static_cast<std::uint16_t>(cx + 1 + w);
    const bool hideEven = (f & 0x20) != 0, hideOdd = (f & 0x40) != 0, alt = (f & 0x200) != 0;
    if ((f & 0x3000) == 0) {
        if (hideEven) { t[0] = t[1] = t[2] = cx; }
        else if (alt) { t[0] = diag; t[1] = rowNext; t[2] = cx; }
        else { t[0] = cx; t[1] = rowNext; t[2] = diag; }
        if (hideOdd) { t[3] = t[4] = t[5] = cx; }
        else if (alt) { t[3] = cx1; t[4] = diag; t[5] = cx; }
        else { t[3] = cx; t[4] = diag; t[5] = cx1; }
    } else {
        if (hideEven) { t[0] = t[1] = t[2] = cx; }
        else if (alt) { t[0] = cx1; t[1] = rowNext; t[2] = cx; }
        else { t[0] = cx; t[1] = rowNext; t[2] = cx1; }
        if (hideOdd) { t[3] = t[4] = t[5] = cx; }
        else if (alt) { t[3] = diag; t[4] = rowNext; t[5] = cx1; }
        else { t[3] = cx1; t[4] = rowNext; t[5] = diag; }
    }
}

void cSTETerrainView3D::SetTriangleIndicesForAllRows() {                                           // 0x10074d80a
    Shared& s = S();
    for (std::uint32_t z = 0; z < s.cellCountZ; ++z)
        for (std::uint32_t x = 0; x < s.cellCountX; ++x) SetTriangleIndicesForCell(static_cast<int>(z), static_cast<int>(x));
}

void cSTETerrainView3D::UpdateLevelCellsFromTerrain(int row, int firstCell, int lastCell) {        // 0x10074f0a8
    Shared& s = S();
    const std::size_t vx = s.vertexCountX;
    for (int c = firstCell; c < lastCell; ++c) {
        TerrainCellQuad& q = s.levelCellQuads[static_cast<std::size_t>(row)][static_cast<std::size_t>(c)];
        const TerrainLevelCellInfo& info = s.levelCellInfos[static_cast<std::size_t>(row)][static_cast<std::size_t>(c)];
        const std::size_t base = vx * static_cast<std::size_t>(row) + info.column;
        q.corner[0] = s.points[base];
        q.corner[1] = s.points[base + vx];
        q.corner[2] = s.points[base + vx + 1];
        q.corner[3] = s.points[base + 1];
        for (int k = 0; k < 4; ++k) q.corner[k].y = info.height;
    }
}

void cSTETerrainView3D::LevelCell(int x, int z, float height) {                                    // 0x10074e7d2
    Shared& s = S();
    if (x < 0 || z < 0 || static_cast<std::uint32_t>(x) >= s.vertexCountX || static_cast<std::uint32_t>(z) >= s.vertexCountZ) return;
    const std::size_t rowBase = static_cast<std::size_t>(s.cellCountX + 1) * static_cast<std::size_t>(z);
    std::uint16_t* idx = &s.allLevelCellIndices[rowBase];
    const std::uint16_t cur = idx[x];
    if (cur == idx[x + 1]) {
        if (static_cast<std::uint32_t>(x + 1) <= s.cellCountX) {
            idx[x + 1] = static_cast<std::uint16_t>(cur + 1);
            for (std::uint32_t k = static_cast<std::uint32_t>(x) + 2; k <= s.cellCountX; ++k) ++idx[k];
        }
        auto& quads = s.levelCellQuads[static_cast<std::size_t>(z)];
        auto& infos = s.levelCellInfos[static_cast<std::size_t>(z)];
        quads.insert(quads.begin() + cur, TerrainCellQuad{});
        infos.insert(infos.begin() + cur, TerrainLevelCellInfo{static_cast<std::uint32_t>(x), height});
    }
    s.levelCellInfos[static_cast<std::size_t>(z)][cur].height = height;
    UpdateLevelCellsFromTerrain(z, cur, cur + 1);
    UpdateLevelCellLighting(z, cur, cur + 1);
    terrain_->Vertices()[static_cast<std::size_t>(s.vertexCountX) * static_cast<std::size_t>(z) + static_cast<std::size_t>(x)].flags |= 0x60;
    SetTriangleIndicesForCell(z, x);
}

void cSTETerrainView3D::RemoveCellLevelling(int x, int z) {                                        // 0x10074f4e6
    Shared& s = S();
    if (x < 0 || z < 0 || static_cast<std::uint32_t>(x) >= s.vertexCountX || static_cast<std::uint32_t>(z) >= s.vertexCountZ) return;
    const std::size_t rowBase = static_cast<std::size_t>(s.cellCountX + 1) * static_cast<std::size_t>(z);
    std::uint16_t* idx = &s.allLevelCellIndices[rowBase];
    const std::uint16_t cur = idx[x], next = idx[x + 1];
    if (cur == next) return;
    if (static_cast<std::uint32_t>(x + 1) <= s.cellCountX) {
        idx[x + 1] = static_cast<std::uint16_t>(next - 1);
        for (std::uint32_t k = static_cast<std::uint32_t>(x) + 2; k <= s.cellCountX; ++k) --idx[k];
    }
    auto& quads = s.levelCellQuads[static_cast<std::size_t>(z)];
    auto& infos = s.levelCellInfos[static_cast<std::size_t>(z)];
    quads.erase(quads.begin() + cur);
    infos.erase(infos.begin() + cur);
    terrain_->Vertices()[static_cast<std::size_t>(s.vertexCountX) * static_cast<std::size_t>(z) + static_cast<std::size_t>(x)].flags &= 0xff9f;
    SetTriangleIndicesForCell(z, x);
}

// ===============================================================================================================
// Picking
// ===============================================================================================================
bool cSTETerrainView3D::Pick(const float origin[3], const float dir[3], float* outT, bool includeWater) {   // 0x10074da8e (and 0x10074da7c)
    Shared& s = S();
    const float boxMin[3] = {0.0f, -32768.0f, 0.0f}, boxMax[3] = {s.citySizeX, 32768.0f, s.citySizeZ};   // ptCityBoundingBoxMin @0x100a29ee0
    float entry[3] = {origin[0], origin[1], origin[2]};
    if (!SC4GeometryUtilities::RayIntersectsBoundingBox(origin, dir, boxMin, boxMax, entry)) return false;
    // Nudge the entry point 4 m inside the city, then walk cell by cell along the dominant axis.
    const float nx = entry[0] + (entry[0] < s.citySizeX * 0.5f ? 4.0f : -4.0f);
    const float nz = entry[2] + (entry[2] < s.citySizeZ * 0.5f ? 4.0f : -4.0f);
    float fx = nx * 0.0625f, fz = nz * 0.0625f;
    const float adx = std::fabs(dir[0]), adz = std::fabs(dir[2]);
    float step[2];
    if (adx <= adz) { step[0] = adz == 0.0f ? 0.0f : adx / adz; step[1] = 1.0f; }
    else { step[0] = 1.0f; step[1] = adz / adx; }
    if (dir[0] < 0.0f) step[0] = -step[0];
    if (dir[2] < 0.0f) step[1] = -step[1];
    const std::vector<float>* water = includeWater ? &terrain_->WaterDepth() : nullptr;
    for (int iter = 0; iter < 0x300; ++iter) {
        const std::int32_t cx = static_cast<std::int32_t>(fx), cz = static_cast<std::int32_t>(fz);
        if (cx < 0 || cz < 0 || static_cast<std::uint32_t>(cx) >= s.cellCountX || static_cast<std::uint32_t>(cz) >= s.cellCountZ) return false;
        std::int32_t cand[3][2];
        if (adx <= adz) { cand[0][0] = cx; cand[0][1] = cz - 1; cand[1][0] = cx; cand[1][1] = cz; cand[2][0] = cx; cand[2][1] = cz + 1; }
        else { cand[0][0] = cx - 1; cand[0][1] = cz; cand[1][0] = cx; cand[1][1] = cz; cand[2][0] = cx + 1; cand[2][1] = cz; }
        for (auto& c : cand) {
            const std::int32_t ux = c[0], uz = c[1];
            if ((ux | uz) < 0 || static_cast<std::uint32_t>(ux) + 1 >= s.vertexCountX || static_cast<std::uint32_t>(uz) + 1 >= s.vertexCountZ) continue;
            const float x0 = ux * 16.0f, z0 = uz * 16.0f, x1 = (ux + 1) * 16.0f, z1 = (uz + 1) * 16.0f;
            const std::int32_t i00 = static_cast<std::int32_t>(s.vertexCountX) * uz + ux;
            const std::int32_t i01 = i00 + static_cast<std::int32_t>(s.vertexCountX), i11 = i01 + 1, i10 = i00 + 1;
            float y00 = TV(i00).altitude, y01 = TV(i01).altitude, y11 = TV(i11).altitude, y10 = TV(i10).altitude;
            if (water) {
                auto up = [&](float y, std::int32_t i) { const float w = (*water)[static_cast<std::size_t>(i)]; return (std::fabs(w) + w) * 0.5f + y; };
                y00 = up(y00, i00); y01 = up(y01, i01); y11 = up(y11, i11); y10 = up(y10, i10);
            }
            const float P0[3] = {x0, y00, z0}, P1[3] = {x0, y01, z1}, P2[3] = {x1, y11, z1}, P3[3] = {x1, y10, z0};
            bool hit;
            if ((TV(i00).flags & 0x3000) == 0) {
                hit = SC4GeometryUtilities::RayIntersectsTriangle(origin, dir, P0, P1, P2, outT) ||
                      SC4GeometryUtilities::RayIntersectsTriangle(origin, dir, P0, P2, P3, outT);
            } else {
                hit = SC4GeometryUtilities::RayIntersectsTriangle(origin, dir, P0, P1, P3, outT) ||
                      SC4GeometryUtilities::RayIntersectsTriangle(origin, dir, P3, P1, P2, outT);
            }
            if (hit) {
                if (cursorAutoUpdate_ && selection_) selection_->MarkSelected(std::vector<std::pair<int, int>>{{ux, uz}}, 2, true);
                return true;
            }
        }
        fx += step[0];
        fz += step[1];
    }
    return false;
}

}  // namespace sc4
