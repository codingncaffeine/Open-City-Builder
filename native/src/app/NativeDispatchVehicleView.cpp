// cSC4DispatchVehicleView (sc4/DispatchVehicleView.*) on the native application: its IDispatchVehicleViewHost (the renderer,
// the view utilities, the 3-D view window and its children, the terrain, the simulator, the automata, the icon textures and
// the player-drive arrow model) and the forwards its users reach it through. The dispatch manager creates and owns the view
// (CreateDispatchVehicleView, then the renderer's view-object list 5 at priority 1000); the city situations, the MySim, the
// player-drive tool and the automata cheat all reach it the way the original does, through SL::spDispatchManager vslot 0xb0
// GetDispatchVehicleView.
//   PARTIAL: nsTexUtil::SetupTexture / ReleaseTexture (the upload of a cIGZBuffer as a driver texture) and RZ::WCM()'s call
//   number font are not recovered (the call boxes, sc4/PylonOccupant, stop at the same place): the indicators draw their
//   icons and arrows, but no call number digits and no MySim / situation pictures.
#include <cstdint>
#include <fstream>
#include <iterator>
#include <vector>

#include "platform/Unwired.h"
#include "app/NativeAnimationCoordinator.h"
#include "app/NativeCity.h"
#include "app/NativeCitySystems.h"
#include "app/NativeCityView.h"
#include "app/NativeGraphics.h"
#include "app/NativeUi.h"
#include "gz/Buffer.h"
#include "gz/Font.h"
#include "gz/WinFont.h"
#include "render/gd/IGZGDriver.h"
#include "sc4/TexUtil.h"
#include "gz/WinInterfaces.h"
#include "rz/cRZTimer.h"
#include "sc4/AutomataDriveParams.h"
#include "sc4/AutomataStates.h"
#include "sc4/Automaton.h"
#include "sc4/CitySituationEntities.h"
#include "sc4/DispatchManager.h"
#include "sc4/DispatchVehicleView.h"
#include "sc4/DrawContext.h"
#include "sc4/LightingManager.h"
#include "sc4/Render3D.h"
#include "sc4/RenderProperties.h"
#include "sc4/SLBuffer.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/View3DWin.h"
#include "sc4/ViewUtilities.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

namespace native {
std::uint32_t NativeFshTexture(std::uint32_t group, std::uint32_t instance);                  // app/NativeModelMaker.cpp
sc4::cISC4DispatchManager* NativeDispatchManager();                                          // app/NativeDispatch.cpp
const sc4::tAutomataPlayerDriveParams* NativePlayerDriveParams(sc4::cSC4Automaton* automaton);   // app/NativePlayerDrive.cpp
cIGZUnknown* SituationEngineOccupant(sc4::ISituationOccupant* occupant);                     // app/NativeSituationAutomata.cpp
sc4::cSC4DispatchVehicleView* NativeDispatchVehicleView();

namespace {
using namespace sc4;

constexpr std::uint32_t kRenderSnapshotProperty = 28;   // cSC4RenderProperties bool kRenderSnapshot
constexpr std::uint32_t kWinParamFading = 0xe6e998fe;   // the window parameter a fading window carries

// SL::spTerrain: the city's terrain component.
cSTETerrain* Terrain() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::Terrain) : nullptr;
    void* serializable = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &serializable)) return nullptr;
    auto* terrain = static_cast<cSTETerrain*>(static_cast<cIGZSerializable*>(serializable));
    static_cast<cIGZSerializable*>(serializable)->Release();
    return terrain;
}

SC4ViewUtilities* Utilities() {
    cSC43DRender* renderer = CityRenderer();
    return renderer ? renderer->GetViewUtilities() : nullptr;
}

// cS3DTextureBinding as the view holds it: the FSH texture {0x7ab50e44, 0x1abe787d, instance} of the process cache.
class IconTexture final : public IPylonTextureBinding {
public:
    explicit IconTexture(std::uint32_t id) : id_(id) {}
    std::uint32_t TextureId() const override { return id_; }
    void Release() override { delete this; }   // the texture itself stays in the process cache
private:
    std::uint32_t id_;
};

