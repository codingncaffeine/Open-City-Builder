// The native IView3DWinHost (started from scripts/gen_stub_hosts.py's neutral host, now hand edited and skipped by the
// generator): the services the city view reaches outside itself. AddGUI builds its windows through the UI-script service and
// nSC4UI's window-set helpers; the compass, the alert border, the progress screen, the building style selector, the render
// properties, the scroll lighting tint and the contour overlay are the original's classes and calls. Still neutral: the
// framework command table, the city mouse router, the graphics system's self-scroll query, the change-terrain dialog, the
// level-terrain tool, the animation recorder, the simulation frame marker and the render-stack shutdown hooks (the native
// render stack owns those objects).
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "sc4/View3DWin.h"
#include "app/NativeObliterate.h"
#include "app/NativeSignLabel.h"
#include "app/NativeTips.h"
#include "app/NativeSnapshot.h"
#include "app/NativeTutorial.h"
#include "app/NativeCheats.h"
#include "app/NativeKeyboard.h"
#include "gz/WinKeyAcceleratorRes.h"
#include "sc4/StringDetokenizer.h"
#include "sc4/AdvisorPopupTracker.h"
#include "sc4/AdvisorUIImageWinProc.h"
#include "sc4/Advisor.h"
#include "sc4/AnimationTickManager.h"
#include "sc4/DrawContext.h"
#include "sc4/Render3D.h"
#include "app/NativeAnimationCoordinator.h"
#include "app/NativeAnimationTicks.h"
#include "sc4/AdvisorSystem.h"
#include "gz/Font.h"
#include "gz/WinFont.h"
#include "rz/cRZMessage2Standard.h"
#include "app/NativeDemolishTool.h"
#include "app/NativeCity.h"
#include "platform/Unwired.h"
#include "app/NativeLighting.h"
#include "app/NativeCitySystems.h"
#include "sc4/CitySituationManager.h"
#include "sc4/Ordinance.h"
#include "sc4/cSTETerrain.h"
#include "sc4/ServiceLocator.h"
#include "sc4/WinLocationSaver.h"
#include "sc4/ViewInputControlCityScroll.h"
#include "app/NativeUi.h"
#include "app/NativeBoot.h"
#include "app/NativeCityView.h"
#include "app/NativeModelMaker.h"
#include "sc4/App.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/ResidentialSimulator.h"
#include "sc4/Simulator.h"
#include "sc4/cSC4City.h"
#include <cstdio>
#include <cstdlib>
#include "app/NativeCityPanel.h"
#include "app/NativeCityUi.h"
#include "app/NativeBudgetUi.h"
#include "app/NativeNetworkTool.h"
#include "app/NativeZoneTool.h"
#include "app/NativeCatalogUi.h"
#include "app/NativeTerrainTool.h"
#include "app/NativeFlora.h"
#include "app/NativePlaceLot.h"
#include "gz/Buffer.h"
#include "gz/WinBtn.h"
#include "sc4/SC4UI.h"
#include "sc4/ViewInputControlQuery.h"
#include "sc4/ViewInputControlPlaceLot.h"
#include "sc4/ViewInputControlSignTool.h"
#include "sc4/GamePreferencesWinProc.h"
#include "platform/GameDataPath.h"
#include "app/NativeAudio.h"
#include "sc4/AudioPreferencesWinProc.h"
#include "sc4/VideoPreferencesWinProc.h"
#include "sc4/SmallWinProcs.h"
#include <chrono>
#include "sc4/PropDeveloper.h"
#include "sc4/AnimationTickManager.h"
#include "app/NativeAnimationTicks.h"
#include "app/NativeWinMgr.h"
#include "sc4/WinProcs.h"
#include "sc4/WinAuraAlert.h"
#include "sc4/RenderProperties.h"
#include "sc4/CameraControl.h"
#include "sc4/LightingManager.h"
#include "sc4/ProgressScreen.h"
#include "sc4/BuildingSelectWinProc.h"
#include "sc4/OverlayManager.h"
#include "app/NativeDecalOverlay.h"
#include "gz/WinBMP.h"
#include "rz/cRZKeyboard.h"
#include "sc4/TractDeveloper.h"
#include "sc4/Region.h"
#include "dbpf/DBPFReader.h"
#include "gz/GZCOMDirector.h"

namespace native { void GoToNextDisaster(); bool NativeDisasterOngoing(); }   // NativeDisasters.cpp, NativeRiot.cpp
#include "app/NativeDisasterTools.h"
namespace native {
sc4::INewsTickerWindow* CreateNewsTickerWindow();     // app/NativeWinHtml.cpp
void ShutdownNewsTickerWindow(cIGZUnknown* proc);
sc4::IAdvisorPopupQueue* CreateAdvisorPopupQueue();
bool NativeLlamaCheatActive();   // app/NativeAdvisors.cpp
void NativeInitMapView(cIGZWin* window);   // app/NativeWinMapView.cpp
std::string FontStyleName(std::uint32_t guid);   // linux/game_main_linux.cpp
namespace {
using namespace sc4;

// The view's cRZAutoMsg2Notification (+0x1d0): the message types it listens for on SL::spMS2 (constructor @ 0x1000419bc,
// AddAllNotifications @ 0x100041aae, RemoveAllNotifications @ 0x100041b6e).
class View3DNotifications final : public INotificationHelper3D {
public:
    void AddMessageTypes(const std::uint32_t* types, int count) override { types_.assign(types, types + count); }
    void AddAllNotifications(cIGZMessageTarget2* target) override {
        static const char* skip = std::getenv("SC4_VIEWNOTIFY_SKIP");   // dev: hex types to leave out ("all" for every one)
        if (SL::spMS2)
            for (std::uint32_t type : types_) {
                char hex[16];
                std::snprintf(hex, sizeof hex, "%08x", type);
                if (skip && (std::strcmp(skip, "all") == 0 || std::strstr(skip, hex))) continue;
                SL::spMS2->AddNotification(target, type);
            }
    }
    void RemoveAllNotifications(cIGZMessageTarget2* target) override {
        if (SL::spMS2) for (std::uint32_t type : types_) SL::spMS2->RemoveNotification(target, type);
    }
private:
    std::vector<std::uint32_t> types_;
};

cSC4City* LoadedCity() {
    cSC4App* app = BootApplication();
    return app ? dynamic_cast<cSC4City*>(app->GetCity()) : nullptr;
}
// A city component as its concrete class: through its serializable view, checked by class id (dynamic_cast is not
// reliable across the port's translation units).
template <class T> T* CityComponentAs(CityComponent c, std::uint32_t clsid) {
    cSC4City* city = LoadedCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = serializable->GetGZCLSID() == clsid ? static_cast<T*>(serializable) : nullptr;
    serializable->Release();
    return out;
}
// "1,234,567": the thousands separator of the English locale (RZ::LM() string 0, vslot 0x90).
std::string GroupDigits(std::int64_t value) {
    const bool negative = value < 0;
    std::string digits = std::to_string(negative ? -value : value), out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return negative ? "-" + out : out;
}
// SL::spSimulator as the view sees it (+0x238): the city's cSC4Simulator. The view's hidden pause is the simulator's HiddenPause
// (vslots 0x50 / 0x68), the speed buttons its Pause / Resume / SetSimSpeed.
class SimulatorView final : public ISimulatorPauseSink {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 2; }   // static: the view's reference never frees it
    std::uint32_t Release() override { return 1; }
    void Pause() override { if (auto* s = Sim()) s->HiddenPause(); }
    void Resume() override { if (auto* s = Sim()) s->HiddenResume(); }
    int Speed() override { auto* s = Sim(); return s ? s->GetSimSpeed() : 0; }
    bool IsPaused() override { auto* s = Sim(); return !s || s->IsPaused(); }
    void UserPause() override { if (auto* s = Sim()) s->Pause(); }
    void UserResume() override { if (auto* s = Sim()) s->Resume(); }
    void SetSpeed(int speed) override { if (auto* s = Sim()) s->SetSimSpeed(speed); }
private:
    static cSC4Simulator* Sim() { return CityComponentAs<cSC4Simulator>(CityComponent::Simulator, kGZCLSID_cSC4Simulator); }
};

constexpr std::uint32_t kContourHeightMapInstance = 0x07f94205;   // ToggleContourDisplay's height-map texture

// SL::spRenderProperties: the application's render property set (cSC4App's, published by SL::OnAppInit1); null before it.
cSC4RenderProperties* AppRenderPropertySet() { return dynamic_cast<cSC4RenderProperties*>(SL::spRenderProperties); }

// kDisplayAlertBorders: bool render property 34 (the record at +0x8b8 the alert border reads), on by default.
constexpr std::uint32_t kPropDisplayAlertBorders = 34;

// The alert border's SL::spRenderProperties (cSC4WinAlertBorder keeps it from its constructor and paints its frame only while
// kDisplayAlertBorders is on).
class AlertBorderProperties final : public IRenderProperties {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 2; }   // static: the border's reference never frees it
    std::uint32_t Release() override { return 1; }
    bool AlertBordersEnabled() override {
        cSC4RenderProperties* properties = AppRenderPropertySet();
        return properties && properties->BoolValue(kPropDisplayAlertBorders);
    }
};

