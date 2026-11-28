#include "gz/Buffer.h"
#include "gz/WinCtrlMgr.h"
#include "gz/WinCtrlFactory.h"
#include "sc4/SLBuffer.h"
#include "app/NativeWaterRipple.h"
#include "platform/Unwired.h"
#include "app/NativeVolcanoLava.h"
#include "app/NativeDemolishTool.h"
#include "app/NativeNetworkTool.h"
#include "app/NativeAutomata.h"
#include "app/NativeCitySystems.h"
#include "app/NativeRuntimeWire.h"
#include "app/NativeBudgetUi.h"
#include "app/NativeLighting.h"
#include "app/NativeEffects.h"
#include "app/NativeCityPanel.h"
#include "sc4/CommonIIDs.h"
#include "sc4/cSC424HourClock.h"
#include "sc4/Simulator.h"
#include "app/NativeCityView.h"
#include "app/NativeBoot.h"
#include "sc4/App.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "gz/DrawContext.h"
#include "render/gd/IGZGDriver.h"
#include "s3d/Camera.h"
#include "sc4/ViewUtilities.h"
#include "sc4/CameraControl.h"
#include "app/NativeTerrainView.h"
#include "app/NativeCity.h"
#include "app/NativeZoneDeveloperHost.h"
#include "app/NativeModelMaker.h"
#include "app/NativeZoneTool.h"
#include "app/NativePlaceLot.h"
#include "app/NativeAudio.h"
#include "app/NativeCatalogUi.h"
#include "sc4/Render3D.h"
#include "sc4/View3DWin.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"
#include "sc4/ISC4City.h"
#include "sc4/OccupantManager.h"
#include "sc4/SCPropertyHelp.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/ServiceLocator.h"
#include "sc4/DBSegment.h"
#include "sc4/AutoHelpers.h"
#include "sc4/ModelMaker.h"
#include "sc4/ViewInputControlEffect.h"
#include "sc4/ViewInputControlCityScroll.h"
#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#else
#include "platform/PointerState.h"
#endif
#include <map>

namespace native {
namespace {

// Where the line through `origin` along `direction` meets the terrain (the heights come from the terrain's altitude).
bool MarchTerrain(sc4::cSTETerrain* terrain, const cS3DVector3& origin, const cS3DVector3& direction, cS3DVector3& hit, float from = -6000.0f, float to = 6000.0f) {
    if (!terrain) return false;
    const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
    if (length <= 0.0f || direction.y >= 0.0f) return false;
    const cS3DVector3 d{direction.x / length, direction.y / length, direction.z / length};
    auto above = [&](float t) {
        const float x = origin.x + d.x * t, y = origin.y + d.y * t, z = origin.z + d.z * t;
        // outside the city there is no ground: the line is above it
        if (x < 0.0f || z < 0.0f || x >= terrain->CellCountX() * 16.0f || z >= terrain->CellCountZ() * 16.0f) return 1.0f;
        return y - terrain->GetAltitude(x, z);
    };
    // walk down the line from well above the terrain until it goes under, then halve the step. The camera asks from its own
    // plane, thousands of metres up: the walk goes on until the line is below the lowest possible ground (altitude 0).
    to = std::max(to, (origin.y + 16.0f) / -d.y);
    float t = from, step = 8.0f;
    if (above(t) <= 0.0f) return false;
    while (above(t + step) > 0.0f) {
        t += step;
        if (t > to) return false;
    }
    for (int i = 0; i < 20; ++i) {
        step *= 0.5f;
        if (above(t + step) > 0.0f) t += step;
    }
    hit = cS3DVector3{origin.x + d.x * t, origin.y + d.y * t, origin.z + d.z * t};
    return true;
}

// The window's IRenderer3D seam over the recovered renderer.
class RendererAdapter final : public sc4::IRenderer3D {
public:
    RendererAdapter(sc4::cSC43DRender* render, sc4::cSC4CameraControl* control, gd::cIGZGDriver* driver, gz::cIGZDrawContext* screen) : render_(render), control_(control), driver_(driver), screen_(screen) { render_->AddRef(); }
    ~RendererAdapter() override { render_->Release(); }

