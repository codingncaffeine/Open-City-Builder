#include "app/NativeLighting.h"
#include "sc4/LightingManager.h"
#include "app/NativeTerrainView.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "app/NativeUi.h"
#include "render/gd/IGZGDriver.h"
#include "s3d/FSH.h"
#include "gz/cGZPersistResourceManager.h"
#include "sc4/DrawContext.h"
#include "sc4/Exemplar.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Render3D.h"
#include "sc4/ViewUtilities.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/cSTETerrain.h"
#include "sc4/cSTETerrainView3D.h"

namespace native {
extern sc4::cSTETerrainView3D* g_terrainView;
namespace {

constexpr std::uint32_t kFshType = 0x7ab50e44, kFshGroup = 0x891b0e1a;

// The view's message-server registrations (cSTETerrainView3D::Init: grid toggle, terrain and water changed, camera
// changes, night factor). Without them the view never re-read the heights a brush changed.
class ViewMessageTarget final : public cIGZMessageTarget2 {
public:
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 0x452294aa && iid != 1) return false;   // cIGZMessageTarget2
        *out = static_cast<cIGZMessageTarget2*>(this);
        return true;
    }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool DoMessage(cIGZMessage2* message) override {
        if (!g_terrainView || !message) return true;
        sc4::TerrainViewMessage m;
        m.type = message->GetType();
        void* standard = nullptr;
        if (message->QueryInterface(kGZIID_cIGZMessage2Standard, &standard) && standard) {
            auto* s = static_cast<cIGZMessage2Standard*>(standard);
            m.data1 = static_cast<std::uint32_t>(s->GetData1());
            m.data2 = static_cast<std::uint32_t>(s->GetData2());
            s->Release();
        }
        if (std::getenv("SC4_TVDEBUG")) std::fprintf(stderr, "terrain view: message %08x %08x %08x\n", m.type, m.data1, m.data2);
        return g_terrainView->DoMessage(m);
    }
} g_viewMessages;

gd::cIGZGDriver* g_driver = nullptr;

// FSH frames -> driver textures (RGBA8, base level), one binding per resource.
std::shared_ptr<sc4::TerrainTextureBinding> LoadFsh(std::uint32_t instance, std::uint32_t group = kFshGroup) {
    std::vector<std::uint8_t> bytes;
    if (!LoadResourceBytes(kFshType, group, instance, bytes)) return nullptr;
    s3d::FshFile file;
    if (!file.Open(bytes.data(), bytes.size())) return nullptr;
    auto binding = std::make_shared<sc4::TerrainTextureBinding>();
    for (std::uint32_t i = 0; i < file.FrameCount(); ++i) {
        std::vector<std::uint8_t> rgba;
        int w = 0, h = 0;
        if (std::getenv("SC4_DUMPFSH")) { s3d::FshFrameInfo info; if (file.GetFrame(i, info)) std::fprintf(stderr, "fsh %08x frame %u code %#x %dx%d mips %u tag %08x\n", instance, i, info.code, info.width, info.height, info.mipCount, info.tag); }
        if (!file.DecodeRgba8(i, rgba, w, h)) { binding->ids.push_back(0); continue; }
        if (std::getenv("SC4_DUMPFSH")) {
            char name[96];
            std::snprintf(name, sizeof name, "/tmp/fsh_%08x_%u.ppm", instance, i);
            if (FILE* f = std::fopen(name, "wb")) {
                std::fprintf(f, "P6 %d %d 255\n", w, h);
                for (std::size_t q = 0; q + 3 < rgba.size(); q += 4) std::fwrite(&rgba[q], 1, 3, f);
                std::fclose(f);
            }
        }
        for (std::size_t p = 0; p + 3 < rgba.size(); p += 4) std::swap(rgba[p], rgba[p + 2]);   // RGBA -> BGRA (ARGB8888, little endian)
        // the buffer cache's calling convention (gz/BufferSimGLCache.cpp): internal format 4 = 32-bit, upload format 3, pixel type 1
        const std::uint32_t id = g_driver->CreateTexture(4, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 1, 1);
        g_driver->LoadTextureLevel(id, 0, 0, 0, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 3, 1, static_cast<std::uint32_t>(w), rgba.data());
        binding->ids.push_back(static_cast<int>(id));
    }
    return binding;
}

