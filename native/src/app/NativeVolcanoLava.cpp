// The volcano's lava in the native game: cSTEVolcanoLava (sc4/VolcanoLava.h) over the city's terrain, fire manager,
// effects and overlay managers, drawn as a view object of the renderer (list 5, priority 1000) and ticked by the animation
// tick manager (priority 0xc0), as the original registers itself (StartLavaFlow @ 0x004fa97a).
//
// The drawing is Update @ 0x004fa640: the surface is the lava's vertex grid (vertex format 11) drawn row by row as indexed
// triangles with the animated surface texture of the zoom, self-lit; at the city's edge the side quads (format 10) with the
// side texture.
//
// The scorch mark under settling lava is a cSC4PropOccupantTerrainDecal (sc4/PropOccupantTerrainDecal), made and retired the
// original's way.
#include "app/NativeVolcanoLava.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <vector>

#include "app/NativeAnimationTicks.h"
#include "app/NativeCity.h"
#include "app/NativeDecalOverlay.h"
#include "app/NativeDisasterTools.h"
#include "app/NativeModelMaker.h"
#include "gz/cGZPersistResourceManager.h"
#include "gz/cIGZMessage2.h"
#include "sc4/DrawContext.h"
#include "sc4/OverlayManager.h"
#include "sc4/PackedRenderState.h"
#include "sc4/Render3D.h"
#include "sc4/AnimationTickManager.h"
#include "sc4/DisasterInstanceVolcano.h"
#include "sc4/DisasterManagers.h"
#include "sc4/Exemplar.h"
#include "sc4/ServiceLocator.h"
#include "sc4/VolcanoLava.h"
#include "app/NativeProps.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4FlammabilitySimulator.h"
#include "sc4/cSTETerrain.h"

namespace native {
namespace {

bool Debug() { static const bool on = std::getenv("SC4_LAVADEBUG") != nullptr; return on; }

constexpr std::uint32_t kTextureGroup = 0x891b0e1a;             // FSH {0x7ab50e44, ...}
constexpr std::uint32_t kSurfaceTextureInstance = 0x09909b00;   // + zoom
constexpr std::uint32_t kSideTextureInstance = 0x699ca236;
constexpr std::uint32_t kGradientInstance = 0xc9909fb8;
constexpr std::uint32_t kScorchTextureInstance = 0x0999d1b7;
constexpr cGZPersistResourceKey kTuningKey{0x6534284a, 0x88cd66e9, 5};

sc4::cSC43DRender* g_render = nullptr;

sc4::cSTETerrain* Terrain() {
    sc4::cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(sc4::CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* terrain = static_cast<sc4::cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
    static_cast<cIGZSerializable*>(raw)->Release();
    return terrain;
}

sc4::cSC4FlammabilitySimulator* Flammability() {
    sc4::cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(sc4::CityComponent::FlammabilitySimulator) : nullptr;
    return object ? dynamic_cast<sc4::cSC4FlammabilitySimulator*>(object) : nullptr;
}

std::uint32_t MessageData(cIGZMessage2* message, int which) {
    void* raw = nullptr;
    if (!message->QueryInterface(kGZIID_cIGZMessage2Standard, &raw) || !raw) return 0;
    auto* standard = static_cast<cIGZMessage2Standard*>(raw);
    const std::uint64_t value = which == 1 ? standard->GetData1() : which == 2 ? standard->GetData2() : standard->GetData3();
    standard->Release();
    return static_cast<std::uint32_t>(value);
}

// ------------------------------------------------------------------------------------------------- the gradient image
// spColorGradientBuf: the image's first column, top to bottom, as 0x00RRGGBB.
const std::vector<std::uint32_t>& Gradient() {
    static std::vector<std::uint32_t> column;
    static bool loaded = false;
    if (loaded) return column;
    loaded = true;
    std::vector<std::uint8_t> rgba;
    int width = 0, height = 0;
    if (FshPixelsRgba(kTextureGroup, kGradientInstance, rgba, width, height) && width > 0) {
        column.resize(static_cast<std::size_t>(height));
        for (int row = 0; row < height; ++row) {
            const std::uint8_t* p = &rgba[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) * 4];
            column[static_cast<std::size_t>(row)] = (std::uint32_t(p[0]) << 16) | (std::uint32_t(p[1]) << 8) | std::uint32_t(p[2]);
        }
    }
    if (Debug()) std::fprintf(stderr, "lava: gradient image %d x %d\n", width, height);
    return column;
}

// ------------------------------------------------------------------------------------------------- one lava
class NativeLava;
std::vector<NativeLava*> g_lavas;

class NativeLava final : public sc4::IVolcanoLava, public sc4::ILavaHost {
public:
    NativeLava() : lava_(this) { target_.owner = this; view_.owner = this; }
    ~NativeLava() override { lava_.Shutdown(); }

