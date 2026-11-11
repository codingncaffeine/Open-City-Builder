#include "app/NativeShadowOverlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

#include "sc4/DrawContext.h"
#include "sc4/PackedRenderState.h"
#include "sc4/Render3D.h"
#include "app/NativeModelMaker.h"
#include "app/NativeTerrainView.h"
#include "sc4/CellTextureModel.h"
#include "sc4/LightingManager.h"
#include "sc4/ModelMaker.h"
#include "sc4/OverlayManager.h"
#include "sc4/ServiceLocator.h"
#include "sc4/ViewUtilities.h"
#include "sc4/cSTETerrain.h"

namespace native {
sc4::cSC4LightingManager* LightingManager();   // NativeLighting.cpp

namespace {
using namespace sc4;

cSTETerrain* g_terrain = nullptr;
cSTEOverlayManager* g_manager = nullptr;
cSC43DRender* g_render = nullptr;

// The model material's texture binding (frame ids) as the overlay manager holds it.
class Texture final : public IOverlayTexture {
public:
    explicit Texture(const CellTextureBinding* binding) : binding_(binding) {}
    void AddRef() override { ++refs_; }
    void Release() override { if (--refs_ == 0) delete this; }
    int GlTextureId() const override { return binding_ && binding_->frameCount ? static_cast<int>(binding_->frames[0]) : 0; }
private:
    const CellTextureBinding* binding_;
    int refs_ = 1;
};

class ShadowProjector final : public IOverlayShadowService {
public:
    bool ProjectShadow(int count, const cS3DVector3* points, const cS3DVector2* uvs, const cS3DTransform& transform, cS3DMatrix4& out) override {
        cSC4LightingManager* lighting = LightingManager();
        return lighting && lighting->CalcShadowProjection(count, points, uvs, transform, &out);
    }
    void GetShadowDirection(cS3DVector3& out) override {
        cSC4LightingManager* lighting = LightingManager();
        out = lighting ? lighting->GetShadowDirection() : cS3DVector3{0.0f, -1.0f, 0.0f};
    }
    bool CalcShadowRect(int count, const cS3DVector3* points, const cS3DBoundingBox& box, cRZRect& out) override {
        cSC4LightingManager* lighting = LightingManager();
        return lighting && lighting->CalcShadowTerrainRect(count, points, box, &out);
    }
    void GetShadowColor(cS3DVector4& colour, float& scale, float& intensity) override {
        cSC4LightingManager* lighting = LightingManager();
        cS3DVector3 rgb{};
        float strength = 0.5f, threshold = 0.0f;
        if (lighting) lighting->GetShadowParams(strength, threshold, rgb);
        colour = {rgb.x, rgb.y, rgb.z, 1.0f};
        scale = strength;
        intensity = threshold;
    }
    float ShadowFalloff(const cS3DVector2& position) override {
        cSC4LightingManager* lighting = LightingManager();
        return lighting ? lighting->GetShadowStrengthFromLocation(position) : 1.0f;
    }
};

class Host final : public IOverlayHost {
public:
    cIGZMessageServer2* Messages() override { return SL::spMS2; }
    IOverlayTextureFactory* TextureFactory() override { return nullptr; }   // the decal textures (0xaa592b77) are not needed for model shadows
    IOverlayShadowService* ShadowService() override { return &service_; }
    int RenderQuality() override { const char* q = std::getenv("SC4_SHADOWQ"); return q ? std::atoi(q) : 5; }
    int RenderSecondTextureStage() override { return 0; }
    bool RenderSpatialIndexValid() override { return false; }
    int TerrainCellCountX() override { return g_terrain ? static_cast<int>(g_terrain->CellCountX()) : 0; }
    int TerrainCellCountZ() override { return g_terrain ? static_cast<int>(g_terrain->CellCountZ()) : 0; }
    float TerrainCellSize() override { return 16.0f; }
    int TerrainCellCountInRect(const cRZRect& rect) override { return std::max(0, rect.right - rect.left) * std::max(0, rect.bottom - rect.top); }
    void RedisplayTerrain(int, const cRZRect&) override {}
    void GetTerrainCullingInfo(int& rowBegin, int& rowEnd, const std::int32_t*& colStart, const std::int32_t*& colEnd) override {
        rowBegin = rowEnd = 0; colStart = colEnd = nullptr;
    }
    void DrawTerrainMeshSubset(bool, const cRZRect&, ITerrainVertBufExtn*) override {}
    void DrawWaterSurfaceSubset(bool, const cRZRect&) override {}
    void DrawMixedMeshSubset(bool, const cRZRect&) override {}
private:
    ShadowProjector service_;
};

Host g_host;

class Adapter final : public IShadowOverlay {
public:
    std::uint32_t AddShadow(std::uint64_t texture, int count, const cS3DVector3* points, const cS3DVector2* uvs,
                            const cS3DBoundingBox& box, const cS3DTransform& transform) override {
        if (!g_manager || !texture) return 0xffffffffu;
        if (std::getenv("SC4_DEBUG")) {
            static int shown = 0;
            if (shown++ < 3) {
                cRZRect r{};
                const bool ok = g_host.ShadowService()->CalcShadowRect(count, points, box, r);
                std::fprintf(stderr, "shadow add: box %.1f,%.1f,%.1f..%.1f,%.1f,%.1f translation %.1f,%.1f,%.1f count %d rect(%d) %d,%d,%d,%d\n", box.min[0], box.min[1], box.min[2], box.max[0],
                             box.max[1], box.max[2], transform.translation[0], transform.translation[1], transform.translation[2], count, ok, r.left, r.top, r.right, r.bottom);
            }
        }
        if (std::getenv("SC4_SHADOWLOG")) {   // dev: what each shadow mesh looks like
            static int logged = 0;
            const bool tallOnly = std::getenv("SC4_SHADOWLOG")[0] == 't';
            float top = -1e9f;
            for (int k = 0; k < count; ++k) top = std::max(top, points[k].y);
            float atX = 0, atZ = 0;
            const bool near = std::sscanf(std::getenv("SC4_SHADOWLOG"), "%f,%f", &atX, &atZ) == 2;
            const bool wanted = near ? std::abs(transform.translation[0] - atX) < 40.0f && std::abs(transform.translation[2] - atZ) < 40.0f : (!tallOnly || top > 25.0f);
            if (wanted && logged++ < 40) {
                float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f}, ulo[2] = {1e9f, 1e9f}, uhi[2] = {-1e9f, -1e9f};
                for (int k = 0; k < count; ++k) {
                    const float p[3] = {points[k].x, points[k].y, points[k].z}, t[2] = {uvs[k].x, uvs[k].y};
                    for (int j = 0; j < 3; ++j) { lo[j] = std::min(lo[j], p[j]); hi[j] = std::max(hi[j], p[j]); }
                    for (int j = 0; j < 2; ++j) { ulo[j] = std::min(ulo[j], t[j]); uhi[j] = std::max(uhi[j], t[j]); }
                }
                cRZRect r{};
                g_host.ShadowService()->CalcShadowRect(count, points, box, r);
                std::fprintf(stderr, "shadowlog tex %llx at %.1f,%.1f,%.1f: %d points %.1f,%.1f,%.1f..%.1f,%.1f,%.1f uv %.2f,%.2f..%.2f,%.2f rect %d,%d..%d,%d\n",
                             static_cast<unsigned long long>(texture), transform.translation[0], transform.translation[1], transform.translation[2], count, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], ulo[0], ulo[1], uhi[0], uhi[1], r.left, r.top, r.right, r.bottom);
            }
        }
        auto* wrapped = new Texture(reinterpret_cast<const CellTextureBinding*>(texture));
        const std::uint32_t id = g_manager->AddShadow(wrapped, count, points, uvs, box, transform);
        wrapped->Release();   // the manager holds its own reference
        static int adds = 0, failed = 0;
        (id == 0xffffffffu ? failed : adds)++;
        if (std::getenv("SC4_DEBUG") && ((adds + failed) % 500 == 1))
            std::fprintf(stderr, "shadow overlay: %d added, %d refused (lighting %p, quality %d)\n", adds, failed, static_cast<void*>(LightingManager()), g_manager->Quality());
        return id;
    }
};
Adapter g_adapter;