    bool QueryInterface(std::uint32_t iid, void** out) override { return render_->QueryInterface(iid, out); }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_ == 0) { delete this; return 0; }
        return refs_;
    }

    bool Init() override { return render_->Init(); }
    void Shutdown() override {
        native::DetachVolcanoLava();   // first: it stops its steam effects and drops its scorch decals
        native::DetachWaterRipple();
        native::DetachEffects();
        native::DetachModelMaker();
        native::DetachTerrainView();
        render_->Shutdown();
        native::ShutdownCityLighting();
    }
    void* GetDrawContext() override { return render_->GetDrawContext(); }
    void* GetCamera() override { return render_->GetCamera(); }
    void ForceFullRedraw() override { render_->ForceFullRedraw(); }
    void Draw() override {
        // The window manager's 2-D drawing leaves its clip as the driver viewport and its own state on the device: hand the
        // device to the 3-D renderer for the frame (full viewport) and give it back afterwards.
        if (screen_) screen_->Suspend();
        driver_->SetViewport();
        // The terrain does not cover every pixel yet (and the window frame persists between paints): start each frame from a clear,
        // in the renderer's own viewport clear colour (black: nothing in the game sets another), so the void around the city is the
        // same whether the static view comes from the backing store or is drawn again.
        driver_->ClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        driver_->ClearDepth(1.0);
        driver_->Clear(0x5000);
        // PARTIAL: the renderer's camera / city notifications (DoMessage) are not recovered; the view utilities follow the camera here.
        if (SC4ViewUtilities* utilities = render_->GetViewUtilities()) utilities->UpdateCameraPosition(control_);
        if (std::getenv("SC4_DEBUG")) {
            static int printed = 0, lastRotation = -1, lastZoom = -1;
            if (printed++ < 2 || control_->Rotation() != lastRotation || control_->Zoom() != lastZoom) {
                lastRotation = control_->Rotation();
                lastZoom = control_->Zoom();
                const cS3DVector3 t = control_->ViewTarget(), c = control_->CameraPosition(), d = control_->ViewDirection();
                if (s3d::cS3DCamera* camera = control_->Camera()) {
                    float l, r, t, b, n, f;
                    camera->GetViewVolume(l, r, t, b, n, f);
                    std::fprintf(stderr, "camera: view volume l %.1f r %.1f t %.1f b %.1f near %.1f far %.1f\n", l, r, t, b, n, f);
                }
                std::fprintf(stderr, "camera: target %.1f %.1f %.1f  pos %.1f %.1f %.1f  dir %.3f %.3f %.3f  zoom %d rot %d wpp %.4f\n", t.x, t.y, t.z, c.x, c.y, c.z, d.x, d.y, d.z, control_->Zoom(), control_->Rotation(), control_->WorldPerPixel());
            }
        }
        render_->Draw();
        if (screen_) screen_->Resume();
    }
    bool SetZoom(int zoom) override { render_->SetZoom(zoom); return true; }
    bool ZoomIn() override { render_->ZoomIn(); return true; }
    bool ZoomOut() override { render_->ZoomOut(); return true; }
    bool SetRotation(int rotation) override { render_->SetRotation(rotation); return true; }
    bool RotateLeft() override { render_->RotateLeft(); return true; }
    bool RotateRight() override { render_->RotateRight(); return true; }
    bool SetZoomAndRotation(int zoom, int rotation) override { render_->SetZoomAndRotation(zoom, rotation); return true; }
    // The pointer's terrain point: the renderer's pick (the camera's eye ray through the pixel onto the terrain view).
    bool PickTerrain(int x, int y, float* out3, bool always, bool arg5) override { return render_->PickTerrain(x, y, out3, always, arg5); }
    bool Pick(int x, int y, sc4::cISC4OccupantFilter* filter, s3d::cIS3DModelInstance*& model) override { return render_->Pick(x, y, filter, model); }
    SC4ViewUtilities* ViewUtilities() override { return render_->GetViewUtilities(); }
    cIGZUnknown* LightingManager() override { return nullptr; }

private:
    sc4::cSC43DRender* render_;
    sc4::cSC4CameraControl* control_;
    gd::cIGZGDriver* driver_;
    gz::cIGZDrawContext* screen_;
    std::uint32_t refs_ = 0;
};