// A gradient strip (the water colour ramps): frame 0 of an FSH, read pixel by pixel as 0x00RRGGBB.
class FshPixels final : public sc4::IPixelBuffer {
public:
    explicit FshPixels(const std::vector<std::uint8_t>& bytes) {
        s3d::FshFile file;
        if (file.Open(bytes.data(), bytes.size())) file.DecodeRgba8(0, rgba_, w_, h_);
    }
    bool Valid() const { return w_ > 0 && h_ > 0; }
    int Width() const override { return w_; }
    int Height() const override { return h_; }
    std::uint32_t Pixel(int x, int y) const override {
        if (x < 0 || y < 0 || x >= w_ || y >= h_) return 0;
        const std::uint8_t* p = &rgba_[(static_cast<std::size_t>(y) * w_ + x) * 4];
        return (static_cast<std::uint32_t>(p[0]) << 16) | (static_cast<std::uint32_t>(p[1]) << 8) | p[2];
    }
private:
    std::vector<std::uint8_t> rgba_;
    int w_ = 0, h_ = 0;
};

class ViewHost final : public sc4::ITerrainViewHost {
public:
    std::shared_ptr<sc4::TerrainTextureBinding> LoadWaterTexture(int zoom, bool polluted) override {
        // 0x10074d478: {FSH, 0x891b0e1a, 0x9187300 + zoom} (polluted: 0x9187400 + zoom)
        return LoadFsh(static_cast<std::uint32_t>((polluted ? 0x9187400 : 0x9187300) + zoom));
    }
    std::shared_ptr<sc4::TerrainTextureBinding> LoadLotTexture() override { return LoadFsh(0x912220f, 0x1abe787d); }
    std::shared_ptr<sc4::IPixelBuffer> GetBuffer(std::uint32_t instance, int) override {
        std::vector<std::uint8_t> bytes;
        if (!LoadResourceBytes(kFshType, kFshGroup, instance, bytes)) return nullptr;
        auto buffer = std::make_shared<FshPixels>(bytes);
        return buffer->Valid() ? buffer : nullptr;
    }
    void SetTerrain(sc4::cSTETerrain* terrain) { terrain_ = terrain; }
    void ApplyTerrainLighting(const sc4::TerrainRect& rect) override { if (terrain_) terrain_->DoLighting(rect); }
    void RegisterMessage(std::uint32_t type) override { if (SL::spMS2) SL::spMS2->AddNotification(&g_viewMessages, type); }
    void UnregisterMessage(std::uint32_t type) override { if (SL::spMS2) SL::spMS2->RemoveNotification(&g_viewMessages, type); }
    bool HasLighting() const override { return true; }
    void GetNormalLight(const float normal[3], float rgba[4]) const override {
        if (sc4::cSC4LightingManager* lighting = native::LightingManager()) {
            const cS3DVector3 c = lighting->GetColor(cS3DVector3{normal[0], normal[1], normal[2]});
            rgba[0] = c.x; rgba[1] = c.y; rgba[2] = c.z; rgba[3] = 1.0f;
            return;
        }
        const float dot = normal[0] * 0.35f + normal[1] * 0.80f + normal[2] * 0.45f;
        const float v = 0.55f + 0.45f * (dot > 0.0f ? dot : 0.0f);
        rgba[0] = rgba[1] = rgba[2] = v > 1.0f ? 1.0f : v;
        rgba[3] = 1.0f;
    }
    std::shared_ptr<sc4::TerrainTextureBinding> LoadTerrainTexture(std::uint32_t instance) override {
        auto it = cache_.find(instance);
        if (it != cache_.end()) return it->second;
        auto binding = LoadFsh(instance);
        cache_[instance] = binding;
        return binding;
    }
    // The terrain view's tuning exemplar {0x6534284a, 0x88cd66e9, 1} (cSTETerrainView3D::ReadTuningParameters @ 0x10074e18c).
    bool HasTuningExemplar() override {
        const bool has = Tuning() != nullptr;
        if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "terrain view: tuning exemplar %s\n", has ? "found" : "MISSING");
        return has;
    }
    bool ReadTuningFloat(std::uint32_t id, float& out) override {
        cIGZVariant* value = Property(id);
        return value && value->GetValFloat32(out);
    }
    bool ReadTuningBool(std::uint32_t id, bool& out) override {
        cIGZVariant* value = Property(id);
        std::uint8_t byte = 0;
        if (!value || !value->GetValUint8(byte)) return false;
        out = byte != 0;
        return true;
    }
    bool ReadTuningFloatVector(std::uint32_t id, std::vector<float>& out) override {
        cIGZVariant* value = Property(id);
        if (!value) return false;
        const float* data = value->RefFloat32();
        out.assign(data, data + value->GetCount());
        return true;
    }
    const std::vector<std::uint32_t>& TexturesUsedInTerrainType(int terrainType) const override {
        return sc4::cSTETerrain::TexturesUsedInTerrainType(terrainType);
    }