// cISC4Automaton (QueryInterface 0xa9b40f05 of the occupant): the engine automaton, one reference.
class AutomatonView final : public IDispatchViewAutomaton {
public:
    explicit AutomatonView(cSC4Automaton* automaton) : a_(automaton) {}
    void Release() override {
        a_->Release();
        delete this;
    }
    void SetAutomataFlag(std::uint32_t flag, bool on) override { a_->SetAutomataFlag(flag, on); }
    bool CheckAutomataFlag(std::uint32_t flag) override { return a_->CheckAutomataFlag(flag); }
    // GetState / GetAutomataGroup; the u32 at +0x60 of the state is the player-drive record's damage (the port keeps it as a
    // field of tAutomataStatePlayerDrive: another state has no such field, it answers 0).
    bool StateAndGroup(std::uint32_t& stateValue) override {
        tAutomataState* state = a_->GetState();
        if (!state || !a_->GetAutomataGroup()) return false;
        auto* drive = dynamic_cast<tAutomataStatePlayerDrive*>(state);
        stateValue = drive ? static_cast<std::uint32_t>(drive->damage70) : 0u;
        return true;
    }
    bool PlayerDriveParam5c(float& value) override {
        const tAutomataPlayerDriveParams* params = NativePlayerDriveParams(a_);
        if (!params) return false;
        value = params->F(0x5c);
        return true;
    }
private:
    cSC4Automaton* a_;
};

}  // namespace

using namespace sc4;

// SL::spGDriver: the city renderer's draw-context driver (null before the city view draws).
gd::cIGZGDriver* NativeCallNumberDriver() {
    cSC43DRender* renderer = CityRenderer();
    SC4DrawContext* context = renderer ? renderer->GetDrawContext() : nullptr;
    return context ? context->Driver() : nullptr;
}

// ---- the call-number services the indicators and the dispatch call box share --------------------------------------------

namespace {
// RZ::WCM() vslot 0x14(0x68963c4c): the window system's default font as the call boxes use it. The digits are drawn
// into a private window buffer of the target's size and copied into the target (the target is cleared to transparent
// first by its caller, so the copy is the text on transparency, as the original's draw into the target).
class CallNumberFontAdapter final : public IPylonFont {
public:
    void AddRef() override {}
    void Release() override {}
    int GetFontSize() override { cIGZFont* f = Font(); return f ? static_cast<int>(f->GetSize()) : 0; }
    void SetFontSize(int size) override { if (cIGZFont* f = Font()) f->SetSize(static_cast<std::uint32_t>(size)); }
    int TextHeight(const char*, std::uint32_t) override { gz::IWinFont* w = WinFont(); return w ? w->LineHeight(0, 0) : 0; }
    int TextWidth(const char* text, std::uint32_t length) override { gz::IWinFont* w = WinFont(); return w ? w->TextWidth(text, length) : 0; }
    void DrawText(cIGZBuffer* target, int x, int y, const char* text, std::uint32_t length, std::uint32_t color,
                  std::uint32_t color2, const void*, int flags) override {
        gz::IWinFont* w = WinFont();
        if (!w || !target) return;
        const int width = static_cast<int>(target->Width()), height = static_cast<int>(target->Height());
        Buffer scratch(width, height, false);
        w->DrawText(&scratch, x, y, text, length, color, color2, nullptr, flags);
        const cRZRect area{0, 0, width, height};
        target->Blt(scratch.Pixels(), area, area, nullptr);
    }
private:
    static gz::IWinFont* WinFont() {
        gz::IWinFontSource* fonts = gz::WinFontSource();
        return fonts ? fonts->Font(0x68963c4c) : nullptr;
    }
    static cIGZFont* Font() { gz::IWinFont* w = WinFont(); return w ? w->AsFont() : nullptr; }
};
CallNumberFontAdapter g_callNumberFont;
}  // namespace