// V3F_C4UB_2T2F: stage 0 the model texture through the overlay's projection (texMatrix), stage 1 the soft-edge mask through its
// shadowMatrix (the projection followed by the map of the mesh's UV box onto [0.25, 0.75]): the mask clips each shadow to its own mesh,
// without it a model whose texture is an atlas casts the neighbouring pictures too.
struct Vertex { float x, y, z; std::uint8_t rgba[4]; float u, v, u1, v1; };

// The overlay manager's shadow texture (Init: factory slot 0x28, instance 0xaa592b77, the factory's shadow group 0x1abe787d).
int MaskTexture() {
    static const int id = static_cast<int>(FshTextureId(0x1abe787d, kOverlayTextureShadow));
    return id;
}

class DrawObject final : public cISC4ViewObject3D {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override { return --refs_; }
    void DrawViewObject(SC4DrawContext& context) override {
        static const bool enabled = std::getenv("SC4_NOSHADOWS") == nullptr;
        if (!enabled || !g_manager || !g_terrain) return;
        IOverlayShadowService* service = g_host.ShadowService();
        cS3DVector4 colour; float strength, threshold;
        service->GetShadowColor(colour, strength, threshold);
        // the shadow colour the original puts in the texture factor (SetTexColor), its alpha being strength * falloff per overlay;
        // the port's quad pass carries both in the vertex colour (the accurate combiner reads the colour and alpha of "current")
        const std::uint8_t rgb[3] = {static_cast<std::uint8_t>(std::clamp(colour.x, 0.0f, 1.0f) * 255.0f), static_cast<std::uint8_t>(std::clamp(colour.y, 0.0f, 1.0f) * 255.0f),
                                     static_cast<std::uint8_t>(std::clamp(colour.z, 0.0f, 1.0f) * 255.0f)};
        static cSC4PackedRenderState state;
        static bool packed = false;
        if (!packed) { state.PackRenderState(true, true, true, true, true, 1, false, 1, 4, 5, 4, 0.5f, -1, -1); packed = true; }   // renderStates_[kOverlayDrawModeShadow] (InitDrawStates)
        const int maxX = static_cast<int>(g_terrain->CellCountX()), maxZ = static_cast<int>(g_terrain->CellCountZ());
        auto& slots = g_manager->Overlays();
        // only the cells in view: the terrain view's view-frustum row tables (rows are z, colStart[z]..colEnd[z] the x range). The
        // original draws the shadows inside the terrain draw and reads its draw-frustum tables (SDSFindOverlaysToDraw through
        // GetTerrainCullingInfo); this pass runs every frame after the dynamic view, and with the backing store the terrain is only
        // drawn where the view changed, so its draw-frustum tables would cover that region alone.
        int rowBegin = 0, rowEnd = maxZ;
        const std::int32_t* colStart = nullptr;
        const std::int32_t* colEnd = nullptr;
        const bool culled = TerrainViewCullingInfo(rowBegin, rowEnd, colStart, colEnd);
        rowBegin = std::max(0, rowBegin);
        rowEnd = std::min(maxZ, rowEnd);
        // dev: SC4_SHADOWDBG=1 draws the rectangles untextured in red, =2 keeps the textures and the mask but draws red at full strength
        static const int dbgMode = std::getenv("SC4_SHADOWDBG") ? std::atoi(std::getenv("SC4_SHADOWDBG")) : 0;
        static const bool dbg = dbgMode == 1, red = dbgMode != 0;
        // one draw per (texture, faded strength): DrawShadows sets the alpha reference per overlay to strength * falloff * threshold
        std::map<std::pair<int, std::uint8_t>, std::vector<Vertex>> byTexture;
        std::size_t live = 0;
        for (std::uint32_t i = 0; i < slots.size(); ++i) {
            const cOverlay& overlay = slots[i];
            if (overlay.freeLink == -1 && (overlay.flags & kOverlayFlagShadow)) ++live;
            if (overlay.freeLink != -1 || (overlay.flags & 9u) != 9u || !overlay.texture) continue;
            const int x0 = std::max(0, overlay.rect.left), z0 = std::max(rowBegin, overlay.rect.top);
            const int x1 = std::min(maxX, overlay.rect.right), z1 = std::min(rowEnd, overlay.rect.bottom);
            if (x1 <= x0 || z1 <= z0) continue;
            // DrawShadows: the strength falls off with the location of the rectangle's centre (land pass), and a faint shadow is skipped
            const cS3DVector2 centre{8.0f * static_cast<float>(overlay.rect.left + overlay.rect.right), 8.0f * static_cast<float>(overlay.rect.top + overlay.rect.bottom)};
            const float strengthHere = std::clamp(strength * service->ShadowFalloff(centre), 0.0f, 1.0f);
            if (strengthHere < 0.05f) continue;
            const std::uint8_t overlayAlpha = static_cast<std::uint8_t>(strengthHere * 255.0f);
            std::vector<Vertex>& triangles = byTexture[{dbg ? 0 : overlay.texture->GlTextureId(), overlayAlpha}];
            // world position -> texture coordinates through a row-vector matrix (the texture transform of DrawShadows, which the
            // original applies to the camera-space position after the inverse view: the same as applying it to the world position)
            auto project = [](const cS3DMatrix4& m, float wx, float wy, float wz, float& u, float& v) {
                const float w = wx * m.m[0][3] + wy * m.m[1][3] + wz * m.m[2][3] + m.m[3][3];
                const float inverseW = (w > 1e-6f || w < -1e-6f) ? 1.0f / w : 1.0f;
                u = (wx * m.m[0][0] + wy * m.m[1][0] + wz * m.m[2][0] + m.m[3][0]) * inverseW;
                v = (wx * m.m[0][1] + wy * m.m[1][1] + wz * m.m[2][1] + m.m[3][1]) * inverseW;
            };
            auto vertex = [&](int x, int z) {
                const float wx = x * 16.0f, wz = z * 16.0f, wy = g_terrain->GetAltitudeAtVertex(x, z);
                Vertex out{wx, wy, wz, {red ? std::uint8_t(255) : rgb[0], red ? std::uint8_t(0) : rgb[1], red ? std::uint8_t(0) : rgb[2], red ? std::uint8_t(255) : overlayAlpha}, 0, 0, 0, 0};
                project(overlay.texMatrix, wx, wy, wz, out.u, out.v);
                project(overlay.shadowMatrix, wx, wy, wz, out.u1, out.v1);
                return out;
            };
            for (int z = z0; z < z1; ++z)
                for (int x = std::max(x0, culled ? static_cast<int>(colStart[z]) : 0); x < std::min(x1, culled ? static_cast<int>(colEnd[z]) : maxX); ++x) {
                    const Vertex a = vertex(x, z), b = vertex(x + 1, z), c = vertex(x, z + 1), d = vertex(x + 1, z + 1);
                    triangles.insert(triangles.end(), {a, b, c, b, d, c});
                }
        }
        if (byTexture.empty()) return;
        context.ResetModelViewTransform();
        // the setup of cSTEOverlayManager::DrawShadows (0x10072aee0): shadow colour, alpha test at intensity * strength, combiner 0 on stage 0
        context.SetRenderState(state);
        context.SetTexColor(colour);
        context.EnableAlphaTestFlag(threshold > 0.0f);
        context.SetTexFiltering(1, 1, 0);
        static const gd::cGDCombiner combiner{0x01030100u, 0x01010001u, 0u};
        context.SetTexCombiner(combiner, 0);
        // stage 1: the soft-edge mask (DrawShadows with a second stage: enable, texture, point filtering, clamp, combiner 0)
        static const bool useMask = std::getenv("SC4_NOSHADOWMASK") == nullptr;
        const int mask = useMask && !dbg ? MaskTexture() : 0;
        if (mask) {
            context.EnableTextureStateFlag(true, 1);
            context.SetTexture(mask, 1);
            context.SetTexFiltering(0, 0, 1);
            context.SetTexWrapModes(2, 2, 1);
            context.SetTexCombiner(combiner, 1);
        }
        context.SetDepthOffset(std::getenv("SC4_SHADOWDEPTH") ? std::atoi(std::getenv("SC4_SHADOWDEPTH")) : 3);   // DrawShadows (0x10072aee0): 3, toward the camera like the decals' 2
        std::size_t total = 0;
        for (const auto& batch : byTexture) {
            // The original tests tex.a * mask.a (the terrain vertices' alpha is 0xff) against threshold * a, a = strength * falloff;
            // here a is also in the vertex alpha, so the same test is tex.a * mask.a * a >= threshold * a * a.
            const float a = static_cast<float>(batch.first.second) / 255.0f;
            if (threshold > 0.0f) context.SetAlphaFunc(6, threshold * a * a);
            context.SetTexture(batch.first.first, 0);
            context.SetTexWrapModes(3, 3, 0);   // DrawShadows: (flags & 10) != 0 ? 3 : 2; every shadow has flag 8, so 3: repeat (GD 2 is clamp, 3 repeat)
            for (std::size_t n = 0; n < batch.second.size(); n += 3000)
                context.DrawPrims(0, 11, static_cast<int>(std::min<std::size_t>(3000, batch.second.size() - n)), batch.second.data() + n);
            total += batch.second.size();
        }
        context.EnableTextureStateFlag(false, 1);   // DrawShadows' last call
        context.SetDepthOffset(0);
        context.EnableAlphaTestFlag(false);
        // the combiners leave both stages in combiner mode and stage 1 current in the driver; the UI draws that follow set stage 0 only
        context.SetDefaultRenderState();
        static int reported = 0;
        if (std::getenv("SC4_DEBUG") && (reported++ < 3 || reported % 300 == 0))
            std::fprintf(stderr, "shadow overlay: %zu live, %zu vertices in %zu texture batches, colour %.2f %.2f %.2f strength %.2f threshold %.2f mask %d\n", live, total, byTexture.size(), colour.x, colour.y, colour.z, strength, threshold, mask);
    }
private:
    std::uint32_t refs_ = 1;
};

}  // namespace

void AttachShadowOverlay(cSC43DRender& render, cSTETerrain& terrain) {
    if (g_manager) return;
    g_terrain = &terrain;
    g_render = &render;
    g_manager = new cSTEOverlayManager(&g_host);
    g_manager->Init(nullptr, true, false);
    render.AddViewObject(new DrawObject(), 5, 150);   // after the dynamic view: the lot base textures and roads are drawn there and would cover shadows drawn earlier
}

IShadowOverlay* TheShadowOverlay() { return g_manager ? &g_adapter : nullptr; }

// cSC43DPlaceableObject::RemoveAll (0x100120bc2) and DoOverlayVisibilityChange (0x100120a16) call the land overlay manager it holds at +0x38
// directly: RemoveOverlay (vslot 0x78) and SetOverlayEnabled (vslot 0x80).
void RemoveShadowOverlay(std::uint32_t id) { if (g_manager) g_manager->RemoveOverlay(id); }
void SetShadowOverlayEnabled(std::uint32_t id, bool enabled) { if (g_manager) g_manager->SetOverlayEnabled(id, enabled); }

}  // namespace native