    bool Init() {
        if (!Terrain() || !HostCity()) return false;
        return lava_.Init();
    }
    // The city view goes: nothing of the city may be touched afterwards.
    void Detach() {
        lava_.Shutdown();
        detached_ = true;
    }

    // ---- IVolcanoLava (what the disaster instance drives)
    void StartLavaFlow(float x, float z, float ventRadius, bool burn) override {
        if (detached_ || !lava_.IsInitialized()) return;
        ventX_ = x;
        ventZ_ = z;
        lava_.StartLavaFlow(x, z, ventRadius, burn);
        if (Debug()) ReportVentTerrain("flow start");
        if (Debug())
            std::fprintf(stderr, "lava: flow started at (%.0f, %.0f) radius %.0f: vent vertices %d,%d..%d,%d, renderer %d, ticks %d\n", x, z, ventRadius,
                         lava_.StartRect().left, lava_.StartRect().top, lava_.StartRect().right, lava_.StartRect().bottom, int(lava_.Registered()),
                         int(lava_.Ticking()));
    }
    void SetLavaProductionRate(float rate) override { lava_.SetLavaProductionRate(rate); }
    void SetSolidificationRate(float rate) override { lava_.SetSolidificationRate(rate); }
    void Shutdown() override {
        if (Debug()) std::fprintf(stderr, "lava: shut down after %d ticks, %d draws\n", ticks_, draws_);
        lava_.Shutdown();
    }
    float GetLavaHeightByCityCell(int x, int z) override { return lava_.GetLavaHeightByCityCell(x, z); }
    void Release() override {
        if (--refs_ != 0) return;
        for (auto it = g_lavas.begin(); it != g_lavas.end(); ++it)
            if (*it == this) { g_lavas.erase(it); break; }
        delete this;
    }