IPylonFont* NativeCallNumberFont() { return gz::WinFontSource() ? &g_callNumberFont : nullptr; }
bool NativeCallNumberCreateBuffer(cIGZBuffer** out) {   // RZ::GS() vslot 0x10
    auto* buffer = new cGZBuffer();
    buffer->AddRef();
    *out = buffer;
    return true;
}
bool NativeCallNumberUpload(cIGZBuffer* buffer, std::uint32_t& handle) {   // nsTexUtil::SetupTexture(buffer, spGDriver, &h, true, 0, 0, 0)
    gd::cIGZGDriver* driver = NativeCallNumberDriver();
    return driver && nsTexUtil::SetupTexture(buffer, driver, handle, true, nullptr, nullptr, nullptr);
}
void NativeCallNumberReleaseTexture(std::uint32_t handle) { nsTexUtil::ReleaseTexture(NativeCallNumberDriver(), handle); }
// GZCOM 0xa2ffb5d3 / 0x02ffbb60 vslot 0x20(path, &buffer, 8): a bitmap file decoded into a new buffer (one reference).
bool NativeBitmapFromFile(const cIGZString& path, cIGZBuffer** out) {
    *out = nullptr;
    PngDecoder decode = DecodeFunction();
    std::ifstream file(path.ToChar(), std::ios::binary);
    if (!decode || !file) return false;
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    cGZBuffer* buffer = decode(bytes);
    *out = buffer;
    return buffer != nullptr;
}