// The view input tools' services (SL::spView3D / RZ::WM()) and the right-button scroll tool's.
class ViewInput final : public sc4::IViewInputHost, public sc4::ICityScrollHost {
public:
    struct ViewAdapter final : sc4::IEffectView {   // the effect tools pick the terrain through it
        sc4::cSC4View3DWin* view = nullptr;
        bool PickTerrain(int x, int y, cS3DVector3* out, bool flag) override {   // vslot 0x120
            float p[3] = {0.0f, 0.0f, 0.0f};
            if (!view || !out || !view->PickTerrain(x, y, p, flag)) return false;
            out->x = p[0]; out->y = p[1]; out->z = p[2];
            return true;
        }
        bool QueryInterface(std::uint32_t, void**) override { return false; }
        std::uint32_t AddRef() override { return 1; }
        std::uint32_t Release() override { return 1; }
        void EndInput(int) override { if (view) view->RemoveCurrentViewInputControl(false); }   // vslot 0xd0
        sc4::cSC4ViewInputControl* TopControl() override { return view ? view->GetCurrentViewInputControl() : nullptr; }
    };
    explicit ViewInput(sc4::cSC4View3DWin& view) { adapter_.view = &view; }
    sc4::IViewInputView* View3D() override { return &adapter_; }
    cIGZWinMgr* WindowManager() override { return adapter_.view->GetWindowManager(); }
    void PlaySound(int, std::uint32_t id) override { PlayGameSound(id); }
    bool RightButtonDown() override {
#if defined(__APPLE__)
        return CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, kCGMouseButtonRight);
#else
        return platform::RightMouseButtonDown();
#endif
    }
    bool ReadTunables(std::uint32_t&, std::uint32_t&) override { return false; }   // the constructor's 12 / 80 are the shipped values
    void SetScrolling(bool scrolling, float angle, float speed) override { adapter_.view->SetScrolling(scrolling, angle, speed); }
    void ScrollStop() override { adapter_.view->ScrollStop(); }
    // i386 0x00206630 (cSC4ViewInputControlCityScroll::SetMouseScroll): the press-point picture as a bitmap window centred on
    // the press point, a child of the city view.
    bool ShowPressPoint(int x, int y) override {
        cSLBuffer picture(0x856ddbac, 0x46a006b0, 0x094ac89a, 1, false);
        if (!picture.Load() || !picture.GetBuffer()) return false;
        cIGZBuffer* image = picture.GetBuffer();
        image->SetTransparency(image->ConvertRGBValueToNative(0xff, 0, 0xff));
        const cRZRect* b = image->GetBufferArea();
        const int halfW = (b->left - b->right) / 2, halfH = (b->top - b->bottom) / 2;
        gz::cIGZWinCtrlMgr* controls = gz::WinCtrlMgr();
        cIGZWin* view = adapter_.view ? adapter_.view->AsIGZWin() : nullptr;
        gz::cIGZWinBMP* bmp = controls && view ? controls->CreateWinBMP(kPressPointWindowId, image, true) : nullptr;
        if (!bmp) return true;   // the picture loaded: the original keeps it and does not try again
        cIGZWin* marker = bmp->AsIGZWin();
        marker->SetArea(halfW + b->left + x, halfH + b->top + y, halfW + b->right + x, halfH + b->bottom + y);
        marker->SetFlag(2, false);   // disabled
        marker->SetFlag(4, true);
        marker->ShowWindow();
        if (view->ChildAdd(marker)) view->ChildToFront(marker);
        else view->GetWindowManager()->DestroyWindow(marker);
        bmp->Release();
        return true;
    }
    void RemovePressPoint() override {
        cIGZWin* view = adapter_.view ? adapter_.view->AsIGZWin() : nullptr;
        if (cIGZWin* marker = view ? view->GetChildWindowFromID(kPressPointWindowId) : nullptr) view->ChildDelete(marker);
    }
    static constexpr std::uint32_t kPressPointWindowId = 0x48e945b4;

private:
    ViewAdapter adapter_;
};

