// The emergency dispatch tool's services on the native application (cSC4ViewInputControlDispatch's IDispatchToolHost) and the call
// boxes' (cSC4PylonOccupant's IPylonOccupantHost): the city view's picks and cursor text, the renderer, the view utilities and the
// pylon textures. The dispatch manager side is app/NativeDispatch.cpp.
#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "sc4/DrawContext.h"
#include "sc4/Render3D.h"
#include "app/NativeAnimationTicks.h"
#include "app/NativeCity.h"
#include "app/NativeCityView.h"
#include "app/NativeDemolishTool.h"
#include "s3d/ModelInstance.h"
#include "sc4/DispatchManager.h"
#include "sc4/OccupantManager.h"
#include "sc4/PylonOccupant.h"
#include "sc4/SLString.h"
#include "sc4/ServiceLocator.h"
#include "sc4/View3DWin.h"
#include "sc4/ViewInputControlDispatch.h"
#include "sc4/ViewUtilities.h"
#include "sc4/cSC4City.h"

namespace native {
std::uint32_t NativeFshTexture(std::uint32_t group, std::uint32_t instance);   // app/NativeModelMaker.cpp
sc4::cISC4DispatchManager* NativeDispatchManager();                            // app/NativeDispatch.cpp
sc4::IPylonOccupant* NativePylonView(sc4::cSC4PylonOccupant* pylon);
float NativeDispatchAltitude(float x, float z);
void NativeFollowDispatchedAutomaton(sc4::IDispatchAutomaton* automaton);

namespace {
using namespace sc4;

bool Debug() {
    static const bool on = std::getenv("SC4_DISPATCHDEBUG") != nullptr;
    return on;
}

// ---- the dispatch tool's services (cSC4ViewInputControlDispatch) -----------------------------------------------------------
cSC4View3DWin* g_view = nullptr;

// cPylonFilter::IsOccupantIncluded (i386 0x006f9af6): a pylon occupant (type 0xa9b6d7de) that answers cISC4PylonOccupant with a
// non-zero UI state. PARTIAL: the original also turns away a pylon whose slot 0x40 is set and one whose owner (slot 0x50)
// answers 0x0a1da42f (a MySim's call box).
class PylonFilter final : public cSC4OccupantFilter {
public:
    bool IsOccupantIncluded(sc4::cISC4Occupant* occupant) override {
        if (!occupant || occupant->GetType() != 0xa9b6d7deu) return false;
        void* raw = nullptr;
        if (!occupant->QueryInterface(kDispatchPylonIid, &raw) || !raw) return false;
        auto* pylon = static_cast<cISC4PylonOccupant*>(raw);
        const bool shown = pylon->GetUIState() != 0;
        pylon->Release();
        return shown;
    }
} g_pylonFilter;

// ---- the call boxes' services (cSC4PylonOccupant's IPylonOccupantHost) ----------------------------------------------------
class PylonTexture final : public IPylonTextureBinding {
public:
    explicit PylonTexture(std::uint32_t id) : id_(id) {}
    std::uint32_t TextureId() const override { return id_; }
    void Release() override { delete this; }   // the texture itself stays in the process cache
private:
    std::uint32_t id_;
};

// PARTIAL: the call number digits (the font, buffer and texture upload behind CreateBuffer / CallNumberFont / UploadTexture) and
// the MySim portraits are not native yet: a call box shows its picture without its number.
class PylonHost final : public IPylonOccupantHost {
public:
    cIGZMessageServer2* Messages() override { return SL::spMS2; }
    void AddTickNotification(cIGZMessageTarget2* target, int channel) override {
        if (cIGZMessageServer2* ticks = AnimationTickServerView()) ticks->AddNotification(target, static_cast<std::uint32_t>(channel));
    }
    void RemoveTickNotification(cIGZMessageTarget2* target, int channel) override {
        if (cIGZMessageServer2* ticks = AnimationTickServerView()) ticks->RemoveNotification(target, static_cast<std::uint32_t>(channel));
    }
    cISC4OccupantManager* OccupantManager() override {
        cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(CityComponent::OccupantManager) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
        auto* manager = static_cast<cISC4OccupantManager*>(raw);
        manager->Release();
        return manager;
    }
    bool RendererPresent() override { return CityRenderer() != nullptr; }
    bool RendererHasModel(s3d::cIS3DModelInstance* model) override { cSC43DRender* r = CityRenderer(); return r && r->IsModelInserted(model); }
    bool RendererInsertModel(s3d::cIS3DModelInstance* model) override { cSC43DRender* r = CityRenderer(); return r && r->InsertModel(model); }
    bool RendererRemoveModel(s3d::cIS3DModelInstance* model) override { cSC43DRender* r = CityRenderer(); return r && r->RemoveModel(model); }
    bool RendererMoveModel(s3d::cIS3DModelInstance* model) override { cSC43DRender* r = CityRenderer(); return r && r->MoveModel(model); }
    bool RendererChangeModel(s3d::cIS3DModelInstance* model, int reason) override { cSC43DRender* r = CityRenderer(); return r && r->ChangeModel(model, reason); }
    void RendererGetDrawContext() override {}
    float ViewportUnitsToWorldUnits(float units) override { SC4ViewUtilities* v = Utilities(); return v ? v->ViewportUnitsToWorldUnits(units) : units; }
    float ViewPitchAngle() override { SC4ViewUtilities* v = Utilities(); return v ? v->PitchAngle() : 0.0f; }
    void ViewDirection(float& x, float& z) override {
        SC4ViewUtilities* v = Utilities();
        x = v ? v->ViewDirection().x : 0.0f;
        z = v ? v->ViewDirection().z : 1.0f;
    }
    bool OccupantDrawSuppressed() override { return false; }
    bool CreateBuffer(cIGZBuffer**) override { return false; }
    IPylonFont* CallNumberFont() override { return nullptr; }
    bool UploadTexture(cIGZBuffer*, std::uint32_t&) override { return false; }
    void ReleaseTexture(std::uint32_t) override {}
    IPylonTextureBinding* CreateTextureBinding(std::uint32_t instance) override {
        const std::uint32_t id = NativeFshTexture(kPylonTextureGroup, instance);
        if (Debug()) std::fprintf(stderr, "dispatch: pylon texture %08x -> %u\n", instance, id);
        return id ? new PylonTexture(id) : nullptr;
    }
    bool LoadBitmapFromFile(const cIGZString&, cIGZBuffer**) override { return false; }
private:
    static SC4ViewUtilities* Utilities() { cSC43DRender* r = CityRenderer(); return r ? r->GetViewUtilities() : nullptr; }
} g_pylonHost;

class DispatchToolHostImpl final : public IDispatchToolHost {
public:
    cISC4DispatchManager* DispatchManager() override { return NativeDispatchManager(); }
    // PARTIAL: cSC43DRender::Pick (spRenderer vslot 0x100 with cPylonFilter) is not recovered natively; the call boxes the
    // manager holds are tested in screen space instead: the projection of each one's world box, nearest the camera last drawn
    // first (the original's pick takes the frontmost model).
    IPylonOccupant* PickPylon(int x, int y) override {
        auto* manager = dynamic_cast<cSC4DispatchManager*>(NativeDispatchManager());
        if (!manager) return nullptr;
        IPylonOccupant* best = nullptr;
        float bestDepth = 0.0f;
        for (IPylonOccupant* view : manager->AllPylons()) {
            auto* object = view ? dynamic_cast<cSC4PylonOccupant*>(view->AsOccupant()) : nullptr;
            if (!object || !g_pylonFilter.IsOccupantIncluded(view->AsOccupant())) continue;
            const float* lo = object->WorldBoundsMin();
            const float* hi = object->WorldBoundsMax();
            float sx0 = 1e30f, sy0 = 1e30f, sx1 = -1e30f, sy1 = -1e30f;
            for (int corner = 0; corner < 8; ++corner) {
                const float p[3] = {(corner & 1) ? hi[0] : lo[0], (corner & 2) ? hi[1] : lo[1], (corner & 4) ? hi[2] : lo[2]};
                float px = 0, py = 0;
                if (!ProjectToViewport(p, px, py)) return nullptr;
                sx0 = std::min(sx0, px); sx1 = std::max(sx1, px);
                sy0 = std::min(sy0, py); sy1 = std::max(sy1, py);
            }
            if (float(x) < sx0 || float(x) > sx1 || float(y) < sy0 || float(y) > sy1) continue;
            const float depth = sy1;   // lower on the screen = nearer the camera
            if (!best || depth > bestDepth) {
                best = view;
                bestDepth = depth;
            }
        }
        if (best) best->AddRef();   // one reference for the caller, like the original QI
        return best;
    }
    bool PickTerrain(int x, int y, float out[3]) override { return g_view && g_view->PickTerrain(x, y, out, false); }
    float Altitude(float x, float z) override { return NativeDispatchAltitude(x, z); }
    void CellToWorld(int cellX, int cellZ, float& x, float& z) override { x = float(cellX) * 16.0f; z = float(cellZ) * 16.0f; }
    bool ProjectToViewport(const float world[3], float& x, float& y) override {
        cSC43DRender* render = CityRenderer();
        SC4ViewUtilities* utilities = render ? render->GetViewUtilities() : nullptr;
        if (!utilities) return false;
        float point[3] = {world[0], world[1], world[2]};
        utilities->ProjectPointToViewportCoordinates2D(point);
        x = point[0];
        y = point[1];
        return true;
    }
    void SetCursorText(std::uint32_t id, const cRZString& text) override { if (g_view) g_view->SetCursorText(id, 0, nullptr, &text, 0); }
    void ClearCursorText(std::uint32_t id) override { if (g_view) g_view->ClearCursorText(id); }
    void CursorTextFormat(cRZString& out) override {
        const cSLString format(0x6a231eaa, 0x4a54ed13, true);
        out = format;
    }
    void FollowAutomaton(IDispatchAutomaton* automaton) override { NativeFollowDispatchedAutomaton(automaton); }
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
} g_toolHost;

}  // namespace

void InstallDispatchTool(cSC4View3DWin& view) {
    g_view = &view;
    SetDispatchToolHost(&g_toolHost);
    SetPylonOccupantHost(&g_pylonHost);
}

cSC4ViewInputControl* CreateDispatchTool(int type) { return new cSC4ViewInputControlDispatch(static_cast<std::uint32_t>(type)); }

}  // namespace native