// cSC4CompassWinProc's services: the camera control's rotation (SL::spRenderer vslot 0x80 -> +0x114) and SL::spMS2, where the
// proc listens for the city-ready (0x26d31ec1) and view-changed (0xa6b79602) messages.
class CompassHost final : public ICompassHost {
public:
    int CameraRotation() override {
        auto* renderer = SL::spRenderer ? static_cast<cISC43DRender*>(SL::spRenderer) : nullptr;
        cSC4CameraControl* control = renderer ? renderer->GetCameraControl() : nullptr;
        return control ? control->Rotation() : 0;
    }
    void AddNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->AddNotification(target, message); }
    void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->RemoveNotification(target, message); }
};
CompassHost g_compassHost;

LoadingScreenPresenter* g_loadingPresenter = nullptr;

// cSC4ProgressScreen's services. The original paints the screen through the graphics system (SL::spGS2) and the driver; natively
// the application's presenter copies the buffer onto the frame and shows it.
class ProgressScreenServices final : public IProgressScreenHost {
public:
    void AddNotification(cIGZMessageTarget2* target, std::uint32_t type) override { if (SL::spMS2) SL::spMS2->AddNotification(target, type); }
    void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) override { if (SL::spMS2) SL::spMS2->RemoveNotification(target, type); }
    cIGZFont* Font(std::uint32_t id) override {   // RZ::WCM() vslot 0x14: the window control manager's font of that style
        gz::IWinFontSource* fonts = gz::WinFontSource();
        gz::IWinFont* style = fonts ? fonts->Font(id) : nullptr;
        return style ? style->AsFont() : nullptr;
    }
    cIGZBuffer* CreateScreenBuffer() override { auto* buffer = new cGZBuffer(); buffer->AddRef(); return buffer; }
    cIGZBuffer* LoadImage(std::uint32_t group, std::uint32_t instance) override { return native::LoadBitmapResource(group, instance); }
    bool LoadString(std::uint32_t group, std::uint32_t instance, cIGZString& out) override { return native::LoadLocalizedString(group, instance, out); }
    // SC4ReadCityRegionViewThumbnails(path): the city file's region-view picture {0x8a2482b9, 0x4a2482bb, 0} and its mask (instance 2).
    bool ReadCityThumbnails(cIGZBuffer** image, cIGZBuffer** mask) override {
        *image = nullptr;
        *mask = nullptr;
        cSC4City* city = LoadedCity();
        if (!city || !native::DecodeFunction()) return false;
        cRZString path;
        city->GetCitySaveFilePath(path);
        dbpf::Package package;
        if (path.Strlen() == 0 || !package.Open(path.ToChar())) return false;
        auto load = [&](std::uint32_t instance) -> cIGZBuffer* {
            const dbpf::Entry* entry = package.Find(0x8a2482b9, 0x4a2482bb, instance);
            std::vector<std::uint8_t> bytes;
            if (!entry || !package.Read(*entry, bytes)) return nullptr;
            return native::DecodeFunction()(bytes);
        };
        *image = load(0);
        *mask = *image ? load(2) : nullptr;
        if (*image && *mask) return true;
        if (*image) (*image)->Release();
        *image = nullptr;
        return false;
    }
    int BaseTerrainType() override {
        cSC4App* app = BootApplication();
        cISC4Region* region = app ? app->GetRegion() : nullptr;
        return region ? static_cast<int>(region->GetBaseTerrainType()) : 0;
    }
    int CitySizeType() override { cSC4City* city = LoadedCity(); return city ? city->GetCitySizeType() : 0; }
    void BeginScreenDraw() override {}   // nothing natively: the presenter draws into the frame directly
    void ShowBuffer(cIGZBuffer* buffer, const cRZRect& source, const cRZRect& target) override {
        if (g_loadingPresenter) g_loadingPresenter->Show(buffer, source, target);
    }
    void ClearScreen(float r, float g, float b, float a) override { if (g_loadingPresenter) g_loadingPresenter->Clear(r, g, b, a); }
    void EndScreenDraw() override { if (g_loadingPresenter) g_loadingPresenter->Present(); }
};
ProgressScreenServices g_progressServices;

cSC4ProgressScreen* AsProgressScreen(cIGZUnknown* screen) { return static_cast<cSC4ProgressScreen*>(static_cast<cIGZMessageTarget2*>(screen)); }

// cSC4BuildingSelectWinProc's environment: SL::spTractDeveloper's style settings and the tutorial's UI command filter.
class TractStyles final : public ITractStyles {
public:
    std::vector<std::uint32_t> GetActiveStyles() override { auto* t = Tract(); return t ? t->GetActiveStyles() : std::vector<std::uint32_t>{}; }
    void SetActiveStyles(const std::vector<std::uint32_t>& styles) override { if (auto* t = Tract()) t->SetActiveStyles(styles); }
    bool IsUsingAllStylesAtOnce() override { auto* t = Tract(); return t && t->IsUsingAllStylesAtOnce(); }
    void UseAllStylesAtOnce(bool all) override { if (auto* t = Tract()) t->UseAllStylesAtOnce(all); }
    int GetYearsBetweenStyleChanges() override { auto* t = Tract(); return t ? t->GetYearsBetweenStyleChanges() : 0; }
    void SetYearsBetweenStyleChanges(int years) override { if (auto* t = Tract()) t->SetYearsBetweenStyleChanges(years); }
private:
    static cISC4TractDeveloper* Tract() {   // borrowed: the city holds the developer
        void* raw = nullptr;
        if (!SL::spTractDeveloper || !SL::spTractDeveloper->QueryInterface(kGZIID_cISC4TractDeveloper, &raw) || !raw) return nullptr;
        auto* tract = static_cast<cISC4TractDeveloper*>(raw);
        tract->Release();
        return tract;
    }
};
class TutorialCommandFilter final : public ITutorialFilter {
public:
    bool Allows(const cGZMessage& message) override { cGZMessage copy = message; return native::NativeTutorialFilterUiCommand(copy); }
};

// The view's handle on its style selector (+0x490): the recovered cSC4BuildingSelectWinProc, which installs itself as the
// window's proc in Init and takes it off again in Shutdown.
class BuildingSelectWindow final : public IBuildingSelectWindow {
public:
    explicit BuildingSelectWindow(cIGZWin* window) : proc_(new cSC4BuildingSelectWinProc(window)) { proc_->AddRef(); }
    ~BuildingSelectWindow() { proc_->Release(); }
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != gz::kGZIID_cIGZWinProc && iid != 1) return false;
        *out = static_cast<gz::cIGZWinProc*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_ == 0) { delete this; return 0; }
        return refs_;
    }
    bool DoWinProcMessage(cIGZWin* window, cGZMessage& message) override { return proc_->DoWinProcMessage(window, message); }
    bool Init() override { return proc_->Init(); }
    void ShowWindow() override { proc_->ShowWindow(); }
    void HideWindow() override { proc_->HideWindow(); }
    bool IsVisible() override { return proc_->IsVisible(); }
    void Shutdown() { proc_->Shutdown(); }
    cSC4BuildingSelectWinProc* Proc() const { return proc_; }

private:
    std::uint32_t refs_ = 0;
    cSC4BuildingSelectWinProc* proc_;
};