// The camera control's services: its view-change notifications go out on the game's message server (the model maker,
// the renderer, the terrain view and the lighting manager listen for them) and the city renderer's view utilities follow it.
class CameraHost final : public sc4::ICameraControlHost {
public:
    sc4::cSC43DRender* render = nullptr;
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    cIGZMessage2Standard* CreateMessage() override {
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        return message;
    }
    bool HasViewUtilities() override { return render && render->GetViewUtilities(); }
    void UpdateViewUtilities(sc4::cSC4CameraControl* control) override { render->GetViewUtilities()->UpdateCameraPosition(control); }
    void GetCityCornerPosition(int corner, cS3DVector3& out) override {
        if (HasViewUtilities()) render->GetViewUtilities()->GetCityCornerPosition(corner, &out.x);
    }
    // SL::TerrainView() vslot 0x18: where the line through `origin` along `direction` meets the terrain. The camera
    // keeps its target on the ground with it, so it rotates and zooms about the point the view is centred on.
    sc4::cSTETerrain* terrain = nullptr;
    bool PickTerrain(const cS3DVector3& origin, const cS3DVector3& direction, cS3DVector3& hit) override { return MarchTerrain(terrain, origin, direction, hit); }
};

}  // namespace

// SC4_DEBUG: how many occupants of each type the city holds (printed on load and when a save starts).
void PrintOccupantCensus(cISC4City* cityInterface, const char* label) {
    auto* city = dynamic_cast<sc4::cSC4City*>(cityInterface);
    cIGZUnknown* om = city ? city->GetComponent(sc4::CityComponent::OccupantManager) : nullptr;
    void* iface = nullptr;
    if (!om || !om->QueryInterface(sc4::kGZIID_cISC4OccupantManager, &iface)) return;
    auto* manager = static_cast<sc4::cISC4OccupantManager*>(iface);
    static std::map<std::uint32_t, int> census;
    census.clear();
    const float lo[3] = {-1e6f, -1e6f, -1e6f}, hi[3] = {1e6f, 1e6f, 1e6f};
    auto* all = new sc4::cSC4OccupantFilter();
    all->AddRef();
    manager->IterateOccupantsByBBox([](sc4::cISC4Occupant* o, void*) { ++census[o->GetType()]; return true; }, nullptr, lo, hi,
                                    static_cast<sc4::cISC4OccupantFilter*>(all));
    all->Release();
    manager->Release();
    for (auto& [type, n] : census) std::printf("%s: type %08x x%d\n", label, type, n);
}

// The buildings within `radius` metres of a ground point, with their models and texture sizes (the scenario `probe` command).
void ProbeBuildingsAt(float x, float z, float radius) {
    sc4::cSC4City* city = HostCity();
    cIGZUnknown* om = city ? city->GetComponent(sc4::CityComponent::OccupantManager) : nullptr;
    void* iface = nullptr;
    if (!om || !om->QueryInterface(sc4::kGZIID_cISC4OccupantManager, &iface)) return;
    auto* manager = static_cast<sc4::cISC4OccupantManager*>(iface);
    const float lo[3] = {x - radius, -1e5f, z - radius}, hi[3] = {x + radius, 1e5f, z + radius};
    sc4::OccupantList list;
    manager->GetOccupantsByBBox(list, lo, hi, 0x278128a0u, 0xffffffffu);
    for (sc4::cISC4Occupant* o : list) {
        cRZString name;
        if (sc4::cISCPropertyHolder* holder = o->AsPropertyHolder()) sc4::cSCPropertyHelp::GetPropertyValue(holder, 0x00000020, name);
        float p[3];
        o->GetPosition(p);
        std::printf("probe: %s at %.0f,%.0f %s\n", name.str().c_str(), p[0], p[2], DescribeOccupantModels(o).c_str());
    }
    manager->ReleaseOccupantList(list);
    manager->Release();
}

sc4::cSC43DRender* g_cityRender = nullptr;
sc4::cSC43DRender* CityRenderer() { return g_cityRender; }