private:
    sc4::cISCPropertyHolder* Tuning() {
        if (!tuning_ && SL::spRM) {
            void* resource = nullptr;
            const bool got = SL::spRM->GetResource(cGZPersistResourceKey{0x6534284a, 0x88cd66e9, 1}, 0xa52160f5, &resource, 0, nullptr);
            if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "terrain view: tuning GetResource %d %p\n", int(got), resource);
            if (got && resource) tuning_ = static_cast<sc4::cISCResExemplar*>(resource)->AsISCPropertyHolder();
        }
        return tuning_;
    }
    cIGZVariant* Property(std::uint32_t id) {
        sc4::cISCPropertyHolder* holder = Tuning();
        sc4::cISCProperty* property = holder ? holder->GetProperty(id) : nullptr;
        return property ? property->GetPropertyValue() : nullptr;
    }
    sc4::cISCPropertyHolder* tuning_ = nullptr;
    sc4::cSTETerrain* terrain_ = nullptr;
    std::map<std::uint32_t, std::shared_ptr<sc4::TerrainTextureBinding>> cache_;
};

// ITerrainDrawContext over the recovered SC4DrawContext and the renderer's view utilities.
class DrawContext final : public sc4::ITerrainDrawContext {
public:
    DrawContext(SC4DrawContext& dc, SC4ViewUtilities& vu, int pass) : dc_(dc), vu_(vu), pass_(pass) {
        Frustum(draw_, vu.DrawFrustumBounds(), vu.DrawEdges(), vu.DrawPlanes());
        Frustum(view_, vu.ViewFrustumBounds(), vu.ViewEdges(), vu.ViewPlanes());
    }
    int Zoom() const override { return vu_.Zoom(); }
    int Rotation() const override { return vu_.Rotation(); }
    int Pass() const override { return pass_; }
    const sc4::TerrainFrustumPlanes& DrawFrustum() const override { return draw_; }
    const sc4::TerrainFrustumPlanes& ViewFrustum() const override { return view_; }
    void GetVisibleEdges(int& a, int& b) const override { vu_.GetVisibleEdges(a, b); }
    void SetRenderState(const cSC4PackedRenderState& state) override { dc_.SetRenderState(state); }
    void SetTexture(int texture, int stage) override { dc_.SetTexture(texture, stage); }
    void SetTexFiltering(int a, int b, int stage) override { dc_.SetTexFiltering(a, b, stage); }
    void SetTexWrapModes(int u, int v, int stage) override { dc_.SetTexWrapModes(u, v, stage); }
    void SetTexEnvMode(int mode, int stage) override { dc_.SetTexEnvMode(mode, stage); }
    void SetTexTransform4(const float m[16], int stage) override {
        cS3DMatrix4 matrix;
        std::memcpy(&matrix, m, sizeof matrix);
        dc_.SetTexTransform4(matrix, stage);
    }
    void ClearTexTransform(int stage) override { dc_.ClearTexTransform(stage); }
    void ResetModelViewTransform() override { dc_.ResetModelViewTransform(); }
    void GetModelViewMatrix(float m[16]) const override { dc_.GetModelViewMatrix(m); }
    void SetDepthOffset(int offset) override { dc_.SetDepthOffset(offset); }
    void SetDepthFunc(int function) override { dc_.SetDepthFunc(function); }
    void SetBlendFunc(int s, int d) override { dc_.SetBlendFunc(s, d); }
    void EnableTextureStateFlag(bool on, int stage) override { dc_.EnableTextureStateFlag(on, stage); }
    void EnableBlendStateFlag(bool on) override { dc_.EnableBlendStateFlag(on); }
    void EnableCullFaceFlag(bool on) override { dc_.EnableCullFaceFlag(on); }
    void EnableDepthMaskFlag(bool on) override { dc_.EnableDepthMaskFlag(on); }
    void SetVertexBuffer(int format, int count, const void* vertices) override { dc_.SetVertexBuffer(format, static_cast<std::uint32_t>(count), vertices); }
    void SetIndexBuffer(int count, const std::uint16_t* indices) override { dc_.SetIndexBuffer(static_cast<std::uint32_t>(count), indices); }
    void DrawPrimsIndexed(int primitive, int first, int count) override { dc_.DrawPrimsIndexed(primitive, first, count); }
    void DrawPrims(int mode, int format, int count, const void* vertices) override { dc_.DrawPrims(mode, format, count, vertices); }

private:
    static void Frustum(sc4::TerrainFrustumPlanes& out, const FrustumBounds& f, const ViewEdgeBlock& e, const ViewPlaneBlock& p) {
        out.minZ = f.clippedMin[2];
        out.maxZ = f.clippedMax[2];
        out.leftOrigin[0] = e.leftOriginX; out.leftOrigin[1] = e.leftOriginZ;
        out.leftDir[0] = e.leftDirX; out.leftDir[1] = e.leftDirZ;
        out.rightOrigin[0] = e.rightOriginX; out.rightOrigin[1] = e.rightOriginZ;
        out.rightDir[0] = e.rightDirX; out.rightDir[1] = e.rightDirZ;
        out.planeA = p.up[0]; out.planeB = p.up[1]; out.planeC = p.up[2]; out.planeD = p.upD;   // the bottom edge plane (corner 2): terrain under it is not visible
    }
    SC4DrawContext& dc_;
    SC4ViewUtilities& vu_;
    int pass_;
    sc4::TerrainFrustumPlanes draw_, view_;
};

