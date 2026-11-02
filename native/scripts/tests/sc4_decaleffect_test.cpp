// cSC4DecalEffect against a real cSTEOverlayManager: the calls of Start / Update / Stop / SetFloatParams / SetVoidParams
// / SetTransforms as particles.md §15 traces them (the worked trace of §15.4 with flags 0, 0x08, 0x20 and 0x40, the lifetime
// table, the explicit rectangle and the whole-city placement).
#include <cmath>
#include <cstdio>

#include "sc4/DecalEffect.h"
#include "sc4/OverlayManager.h"

using namespace sc4;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

struct FakeTexture : IOverlayTexture {
    int refs = 1;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    int GlTextureId() const override { return 7; }
};
struct FakeFactory : IOverlayTextureFactory {
    std::uint32_t lastInstance = 0;
    FakeTexture texture;
    void AddRef() override {}
    void Release() override {}
    IOverlayTexture* GetTexture(const cGZPersistResourceKey&) override { texture.AddRef(); return &texture; }
    IOverlayTexture* GetTexture(std::uint32_t instance) override { lastInstance = instance; texture.AddRef(); return &texture; }
    void GetTextureSize(const IOverlayTexture*, std::int32_t& w, std::int32_t& h) override { w = 64; h = 64; }
    std::uint32_t ShadowTextureGroup() override { return 0; }
};
struct Host : IOverlayHost {
    FakeFactory factory;
    cIGZMessageServer2* Messages() override { return nullptr; }
    IOverlayTextureFactory* TextureFactory() override { return &factory; }
    IOverlayShadowService* ShadowService() override { return nullptr; }
    int RenderQuality() override { return 5; }
    int RenderSecondTextureStage() override { return 0; }
    bool RenderSpatialIndexValid() override { return false; }
    int TerrainCellCountX() override { return 64; }
    int TerrainCellCountZ() override { return 64; }
    float TerrainCellSize() override { return 16.0f; }
    int TerrainCellCountInRect(const cRZRect&) override { return 0; }
    void RedisplayTerrain(int, const cRZRect&) override {}
    void GetTerrainCullingInfo(int& a, int& b, const std::int32_t*& c, const std::int32_t*& d) override { a = b = 0; c = d = nullptr; }
    void DrawTerrainMeshSubset(bool, const cRZRect&, ITerrainVertBufExtn*) override {}
    void DrawWaterSurfaceSubset(bool, const cRZRect&) override {}
    void DrawMixedMeshSubset(bool, const cRZRect&) override {}
};
struct DecalHost : IDecalEffectHost {
    cSTEOverlayManager* managers[4] = {};
    int lastIndex = -1;
    bool HasTerrainView() override { return true; }
    cSTEOverlayManager* OverlayManager(int index) override { lastIndex = index; return managers[index]; }
    float CityWorldSizeX() override { return 1024.0f; }
    float CityWorldSizeZ() override { return 512.0f; }
};

// The worked trace's description: singleton curves size [3], rotation [0.25], aspect [0.5], opacity [0.75], RGB (0.25, 0.5, 1),
// mapping 4, offsets 0.125 / 0.25.
cSC4DecalDescription Trace(std::uint64_t flags, std::uint8_t lifetime = 0) {
    cSC4DecalDescription d;
    d.flags = flags;
    d.textureId = 0x13c14520;
    d.layer = 3;
    d.lifetime = lifetime;
    d.life = 4.0f;
    d.rotationCurve = {0.25f};
    d.sizeCurve = {3.0f};
    d.opacityCurve = {0.75f};
    d.colorCurve = {cS3DVector3{0.25f, 0.5f, 1.0f}};
    d.aspectCurve = {0.5f};
    d.mapping = 4.0f;
    d.offset = cS3DVector2{0.125f, 0.25f};
    return d;
}
cS3DTransform At(float x, float z, float scale) {
    cS3DTransform t;
    t.flags = 3;
    t.translation[0] = x;
    t.translation[2] = z;
    t.scale = scale;
    return t;
}