void PickOccupantAt(int x, int y, int kind) {
    s3d::cIS3DModelInstance* model = nullptr;
    sc4::cSC4OccupantFilter* filter = kind == 9 ? NativeDemolishableFilter() : kind >= 0 ? NativeQueryFilter(kind) : nullptr;
    if (filter && kind != 9) filter->AddRef();
    const bool hit = g_cityRender && g_cityRender->Pick(x, y, filter, model) && model;
    if (filter) filter->Release();
    if (!hit) {
        std::printf("pick %d,%d kind %d: nothing\n", x, y, kind);
        return;
    }
    cIGZUnknown* owner = model->GetOwner();
    void* raw = nullptr;
    if (owner && owner->QueryInterface(sc4::kGZIID_cISC4Occupant, &raw) && raw) {
        auto* o = static_cast<sc4::cISC4Occupant*>(raw);
        cRZString name;
        if (sc4::cISCPropertyHolder* holder = o->AsPropertyHolder()) sc4::cSCPropertyHelp::GetPropertyValue(holder, 0x00000020, name);
        float p[3];
        o->GetPosition(p);
        std::printf("pick %d,%d kind %d: %s type %08x at %.0f,%.0f\n", x, y, kind, name.str().c_str(), o->GetType(), p[0], p[2]);
        o->Release();
    } else {
        std::printf("pick %d,%d: a model without an occupant owner\n", x, y);
    }
    model->Release();
}
void AnnounceCameraZoomRotation() {
    if (sc4::cSC4CameraControl* camera = g_cityRender ? g_cityRender->GetCameraControl() : nullptr) camera->SendPostViewChangeMessage(sc4::kCameraUpdateZoomRotation);
}
bool CityRendererChangeModel(s3d::cIS3DModelInstance* model, int change) { return g_cityRender && g_cityRender->ChangeModel(model, change); }

// The saved camera. cSC4View3DWin::DoLoad (i386 0x0052272c) reads the view record when the city was read (0x26c63341): the camera
// control, the 3D render, the model maker and the simulator references, and the global effect triggers. The original reads the
// references into its view's pointers; the native view built its renderer, camera and model maker for the window before the
// load, so the saved camera's target, zoom and rotation go to the live camera (the city opens where it was saved; a camera
// that does not exist yet gets them when it is set up), and the saved renderer / model maker objects (viewport, frustum and
// zoom state the view's set-up rewrites anyway) are dropped. The simulator reference is the city's own simulator.
namespace {
struct SavedView {
    bool pending = false;
    cS3DVector3 target{};
    int zoom = 0, rotation = 0;
} g_savedView;

void ApplySavedView() {
    if (!g_savedView.pending) return;
    sc4::cSC4CameraControl* camera = g_cityRender ? g_cityRender->GetCameraControl() : nullptr;
    if (!camera) return;
    g_savedView.pending = false;
    if (std::getenv("SC4_ZOOM") || std::getenv("SC4_FOCUS")) return;   // the dev switches win
    camera->SetZoomAndRotation(g_savedView.zoom, g_savedView.rotation);
    camera->SetViewTargetPosition(g_savedView.target);
    camera->SendViewChangeMessage();
    if (std::getenv("SC4_DEBUG"))
        std::fprintf(stderr, "view state: camera target %.0f,%.0f zoom %d rotation %d\n", g_savedView.target.x, g_savedView.target.z,
                     g_savedView.zoom, g_savedView.rotation);
}

}  // namespace

void ViewObjectsLoaded(void* camera, void* render, void* modelMaker, void* simulator) {
    if (camera) {
        auto* saved = static_cast<sc4::cSC4CameraControl*>(static_cast<cIGZSerializable*>(camera));
        g_savedView = SavedView{true, saved->ViewTarget(), saved->Zoom(), saved->Rotation()};
        static_cast<cIGZSerializable*>(camera)->Release();
        ApplySavedView();
    }
    if (render) static_cast<cISC43DRender*>(render)->Release();
    if (modelMaker) static_cast<cIGZSerializable*>(modelMaker)->Release();
    if (simulator) static_cast<sc4::cISC4Simulator*>(simulator)->Release();
}