struct StubView3DWin final : IView3DWinHost {
    View3DNotifications notifications_;
    cIGZMessageServer2* MessageServer2() override { return SL::spMS2; }   // SL::spMS2: erosion, view filter / level and tool changes
    // SL::spPreferences: the application's preferences blob (0x1734 bytes, SimCity 4.cfg; edge scrolling +0xf09 on by default).
    // A zeroed stand-in turned edge scrolling off, and with it off every mouse move stopped a right-drag scroll (the edge pass
    // recomputes the scroll velocity, finds no direction and stops), so panning ran a frame and halted, over and over.
    std::uint8_t* Preferences() override {
        if (cSC4App* app = native::BootApplication()) return app->GetPreferences();
        static std::uint8_t blob[0x1734] = {};
        return blob;
    }
    bool CityExists() override { return LoadedCity() != nullptr; }
    // RZ::MS2 post of a bare message: scroll begin / end (the model maker pauses its per-step loading while the view
    // scrolls) and render properties changed.
    void PostGameMessage(std::uint32_t type) override {
        if (!SL::spMS2) return;
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(type);
        SL::spMS2->MessagePost(message, false);
        message->Release();
    }
    INotificationHelper3D* NotificationHelper() override { return &notifications_; }
    void RegisterCommandTable() override {}
    void UnregisterCommandTable() override {}
    // SL::spCCM vslots 0x28 / 0x30 (window, 0): the window's third slot is DoMessage(cGZMessage&), the old target's call.
    void AddCheatCodeTarget(cIGZWin* target) override { native::AddGameCheatListener(reinterpret_cast<cIGZMessageTarget*>(target), 0); }
    void RemoveCheatCodeTarget(cIGZWin* target) override { native::RemoveGameCheatListener(reinterpret_cast<cIGZMessageTarget*>(target), 0); }
    bool RegisterCheatCode(std::uint32_t id, const char* text) override { return native::RegisterGameCheat(id, text); }
    bool UnregisterCheatCode(std::uint32_t id) override { native::UnregisterGameCheat(id); return true; }
    bool CheatCodeManagerPresent() override { return native::GameCheatCodeManager() != nullptr; }
    bool LotPlopCheatsEnabled() override { return native::GameDebugMode(); }
    cIGZWinKeyAccelerator* CreateKeyAccelerator() override { return native::CreateGameKeyAccelerator(gz::kKeyAcceleratorCityView); }
    cSC4RenderProperties* GlobalRenderProperties() override { return AppRenderPropertySet(); }   // SL::spRenderProperties
    cSC4RenderProperties* AppRenderProperties() override {   // SL::spSC4 vslot 0x160 GetRenderProperties: the same set
        cSC4App* app = BootApplication();
        return app ? dynamic_cast<cSC4RenderProperties*>(app->GetRenderProperties()) : nullptr;
    }
    // While the view scrolls without partial backing-store copies, the lighting manager tints the scene: SetLightingTint(nS3D::kColorYellow
    // (1, 1, 0), 0.1), and ResetLightingTint when the scroll ends.
    void SetScrollLightingHighlight() override {
        if (auto* lighting = SceneLighting()) lighting->SetLightingTint(cS3DVector3{1.0f, 1.0f, 0.0f}, 0.1f);
    }
    void RestoreLighting() override { if (auto* lighting = SceneLighting()) lighting->ResetLightingTint(); }
    static cISC4LightingManager* SceneLighting() { return SL::spLightingManager ? static_cast<cISC4LightingManager*>(SL::spLightingManager) : nullptr; }
    int VirtualZoomLimit() override { return 4; }
    // The new-city specification's obliterated-city segment: the city being made is an obliterated one.
    bool CityBeingObliterated() override { cSC4App* app = BootApplication(); return app && app->ObliterationSegment() != nullptr; }
    // new cSC4ProgressScreen(area, obliterating) (the view takes the first reference), its Init, SetCaption and Shutdown.
    cIGZUnknown* CreateProgressScreen(cIGZWin*, const cRZRect& area, bool obliterating) override {
        return static_cast<cIGZMessageTarget2*>(new cSC4ProgressScreen(&g_progressServices, area, obliterating));
    }
    void InitProgressScreen(cIGZUnknown* screen) override { AsProgressScreen(screen)->Init(); }
    void SetProgressCaption(cIGZUnknown* screen, int id, const cIGZString& text) override {
        cSC4ProgressScreen* progress = AsProgressScreen(screen);
        progress->SetCaption(static_cast<std::uint32_t>(id), text);
        static const bool debug = std::getenv("SC4_DEBUG") != nullptr;
        if (debug)
            std::printf("progress screen: caption %d -> \"%s\" / \"%s\" thumbnail %d tips left %zu\n", id, progress->Caption(0).ToChar(),
                        progress->Caption(1).ToChar(), progress->HasThumbnail() ? 1 : 0, progress->TipsLeft());
    }
    void ShutdownProgressScreen(cIGZUnknown* screen) override { AsProgressScreen(screen)->Shutdown(); }
    void ShutdownValidationWindow(cIGZUnknown* window) override {}
    void ShutdownLotConfigurationEditors() override {}
    void ShutdownModelMaker(cIGZUnknown* modelMaker) override { native::ShutdownModelMaker(modelMaker); }
    cIGZUnknown* CreateModelMaker() override { return native::CreateModelMaker(); }
    void InitModelMaker(cIGZUnknown* modelMaker) override { native::InitModelMaker(modelMaker); }
    void ShutdownCameraObject(cIGZUnknown* camera) override {}
    void ShutdownSimulationClock(cIGZUnknown* clock) override {}
    bool UserDataDirectory(cIGZString& out) override { SL::UserDataDirectory(out); return SL::spSC4 != nullptr; }   // SL::UserDataDirectory
    cIGZWin* CreateWinAnimationRecorder() override { return {}; }
    void RecorderSetAnimationOutputDirectory(cIGZWin* recorder, const cIGZString& directory, bool create) override {}
    ICityMouseRouter* CityMouseRouter() override { return {}; }
    bool CanInvokeZoningTool() override {   // SL::spCity vslot 0xb0: GetEstablished
        sc4::cSC4City* city = native::HostCity();
        return city && city->GetEstablished();
    }
    ITerrainEditor* TerrainEditor() override { return native::CityTerrainEditor(); }
    bool GraphicsSystemAllowsSelfScroll() override { return {}; }
    void RunChangeTerrainDialog(int& type) override {}
    std::uint32_t GetWindowChildrenSet(cIGZWin* window, std::set<cIGZWin*>& children) override { return nSC4UI::GetWindowChildrenSet(window, children); }
    std::uint32_t GetWindowChildrenNotInSet(cIGZWin* window, const std::set<cIGZWin*>& excluded, std::set<cIGZWin*>& out) override { return nSC4UI::GetWindowChildrenNotInSet(window, excluded, out); }
    void AlignWindowSetToWindow(const std::set<cIGZWin*>& windows, cIGZWin* parent) override { nSC4UI::AlignWindowSetToWindow(windows, parent); }
    void ClearAcceptFocusFlagRecursive(cIGZWin* window) override { nSC4UI::ClearAcceptFocusFlagRecursive(window); }
    cIGZWin* CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id) override { return native::CreateWindowFromScript(key, parent, id); }
    bool CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id, std::uint32_t iid, void** out) override {
        cIGZWin* window = native::CreateWindowFromScript(key, parent, id);
        if (!window) return false;
        const bool ok = window->QueryInterface(iid, out);
        window->Release();
        return ok;
    }
    cIGZBuffer* LoadImageBuffer(std::uint32_t, std::uint32_t group, std::uint32_t instance) override { return native::LoadBitmapResource(group, instance); }
    bool ReadExemplarProperties(const cGZPersistResourceKey& key, const std::function<void(const sc4::cISCPropertyHolder*)>& read) override {
        void* raw = nullptr;
        if (!SL::spRM || !SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) || !raw) return false;
        auto* exemplar = static_cast<cISCResExemplar*>(raw);
        read(exemplar->AsISCPropertyHolder());
        exemplar->Release();
        return true;
    }
    void AppIniFilePath(cIGZString& out) override { if (cSC4App* app = native::BootApplication()) app->GetAppIniFilePath(out); }
    cIGZCmdLine* CommandLine() override { cIGZFrameWork* framework = RZGetFramework(); return framework ? framework->CommandLine() : nullptr; }
    // GZCOM()->CreateInstance(0x22ecfc47, cIGZWin): the button that brings the hidden UI back (AddGUI skins it, gives it id 0x43 and
    // hides it until MinimizeUI). It was null natively, so a hidden UI could not be shown again.
    cIGZWin* CreateToolbarButton() override {
        auto* button = new gz::cGZWinBtn(ActiveWinMgr());
        button->AddRef();
        return static_cast<cIGZWin*>(static_cast<cGZWin*>(button));
    }
    // GZCOM()->CreateInstance(0xca5d3294, 0xca5d3290): the alert border (cSC4WinAlertBorder) the view lays over its whole area; its
    // window with one reference.
    cIGZWin* CreateCompassWindow() override {
        void* raw = nullptr;
        if (!GZCOM() || !GZCOM()->CreateInstance(kCompassWindowExemplar, kCompassWindowIid, &raw) || !raw) return nullptr;
        auto* border = static_cast<cISC4WinAlertBorder*>(raw);
        cIGZWin* window = border->AsIGZWin();
        window->AddRef();
        border->Release();
        return window;
    }
    // cSLString(group, instance, true): the localized string (empty when there is none).
    void LocalisedString(std::uint32_t group, std::uint32_t instance, cRZString& out) override {
        if (!native::LoadLocalizedString(group, instance, out)) out.Erase(0, 0xffffffffu);
    }
    // new cSC4PanelUIWinProc (AddGUI @ 0x1007892fc): the options bar's save / region / quit / snapshot buttons become view commands
    gz::cIGZWinProc* CreatePanelUIWinProc() override {
        PanelServices& services = GetPanelServices();
        if (!services.hasView3D) {
            services.hasView3D = [] { return CityView3DWin() != nullptr; };
            services.executeCommand = [this](std::uint32_t command) { ExecuteCommand(command); };
            services.nowMs = [] {
                return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            };
        }
        auto* proc = new cSC4PanelUIWinProc();
        proc->AddRef();
        return proc;
    }
    INewsTickerWindow* CreateNewsTickerWinProc() override { return native::CreateNewsTickerWindow(); }
    IMySimWindow* CreateMySimWinProc(gz::cIGZWinGen* panel) override { return native::CreateMySimWindow(panel); }
    IBudgetUIWindow* CreateBudgetUIWinProc() override { return native::CreateBudgetUiWindow(); }
    IGraphsViewWindow* CreateGraphsViewWinProc(cIGZWin* window) override { return native::CreateGraphsViewWindow(window); }
    // new cSC4BuildingSelectWinProc(window) (+ AddRef): the tract developer's building style selector.
    IBuildingSelectWindow* CreateBuildingSelectWinProc(cIGZWin* window) override {
        static TractStyles styles;
        static TutorialCommandFilter tutorial;
        sc4::SetBuildingSelectEnvironment(&styles, &tutorial);
        auto* proc = new BuildingSelectWindow(window);
        proc->AddRef();
        return proc;
    }
    IAdvisorPopupQueue* CreateAdvisorPopupTracker() override { return CreateAdvisorPopupQueue(); }
    // new cSC4CompassWinProc; gen->SetWinProc(proc): the window holds the only reference. The original builds the GUI on the city's
    // init-begins message, so the proc is listening when the city-ready message (0x26d31ec1) shows the face of the camera's rotation;
    // natively AddGUI runs on the city-ready message itself, after its delivery list was made, so the proc takes that update here.
    void AttachCompassWinProc(gz::cIGZWinGen* window) override {
        auto* proc = new cSC4CompassWinProc(&g_compassHost);
        proc->AddRef();
        window->SetWinProc(proc);
        if (CityExists() && window->GetWinProc() == proc) proc->UpdateRotation();
        proc->Release();
    }
    cISC4WinCalloutBox* CreateCalloutBox() override { return native::CreateCalloutBox(); }
    gz::cIGZWinBMP* CreateWinBMP() override {   // new cGZWinBMP (+ AddRef): its cIGZWinBMP view
        auto* bmp = new gz::cGZWinBMP(ActiveWinMgr());
        bmp->AddRef();
        return static_cast<gz::cIGZWinBMP*>(bmp);
    }
    void InitMapView(cIGZWin* window) override { native::NativeInitMapView(window); }   // cSC4WinMapView::InitWindow
    bool IsKeyDownNow(std::uint32_t key) override { return cRZKeyboard::IsKeyDownNowStatic(key); }
    // SL::spCommandServer vslot 0x98(command, 0, 0). The command server is not set up natively: a command goes to the city view, the
    // handler of every command its buttons send (ProcessCommand via its DoMessage in the original). COMMAND-1.
    void ExecuteCommand(std::uint32_t command) override {
        if (sc4::cSC4View3DWin* view = CityView3DWin()) view->ProcessViewCommand(command);
    }
    void SetDayNightMode(int mode) override { native::SetCityDayNightMode(mode); }
    void ReconcileTerrainEdges() override {
        if (auto* terrain = CityComponentAs<cSTETerrain>(CityComponent::Terrain, kGZCLSID_cSTETerrain)) terrain->ReconcileEdgesWithNeighboringCities(true, true, true);
    }
    bool OrdinanceOn(std::uint32_t ordinance) override {
        auto* sim = CityComponentAs<cSC4OrdinanceSimulator>(CityComponent::OrdinanceSimulator, kGZCLSID_cSC4OrdinanceSimulator);
        return sim && sim->IsOrdinanceOn(ordinance);
    }
    static cSC4CitySituationManager* SituationManager() {
        sc4::cSC4City* city = native::HostCity();
        return city ? dynamic_cast<cSC4CitySituationManager*>(city->GetComponent(CityComponent::CitySituationManager)) : nullptr;
    }
    bool CitySituationIndicatorVisible() override {
        auto* m = SituationManager();
        return m && m->GetVisible();   // GetCSIVisible (vslot 0xf0)
    }
    void SetCitySituationIndicatorVisible(bool visible) override {
        if (auto* m = SituationManager()) m->SetVisible(visible);   // SetCSIVisible (vslot 0xe8)
    }
    void GoToNextDisaster() override { native::GoToNextDisaster(); }
    cSC4ViewInputControl* CreateDispatchTool(int type) override { return native::CreateDispatchTool(type); }
    cSC4ViewInputControl* CreateNetworkTool(int type) override { return native::CreateNetworkTool(type); }
    void ShowMyStreetSim() override { SC4_UNWIRED("IView3DWinHost::ShowMyStreetSim"); }
    void MayorRatingChanged() override { if (sc4::cSC4View3DWin* view = CityView3DWin()) native::UpdateMayorRatingBar(*view); }
    ISimulatorPauseSink* Simulator() override { static SimulatorView view; return CityExists() ? &view : nullptr; }
    void PreCityInit() override { native::PrepareCityRenderStack(native::BootApplication() ? native::BootApplication()->GetCity() : nullptr); }
    void PostCityInit() override {
        native::FinishCityRenderStack(native::BootApplication() ? native::BootApplication()->GetCity() : nullptr);
        native::ShowTipOfTheDayOnce();   // DoPostCityInit's end: the tip of the day, once per run
    }
    cSC4ViewInputControl* CreateSignTool() override { return new cSC4ViewInputControlSignTool(); }
    cSC4ViewInputControl* CreatePlaceLotTool(std::uint32_t lot) override { return new cSC4ViewInputControlPlaceLot(0, lot, 0, false); }
    cSC4ViewInputControl* CreateDemolishTool() override { return native::CreateDemolishTool(); }
    cSC4ViewInputControl* CreateLabelTool() override { return native::CreateLabelTool(); }   // NativeSignLabel.cpp
    cSC4ViewInputControl* CreateSignLabelDemolishTool() override { return native::CreateSignLabelDemolishTool(); }   // NativeSignLabel.cpp
    cSC4ViewInputControl* CreateObliterateCityTool() override { return native::CreateObliterateCityTool(); }   // NativeObliterate.cpp
    cSC4ViewInputControl* CreateFloraTool(std::uint32_t type) override { return native::CreateFloraTool(type); }
    cSC4ViewInputControl* CreateNetworkIntxTool(std::uint32_t rule) override { return native::CreateNetworkIntxTool(rule); }   // the puzzle pieces
    bool CursorOverView(cIGZWin* view, int& x, int& y) override {
        WinMgr* manager = ActiveWinMgr();
        if (!manager) return false;
        manager->GetCursorPos(view, x, y);                              // RZ::WM() vslot 0x110
        return true;
    }
    void InvokeDisaster(cSC4View3DWin& view, std::uint32_t id) override { native::InvokeDisasterTool(view, id); }
    bool FocusVehicle(std::uint32_t) override { SC4_UNWIRED("IView3DWinHost::FocusVehicle (vehicle managers)"); return false; }
    // new cSC4GamePreferencesWinProc; AddRef; ShowDialog(kGameOptions) (vslot 0x28); DoModalWin; the view takes the focus back
    void RunPreferencesDialog(int which) override {
        if (which == 2) {
            RunAudioOptionsDialog([] { return BootApplication() ? BootApplication()->GetPreferences() : nullptr; });
            if (sc4::cSC4View3DWin* view = CityView3DWin()) view->SetFocus();
            return;
        }
        if (which == 1) {
            RunGraphicOptionsDialog([] { return BootApplication() ? BootApplication()->GetPreferences() : nullptr; });
            if (sc4::cSC4View3DWin* view = CityView3DWin()) view->SetFocus();
            return;
        }
        if (which != 0) return;
        RunGameOptionsDialog([this] { return Preferences(); });
        if (sc4::cSC4View3DWin* view = CityView3DWin()) view->SetFocus();
    }
    void ShowSnapshotAlbum(cIGZWin* parent) override { native::ShowSnapshotAlbum(parent); }
    void CreateSnapshotTaker(cIGZWin* parent) override { native::CreateSnapshotTaker(parent); }
    void RequestSaveCity(bool announce, bool alternate) override { if (cSC4App* app = BootApplication()) app->RequestSaveCity(announce, alternate); }
    void RequestGoToRegionView(bool ask) override { if (cSC4App* app = BootApplication()) app->RequestGoToRegionView(ask); }
    void RequestQuit(bool ask, bool save) override { if (cSC4App* app = BootApplication()) app->RequestQuit(ask, save); }
    bool AnimationSystemPaused() override { auto* atm = AnimationTickManager(); return atm && atm->IsAnimationSystemPaused(); }
    void PauseAnimationSystem(bool pause) override { if (auto* atm = AnimationTickManager()) atm->PauseAnimationSystem(pause); }
    void StepAnimationSystem() override { if (auto* atm = AnimationTickManager()) atm->StepAnimationSystem(); }
    void ToggleSignsAndLabels() override {
        if (auto* props = CityComponentAs<cSC4PropDeveloper>(CityComponent::PropDeveloper, kGZCLSID_cSC4PropDeveloper)) props->ToggleSignsAndLabels();
    }
    void PlayUISound(std::uint32_t sound) override { PostSoundSystemEvent(1, sound); }
    void AlignWindowToWindow(std::uint32_t windowId, std::uint32_t childId, std::uint32_t alignId) override {
        nSC4UI::AlignWindowToWindow(windowId, childId, alignId, nullptr);
    }
    cSC4ViewInputControl* CreateQueryTool(int type, std::uint32_t tipPeriod) override {
        return new cSC4ViewInputControlQuery(static_cast<cSC4ViewInputControlQuery::EQueryType>(type), tipPeriod);
    }
    bool ShowEstablishCityDialog() override {
        sc4::cSC4View3DWin* view = CityView3DWin();
        return view && native::RunEstablishCityDialog(static_cast<cIGZWin*>(static_cast<cGZWin*>(view)));
    }
    bool DisasterAlertActive() override { return native::NativeDisasterOngoing(); }   // the disaster layer's IsADisasterOngoing
    bool SituationAlertActive() override {   // the situation manager's GetActiveSituation is set
        auto* m = SituationManager();
        return m && m->GetActiveSituation() != nullptr;
    }
    bool SimulatorPaused() override { auto* s = CityComponentAs<cSC4Simulator>(CityComponent::Simulator, kGZCLSID_cSC4Simulator); return s && s->IsPaused(); }
    cIGZBuffer* CreateImageBuffer() override { auto* buffer = new cGZBuffer(); buffer->AddRef(); return buffer; }   // SL::spGS vslot 0x20: an uninitialised buffer
    IAdvisorWindow* AdvisorWindow() override { return native::NativeAdvisorWindow(); }
    cSC4ViewInputControl* CreateZoningTool(std::uint32_t type) override { return native::CreateZoneTool(static_cast<int>(type)); }
    cSC4ViewInputControl* CreateTerrainScrubberTool(std::uint32_t type, bool flag) override { return native::CreateTerrainScrubberTool(type, flag); }
    cSC4ViewInputControl* CreateLevelTerrainTool() override { return {}; }
    cSC4ViewInputControl* CreateCityScrollTool() override { return new cSC4ViewInputControlCityScroll(); }
    void CityScrollSetMouseScroll(cSC4ViewInputControl* tool, int x, int y) override {
        if (auto* scroll = dynamic_cast<cSC4ViewInputControlCityScroll*>(tool)) scroll->SetMouseScroll(x, y);
    }
    // RunGlobalTerrainTool (Mac i386 0x0051dce4): the effect's sound for the view's zoom (kGlobalTerrainEffectSoundPerZoomIds
    // 0x00850fd0), started at once under a new instance id.
    void PlayGlobalTerrainEffectSound() override {
        static constexpr std::uint32_t kSoundPerZoom[5] = {0xea8b965c, 0x0a8b96a0, 0x4a8b96d4, 0x0a8b96f1, 0xca8b9720};
        const int zoom = SL::spViewUtilities ? SL::spViewUtilities->Zoom() : 0;
        if (zoom < 0 || zoom > 4) return;   // the original indexes the table with the zoom unchecked
        DoSoundSystemEvent(1, kSoundPerZoom[zoom], NewSoundInstanceId());
    }
    // SL::TerrainView()->vslot 0x48(0) (the dynamic land overlay manager) -> AddHeightMap(0x7f94205): the contour lines as a
    // height-map overlay; RemoveOverlay takes it off again. -1 without a city view.
    int AddContourCallback() override {
        sc4::cSTEOverlayManager* overlays = native::DecalOverlayManager(0);
        const int id = overlays ? static_cast<int>(overlays->AddHeightMap(kContourHeightMapInstance)) : -1;
        static const bool debug = std::getenv("SC4_DEBUG") != nullptr;
        if (debug) std::printf("contours: height-map overlay %d (enabled %d)\n", id, overlays && id != -1 && overlays->OverlayIsEnabled(static_cast<std::uint32_t>(id)) ? 1 : 0);
        return id;
    }
    void RemoveContourCallback(int handle) override {
        if (sc4::cSTEOverlayManager* overlays = native::DecalOverlayManager(0)) overlays->RemoveOverlay(static_cast<std::uint32_t>(handle));
    }
    bool ProjectPointToViewport(cS3DVector3& point) override {
        if (!SL::spViewUtilities) return false;
        float xyz[3] = {point.x, point.y, point.z};
        SL::spViewUtilities->ProjectPointToViewportCoordinates2D(xyz);
        point.x = xyz[0];
        point.y = xyz[1];
        point.z = xyz[2];
        return true;
    }
    bool PopupModalDialogsSafe() override { return ActiveWinMgr() && ActiveWinMgr()->modalDialogs; }
    bool PopupModalDialogsEnabled() override { return ActiveWinMgr() && ActiveWinMgr()->modalDialogs; }
    std::uint32_t SimulationFrameCounter() override { return {}; }
    bool FilterUiCommandMessage(cGZMessage& message) override { return native::NativeTutorialFilterUiCommand(message); }
    void TutorialCatalogOpened(cISC4WinCatalogView* catalog) override { native::NativeTutorialCatalogOpened(catalog); }
    bool HandleUrl(cIGZString* link) override { return link && native::NativeHtmlLinkDoURL(*link); }
    // Only the reward cheat so far (it sets the view's +0x456 byte, ProcessCheatCode 0x10078526a); the other cheats are not ported.
    void ProcessCheatCode(std::uint32_t id, const char* cmdLine) override {
        if (id == 0x4a82253eu) rewardsUnlocked_ = true;   // "You don't deserve it"
        else if (id == 0xca7bc5b5u || id == 0xaa7a2746u || id == 0x6a7a276eu || id == 0x6976afa0u || id == 0x4a972f37u || id == 0x4ac096cau)
            SC4_UNWIRED("cSC4View3DWin::ProcessCheatCode: the MagnifyView / ToggleZots / ToggleZones / AnimationRecorder / EnableAnimationStepping / BuildingPlop cheats are not ported");
    }
    void AddBuildingsToItemList(std::uint32_t menuId, const std::set<std::uint32_t>& include, const std::set<std::uint32_t>& exclude,
                                std::multimap<int, cISC4CatalogItem*>& items) override {
        native::AddBuildingsToItemList(include, exclude, rewardsUnlocked_, items);
    }
    void InvokeCatalogItemCommand(std::uint32_t, std::uint32_t, bool) override {}   // unused: the view's InvokeCatalogItemCommand is whole
    void ExecuteAcceleratorCommand(std::uint32_t command) override { native::ExecuteGameCommand(command); }
    void Update24HourClockHover() override {}
    void UpdateCursorTextPositions(int x, int y) override {}
    void SetDrawContextFrameMarker(void* drawContext, std::uint32_t frame) override {}
    void OnView3DCreate(cISC4View3DWin* view) override {}
    void OnView3DShutdown() override { SL::OnView3DShutdown(); }
    void OnView3DInit() override { native::CityViewInit(); }   // SL::OnView3DInit with the view's objects
    void GetCityName(cIGZString& out) override { if (cSC4City* city = LoadedCity()) city->GetCityName(out); }
    int VisualTimeMode() override { return native::NativeVisualTimeMode(); }   // SL::sp24HourClock vslot 0x30
    int CityPopulation() override { auto* r = CityComponentAs<cSC4ResidentialSimulator>(CityComponent::ResidentialSimulator, kGZCLSID_cSC4ResidentialSimulator); return r ? r->GetPopulation() : 0; }
    std::int64_t CityFunds() override { auto* b = CityComponentAs<cSC4BudgetSimulator>(CityComponent::BudgetSimulator, kGZCLSID_cSC4BudgetSimulator); return b ? b->GetTotalFunds() : 0; }
    void FormatNumber(std::int64_t value, cIGZString& out) override { out.AssignFromCString(GroupDigits(value).c_str()); }
    // SC4MakeMoneyString: the simoleon sign and the grouped amount ("§1,290,385", "-§500")
    void MakeMoneyString(std::int64_t value, cIGZString& out) override {
        const std::string grouped = GroupDigits(value < 0 ? -value : value);
        out.AssignFromCString(((value < 0 ? "-\xc2\xa7" : "\xc2\xa7") + grouped).c_str());   // UTF-8, as the fonts decode
    }
    bool SimulationDate(int& day, int& month, int& year, int& startYear) override {
        auto* simulator = CityComponentAs<cSC4Simulator>(CityComponent::Simulator, kGZCLSID_cSC4Simulator);
        if (!simulator) return false;
        simulator->GetSimDate(&year, &month, &day, nullptr, nullptr);
        cGZDate start;
        simulator->GetSimStartDate(start);
        startYear = static_cast<int>(start.Year());
        return true;
    }
    // RZ::LM() vslot 0x50 (the English short date): "Jan 1, 00"
    void FormatDate(int day, int month, int year, cIGZString& out) override {
        static const char* const kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        char text[32];
        std::snprintf(text, sizeof text, "%s %d, %02d", kMonths[(month >= 1 && month <= 12) ? month - 1 : 0], day, year);
        out.AssignFromCString(text);
    }
    std::uint32_t MakeNativeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) override { return 0xff000000u | (std::uint32_t(r) << 16) | (std::uint32_t(g) << 8) | b; }
    // RZ::WCM() vslot 0x28: the window control manager's font of that style (borrowed)
    cIGZFont* WindowFont(std::uint32_t id) override { return g_progressServices.Font(id); }
    bool BudgetDialogOpen(cIGZUnknown* dialog) override { return native::BudgetUiOpen(dialog); }
    void RefreshBudgetDialog(cIGZUnknown* dialog) override { native::BudgetUiRefresh(dialog); }
    void ShutdownNewsTickerWinProc(cIGZUnknown* proc) override { native::ShutdownNewsTickerWindow(proc); }
    void ShutdownMySimWinProc(cIGZUnknown* proc) override {}
    void ShutdownBudgetUIWinProc(cIGZUnknown* proc) override {}
    void ShutdownGraphsViewWinProc(cIGZUnknown* proc) override {}
    void ShutdownBuildingSelectWinProc(cIGZUnknown* proc) override { static_cast<BuildingSelectWindow*>(static_cast<IBuildingSelectWindow*>(proc))->Shutdown(); }