    // Update @ 0x004fa640. The visible rectangle is the whole city here (the original cuts to the cells on screen, which only
    // saves drawing), and the terrain's visible rectangle is that widened by twelve cells as the original widens its own.
    void Draw(SC4DrawContext& context) {
        if (detached_ || !lava_.IsInitialized() || !lava_.HasLava()) return;
        const sc4::LavaRect visible{0, 0, cellsX_, cellsZ_};
        const sc4::LavaRect terrainVisible{-12, -12, cellsX_ + 12, cellsZ_ + 12};
        if (!lava_.ComputeDrawRect(visible, terrainVisible)) return;
        if (!stateReady_) {
            state_.PackRenderState(true, true, true, false, true, 1, true, 3, 4, 5, -1, -1.0f, 1, -1);
            stateReady_ = true;
        }
        context.SetRenderState(state_);
        context.ResetModelViewTransform();
        // DrawLavaSurface @ 0x004f9640
        lava_.SetDrawZoom(context.Zoom());
        const std::vector<std::uint32_t>& frames = FshFrameTextureIds(kTextureGroup, kSurfaceTextureInstance + static_cast<std::uint32_t>(Zoom()));
        if (!frames.empty()) context.SetTexture(static_cast<int>(frames[lava_.Frame() % frames.size()]), 0);
        const cS3DVector3 white{1.0f, 1.0f, 1.0f};
        context.SetSelfLitShade(white);
        lava_.CollectSurfaceRuns(runs_);
        const std::vector<sc4::LavaVertex>& vertices = lava_.Vertices();
        for (const sc4::LavaRun& run : runs_) {
            context.SetVertexBuffer(11, static_cast<std::uint32_t>(lava_.VertsX()), &vertices[static_cast<std::size_t>(run.row * lava_.VertsX())]);
            context.DrawPrimsIndexed(0, run.cellCount * 6, &indices_[static_cast<std::size_t>(run.firstCell) * 6]);
        }
        context.ResetShade();
        // DrawSides @ 0x004f87d0
        lava_.CollectSideQuads(quads_);
        if (!quads_.empty()) {
            if (const unsigned int side = FshTextureId(kTextureGroup, kSideTextureInstance)) context.SetTexture(static_cast<int>(side), 0);
            for (const sc4::LavaSideQuad& quad : quads_) context.DrawPrims(6, 10, 4, quad.vertices);
        }
        ++draws_;
        if (Debug() && (draws_ == 1 || draws_ % 300 == 0))
            std::fprintf(stderr, "lava: draw %d: vertices %d,%d..%d,%d, %zu runs, %zu side quads, frame %u of %zu at zoom %d\n", draws_, lava_.DrawRect().left,
                         lava_.DrawRect().top, lava_.DrawRect().right, lava_.DrawRect().bottom, runs_.size(), quads_.size(), lava_.Frame(), frames.size(),
                         lava_.TextureZoom());
    }

    // ---- ILavaHost: the city and the terrain
    std::int32_t CityCellCountX() override {
        cellsX_ = static_cast<std::int32_t>(HostCity()->CellCountX());
        sc4::cSTEVolcanoLava::TriangleIndices(cellsX_, indices_);
        return cellsX_;
    }
    std::int32_t CityCellCountZ() override { return cellsZ_ = static_cast<std::int32_t>(HostCity()->CellCountZ()); }
    float CellWidth() override { return sc4::cSTETerrain::CellWidth(); }
    std::uint8_t GetNeighbors(std::int32_t x, std::int32_t z) override { return Terrain()->GetNeighbors(x, z); }
    std::int32_t NeighborOffset(int direction) override { return Terrain()->NeighborOffset(direction); }
    std::int32_t TerrainStride() override { return static_cast<std::int32_t>(Terrain()->VertexCountX()); }
    std::int32_t TerrainMaxX() override { return static_cast<std::int32_t>(Terrain()->CellCountX()); }
    std::int32_t TerrainMaxZ() override { return static_cast<std::int32_t>(Terrain()->CellCountZ()); }
    float TerrainAltitude(std::int32_t index) override { return Terrain()->GetAltitude(index); }
    float WaterLevel(std::int32_t index) override { return Terrain()->GetWaterLevel(index); }
    bool IsWaterVertex(std::int32_t index) override { return Terrain()->IsWaterVertex(index); }
    void RaiseVertex(std::uint32_t index, float amount) override { Terrain()->RaiseVertex(index, amount); }
    void PadRectangle(sc4::LavaRect& rect, int amount) override {
        sc4::TerrainRect r{rect.left, rect.top, rect.right, rect.bottom};
        Terrain()->PadRectangle(r, amount);
        rect = {r.left, r.top, r.right, r.bottom};
    }
    void CreateVertRectAroundVertex(sc4::LavaRect& rect, int x, int z, int radius) override {
        sc4::TerrainRect r;
        Terrain()->CreateVertRectAroundVertex(r, x, z, radius);
        rect = {r.left, r.top, r.right, r.bottom};
    }
    void TerrainRegionChanged(const sc4::LavaRect& changed) override {
        Terrain()->RedisplayTerrain(true, true, sc4::TerrainRect{changed.left, changed.top, changed.right, changed.bottom}, 3);
    }
    void TerrainPeriodicRefresh() override { if (sc4::cSTETerrain* terrain = Terrain()) terrain->StopAccumulatingTextureUpdates(); }
    void TerrainRefresh() override { Terrain()->StartAccumulatingTextureUpdates(); }
    const sc4::cISCPropertyHolder* LoadTuningExemplar() override {
        static sc4::cISCResExemplar* exemplar = nullptr;   // held: the tuning is read once
        if (!exemplar && SL::spRM) {
            void* resource = nullptr;
            cGZPersistResourceKey key = kTuningKey;
            if (SL::spRM->GetResource(key, sc4::kGZIID_cISCResExemplar, &resource, 0, nullptr) && resource) exemplar = static_cast<sc4::cISCResExemplar*>(resource);
        }
        if (Debug()) std::fprintf(stderr, "lava: tuning exemplar %s\n", exemplar ? "read" : "missing");
        return exemplar ? exemplar->AsISCPropertyHolder() : nullptr;
    }