bool ViewObjectsToSave(cIGZSerializable*& camera, cIGZSerializable*& render, cIGZSerializable*& modelMaker, cIGZSerializable*& simulator) {
    camera = render = modelMaker = simulator = nullptr;
    sc4::cSC4CameraControl* control = g_cityRender ? g_cityRender->GetCameraControl() : nullptr;
    sc4::cSC4ModelMaker* maker = CityModelMaker();
    sc4::cSC4Simulator* sim = nullptr;
    if (sc4::cSC4City* city = HostCity()) {
        cIGZUnknown* component = city->GetComponent(sc4::CityComponent::Simulator);
        void* iface = nullptr;
        if (component && component->QueryInterface(0x8695664e, &iface) && iface) {
            sim = static_cast<sc4::cSC4Simulator*>(static_cast<sc4::cISC4Simulator*>(iface));
            static_cast<cIGZUnknown*>(iface)->Release();   // the city keeps it
        }
    }
    if (!control || !maker || !sim) return false;
    camera = static_cast<cIGZSerializable*>(control);
    render = static_cast<cIGZSerializable*>(g_cityRender);
    modelMaker = static_cast<cIGZSerializable*>(maker);
    simulator = static_cast<cIGZSerializable*>(sim);
    return true;
}


namespace {
// The city view being brought up: set when the city screen is made, used by the view's DoPreCityInit / DoPostCityInit.
struct CityRenderTarget {
    sc4::cSC4View3DWin* view = nullptr;
    gd::cIGZGDriver* driver = nullptr;
    gz::cIGZDrawContext* screen = nullptr;
    int width = 0, height = 0;
    sc4::cSC4CameraControl* control = nullptr;
    bool renderOk = false;
} g_target;
CameraHost& TheCameraHost() { static CameraHost host; return host; }

sc4::cSTETerrain* CityTerrainOf(cISC4City* cityInterface) {
    auto* city = dynamic_cast<sc4::cSC4City*>(cityInterface);
    cIGZUnknown* terrainObject = city ? city->GetComponent(sc4::CityComponent::Terrain) : nullptr;
    void* serializable = nullptr;
    if (!terrainObject || !terrainObject->QueryInterface(0xab953253, &serializable) || !serializable) return nullptr;
    auto* terrain = static_cast<sc4::cSTETerrain*>(static_cast<cIGZSerializable*>(serializable));
    static_cast<cIGZSerializable*>(serializable)->Release();
    return terrain;
}
}  // namespace

// The city screen was made: its view gets the render stack when the city starts (the original's view builds it on the city's
// "init begins" message, 0x26d31ec0, which a new city sends before it has any components). The previous city's renderer is gone.
void SetCityRenderTarget(sc4::cSC4View3DWin& view, gd::cIGZGDriver* driver, gz::cIGZDrawContext* screen, int width, int height) {
    g_target = CityRenderTarget{&view, driver, screen, width, height, nullptr, false};
    g_cityRender = nullptr;
    native::DetachTerrainView();   // the previous city's, if its screen did not shut its renderer down
    native::SetCityView3DWin(&view);
    // AddGUI (DoPreCityInit, before the city starts) builds the budget panel inside this view: the previous city's view is gone
    native::SetBudgetViewWindow(view.AsIGZWin());
}

// cSC4View3DWin::DoPreCityInit (i386 0x00528030): the camera, its control (looking at the middle of a 1 km city) and the 3D render
// on the graphics driver, before cSC4City::Init. Nothing here reads the city: a new city has no components yet. The automata
// host goes in now because the occupant manager's Init (in the city's pass) initialises the vehicles a save restored.
bool PrepareCityRenderStack(cISC4City* cityInterface) {
    if (!g_target.view || !g_target.driver) return false;
    // The catalog and the budget window belong to this view before its GUI is added (the end of DoPreCityInit): a
    // reopened city must not reach the previous view's windows.
    native::InstallCatalogUi(g_target.view->GetWindowManager());
    native::InstallAutomataHost();
    native::InstallZoneDeveloperServices();   // the zone developer and the lots are initialized by the city's Init, next
    if (auto* c = dynamic_cast<sc4::cSC4City*>(cityInterface)) native::SetAutomataRenderer(nullptr, c);
    CameraHost& cameraHost = TheCameraHost();
    cameraHost.render = nullptr;
    cameraHost.terrain = nullptr;
    sc4::SetCameraControlHost(&cameraHost);
    auto* camera = new s3d::cS3DCamera();
    camera->AddRef();
    camera->Init();
    auto* control = new sc4::cSC4CameraControl();
    control->AddRef();
    control->SetViewportSize(g_target.width, g_target.height);
    control->Init(cS3DVector3{512.0f, 0.0f, 512.0f}, camera);
    g_target.control = control;
    auto* render = new sc4::cSC43DRender();
    render->AddRef();
    // StaticSetGDriver sizes the backing store from the driver's viewport, which in the original is the whole screen at this point;
    // here the 2-D windows last left it on one of their own rectangles.
    g_target.driver->SetViewport();
    sc4::cSC43DRender::StaticSetGDriver(g_target.driver);
    render->SetViewportSize(static_cast<std::uint32_t>(g_target.width), static_cast<std::uint32_t>(g_target.height));
    render->SetCamera(camera);
    render->SetCameraControl(control);
    cameraHost.render = render;
    g_cityRender = render;
    g_target.renderOk = render->Init();
    std::printf("city view: renderer %s\n", g_target.renderOk ? "up" : "FAILED");
    native::InstallModelMakerHosts(*render);   // the view makes its model maker next (DoPreCityInit)
    return g_target.renderOk;
}