// One pass of the terrain view as a renderer view object.
class PassObject final : public cISC4ViewObject3D {
public:
    PassObject(sc4::cSC43DRender& render, sc4::cSTETerrainView3D& view, int pass) : render_(render), view_(view), pass_(pass) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override { return --refs_; }
    void DrawViewObject(SC4DrawContext& context) override {
        SC4ViewUtilities* utilities = render_.GetViewUtilities();
        if (!utilities) return;
        DrawContext draw(context, *utilities, pass_);
        if (std::getenv("SC4_DEBUG")) {
            static int printed = 0;
            if (printed++ < 4) {
                const sc4::TerrainFrustumPlanes& f = draw.DrawFrustum();
                std::fprintf(stderr, "terrain pass %d: zoom %d rot %d draw frustum z %.1f..%.1f left (%.1f,%.1f)+(%.1f,%.1f) right (%.1f,%.1f)+(%.1f,%.1f) plane %.3f %.3f %.3f %.3f\n", pass_, draw.Zoom(), draw.Rotation(), f.minZ, f.maxZ,
                             f.leftOrigin[0], f.leftOrigin[1], f.leftDir[0], f.leftDir[1], f.rightOrigin[0], f.rightOrigin[1], f.rightDir[0], f.rightDir[1], f.planeA, f.planeB, f.planeC, f.planeD);
            }
        }
        view_.Update(draw);
        if (std::getenv("SC4_DEBUG")) {
            static int cull = 0;
            if (cull++ < 2) {
                int b = 0, e = 0; const std::int32_t* cs = nullptr; const std::int32_t* ce = nullptr;
                view_.GetCullingInfo(&b, &e, &cs, &ce);
                std::fprintf(stderr, "terrain cull rows %d..%d first-row cols %d..%d last-row cols %d..%d\n", b, e, cs ? cs[b] : -1, ce ? ce[b] : -1, cs && e > 0 ? cs[e - 1] : -1, ce && e > 0 ? ce[e - 1] : -1);
            }
        }
    }

private:
    sc4::cSC43DRender& render_;
    sc4::cSTETerrainView3D& view_;
    int pass_;
    std::uint32_t refs_ = 1;
};