int main() {
    Host host;
    DecalHost decals;
    for (int i = 0; i < 4; ++i) {
        decals.managers[i] = new cSTEOverlayManager(&host);
        decals.managers[i]->Init(nullptr, i < 2, (i & 1) != 0);
    }
    SetDecalEffectHost(&decals);

    {   // flags 0, Start(0): AddDecal by instance at (10, 20), size 2*3, turns 0.25; the record, opacity, RGB, layer
        cSC4DecalEffect e;
        e.Init();
        e.SetDescription(Trace(0));
        e.SetTransforms(cS3DTransform{}, At(10.0f, 20.0f, 2.0f));
        e.Start(0);
        CHECK(e.IsStarted() && e.Handle() != 0xffffffffu && decals.lastIndex == 2 && host.factory.lastInstance == 0x13c14520);
        cDecalInfo info;
        e.Overlays()->DecalInfo(e.Handle(), &info);
        CHECK(Near(info.x, 10.0f) && Near(info.z, 20.0f) && Near(info.width, 6.0f) && Near(info.turns, 0.25f));
        CHECK(Near(info.aspectAdjust, 0.5f) && Near(info.uRepeat, 1.0f) && Near(info.vRepeat, 1.0f));
        CHECK(Near(info.uvOffset, 0.125f) && Near(info.userValue, 0.25f));
        CHECK(Near(e.Overlays()->OverlayAlpha(e.Handle()), 0.75f));
        cS3DVector3 rgb{};
        e.Overlays()->OverlayColor(e.Handle(), &rgb);
        CHECK(Near(rgb.x, 0.25f) && Near(rgb.y, 0.5f) && Near(rgb.z, 1.0f));
        CHECK(e.Overlays()->OverlayDrawMode(e.Handle()) == 3);
        cSTEOverlayManager* manager = e.Overlays();
        const std::uint32_t handle = e.Handle();
        e.Stop(0);
        CHECK(!e.IsStarted() && e.Handle() == 0xffffffffu && e.Overlays() == nullptr);
        CHECK(manager->Decals()[handle & 0x7fffffffu].freeLink != -1);   // the decal slot went back to the free list
    }
    {   // flags 8: the mapping words are mapping * 1 and that times the aspect
        cSC4DecalEffect e;
        e.SetDescription(Trace(8));
        e.Start(0);
        cDecalInfo info;
        e.Overlays()->DecalInfo(e.Handle(), &info);
        CHECK(Near(info.uRepeat, 4.0f) && Near(info.vRepeat, 2.0f));
        e.Stop(0);
    }
    {   // flags 0x20: a ring decal (eight overlays); flags 4 + 0x40: the dynamic water manager
        cSC4DecalEffect ring;
        ring.SetDescription(Trace(0x20));
        ring.Start(0);
        CHECK(ring.IsStarted() && ring.Overlays()->RingDecals().size() == 1);
        ring.Stop(0);
        cSC4DecalEffect water;
        water.SetDescription(Trace(0x44));
        water.Start(0);
        CHECK(water.IsStarted() && decals.lastIndex == 1);
        water.Stop(0);
    }
    {   // a texture handed over as void parameter 6 replaces the instance
        FakeTexture texture;
        cSC4DecalEffect e;
        e.SetDescription(Trace(0));
        void* values[1] = {&texture};
        CHECK(e.SetVoidParams(6, values, 1) && texture.refs == 2 && !e.SetVoidParams(5, values, 1));
        host.factory.lastInstance = 0;
        e.Start(0);
        CHECK(e.IsStarted() && host.factory.lastInstance == 0);
        e.Stop(0);
    }
    {   // the lifetime table: lifetime 2 refuses a non-zero Start; lifetime 3 starts at the end with a non-zero Start
        cSC4DecalEffect two;
        two.SetDescription(Trace(0, 2));
        two.Start(1);
        CHECK(!two.IsStarted());
        two.Start(0);
        CHECK(two.IsStarted());
        two.Update(4.0f);   // phase 1: not past it
        CHECK(two.IsStarted());
        two.Update(0.01f);  // past 1: lifetime 2 stops itself
        CHECK(!two.IsStarted() && two.Handle() == 0xffffffffu);
        cSC4DecalDescription d = Trace(0, 1);
        d.opacityCurve = {0.0f, 1.0f};
        cSC4DecalEffect loop;
        loop.SetDescription(d);
        loop.Start(0);
        loop.Update(5.0f);   // 1.25 wraps to 0.25
        CHECK(loop.IsStarted() && Near(loop.Overlays()->OverlayAlpha(loop.Handle()), 0.25f, 0.06f));
        loop.Stop(0);
    }
    {   // the explicit rectangle (cells 2,3 .. 6,5): centre (64, 64), width 64, aspect 0.5, no turn; transforms are ignored after
        cSC4DecalEffect e;
        e.SetDescription(Trace(0));
        const float rect[4] = {2.0f, 3.0f, 6.0f, 5.0f};
        CHECK(e.SetFloatParams(0xc, rect, 4));
        e.SetTransforms(cS3DTransform{}, At(500.0f, 500.0f, 9.0f));
        e.Start(0);
        cDecalInfo info;
        e.Overlays()->DecalInfo(e.Handle(), &info);
        CHECK(Near(info.x, 64.0f) && Near(info.z, 64.0f) && Near(info.width, 192.0f) && Near(info.aspectAdjust, 0.25f));
        const float opacity = 0.5f, size[2] = {0.0f, 0.5f};
        CHECK(e.SetFloatParams(3, &opacity, 1) && e.SetFloatParams(0x102, size, 2) && !e.SetFloatParams(4, &opacity, 1));
        e.Update(0.0f);
        e.Overlays()->DecalInfo(e.Handle(), &info);
        CHECK(Near(info.width, 96.0f) && Near(e.Overlays()->OverlayAlpha(e.Handle()), 0.375f));
        e.Stop(0);
    }
    {   // the whole city: X centre = width / 2, Z centre = depth / 2, size = width
        cSC4DecalEffect e;
        e.SetDescription(Trace(0x10));
        e.Start(0);
        cDecalInfo info;
        e.Overlays()->DecalInfo(e.Handle(), &info);
        CHECK(Near(info.x, 512.0f) && Near(info.z, 256.0f) && Near(info.width, 3072.0f));
        e.Stop(0);
    }
    {   // the curves: uniformly spaced keys, a single key is constant
        CHECK(Near(DecalCurve(std::vector<float>{2.0f}, 0.7f), 2.0f));
        CHECK(Near(DecalCurve(std::vector<float>{0.0f, 1.0f, 0.0f}, 0.25f), 0.5f) && Near(DecalCurve(std::vector<float>{0.0f, 1.0f, 0.0f}, 1.0f), 0.0f));
    }

    SetDecalEffectHost(nullptr);
    if (failures) {
        std::printf("FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("sc4_decaleffect: all ok\n");
    return 0;
}