    // ---- ILavaHost: steam
    void* StartSteam(std::int32_t key, const float xyz[3]) override {
        void* effect = NativeCreateEffectWithFinishMessage("lava_meets_water", xyz, sc4::kLavaMessageSteamEnded, static_cast<std::uint32_t>(key));
        if (Debug()) std::fprintf(stderr, "lava: steam %d at (%.0f, %.0f, %.0f) -> %s\n", key, xyz[0], xyz[1], xyz[2], effect ? "started" : "none");
        return effect;
    }
    void StopSteam(void* steam, bool hard) override { NativeEndEffectHandle(steam, hard ? 1 : 0); }
    void ReleaseSteam(void* steam) override { NativeEndEffectHandle(steam, -1); }

    // ---- ILavaHost: scorch marks (AddDecal @ 0x004f9986): a cSC4PropOccupantTerrainDecal, Init, the scorch texture at the size,
    // SetOpacityFade(fadeIn, keep, 0, 1), into the occupant manager, SetPosition; the lava keeps one reference.
    void* AddScorchDecal(const float xyz[3], float size, float fadeIn) override {
        void* decal = NativeCreateFadingDecal(cGZPersistResourceKey{0x7ab50e44, kTextureGroup, kScorchTextureInstance}, size, fadeIn, xyz);
        ++decalsMade_;
        if (Debug() && decalsMade_ <= 3) std::fprintf(stderr, "lava: scorch decal at (%.0f, %.0f) size %.0f\n", xyz[0], xyz[2], size);
        return decal;
    }
    // TickDecals @ 0x004f9b7a: the expired decal fades out (1 to 0) with the completion action, and the lava lets go of it.
    void RetireScorchDecal(void* handle, float fadeOut, int completionAction) override { NativeRetireFadingDecal(handle, fadeOut, completionAction); }

    // ---- ILavaHost: fire
    bool IsCellFlammable(std::int32_t x, std::int32_t z) override {
        sc4::cSC4FlammabilitySimulator* flammability = Flammability();
        const bool yes = flammability && flammability->IsCellFlammable(x, z);
        ++flammableAsked_;
        if (yes) ++flammableYes_;
        if (!flammability) ++noFlammability_;
        return yes;
    }
    void IgniteCell(std::int32_t x, std::int32_t z) override {
        if (sc4::cSC4DisasterManagerFire* fire = sc4::cSC4DisasterManagerFire::Instance()) {
            const bool lit = fire->IgniteCell(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z), false, true, 0);
            if (lit) ++ignited_;
            else ++igniteRefused_;
        } else {
            ++noFireManager_;
        }
    }
    void QuickBurnCell(std::int32_t x, std::int32_t z) override {
        if (sc4::cSC4DisasterManagerFire* fire = sc4::cSC4DisasterManagerFire::Instance()) {
            fire->QuickBurnCell(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z));
            ++quickBurned_;
        }
    }
    void SetFireCause(std::uint32_t cause) override {
        if (sc4::cSC4DisasterManagerFire* fire = sc4::cSC4DisasterManagerFire::Instance()) fire->SetCause(cause);
    }