// The view as the terrain holds it (cISTETerrainView at +0x110): RedisplayTerrain re-lights through it, and with it
// set the terrain posts the "terrain changed" messages the view re-reads its heights from.
class TerrainViewLink final : public sc4::ITerrainView {
public:
    explicit TerrainViewLink(sc4::cSTETerrainView3D& view) : view_(view) {}
    cIGZSerializable* Serializable() override { return &view_; }
    bool Init(sc4::cSTETerrain* terrain) override { return view_.Init(terrain); }
    void Shutdown() override { view_.Shutdown(); }
    void MarkTextureSpanInfoDirty(std::int32_t firstRow, std::int32_t lastRow) override { sc4::cSTETerrainView3D::MarkTextureSpanInfoDirty(firstRow, lastRow); }
    void SetLightingUpdateRect(const sc4::TerrainRect* r) override { view_.SetLightingUpdateRect(r); }
    void ChangeTerrainType(std::int32_t terrainType) override { sc4::cSTETerrainView3D::ChangeTerrainType(terrainType); }
    void SetUpTexturesForCurrentTerrainTypeAndZoom(std::int32_t zoom) override { sc4::cSTETerrainView3D::SetUpTexturesForCurrentTerrainTypeAndZoom(zoom); }
    float MaxDisplayAltitude() const override { return view_.DisplayAltitudeHigh(); }
    float MinDisplayAltitude() const override { return view_.DisplayAltitudeLow(); }
    cIGZUnknown* OverlayManager(std::uint32_t) override { return nullptr; }
private:
    sc4::cSTETerrainView3D& view_;
};

}  // namespace

sc4::cSTETerrainView3D* g_terrainView = nullptr;

bool TerrainCullingInfo(int& rowBegin, int& rowEnd, const std::int32_t*& colStart, const std::int32_t*& colEnd) {
    if (!g_terrainView) return false;
    g_terrainView->GetCullingInfo(&rowBegin, &rowEnd, &colStart, &colEnd);
    return colStart && colEnd;
}

bool AttachTerrainView(sc4::cSC43DRender& render, sc4::cSTETerrain& terrain, gd::cIGZGDriver* driver) {
    g_driver = driver;
    static ViewHost host;
    host.SetTerrain(&terrain);
    if (std::getenv("SC4_DEBUG")) {
        for (int t = 0; t < 5; ++t) std::fprintf(stderr, "terrain type %d uses %zu textures\n", t, sc4::cSTETerrain::TexturesUsedInTerrainType(t).size());
        std::fprintf(stderr, "terrain type of this city: %d\n", terrain.TerrainType());
    }
    sc4::cSTETerrainView3D::StaticInit(&host);
    sc4::cSTETerrainView3D::SetTerrainSize(terrain.CellCountX(), terrain.CellCountZ());
    auto* view = new sc4::cSTETerrainView3D(&host);
    view->AddRef();
    {   // the surface textures and colours are derived data (the file stores altitudes and flags only): compute them for the whole grid
        sc4::TerrainRect all{0, 0, static_cast<std::int32_t>(terrain.CellCountX()), static_cast<std::int32_t>(terrain.CellCountZ())};
        terrain.AssignSurfaceTexturesAndColors(all);
        if (std::getenv("SC4_DEBUG")) {
            const auto& verts = terrain.Vertices();
            for (std::size_t idx : {std::size_t(129 * 64 + 64), std::size_t(129 * 30 + 40), std::size_t(129 * 100 + 90)}) {
                const auto& v = verts[idx];
                std::fprintf(stderr, "terrain vertex %zu alt %.1f surface:", idx, v.altitude);
                for (int k = 0; k < 0x12; ++k) std::fprintf(stderr, " %02x", v.surface[k]);
                std::fprintf(stderr, "\n");
            }
        }
    }
    const bool ok = view->Init(&terrain);
    std::printf("city view: terrain view %s\n", ok ? "up" : "FAILED");
    if (!ok) return false;
    g_terrainView = view;
    // DoPostCityInit's hook-up: the terrain gets its view and redisplays from here on (brushes, levelling, reconcile).
    static TerrainViewLink* link = nullptr;
    delete link;
    link = new TerrainViewLink(*view);
    terrain.SetView(link);
    terrain.SetDisplayEnabled(true);
    // AddViewObject's list ids: 2 = drawn before the static view (view pass 0: the terrain), 0 = after it (view pass 2: the water)
    render.AddViewObject(new PassObject(render, *view, 0), 2, 100);
    render.AddViewObject(new PassObject(render, *view, 1), 0, 100);
    return true;
}

}  // namespace native