namespace {
// The native steps that need the model maker's models (it builds every loaded occupant in its DoPostCityInit): they hear the
// city's "init done" message (0x26d31ec1) after the model maker, as the city's own components do in the original (they
// subscribe in the city's Init, after DoPreCityInit made the model maker). One message per city.
class AfterModelsTarget final : public cIGZMessageTarget2 {
public:
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != sc4::kGZIID_cIGZMessageTarget2 && iid != 1) return false;
        *out = static_cast<cIGZMessageTarget2*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override { return refs_ > 1 ? --refs_ : 1; }   // static: lives as long as the process
    bool DoMessage(cIGZMessage2* message) override {
        if (!message || message->GetType() != 0x26d31ec1) return true;
        if (SL::spMS2) SL::spMS2->RemoveNotification(this, 0x26d31ec1);
        auto* city = dynamic_cast<sc4::cSC4City*>(native::BootApplication() ? native::BootApplication()->GetCity() : nullptr);
        if (!city || !g_cityRender || !g_target.renderOk) return true;
        native::ReportModelMaker();
        // Occupant messages (inserted / shown / changed / removed: cSC4Occupant::PostOccupantMessage) go to SL::spMS2. Bound only
        // now: the model maker has just built every loaded occupant itself, so announcing them during the load would double them.
        sc4::SetDBSegmentMessageServer(SL::spMS2);
        native::SetAutomataRenderer(g_cityRender, city);
        native::PostCityInitToAutomataManagers(city);
        return true;
    }

private:
    std::uint32_t refs_ = 1;
} g_afterModels;
}  // namespace

// cSC4View3DWin::DoPreCityInit after the model maker's Init: SL::OnView3DInit publishes the view, its renderer, the view
// utilities and the model maker before cSC4City::Init, whose components reach them (the effects manager attaches its pass to
// SL::spRenderer).
void CityViewInit() {
    if (!g_target.view || !g_cityRender) return;
    native::BindView3DServices(g_target.view, g_cityRender);
    if (SL::spMS2) SL::spMS2->AddNotification(&g_afterModels, 0x26d31ec1);
}