    // ---- ILavaHost: display and registration
    std::int32_t ColorGradientLastRow() override { return static_cast<std::int32_t>(Gradient().size()) - 1; }
    std::uint32_t ColorGradientPixel(std::int32_t row) override { return Gradient()[static_cast<std::size_t>(row)]; }
    std::uint32_t SurfaceFrameCount(std::int32_t zoom) override {
        if (zoom < 0 || zoom >= sc4::kLavaZoomCount) return 0;
        return static_cast<std::uint32_t>(FshFrameTextureIds(kTextureGroup, kSurfaceTextureInstance + static_cast<std::uint32_t>(zoom)).size());
    }
    std::int32_t CurrentZoom() override {
        SC4ViewUtilities* view = g_render ? g_render->GetViewUtilities() : nullptr;
        return view ? view->Zoom() : 0;
    }
    bool RegisterWithRenderer() override { return g_render && g_render->AddViewObject(&view_, 5, 1000); }
    void UnregisterFromRenderer() override { if (g_render) g_render->RemoveViewObject(&view_); }
    bool StartTicks() override {
        sc4::cSC4AnimationTickManager* atm = AnimationTickManager();
        return atm && atm->AddTickableObject(&target_, 0xc0);
    }
    void StopTicks() override { if (sc4::cSC4AnimationTickManager* atm = AnimationTickManager()) atm->RemoveTickableObject(&target_, 0xc0); }
    void ListenForMessages(bool listen) override {
        if (!SL::spMS2) return;
        for (std::uint32_t type : {sc4::kLavaMessageShutdown, sc4::kLavaMessageSteamEnded}) {
            if (listen) SL::spMS2->AddNotification(&target_, type);
            else SL::spMS2->RemoveNotification(&target_, type);
        }
    }

private:
    struct MessageTarget final : cIGZMessageTarget2 {
        NativeLava* owner = nullptr;
        bool QueryInterface(std::uint32_t iid, void** out) override {
            if (iid != 0x452294aa && iid != 1) return false;
            *out = static_cast<cIGZMessageTarget2*>(this);
            return true;
        }
        std::uint32_t AddRef() override { return 1; }
        std::uint32_t Release() override { return 1; }
        bool DoMessage(cIGZMessage2* message) override { return owner->DoMessage(message); }
    };
    struct ViewObject final : cISC4ViewObject3D {
        NativeLava* owner = nullptr;
        bool QueryInterface(std::uint32_t, void**) override { return false; }
        std::uint32_t AddRef() override { return 1; }
        std::uint32_t Release() override { return 1; }
        void DrawViewObject(SC4DrawContext& context) override { owner->Draw(context); }
    };

    int Zoom() const { return lava_.TextureZoom() < 0 ? 0 : (lava_.TextureZoom() >= sc4::kLavaZoomCount ? sc4::kLavaZoomCount - 1 : lava_.TextureZoom()); }

