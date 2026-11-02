#include "app/NativeDecalOverlay.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "app/NativeModelMaker.h"
#include "app/NativeTerrainView.h"
#include "sc4/DecalEffect.h"
#include "sc4/DrawContext.h"
#include "sc4/OverlayManager.h"
#include "sc4/PackedRenderState.h"
#include "sc4/Render3D.h"
#include "sc4/ServiceLocator.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

namespace native {
namespace {
using namespace sc4;

constexpr std::uint32_t kTextureGroup = 0x1abe787d;   // the overlay textures (FSH type 0x7ab50e44): decals, rings, the shadow mask

cSTETerrain* g_terrain = nullptr;
cSC4City* g_city = nullptr;
cSTEOverlayManager* g_managers[4] = {};
bool Debug() { static const bool on = std::getenv("SC4_DECALDEBUG") != nullptr; return on; }

// cS3DTextureBinding as the overlay manager holds it: a driver texture and its size. A generated texture owns its driver texture.
class Texture final : public IOverlayTexture {
public:
    Texture(unsigned id, int width, int height, bool owned) : id_(id), width_(width), height_(height), owned_(owned) {}
    ~Texture() override { if (owned_) DeleteDriverTexture(id_); }
    void AddRef() override { ++refs_; }
    void Release() override { if (--refs_ == 0) delete this; }
    int GlTextureId() const override { return static_cast<int>(id_); }
    int Width() const { return width_; }
    int Height() const { return height_; }
private:
    unsigned id_;
    int width_, height_;
    bool owned_;
    int refs_ = 1;
};

// cIS3DTextureBindingFactory: FSH textures by key or by instance in the overlay group (the driver textures are cached for the process).
class TextureFactory final : public IOverlayTextureFactory {
public:
    void AddRef() override {}
    void Release() override {}
    IOverlayTexture* GetTexture(const cGZPersistResourceKey& key) override { return Make(key.group, key.instance); }
    IOverlayTexture* GetTexture(std::uint32_t instance) override { return Make(kTextureGroup, instance); }
    void GetTextureSize(const IOverlayTexture* texture, std::int32_t& width, std::int32_t& height) override {
        const auto* t = dynamic_cast<const Texture*>(texture);
        width = t ? t->Width() : 1;
        height = t ? t->Height() : 1;
    }
    std::uint32_t ShadowTextureGroup() override { return kTextureGroup; }
private:
    static IOverlayTexture* Make(std::uint32_t group, std::uint32_t instance) {
        int width = 0, height = 0;
        if (!FshTextureSize(group, instance, width, height)) {
            if (Debug()) std::fprintf(stderr, "decals: texture %08x/%08x missing\n", group, instance);
            return nullptr;
        }
        return new Texture(FshTextureId(group, instance), width, height, false);
    }
} g_factory;

class Host final : public IOverlayHost {
public:
    cIGZMessageServer2* Messages() override { return nullptr; }   // the render-properties message only re-reads quality
    IOverlayTextureFactory* TextureFactory() override { return &g_factory; }
    IOverlayShadowService* ShadowService() override { return nullptr; }
    int RenderQuality() override { return 5; }
    int RenderSecondTextureStage() override { return 0; }
    bool RenderSpatialIndexValid() override { return false; }
    int TerrainCellCountX() override { return g_terrain ? static_cast<int>(g_terrain->CellCountX()) : 0; }
    int TerrainCellCountZ() override { return g_terrain ? static_cast<int>(g_terrain->CellCountZ()) : 0; }
    float TerrainCellSize() override { return 16.0f; }
    int TerrainCellCountInRect(const cRZRect& rect) override { return std::max(0, rect.right - rect.left) * std::max(0, rect.bottom - rect.top); }
    void RedisplayTerrain(int, const cRZRect&) override {}   // the draw pass below reads the overlays every frame
    void GetTerrainCullingInfo(int& rowBegin, int& rowEnd, const std::int32_t*& colStart, const std::int32_t*& colEnd) override {
        rowBegin = rowEnd = 0;
        colStart = colEnd = nullptr;
    }
    void DrawTerrainMeshSubset(bool, const cRZRect&, ITerrainVertBufExtn*) override {}
    void DrawWaterSurfaceSubset(bool, const cRZRect&) override {}
    void DrawMixedMeshSubset(bool, const cRZRect&) override {}
} g_host;

class EffectHost final : public IDecalEffectHost {
public:
    bool HasTerrainView() override { return g_managers[0] != nullptr; }
    cSTEOverlayManager* OverlayManager(int index) override { return index >= 0 && index < 4 ? g_managers[index] : nullptr; }
    float CityWorldSizeX() override { return g_city ? g_city->SizeX() : 0.0f; }
    float CityWorldSizeZ() override { return g_city ? g_city->SizeZ() : 0.0f; }
} g_effectHost;

// V3F_C4UB_2T2F (the shadow pass's format; the second set is unused here).
struct Vertex { float x, y, z; std::uint8_t rgba[4]; float u, v, u1, v1; };

std::uint8_t Byte(float value) { return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f); }

// DrawDecals (0x10072aa56): depth offset 2, then per enabled decal overlay its draw mode's render state, its texture (clamped, or
// repeated when the transform carries repeats), its colour and alpha; the triangles cover the overlay's cell rectangle on the
// terrain (the water managers on the water surface).
class DrawObject final : public cISC4ViewObject3D {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override { return --refs_; }
    void DrawViewObject(SC4DrawContext& context) override {
        if (!g_terrain || !g_managers[0]) return;
        const int maxX = static_cast<int>(g_terrain->CellCountX()), maxZ = static_cast<int>(g_terrain->CellCountZ());
        const float sea = g_terrain->GetSeaLevel();
        bool begun = false;
        int drawn = 0;
        std::vector<Vertex> triangles;
        for (int index = 0; index < 4; ++index) {
            cSTEOverlayManager* manager = g_managers[index];
            if (!manager) continue;
            const bool water = (index & 1) != 0;
            auto& slots = manager->Overlays();
            for (std::uint32_t i = 0; i < slots.size(); ++i) {
                const cOverlay& overlay = slots[i];
                if (overlay.freeLink != -1 || (overlay.flags & 0x11u) != 0x11u || !overlay.texture) continue;   // DrawDecals' mask without smoothing
                const int x0 = std::max(0, overlay.rect.left), z0 = std::max(0, overlay.rect.top);
                // DrawRect (0x10072b342) hands the terrain view the rect with right / bottom + 1: the stored edges are inclusive cells
                const int x1 = std::min(maxX, overlay.rect.right + 1), z1 = std::min(maxZ, overlay.rect.bottom + 1);
                static int listed = 0;
                if (Debug() && std::getenv("SC4_DECALDEBUG")[0] == '2' && listed++ < 400)
                    std::fprintf(stderr, "decal overlay m%d #%u rect %d,%d..%d,%d alpha %.2f rgb %.2f %.2f %.2f mode %u flags %llx tex %d\n", index, i,
                                 overlay.rect.left, overlay.rect.top, overlay.rect.right, overlay.rect.bottom, overlay.alpha, overlay.color[0], overlay.color[1],
                                 overlay.color[2], overlay.drawMode, static_cast<unsigned long long>(overlay.flags), overlay.texture->GlTextureId());
                if (x1 <= x0 || z1 <= z0) continue;
                const std::uint8_t rgba[4] = {Byte(overlay.color[0]), Byte(overlay.color[1]), Byte(overlay.color[2]), Byte(overlay.alpha)};
                if (rgba[3] == 0) continue;
                auto vertex = [&](int x, int z) {
                    const float wx = static_cast<float>(x) * 16.0f, wz = static_cast<float>(z) * 16.0f;
                    float wy = g_terrain->GetAltitudeAtVertex(x, z);
                    if (water) wy = std::max(wy, sea);
                    const cS3DMatrix4& m = overlay.texMatrix;   // row vectors: (x, y, z, 1) * m
                    Vertex out{wx, wy, wz, {rgba[0], rgba[1], rgba[2], rgba[3]}, 0, 0, 0, 0};
                    out.u = wx * m.m[0][0] + wy * m.m[1][0] + wz * m.m[2][0] + m.m[3][0];
                    out.v = wx * m.m[0][1] + wy * m.m[1][1] + wz * m.m[2][1] + m.m[3][1];
                    return out;
                };
                triangles.clear();
                for (int z = z0; z < z1; ++z)
                    for (int x = x0; x < x1; ++x) {
                        const Vertex a = vertex(x, z), b = vertex(x + 1, z), c = vertex(x, z + 1), d = vertex(x + 1, z + 1);
                        triangles.insert(triangles.end(), {a, b, c, b, d, c});
                    }
                if (!begun) {
                    context.ResetModelViewTransform();
                    context.SetDepthOffset(2);
                    begun = true;
                }
                context.SetRenderState(manager->RenderStates()[overlay.drawMode < 5 ? overlay.drawMode : 0]);
                context.SetTexture(overlay.texture->GlTextureId(), 0);
                context.SetTexFiltering(1, 1, 0);
                const int wrap = (overlay.flags & (kOverlayFlagHeightMap | kOverlayFlagTransformed)) ? 3 : 2;   // GD 2 clamp, 3 repeat
                context.SetTexWrapModes(wrap, (overlay.flags & kOverlayFlagHeightMap) ? 2 : wrap, 0);
                for (std::size_t n = 0; n < triangles.size(); n += 3000)
                    context.DrawPrims(0, 11, static_cast<int>(std::min<std::size_t>(3000, triangles.size() - n)), triangles.data() + n);
                ++drawn;
            }
        }
        if (!begun) return;
        context.SetDepthOffset(0);
        context.SetDefaultRenderState();
        static int reported = 0;
        if (Debug() && (reported++ < 5 || reported % 300 == 0)) std::fprintf(stderr, "decals: %d overlays drawn\n", drawn);
    }
private:
    std::uint32_t refs_ = 1;
};
DrawObject* g_draw = nullptr;

}  // namespace