namespace {

class Host final : public IDispatchVehicleViewHost {
public:
    cIGZMessageServer2* Messages() override { return SL::spMS2; }
    bool RendererPresent() override { return CityRenderer() != nullptr; }
    void RendererGetDrawContext() override {
        if (cSC43DRender* renderer = CityRenderer()) renderer->GetDrawContext();
    }
    IPylonTextureBinding* CreateTextureBinding(std::uint32_t instance) override {
        const std::uint32_t id = NativeFshTexture(kPylonTextureGroup, instance);
        return id ? new IconTexture(id) : nullptr;
    }
    void ReleaseTexture(std::uint32_t handle) override { NativeCallNumberReleaseTexture(handle); }
    bool UploadTexture(cIGZBuffer* buffer, std::uint32_t& handle) override { return NativeCallNumberUpload(buffer, handle); }
    bool CreateBuffer(cIGZBuffer** out) override { return NativeCallNumberCreateBuffer(out); }
    IPylonFont* CallNumberFont() override { return NativeCallNumberFont(); }
    bool LoadBitmapFromFile(const cIGZString& path, cIGZBuffer** out) override { return NativeBitmapFromFile(path, out); }
    // SL::spRM {0x856ddbac, 0x46a006b0, id}: the picture bitmap resource.
    bool LoadPictureBitmap(std::uint32_t id, cIGZBuffer** out) override {
        cSLBuffer bitmap(kPylonBitmapType, kPylonBitmapGroup, id, kPylonBitmapColorType, false);
        bitmap.Load();
        cIGZBuffer* buffer = bitmap.GetBuffer();
        if (!buffer) return false;
        buffer->AddRef();
        *out = buffer;
        return true;
    }
    void GetZoomAndRotation(int& zoom, int& rotation) override {
        if (SC4ViewUtilities* v = Utilities()) v->GetZoomAndRotation(zoom, rotation);
        else zoom = rotation = 0;
    }
    float ViewportUnitsToWorldUnits(float units) override { SC4ViewUtilities* v = Utilities(); return v ? v->ViewportUnitsToWorldUnits(units) : units; }
    float ViewPitchAngle() override { SC4ViewUtilities* v = Utilities(); return v ? v->PitchAngle() : 0.0f; }
    void ViewDirection(float& x, float& z) override {
        SC4ViewUtilities* v = Utilities();
        x = v ? v->ViewDirection().x : 0.0f;
        z = v ? v->ViewDirection().z : 1.0f;
    }
    void ProjectPointToViewportCoordinates2D(float* point) override {
        if (SC4ViewUtilities* v = Utilities()) v->ProjectPointToViewportCoordinates2D(point);
    }
    // Update's function-local static cRZTimer (milliseconds).
    float StopTickTimer() override {
        timer_.Stop();
        float elapsed = 0.0f;
        timer_.GetElapsedTimeFloat(elapsed);
        return elapsed;
    }
    void RestartTickTimer() override { timer_.Restart(); }
    bool RenderSnapshot(SC4DrawContext&) override {
        auto* properties = dynamic_cast<cSC4RenderProperties*>(SL::spRenderProperties);
        return properties && properties->NumBoolProperties() > kRenderSnapshotProperty && properties->BoolValue(kRenderSnapshotProperty);
    }
    std::uint32_t CurrentViewLevel() override { return static_cast<std::uint32_t>(CityViewLevel()); }
    bool SimulatorEmergencyPaused() override { cSC4Simulator* s = CitySimulatorObject(); return s && s->IsEmergencyPaused(); }
    bool View3DWinArea(cRZRect& out) override {
        cSC4View3DWin* view = CityView3DWin();
        if (!view) return false;
        out = static_cast<cIGZWin*>(view)->GetAreaAbsolute();
        return true;
    }
    void View3DChildWindows(std::vector<DispatchViewChildWindow>& out) override {
        out.clear();
        cSC4View3DWin* view = CityView3DWin();
        if (!view) return;
        static_cast<cIGZWin*>(view)->EnumChildren(kGZIID_cIGZWin, [](cIGZWin*, std::uint32_t id, void* object, void* context) {
            auto* child = static_cast<cIGZWin*>(object);
            DispatchViewChildWindow entry;
            entry.id = id;
            entry.visible = child->IsVisible();
            if (entry.visible) {
                entry.area = child->GetAreaAbsolute();
                cIGZVariant* value = nullptr;
                entry.fading = child->GetParam(kWinParamFading, &value);
                if (value) value->Release();
            }
            static_cast<std::vector<DispatchViewChildWindow>*>(context)->push_back(entry);
            return true;
        }, &out);
    }
    bool TerrainLocationInBounds(float x, float z) override { cSTETerrain* t = Terrain(); return t && t->LocationIsInBounds(x, z); }
    float TerrainAltitude(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    float TerrainSeaLevel() override { cSTETerrain* t = Terrain(); return t ? t->GetSeaLevel() : 0.0f; }
    void Set2DCameraView(SC4DrawContext& context) override {
        if (cISC43DRender* renderer = context.Renderer()) renderer->Set2DCameraView();
    }
    void Set3DCameraView(SC4DrawContext& context) override {
        if (cISC43DRender* renderer = context.Renderer()) renderer->Set3DCameraView();
    }
    IDispatchViewAutomaton* QueryAutomaton(sc4::cISC4Occupant* occupant) override {
        void* raw = nullptr;
        if (!occupant || !occupant->QueryInterface(kAutomatonIid, &raw) || !raw) return nullptr;
        return new AutomatonView(static_cast<cSC4Automaton*>(raw));   // cSC4Automaton answers the iid with itself
    }
    bool GetAnimatedModelFromResourceKey(const cGZPersistResourceKey& key, s3d::cIS3DModelInstanceAnimated** out) override {
        *out = NativeCreateAnimatedModel(key);
        return *out != nullptr;
    }
    cISC4LightingManager* LightingManager() override { return dynamic_cast<cISC4LightingManager*>(SL::spLightingManager); }

private:
    cRZTimer timer_{cRZTimer::kMilliseconds};
} g_host;

// The city situations' view of the indicators (sc4::ISituationDispatchView): the situation occupant views turn back into the
// engine occupants; a situation target that is not an engine occupant gets no indicator.
class SituationView final : public ISituationDispatchView {
public:
    void ShowAllIndicators(int type, bool show) override {
        if (cSC4DispatchVehicleView* view = NativeDispatchVehicleView()) view->ShowAllIndicators(type, show);
    }
    void SetIndicatorMax(int type, int max) override {
        if (cSC4DispatchVehicleView* view = NativeDispatchVehicleView()) view->SetIndicatorMax(type, static_cast<std::uint32_t>(max));
    }
    // The fourth argument is the original's label (const cRZString*); the situations always pass null.
    std::uint32_t TrackOccupant(ISituationOccupant* occupant, int type, int value, int) override {
        cSC4DispatchVehicleView* view = NativeDispatchVehicleView();
        cIGZUnknown* engine = occupant ? SituationEngineOccupant(occupant) : nullptr;
        if (!view) return 0;
        return static_cast<std::uint32_t>(
            view->TrackOccupant(static_cast<sc4::cISC4Occupant*>(engine), type, static_cast<std::uint32_t>(value), nullptr));
    }
    void UntrackOccupant(std::uint32_t trackId, bool force) override {
        if (cSC4DispatchVehicleView* view = NativeDispatchVehicleView()) view->Untrack(trackId, force);
    }
    void FlashIndicator(std::uint32_t trackId, bool on, float period) override {
        if (cSC4DispatchVehicleView* view = NativeDispatchVehicleView()) view->FlashIndicator(trackId, on, period);
    }
} g_situationView;

}  // namespace

// cSC4DispatchManager's CreateDispatchVehicleView: operator new + ctor + AddRef (one reference for the manager).
IDispatchVehicleView* NativeCreateDispatchVehicleView() {
    SetDispatchVehicleViewHost(&g_host);
    auto* view = new cSC4DispatchVehicleView();
    view->AddRef();
    return view;
}

// SL::spRenderer vslots 0x100 / 0x108: AddViewObject(view, 5, 1000) / RemoveViewObject(view).
bool NativeAddDispatchVehicleViewLayer(IDispatchVehicleView* view) {
    cSC43DRender* renderer = CityRenderer();
    auto* object = dynamic_cast<cSC4DispatchVehicleView*>(view);
    return renderer && object && renderer->AddViewObject(object, 5, 1000);
}
bool NativeRemoveDispatchVehicleViewLayer(IDispatchVehicleView* view) {
    cSC43DRender* renderer = CityRenderer();
    auto* object = dynamic_cast<cSC4DispatchVehicleView*>(view);
    return renderer && object && renderer->RemoveViewObject(object);
}

// SL::spDispatchManager vslot 0xb0 GetDispatchVehicleView: the dispatch manager's view (null before its Init / without a city).
cSC4DispatchVehicleView* NativeDispatchVehicleView() {
    auto* manager = dynamic_cast<cSC4DispatchManager*>(NativeDispatchManager());
    return manager ? dynamic_cast<cSC4DispatchVehicleView*>(manager->GetDispatchVehicleView()) : nullptr;
}

ISituationDispatchView* NativeSituationDispatchView() { return &g_situationView; }
bool NativeDispatchViewPresent() { return NativeDispatchVehicleView() != nullptr; }

// The player-drive tool and the automata cheat (TrackOccupant(occupant, 6, 0, 0), UntrackOccupant(id, false),
// ShowAllIndicators(4, show), UseAlternateModel).
std::uint32_t NativeDispatchViewTrackPlayerDrive(sc4::cISC4Occupant* occupant) {
    cSC4DispatchVehicleView* view = NativeDispatchVehicleView();
    return view ? static_cast<std::uint32_t>(view->TrackOccupant(occupant, kTrackerPlayerDrive, 0, nullptr)) : 0u;
}
void NativeDispatchViewUntrack(std::uint32_t id, bool force) {
    if (cSC4DispatchVehicleView* view = NativeDispatchVehicleView()) view->Untrack(id, force);
}
void NativeDispatchViewShowAllIndicators(int type, bool show) {
    if (cSC4DispatchVehicleView* view = NativeDispatchVehicleView()) view->ShowAllIndicators(type, show);
}
void NativeDispatchViewUseAlternateModel(std::uint32_t id, bool alternate) {
    if (cSC4DispatchVehicleView* view = NativeDispatchVehicleView()) view->UseAlternateModel(id, alternate);
}

}  // namespace native