private:
    bool rewardsUnlocked_ = false;   // the view's +0x456
};
StubView3DWin& InstalledHost() { static StubView3DWin host; return host; }
}  // namespace

// new cSC4GamePreferencesWinProc; AddRef; ShowDialog(kGameOptions); DoModalWin: the "Game Options" dialog, from the city view's
// options menu and the region screen's (`preferences` is the blob its check boxes mirror, bound per call).
void RunGameOptionsDialog(std::function<std::uint8_t*()> preferences) {
    using sc4::cSC4GamePreferencesWinProc;
    if (!ActiveWinMgr() || !ActiveWinMgr()->modalDialogs) return;
    sc4::GamePreferencesServices& services = sc4::GetGamePreferencesServices();
    services.preferences = std::move(preferences);
    if (!services.windowManager) {
        services.windowManager = [] { return static_cast<cIGZWinMgr*>(ActiveWinMgr()); };
        services.cityLoaded = [] { return LoadedCity() != nullptr; };
        services.appPresent = [] { return BootApplication() != nullptr; };
        services.postMessage = [](std::uint32_t type) {
            if (!SL::spMS2) return;
            auto* message = new cRZMessage2Standard();
            message->AddRef();
            message->SetType(type);
            SL::spMS2->MessagePost(message, false);
            message->Release();
        };
    }
    auto* proc = new cSC4GamePreferencesWinProc();
    proc->AddRef();
    const bool alreadyShowing = cSC4GamePreferencesWinProc::Displayed();
    proc->ShowDialog(cSC4GamePreferencesWinProc::kGameOptions);
    // DoModalWin dereferences the dialog: run it only when this ShowDialog built one (the script loaded, no other copy is up)
    if (!alreadyShowing && cSC4GamePreferencesWinProc::Displayed()) proc->DoModalWin();
    else SC4_UNWIRED("RunGameOptionsDialog: the game options dialog was not built");
    proc->Release();
}

