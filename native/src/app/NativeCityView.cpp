#include "app/NativeNetworkTool.h"
#include "app/NativeAutomata.h"
#include "app/NativeCitySystems.h"
#include "app/NativeRuntimeWire.h"
#include "app/NativeBudgetUi.h"
#include "app/NativeLighting.h"
#include "sc4/CommonIIDs.h"
#include "sc4/cSC424HourClock.h"
#include "sc4/Simulator.h"
#include "app/NativeCityView.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "gz/DrawContext.h"
#include "render/gd/IGZGDriver.h"
#include "s3d/Camera.h"
#include "sc4/ViewUtilities.h"
#include "sc4/CameraControl.h"
#include "app/NativeTerrainView.h"
#include "app/NativeModelMaker.h"
#include "app/NativeZoneTool.h"
#include "app/NativeAudio.h"
#include "app/NativeCatalogUi.h"
#include "sc4/Render3D.h"
#include "sc4/View3DWin.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"
#include "sc4/ISC4City.h"
#include "sc4/OccupantManager.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/ServiceLocator.h"
#include "sc4/DBSegment.h"
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
sc4::cSTETerrain* g_pickTerrain = nullptr;
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
    // walk down the line from well above the terrain until it goes under, then halve the step
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
        // The terrain does not cover every pixel yet (and the window frame persists between paints): start each frame from a clear.
        driver_->ClearColor(0.12f, 0.12f, 0.20f, 1.0f);
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
    // The pointer's terrain point: the viewport ray through the pixel, marched down onto the terrain.
    bool PickTerrain(int x, int y, float* out3, bool always, bool arg5) override {
        SC4ViewUtilities* utilities = render_->GetViewUtilities();
        float origin[3] = {}, direction[3] = {};
        const bool rayOk = utilities && utilities->ViewportPositionToRay(x, y, origin, direction);
        if (!rayOk) return false;
        cS3DVector3 hit{};
        if (!MarchTerrain(g_pickTerrain, cS3DVector3{origin[0], origin[1], origin[2]}, cS3DVector3{direction[0], direction[1], direction[2]}, hit, 0.0f, 30000.0f)) return false;
        out3[0] = hit.x; out3[1] = hit.y; out3[2] = hit.z;
        return true;
    }
    bool Pick(int x, int y, cISC4OccupantFilter* filter, s3d::cIS3DModelInstance*& model) override { return render_->Pick(x, y, filter, model); }
    cIGZUnknown* ViewUtilities() override { return nullptr; }
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
    struct ViewAdapter final : sc4::IViewInputView {
        sc4::cSC4View3DWin* view = nullptr;
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

sc4::cSC43DRender* g_cityRender = nullptr;
sc4::cSC43DRender* CityRenderer() { return g_cityRender; }

bool AttachCityRenderStack(sc4::cSC4View3DWin& view, cISC4City* cityInterface, gd::cIGZGDriver* driver, gz::cIGZDrawContext* screen, int width, int height) {
    sc4::cSTETerrain* terrain = nullptr;
    if (auto* city = dynamic_cast<sc4::cSC4City*>(cityInterface)) {
        cIGZUnknown* terrainObject = city->GetComponent(sc4::CityComponent::Terrain);
        void* serializable = nullptr;
        if (terrainObject && terrainObject->QueryInterface(0xab953253, &serializable)) terrain = static_cast<sc4::cSTETerrain*>(static_cast<cIGZSerializable*>(serializable));
        if (terrain) terrain->Init();   // the port's terrain is not a cIGZInitShutdown, so the city's component pass skips it
        g_pickTerrain = terrain;
        std::printf("city view: terrain component %p -> %p", static_cast<void*>(terrainObject), static_cast<void*>(terrain));
        if (terrain) std::printf(" cells %u x %u", terrain->CellCountX(), terrain->CellCountZ());
        std::printf("\n");
        // The occupant manager is not a cIGZInitShutdown in the port, so the city's component pass skips it; its Init
        // initialises every occupant the save restored (and lets the queries below and the model maker see them).
        native::InstallAutomataHost();   // before the occupant manager initialises the vehicles it restored
        { auto* c = dynamic_cast<sc4::cSC4City*>(cityInterface); if (c) native::SetAutomataRenderer(nullptr, c); }
        cIGZUnknown* om = city->GetComponent(sc4::CityComponent::OccupantManager);
        void* iface = nullptr;
        if (om && om->QueryInterface(sc4::kGZIID_cISC4OccupantManager, &iface)) {
            auto* manager = static_cast<sc4::cISC4OccupantManager*>(iface);
            manager->Init();
            if (std::getenv("SC4_DEBUG")) {   // census of the loaded occupants by type
                static std::map<std::uint32_t, int> census;
                census.clear();
                const float lo[3] = {-1e6f, -1e6f, -1e6f}, hi[3] = {1e6f, 1e6f, 1e6f};
                auto* all = new sc4::cSC4OccupantFilter();
                all->AddRef();
                manager->IterateOccupantsByBBox([](sc4::cISC4Occupant* o, void*) { static int shown = 0; if (o->GetType() == 0xc7cc75f1 && shown++ < 6) { float p[3] = {}; o->GetPosition(p); std::printf("vehicle at %.1f %.1f %.1f\n", p[0], p[1], p[2]); } ++census[o->GetType()]; return true; }, nullptr, lo, hi,
                                                static_cast<sc4::cISC4OccupantFilter*>(all));
                all->Release();
                for (auto& [type, n] : census) std::printf("occupants: type %08x x%d\n", type, n);
            }
            manager->Release();
        }
    }
    static CameraHost cameraHost;
    cameraHost.render = nullptr;
    cameraHost.terrain = terrain;
    sc4::SetCameraControlHost(&cameraHost);
    // DoPreCityInit: the camera, its control (looking at the middle of a 1 km city) and the renderer.
    auto* camera = new s3d::cS3DCamera();
    camera->AddRef();
    camera->Init();
    auto* control = new sc4::cSC4CameraControl();
    control->AddRef();
    control->SetViewportSize(width, height);
    control->Init(cS3DVector3{512.0f, 0.0f, 512.0f}, camera);
    if (terrain) {   // DoPostCityInit: the camera knows the city's size; look at its middle
        const float sizeX = static_cast<float>(terrain->CellCountX()) * 16.0f, sizeZ = static_cast<float>(terrain->CellCountZ()) * 16.0f;
        control->SetCitySize(sizeX, sizeZ);
        control->SetZoomAndRotation(std::getenv("SC4_ZOOM") ? std::atoi(std::getenv("SC4_ZOOM")) : 0, 0);
        float focusX = sizeX * 0.5f, focusZ = sizeZ * 0.5f;
        if (const char* focus = std::getenv("SC4_FOCUS")) std::sscanf(focus, "%f,%f", &focusX, &focusZ);   // dev: look at a world x,z
        control->SetViewTargetPosition(cS3DVector3{focusX, terrain->GetAltitude(focusX, focusZ), focusZ});
        control->SendViewChangeMessage();
    }

    auto* render = new sc4::cSC43DRender();
    render->AddRef();
    sc4::cSC43DRender::StaticSetGDriver(driver);
    if (terrain) native::AttachTerrainView(*render, *terrain, driver);
    render->SetViewportSize(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
    render->SetCamera(camera);
    render->SetCameraControl(control);
    cameraHost.render = render;
    g_cityRender = render;
    const bool ok = render->Init();
    render->SetBackingStoreEnabled(false);   // the Metal driver keeps no backing store yet: a restore would replay whatever the screen held
    std::printf("city view: renderer %s\n", ok ? "up" : "FAILED");
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
        native::AttachModelMaker(*render, *city);
        // Occupant messages (inserted / shown / changed / removed: cSC4Occupant::PostOccupantMessage) go to SL::spMS2. Bound only
        // now: the model maker has just built every loaded occupant itself, so announcing them during the load would double them.
        sc4::SetDBSegmentMessageServer(SL::spMS2);
        native::SetAutomataRenderer(render, city);
        native::PostCityInitToAutomataManagers(city);
    }


    auto* adapter = new RendererAdapter(render, control, driver, screen);
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
    native::InstallCatalogUi(view.GetWindowManager());
    native::SetBudgetViewWindow(view.AsIGZWin());
    view.AddGUI();   // DoPostCityInit's city UI (the view's engine DoMessage that sends it there is not ported)
    // ... and the toolbar readouts the date / funds / population / name messages refresh in the game
    view.UpdateCityName();
    view.UpdateBudget();
    view.UpdatePopulation();
    view.UpdateSimulationClock();
    native::BindCityServices(cityInterface);
    native::BindView3DServices(&view, render);
    native::ReportCityWiring();
    return ok;
}

}  // namespace native