void AttachDecalOverlays(cSC43DRender& render, cSC4City& city, cSTETerrain& terrain) {
    DetachDecalOverlays();
    g_terrain = &terrain;
    g_city = &city;
    for (int index = 0; index < 4; ++index) {
        g_managers[index] = new cSTEOverlayManager(&g_host);   // one reference (the constructor's)
        g_managers[index]->Init(nullptr, index < 2, (index & 1) != 0);
    }
    SetDecalEffectHost(&g_effectHost);
    if (!g_draw) g_draw = new DrawObject();
    render.AddViewObject(g_draw, 5, 151);   // after the models and the shadows (150): draw mode 3 shows where they hide the ground
    if (Debug()) std::fprintf(stderr, "decals: four overlay managers for %dx%d cells\n", static_cast<int>(terrain.CellCountX()), static_cast<int>(terrain.CellCountZ()));
}

void DetachDecalOverlays() {
    SetDecalEffectHost(nullptr);
    for (cSTEOverlayManager*& manager : g_managers) {
        if (!manager) continue;
        manager->Shutdown();
        manager->Release();   // a decal effect still holding one keeps it alive until it stops
        manager = nullptr;
    }
    g_terrain = nullptr;
    g_city = nullptr;
}

cSTEOverlayManager* DecalOverlayManager(int index) { return index >= 0 && index < 4 ? g_managers[index] : nullptr; }

IOverlayTexture* CreateFootprintTexture(int size, int marginX, int marginZ) {
    if (size <= 0) return nullptr;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(size) * static_cast<std::size_t>(size), 0u);   // 0xAARRGGBB = BGRA bytes
    auto fill = [&](std::uint32_t colour, int x0, int z0, int x1, int z1) {
        for (int z = std::max(0, z0); z < std::min(size, z1); ++z)
            for (int x = std::max(0, x0); x < std::min(size, x1); ++x) pixels[static_cast<std::size_t>(z) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x)] = colour;
    };
    fill(0xffffffffu, marginX, marginZ, size - marginX, size - marginZ);
    fill(0xaaffffffu, marginX + 2, marginZ + 2, size - (marginX + 2), size - (marginZ + 2));
    const unsigned id = CreateBgraTexture(size, size, pixels.data());
    if (id == 0) return nullptr;
    return new Texture(id, size, size, true);
}

}  // namespace native