// new cSC4AudioPreferencesWinProc; ShowDialog; DoModalWin: the "Audio Options" dialog, from the city view's options menu and the
// region screen's. Its controls reach the sound through the game's own audio messages.
void RunAudioOptionsDialog(std::function<std::uint8_t*()> preferences) {
    using sc4::cSC4AudioPreferencesWinProc;
    if (!ActiveWinMgr() || !ActiveWinMgr()->modalDialogs || cSC4AudioPreferencesWinProc::Displayed()) return;
    sc4::AudioPreferencesServices& services = sc4::GetAudioPreferencesServices();
    services.preferences = std::move(preferences);
    if (!services.windowManager) {
        services.windowManager = [] { return static_cast<cIGZWinMgr*>(ActiveWinMgr()); };
        services.savePreferences = [] { if (cSC4App* app = BootApplication()) app->SavePreferences(); };
        services.dataDirectory = [] { return platform::GameDataDir(); };
    }
    auto* proc = new cSC4AudioPreferencesWinProc();
    proc->AddRef();
    if (proc->ShowDialog()) proc->DoModalWin();
    else SC4_UNWIRED("RunAudioOptionsDialog: the audio options dialog was not built");
    proc->Release();
}

// new cSC4VideoPreferencesWinProc; ShowDialog; DoModalWin: the "Graphic Options" dialog. A changed resolution becomes the
// game's logical height at the next start (<documents>/display.conf, read before the window opens): on modern displays the
// choice sets how much room the UI has, as it did on the original's screens.
void RunGraphicOptionsDialog(std::function<std::uint8_t*()> preferences) {
    using sc4::cSC4VideoPreferencesWinProc;
    if (!ActiveWinMgr() || !ActiveWinMgr()->modalDialogs || cSC4VideoPreferencesWinProc::Displayed()) return;
    sc4::VideoPreferencesServices& services = sc4::GetVideoPreferencesServices();
    services.preferences = std::move(preferences);
    if (!services.windowManager) {
        services.windowManager = [] { return static_cast<cIGZWinMgr*>(ActiveWinMgr()); };
        services.savePreferences = [] { if (cSC4App* app = BootApplication()) app->SavePreferences(); };
        services.applyVideo = [](std::uint8_t* block) {
            if (cSC4App* app = BootApplication()) app->ApplyVideoPreferences(*reinterpret_cast<sc4::SC4VideoPreferences*>(block));
        };
        services.resolution = [](int index, int& width, int& height) { return ResolutionChoice(index, width, height); };
        services.currentHeight = [] { return CurrentGameHeight(); };
        services.automaticVideo = [](std::uint8_t* block) {
            if (cSC4App* app = BootApplication()) app->GetAutoVideoPreferences(reinterpret_cast<sc4::SC4VideoPreferences*>(block));
        };
        services.postMessage = [](std::uint32_t type) {
            if (!SL::spMS2) return;
            auto* message = new cRZMessage2Standard();
            message->AddRef();
            message->SetType(type);
            SL::spMS2->MessagePost(message, false);
            message->Release();
        };
    }
    std::uint8_t* blob = services.preferences ? services.preferences() : nullptr;
    std::uint32_t height = 0;
    if (blob) std::memcpy(&height, blob + 0xf20, 4);
    if (blob) {   // the block's resolution starts as the one running now (the dialog's buttons compare with it)
        int width = 0, running = CurrentGameHeight();
        for (int i = 0; i < 4; ++i) { int w = 0, h = 0; if (ResolutionChoice(i, w, h) && h == running) width = w; }
        if (width) { const std::uint32_t w32 = std::uint32_t(width), h32 = std::uint32_t(running); std::memcpy(blob + 0xf1c, &w32, 4); std::memcpy(blob + 0xf20, &h32, 4); height = h32; }
    }
    auto* proc = new cSC4VideoPreferencesWinProc();
    proc->AddRef();
    if (proc->ShowDialog()) proc->DoModalWin();
    else SC4_UNWIRED("RunGraphicOptionsDialog: the graphic options dialog was not built");
    proc->Release();
    std::uint32_t chosen = 0;
    if (blob) std::memcpy(&chosen, blob + 0xf20, 4);
    if (blob && chosen != height && chosen >= 480) SaveDisplayHeight(static_cast<int>(chosen));
}