    // DoMessage @ 0x004fac48
    bool DoMessage(cIGZMessage2* message) {
        if (!message || detached_) return true;
        const std::uint32_t type = message->GetType();
        if (type == sc4::kLavaMessageAnimationTick) {
            ++ticks_;
            lava_.DoMessage(type, 0, MessageData(message, 2));
            if (Debug() && ticks_ % 150 == 0) {
                int cityFlammable = 0, rectFlammable = 0;   // the flammability grid itself: the whole city, and the lava's rect
                if (sc4::cSC4FlammabilitySimulator* flammability = Flammability()) {
                    for (int z = 0; z < 256; ++z)
                        for (int x = 0; x < 256; ++x)
                            if (flammability->IsCellFlammable(x, z)) {
                                ++cityFlammable;
                                if (x >= lava_.Bounds().left && x <= lava_.Bounds().right && z >= lava_.Bounds().top && z <= lava_.Bounds().bottom)
                                    ++rectFlammable;
                            }
                }
                std::fprintf(stderr, "lava: flammability grid: %d cells in the city, %d in the lava's rect\n", cityFlammable, rectFlammable);
                ReportVentTerrain("tick");
                float deepest = 0.0f;
                int holding = 0;
                for (float depth : lava_.LavaDepth()) {
                    if (0.0f < depth) ++holding;
                    if (deepest < depth) deepest = depth;
                }
                std::fprintf(stderr, "lava: tick %d: rates %.3g / %.3g, %d vertices hold lava, deepest %.2f, active %d,%d..%d,%d, %zu steam, %zu decals, lit %d, burned down %d; flammable %d of %d asked (no simulator %d), ignitions refused %d, no fire manager %d\n",
                             ticks_, lava_.LavaProductionRate(), lava_.SolidificationRate(), holding, deepest, lava_.Bounds().left, lava_.Bounds().top,
                             lava_.Bounds().right, lava_.Bounds().bottom, lava_.SteamCount(), lava_.DecalCount(), ignited_, quickBurned_,
                             flammableYes_, flammableAsked_, noFlammability_, igniteRefused_, noFireManager_);
            }
        } else if (type == sc4::kLavaMessageSteamEnded) {
            lava_.DoMessage(type, MessageData(message, 1), 0);
        } else if (type == sc4::kLavaMessageShutdown) {
            lava_.DoMessage(type, 0, 0);
        }
        return true;
    }

    sc4::cSTEVolcanoLava lava_;
    MessageTarget target_;
    ViewObject view_;
    std::uint32_t refs_ = 1;
    bool detached_ = false;
    std::int32_t cellsX_ = 0, cellsZ_ = 0;
    std::vector<std::uint16_t> indices_;          // sTriangleIndices
    std::vector<sc4::LavaRun> runs_;
    std::vector<sc4::LavaSideQuad> quads_;
    cSC4PackedRenderState state_;                 // sPackedRenderState
    bool stateReady_ = false;
    int ticks_ = 0, draws_ = 0, ignited_ = 0, quickBurned_ = 0, decalsMade_ = 0;
    int flammableAsked_ = 0, flammableYes_ = 0, noFlammability_ = 0, igniteRefused_ = 0, noFireManager_ = 0;
    float ventX_ = 0.0f, ventZ_ = 0.0f;
    // The ground under the vent and 5 cells out on each side (the cone the volcano's brush raises holds or sheds the lava).
    void ReportVentTerrain(const char* when) {
        sc4::cSTETerrain* terrain = Terrain();
        if (!terrain) return;
        const float d = 80.0f;
        std::fprintf(stderr, "lava: ground at %s: vent %.1f, west %.1f, east %.1f, north %.1f, south %.1f\n", when,
                     terrain->GetAltitude(ventX_, ventZ_), terrain->GetAltitude(ventX_ - d, ventZ_), terrain->GetAltitude(ventX_ + d, ventZ_),
                     terrain->GetAltitude(ventX_, ventZ_ - d), terrain->GetAltitude(ventX_, ventZ_ + d));
    }
};

}  // namespace

sc4::IVolcanoLava* CreateVolcanoLava() {
    auto* lava = new NativeLava();
    if (!lava->Init()) {
        lava->Release();
        return nullptr;
    }
    g_lavas.push_back(lava);
    return lava;
}

void AttachVolcanoLava(sc4::cSC43DRender& render) { g_render = &render; }

void DetachVolcanoLava() {
    for (NativeLava* lava : g_lavas) lava->Detach();
    g_lavas.clear();
    g_render = nullptr;
}

}  // namespace native