// cSC4View3DWin::DoPostCityInit (i386 0x0052bac8) and the native hook-up that needs the initialised city: the camera learns the
// city's size, the terrain view, lighting, models, automata, effects, tools and the city UI. Runs on the city's "init done"
// message (0x26d31ec1), after cSC4City::Init has initialised every component (the terrain and the occupant manager included).
bool FinishCityRenderStack(cISC4City* cityInterface) {
    if (!g_target.view || !g_cityRender || !g_target.control) return false;
    sc4::cSC4View3DWin& view = *g_target.view;
    sc4::cSC43DRender* render = g_cityRender;
    sc4::cSC4CameraControl* control = g_target.control;
    gd::cIGZGDriver* driver = g_target.driver;
    const bool ok = g_target.renderOk;
    sc4::cSTETerrain* terrain = CityTerrainOf(cityInterface);
    std::printf("city view: terrain %p", static_cast<void*>(terrain));
    if (terrain) std::printf(" cells %u x %u", terrain->CellCountX(), terrain->CellCountZ());
    std::printf("\n");
    if (auto* city = dynamic_cast<sc4::cSC4City*>(cityInterface); city && std::getenv("SC4_DEBUG")) native::PrintOccupantCensus(city, "occupants");
    TheCameraHost().terrain = terrain;
    if (terrain) {   // the camera knows the city's size; look at its middle
        const float sizeX = static_cast<float>(terrain->CellCountX()) * 16.0f, sizeZ = static_cast<float>(terrain->CellCountZ()) * 16.0f;
        control->SetCitySize(sizeX, sizeZ);
        control->SetZoomAndRotation(std::getenv("SC4_ZOOM") ? std::atoi(std::getenv("SC4_ZOOM")) : 0, 0);
        float focusX = sizeX * 0.5f, focusZ = sizeZ * 0.5f;
        if (const char* focus = std::getenv("SC4_FOCUS")) std::sscanf(focus, "%f,%f", &focusX, &focusZ);   // dev: look at a world x,z
        control->SetViewTargetPosition(cS3DVector3{focusX, terrain->GetAltitude(focusX, focusZ), focusZ});
        control->SendViewChangeMessage();
        native::AttachTerrainView(*render, *terrain, driver);
    }
    ApplySavedView();   // a saved camera read before the view existed
    // The static view (terrain, buildings) is kept in the driver's backing store below zoom 3 and only the moving things are
    // drawn each frame, as DoViewZoomChange3 decides on every zoom change; this sets it for the zoom the city opened at.
    render->SetBackingStoreEnabled(control->Zoom() < 3);
    // the lighting manager first: the model maker's shadows project through it as the models load
    {
        sc4::cSC4Simulator* simulator = nullptr;
        if (auto* city = dynamic_cast<sc4::cSC4City*>(cityInterface)) {
            void* iface = nullptr;
            cIGZUnknown* component = city->GetComponent(sc4::CityComponent::Simulator);
            if (component && component->QueryInterface(0x8695664e, &iface) && iface) { simulator = static_cast<sc4::cSC4Simulator*>(static_cast<sc4::cISC4Simulator*>(iface)); static_cast<cIGZUnknown*>(iface)->Release(); }
        }
        if (auto* city = dynamic_cast<sc4::cSC4City*>(cityInterface)) {   // the sun follows the 24-hour clock component's visual time
            void* plain = nullptr;
            cIGZUnknown* clock = city->GetComponent(sc4::CityComponent::Clock24Hour);
            if (clock && clock->QueryInterface(sc4::kGZIID_PlainObject, &plain) && plain) native::SetLightingClock(static_cast<sc4::cSC424HourClock*>(plain));
        }
        native::InitCityLighting(terrain, render, simulator);
    }
    if (auto* city = dynamic_cast<sc4::cSC4City*>(cityInterface); city && ok) {
        native::InstallModelResources(driver);
        // The lot manager (the zone developer) is initialised before the model maker's pass, as at the original's city init: the
        // retaining walls of a city file look their lots up while their models are built.
        native::BindSimGridCity(cityInterface);
        native::InstallZoneDeveloperHost(city);
        // the model maker (made in DoPreCityInit) builds the city's models on this same message, after the view
        native::AttachModelMakerOverlays(*render, *city);
    }

    auto* adapter = new RendererAdapter(render, control, driver, g_target.screen);
    static ViewInput* input = nullptr;
    delete input;
    input = new ViewInput(view);
    sc4::SetViewInputHost(input);
    sc4::SetCityScrollHost(input);
    view.AttachRenderStack(adapter, nullptr, control);
    view.EnablePainting(true);
    native::BindSimGridCity(cityInterface);   // the zone developer's Init (in InstallZoneTool) sizes a grid through SimGridCity()
    native::InstallZoneTool(view, cityInterface, *render);
    native::InstallNetworkTool(view, cityInterface);
    native::InstallPlaceLotHost(view);
    native::InstallDemolishTool(view);
    native::InstallDispatchTool(view);
    native::ConfigureCityPanels(view);   // DoPostCityInit's gauges: RCI bars and opinion polls
    // ... and the toolbar readouts the date / funds / population / name messages refresh in the game
    view.UpdateCityName();
    view.UpdateBudget();
    view.UpdatePopulation();
    view.UpdateSimulationClock();
    native::BindCityServices(cityInterface);
    native::ReportCityWiring();
    return ok;
}

}  // namespace native