// #City# #Mayor# #Year# #Population# for the string detokenizer (cSCStringDetokenizerDefault): the loaded city, its simulator
// date and its residential population, numbers grouped as the English locale does. PARTIAL: #Region# (the region's name is
// not asked) answers "unknown".
class DetokenizerHost final : public sc4::IDetokenizerHost {
public:
    bool CityName(cIGZString& out) override {
        cSC4City* city = LoadedCity();
        if (!city) return false;
        city->GetCityName(out);
        return true;
    }
    bool MayorName(cIGZString& out) override {
        cSC4City* city = LoadedCity();
        if (!city) return false;
        city->GetMayorName(out);
        return true;
    }
    bool GetYear(int& year) override {
        auto* simulator = CityComponentAs<cSC4Simulator>(CityComponent::Simulator, kGZCLSID_cSC4Simulator);
        if (!simulator) return false;
        int month = 0, day = 0;
        simulator->GetSimDate(&year, &month, &day, nullptr, nullptr);
        return true;
    }
    bool GetPopulation(int& population) override {
        auto* residential = CityComponentAs<cSC4ResidentialSimulator>(CityComponent::ResidentialSimulator, kGZCLSID_cSC4ResidentialSimulator);
        if (!residential) return false;
        population = static_cast<int>(residential->GetPopulation());
        return true;
    }
    void FormatNumber(long value, cIGZString& out) override { out.AssignFromCString(GroupDigits(value).c_str()); }
    bool GetRegionName(cIGZString&) override {
        SC4_UNWIRED("cSCStringDetokenizerDefault #Region#: the region's name is not asked (the token reads unknown)");
        return false;
    }
};

// The advisor popups (cSC4AdvisorPopupTracker): an activated advice of high enough priority opens the advisor's modal
// page while the game pauses, with the advisor's animated head.
class AdvisorPopupHost final : public sc4::IAdvisorPopupTrackerHost {
    static bool Debug() { static const bool on = std::getenv("SC4_POPUPDEBUG") != nullptr; return on; }
public:
    cIGZMessageServer2* Messages() override { return SL::spMS2; }
    bool PopupModalDialogsSafe() override { return ActiveWinMgr() && ActiveWinMgr()->modalDialogs; }
    bool PopupModalDialogsEnabled() override { return ActiveWinMgr() && ActiveWinMgr()->modalDialogs; }
    bool AdvisorPopupsPreferenceEnabled() override {
        cSC4App* app = BootApplication();
        const std::uint8_t* prefs = app ? app->GetPreferences() : nullptr;
        return prefs && prefs[0xf0b] != 0;   // Game Preferences checkbox 0x4a7f19f5
    }
    bool AdvicePopupPriorityThreshold(int& threshold) override {
        cSC4City* city = LoadedCity();
        cIGZUnknown* object = city ? city->GetComponent(CityComponent::AdvisorSystem) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(sc4::kGZIID_cISC4AdvisorSystem, &raw) || !raw) return false;
        auto* system = static_cast<sc4::cISC4AdvisorSystem*>(raw);
        threshold = system->GetAdvicePopupPriorityThreshold();
        system->Release();
        if (const char* forced = std::getenv("SC4_POPUPTHRESHOLD")) threshold = std::atoi(forced);   // debug: test the dialog on any city
        if (Debug()) std::printf("popup: show tick, threshold %d, paused %d, safe %d, enabled %d, preference %d\n", threshold, SimulatorPaused(),
                                 PopupModalDialogsSafe(), PopupModalDialogsEnabled(), AdvisorPopupsPreferenceEnabled());
        return true;
    }
    bool TutorialRunning() override { return native::NativeTutorialRunning(); }   // any tutorial suppresses the popups
    bool VehicleManagerPaused() override { return false; }
    bool SimulatorPaused() override {
        auto* simulator = CityComponentAs<cSC4Simulator>(CityComponent::Simulator, kGZCLSID_cSC4Simulator);
        return simulator && simulator->IsPaused();
    }
    bool CreateWindowFromScript(const cGZPersistResourceKey& key, std::uint32_t windowId, std::uint32_t iid, void** out) override {
        *out = nullptr;
        WinMgr* manager = ActiveWinMgr();
        cIGZWin* window = native::CreateWindowFromScript(key, manager ? manager->GetMainWindow() : nullptr, windowId);
        if (Debug()) std::printf("popup: dialog script %08x/%08x -> %s\n", key.group, key.instance, window ? "created" : "FAILED");
        if (!window) return false;
        const bool ok = window->QueryInterface(iid, out) && *out;
        window->Release();
        return ok;
    }
    // cSC4AutoGamePause(false, false, true, true): the simulator's hidden pause (the view needs nothing natively).
    void BeginGamePause() override {
        if (auto* simulator = CityComponentAs<cSC4Simulator>(CityComponent::Simulator, kGZCLSID_cSC4Simulator)) simulator->HiddenPause();
    }
    void EndGamePause() override {
        if (auto* simulator = CityComponentAs<cSC4Simulator>(CityComponent::Simulator, kGZCLSID_cSC4Simulator)) simulator->HiddenResume();
    }
    cSLWinLocationSaver* WindowLocationSaver() override { return static_cast<cSLWinLocationSaver*>(SL::spWinLocationSaver); }
    cIGZWinMgr* WindowManager() override { return ActiveWinMgr(); }
    // RZ::WM() vslot 0x128 GZSetFocus(SL::spView3DWin): the keys go back to the city view when the popup is done.
    void FocusView3D() override {
        WinMgr* manager = ActiveWinMgr();
        sc4::cSC4View3DWin* view = CityView3DWin();
        if (manager && view) manager->GZSetFocus(static_cast<cIGZWin*>(static_cast<cGZWin*>(view)));
    }
    // operator new + the constructor: a count of 0, the tracker takes the reference.
    sc4::IAdvisorUIImageWinProc* CreateImageWinProc(int advisorId) override { return new sc4::cSC4AdvisorUIImageWinProc(advisorId); }
    bool SystemFont(std::uint32_t fontId, sc4::AdvisorPopupFont& out) override {
        gz::IWinFontSource* fonts = gz::WinFontSource();
        gz::IWinFont* style = fonts ? fonts->Font(fontId) : nullptr;
        cIGZFont* font = style ? style->AsFont() : nullptr;
        if (!font) return false;
        // The name the page's FONT FACE carries: the style's own name, which the native HTML font system resolves to the
        // style's face and parameters (MessageHeader = StocletITC TT bold, stretched 1.2; MessageBody = Arta 0.95).
        const std::string styleName = FontStyleName(fontId);
        if (!styleName.empty()) out.name = cRZString(styleName.c_str());
        out.bold = font->GetStyle(1);
        out.size = font->GetSize();
        return true;
    }
    bool DoURL(cIGZString* link) override {
        return link && native::NativeHtmlLinkDoURL(*link);
    }
};

// The advisor's animated head (cSC4AdvisorUIImageWinProc): the city's advisor, the S3D head animations, the animation
// tick manager, the main window's size, and the city renderer with its driver for the draw.
class AdvisorHeadHost final : public sc4::IAdvisorUIImageHost {
public:
    sc4::cISC4Advisor* Advisor(int advisorId) override {
        cSC4City* city = LoadedCity();
        cIGZUnknown* object = city ? city->GetComponent(CityComponent::AdvisorSystem) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(sc4::kGZIID_cISC4AdvisorSystem, &raw) || !raw) return nullptr;
        auto* system = static_cast<sc4::cISC4AdvisorSystem*>(raw);
        sc4::cISC4Advisor* advisor = system->GetAdvisor(advisorId);   // borrowed: the system keeps it
        system->Release();
        return advisor;
    }
    bool LlamaCheatActive() override { return NativeLlamaCheatActive(); }
    s3d::cIS3DModelInstanceAnimated* CreateModel(const cGZPersistResourceKey& key) override { return NativeCreateAnimatedModel(key); }
    void AddTickable(cIGZMessageTarget2* target, std::uint32_t priority) override {
        if (auto* ticks = AnimationTickManager()) ticks->AddTickableObject(target, priority);
    }
    void RemoveTickable(cIGZMessageTarget2* target, std::uint32_t priority) override {
        if (auto* ticks = AnimationTickManager()) ticks->RemoveTickableObject(target, priority);
    }
    bool MainWindowSize(std::uint16_t& width, std::uint16_t& height) override {
        WinMgr* manager = ActiveWinMgr();
        cIGZWin* main = manager ? manager->GetMainWindow() : nullptr;
        if (!main) return false;
        width = static_cast<std::uint16_t>(main->GetW());
        height = static_cast<std::uint16_t>(main->GetH());
        return true;
    }
    cISC43DRender* Renderer() override { return SL::spRenderer ? static_cast<cISC43DRender*>(SL::spRenderer) : nullptr; }
    gd::cIGZGDriver* Driver() override {
        cISC43DRender* renderer = Renderer();
        SC4DrawContext* context = renderer ? renderer->GetDrawContext() : nullptr;
        return context ? context->Driver() : nullptr;
    }
};

// cSC4View3DWin's +0x4d0 slot: the recovered tracker behind the view's Init / Shutdown interface.
class AdvisorPopupQueue final : public sc4::IAdvisorPopupQueue {
public:
    AdvisorPopupQueue() : tracker_(new sc4::cSC4AdvisorPopupTracker()) { tracker_->AddRef(); }
    ~AdvisorPopupQueue() { tracker_->Release(); }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_ == 0) { delete this; return 0; }
        return refs_;
    }
    bool Init() override { return tracker_->Init(); }
    bool Shutdown() override { return tracker_->Shutdown(); }

private:
    std::uint32_t refs_ = 0;
    sc4::cSC4AdvisorPopupTracker* tracker_;
};

sc4::IAdvisorPopupQueue* CreateAdvisorPopupQueue() {
    auto* queue = new AdvisorPopupQueue();
    queue->AddRef();   // the view's reference (it Releases after Shutdown)
    return queue;
}

void SetLoadingScreenPresenter(LoadingScreenPresenter* presenter) { g_loadingPresenter = presenter; }

void InstallView3DWinStub() {
    sc4::SetView3DWinHost(&InstalledHost());
    static DetokenizerHost detokenizer;
    sc4::SetDetokenizerHost(&detokenizer);
    static AdvisorPopupHost popups;
    sc4::SetAdvisorPopupTrackerHost(&popups);
    static AdvisorHeadHost heads;
    sc4::SetAdvisorUIImageHost(&heads);
    static AlertBorderProperties alertBorderProperties;
    sc4::SetWinRenderProperties(&alertBorderProperties);
}

// The kit's `cityview`: what the city view's parts show, for the checks pixels cannot make.
void NativeCityViewReport() {
    sc4::cSC4View3DWin* view = CityView3DWin();
    if (!view) { std::printf("cityview: no city view\n"); return; }
    auto* viewWin = static_cast<cIGZWin*>(static_cast<cGZWin*>(view));
    cIGZWin* compass = viewWin->GetChildWindowFromIDRecursive(sc4::kCompassChildId);
    std::printf("cityview: compass %s rotation %d", compass ? "present" : "missing", g_compassHost.CameraRotation());
    if (compass) {
        void* proc = nullptr;
        void* genRaw = nullptr;
        if (compass->QueryInterface(sc4::kGZIID_cIGZWinGenView, &genRaw) && genRaw) {
            auto* gen = static_cast<gz::cIGZWinGen*>(genRaw);
            proc = gen->GetWinProc();
            gen->Release();
        }
        const cRZRect& ca = compass->GetAreaAbsolute();
        std::printf(" at %d,%d,%d,%d visible %d proc %s faces", ca.left, ca.top, ca.right, ca.bottom, compass->IsVisible() ? 1 : 0, proc ? "attached" : "none");
        std::uint32_t id = compass->GetID() + 1;
        for (cIGZWin* face = compass->GetChildWindowFromID(id); face; face = compass->GetChildWindowFromID(++id))
        {
            const cRZRect& a = face->GetArea();
            std::printf(" %08x:%d(%d,%d,%d,%d)", id, face->IsVisible() ? 1 : 0, a.left, a.top, a.right, a.bottom);
        }
    }
    std::printf("\n");
    cIGZWin* border = viewWin->GetChildWindowFromID(sc4::kCompassWindowId);
    StubView3DWin& host = InstalledHost();
    std::printf("cityview: alert border %s", border ? "present" : "missing");
    if (border) {
        const cRZRect& area = border->GetArea();
        std::printf(" area %d,%d,%d,%d visible %d", area.left, area.top, area.right, area.bottom, border->IsVisible() ? 1 : 0);
    }
    cSC4RenderProperties* properties = AppRenderPropertySet();
    std::printf(" disaster %d situation %d paused %d borders-on %d\n", host.DisasterAlertActive() ? 1 : 0, host.SituationAlertActive() ? 1 : 0,
                host.SimulatorPaused() ? 1 : 0, properties && properties->BoolValue(kPropDisplayAlertBorders) ? 1 : 0);
    // the building style selector: the script window and the proc on it, and the tract developer's settings it shows
    cIGZWin* styles = viewWin->GetChildWindowFromIDRecursive(sc4::kBuildingSelectScriptId);
    std::printf("cityview: style selector %s", styles ? "present" : "missing");
    if (styles) {
        void* genRaw = nullptr;
        gz::cIGZWinProc* proc = nullptr;
        if (styles->QueryInterface(gz::kGZIID_cIGZWinGen, &genRaw) && genRaw) {
            auto* gen = static_cast<gz::cIGZWinGen*>(genRaw);
            proc = gen->GetWinProc();
            gen->Release();
        }
        const bool recovered = proc && dynamic_cast<cSC4BuildingSelectWinProc*>(proc) != nullptr;
        std::printf(" visible %d proc %s collapsed %d expanded %d", styles->IsVisible() ? 1 : 0, recovered ? "cSC4BuildingSelectWinProc" : (proc ? "other" : "none"),
                    styles->GetChildWindowFromID(0x8bca20c3) ? 1 : 0, styles->GetChildWindowFromID(0x2bc619af) ? 1 : 0);
    }
    TractStyles tract;
    std::printf(" tract styles");
    for (std::uint32_t style : tract.GetActiveStyles()) std::printf(" %x", style);
    std::printf(" all-at-once %d years %d\n", tract.IsUsingAllStylesAtOnce() ? 1 : 0, tract.GetYearsBetweenStyleChanges());
    // the cost indicator's window and fonts (it shows only while a tool prices something under the cursor)
    std::printf("cityview: cost indicator font %08x %s fallback %08x %s\n", sc4::kCostIndicatorFontId,
                g_progressServices.Font(sc4::kCostIndicatorFontId) ? "present" : "missing", sc4::kCostIndicatorFallbackFontId,
                g_progressServices.Font(sc4::kCostIndicatorFallbackFontId) ? "present" : "missing");
}


}  // namespace native
