// The native IView3DWinHost (started from scripts/gen_stub_hosts.py's neutral host, now hand edited and skipped by the
// generator): the city exists once the view is up, the zoom stops at the last level, the right-button scroll tool
// is the recovered cSC4ViewInputControlCityScroll, and AddGUI builds its windows through the UI-script service and
// nSC4UI's window-set helpers. Everything else is still neutral.
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "sc4/View3DWin.h"
#include "app/NativeCity.h"
#include "platform/Unwired.h"
#include "app/NativeLighting.h"
#include "app/NativeCitySystems.h"
#include "sc4/CitySituationManager.h"
#include "sc4/Ordinance.h"
#include "sc4/cSTETerrain.h"
#include "sc4/ServiceLocator.h"
#include "sc4/ViewInputControlCityScroll.h"
#include "app/NativeUi.h"
#include "app/NativeBoot.h"
#include "sc4/App.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/ResidentialSimulator.h"
#include "sc4/Simulator.h"
#include "sc4/cSC4City.h"
#include <cstdio>
#include "app/NativeCityUi.h"
#include "app/NativeBudgetUi.h"
#include "app/NativeNetworkTool.h"
#include "app/NativeZoneTool.h"
#include "app/NativeCatalogUi.h"
#include "gz/Buffer.h"
#include "gz/WinBtn.h"
#include "sc4/SC4UI.h"
#include "sc4/ViewInputControlQuery.h"
#include "sc4/ViewInputControlPlaceLot.h"
#include "sc4/ViewInputControlSignTool.h"
#include "sc4/GamePreferencesWinProc.h"
#include "platform/GameDataPath.h"
#include "app/GameAudio.h"
#include "app/NativeAudio.h"
#include "sc4/AudioPreferencesWinProc.h"
#include "sc4/VideoPreferencesWinProc.h"
#include "sc4/SmallWinProcs.h"
#include <chrono>
#include "sc4/PropDeveloper.h"
#include "sc4/AnimationTickManager.h"
#include "app/NativeAnimationTicks.h"
#include "app/NativeWinMgr.h"

namespace native {
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

struct StubView3DWin final : IView3DWinHost {
    View3DNotifications notifications_;
    cIGZMessageServer2* MessageServer2() override { return {}; }
    // the preferences blob (0x1734 bytes): all defaults off (no edge scrolling)
    std::uint8_t* Preferences() override { static std::uint8_t blob[0x1734] = {}; return blob; }
    bool CityExists() override { return LoadedCity() != nullptr; }
    void PostGameMessage(std::uint32_t type) override {}
    INotificationHelper3D* NotificationHelper() override { return &notifications_; }
    void RegisterCommandTable() override {}
    void UnregisterCommandTable() override {}
    void AddCheatCodeTarget(cIGZWin* target) override {}
    void RemoveCheatCodeTarget(cIGZWin* target) override {}
    bool RegisterCheatCode(std::uint32_t id, const char* text) override { return {}; }
    bool UnregisterCheatCode(std::uint32_t id) override { return {}; }
    bool CheatCodeManagerPresent() override { return {}; }
    bool LotPlopCheatsEnabled() override { return {}; }
    cIGZWinKeyAccelerator* CreateKeyAccelerator() override { return {}; }
    cSC4RenderProperties* GlobalRenderProperties() override { return {}; }
    cSC4RenderProperties* AppRenderProperties() override { return {}; }
    void SetScrollLightingHighlight() override {}
    void RestoreLighting() override {}
    int VirtualZoomLimit() override { return 4; }
    bool ProgressScreenWanted() override { return {}; }
    cIGZUnknown* CreateProgressScreen(cIGZWin* parent, const cRZRect& area, bool flag) override { return {}; }
    void InitProgressScreen(cIGZUnknown* screen) override {}
    void SetProgressCaption(cIGZUnknown* screen, int id, const cIGZString& text) override {}
    void ShutdownProgressScreen(cIGZUnknown* screen) override {}
    void ShutdownValidationWindow(cIGZUnknown* window) override {}
    void ShutdownLotConfigurationEditors() override {}
    void ShutdownModelMaker(cIGZUnknown* modelMaker) override {}
    void ShutdownCameraObject(cIGZUnknown* camera) override {}
    void ShutdownSimulationClock(cIGZUnknown* clock) override {}
    bool UserDataDirectory(cIGZString& out) override { return {}; }
    cIGZWin* CreateWinAnimationRecorder() override { return {}; }
    void RecorderSetAnimationOutputDirectory(cIGZWin* recorder, const cIGZString& directory, bool create) override {}
    ICityMouseRouter* CityMouseRouter() override { return {}; }
    bool CanInvokeZoningTool() override { return true; }   // an established city: the start panel stays closed
    ITerrainEditor* TerrainEditor() override { return {}; }
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
    // GZCOM()->CreateInstance(0x22ecfc47, cIGZWin): the button that brings the hidden UI back (AddGUI skins it, gives it id 0x43 and
    // hides it until MinimizeUI). It was null natively, so a hidden UI could not be shown again.
    cIGZWin* CreateToolbarButton() override {
        auto* button = new gz::cGZWinBtn(ActiveWinMgr());
        button->AddRef();
        return static_cast<cIGZWin*>(static_cast<cGZWin*>(button));
    }
    cIGZWin* CreateCompassWindow() override { return {}; }
    void LocalisedString(std::uint32_t group, std::uint32_t instance, cRZString& out) override {}
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
    INewsTickerWindow* CreateNewsTickerWinProc() override { return {}; }
    IMySimWindow* CreateMySimWinProc(gz::cIGZWinGen* panel) override { return native::CreateMySimWindow(panel); }
    IBudgetUIWindow* CreateBudgetUIWinProc() override { return native::CreateBudgetUiWindow(); }
    IGraphsViewWindow* CreateGraphsViewWinProc(cIGZWin* window) override { return native::CreateGraphsViewWindow(window); }
    IBuildingSelectWindow* CreateBuildingSelectWinProc(cIGZWin* window) override { return {}; }
    IAdvisorPopupQueue* CreateAdvisorPopupTracker() override { return {}; }
    void AttachCompassWinProc(gz::cIGZWinGen* window) override {}
    cISC4WinCalloutBox* CreateCalloutBox() override { return native::CreateCalloutBox(); }
    gz::cIGZWinBMP* CreateWinBMP() override { return {}; }
    void InitMapView(cIGZWin* window) override {}
    bool IsKeyDownNow(std::uint32_t key) override { return {}; }
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
    void GoToNextDisaster() override { SC4_UNWIRED("IView3DWinHost::GoToNextDisaster (cSC4DisasterLayer not recovered)"); }
    cSC4ViewInputControl* CreateDispatchTool(int) override { SC4_UNWIRED("IView3DWinHost::CreateDispatchTool (cSC4ViewInputControlDispatch not recovered)"); return nullptr; }
    cSC4ViewInputControl* CreateNetworkTool(int type) override { return native::CreateNetworkTool(type); }
    void ShowMyStreetSim() override { SC4_UNWIRED("IView3DWinHost::ShowMyStreetSim"); }
    // The native city save writes an incomplete file the game cannot load back (SAVE-1): until it is whole the city is save-disabled,
    // so every save request takes the game's own "saving is disabled" path instead of replacing the city file.
    void PostCityInit() override {
        if (cSC4City* city = LoadedCity(); city && !city->IsSaveDisabled()) city->DisableSave();
    }
    ISimulatorPauseSink* Simulator() override { static SimulatorView view; return CityExists() ? &view : nullptr; }
    cSC4ViewInputControl* CreateSignTool() override { return new cSC4ViewInputControlSignTool(); }
    cSC4ViewInputControl* CreatePlaceLotTool(std::uint32_t lot) override { return new cSC4ViewInputControlPlaceLot(0, lot, 0, false); }
    cSC4ViewInputControl* CreateDemolishTool() override { SC4_UNWIRED("IView3DWinHost::CreateDemolishTool (cSC4ViewInputControlDemolish not recovered)"); return nullptr; }
    cSC4ViewInputControl* CreateLabelTool() override { SC4_UNWIRED("IView3DWinHost::CreateLabelTool (cSC4ViewInputControlLabelTool not recovered)"); return nullptr; }
    cSC4ViewInputControl* CreateSignLabelDemolishTool() override { SC4_UNWIRED("IView3DWinHost::CreateSignLabelDemolishTool (not recovered)"); return nullptr; }
    cSC4ViewInputControl* CreateObliterateCityTool() override { SC4_UNWIRED("IView3DWinHost::CreateObliterateCityTool (cSC4ObliterateCity not recovered)"); return nullptr; }
    cSC4ViewInputControl* CreateFloraTool(std::uint32_t) override { SC4_UNWIRED("IView3DWinHost::CreateFloraTool (cSC4ViewInputControlFlora not recovered)"); return nullptr; }
    cSC4ViewInputControl* CreateNetworkIntxTool(std::uint32_t) override { SC4_UNWIRED("IView3DWinHost::CreateNetworkIntxTool (not recovered)"); return nullptr; }
    bool CursorOverView(cIGZWin* view, int& x, int& y) override {
        WinMgr* manager = ActiveWinMgr();
        if (!manager) return false;
        manager->GetCursorPos(view, x, y);                              // RZ::WM() vslot 0x110
        return true;
    }
    void InvokeDisaster(std::uint32_t) override { SC4_UNWIRED("IView3DWinHost::InvokeDisaster (cSC4View3DWin::InvokeDisaster not recovered)"); }
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
    void ShowSnapshotAlbum(cIGZWin*) override { SC4_UNWIRED("IView3DWinHost::ShowSnapshotAlbum (cSC4WinSnapshotAlbum not recovered)"); }
    void CreateSnapshotTaker(cIGZWin*) override { SC4_UNWIRED("IView3DWinHost::CreateSnapshotTaker (cSC4WinSnapshotTaker not recovered)"); }
    void RequestSaveCity(bool announce, bool alternate) override { if (cSC4App* app = BootApplication()) app->RequestSaveCity(announce, alternate); }
    void RequestGoToRegionView(bool ask) override { if (cSC4App* app = BootApplication()) app->RequestGoToRegionView(ask); }
    void RequestQuit(bool ask, bool save) override { if (cSC4App* app = BootApplication()) app->RequestQuit(ask, save); }
    bool AnimationSystemPaused() override { auto* atm = AnimationTickManager(); return atm && atm->IsAnimationSystemPaused(); }
    void PauseAnimationSystem(bool pause) override { if (auto* atm = AnimationTickManager()) atm->PauseAnimationSystem(pause); }
    void StepAnimationSystem() override { if (auto* atm = AnimationTickManager()) atm->StepAnimationSystem(); }
    void ToggleSignsAndLabels() override {
        if (auto* props = CityComponentAs<cSC4PropDeveloper>(CityComponent::PropDeveloper, kGZCLSID_cSC4PropDeveloper)) props->ToggleSignsAndLabels();
    }
    void PlayUISound(std::uint32_t) override { SC4_UNWIRED("IView3DWinHost::PlayUISound (sound system)"); }
    void AlignWindowToWindow(std::uint32_t windowId, std::uint32_t childId, std::uint32_t alignId) override {
        nSC4UI::AlignWindowToWindow(windowId, childId, alignId, nullptr);
    }
    cSC4ViewInputControl* CreateQueryTool(int type, std::uint32_t tipPeriod) override {
        return new cSC4ViewInputControlQuery(static_cast<cSC4ViewInputControlQuery::EQueryType>(type), tipPeriod);
    }
    bool ShowEstablishCityDialog() override { return {}; }
    bool DisasterAlertActive() override { return {}; }
    bool SituationAlertActive() override { return {}; }
    bool SimulatorBusy() override { return {}; }
    cIGZBuffer* CreateImageBuffer() override { return {}; }
    IAdvisorWindow* AdvisorWindow() override { return {}; }
    cSC4ViewInputControl* CreateZoningTool(std::uint32_t type) override { return native::CreateZoneTool(static_cast<int>(type)); }
    cSC4ViewInputControl* CreateTerrainScrubberTool(std::uint32_t type, bool flag) override { return {}; }
    cSC4ViewInputControl* CreateLevelTerrainTool() override { return {}; }
    cSC4ViewInputControl* CreateCityScrollTool() override { return new cSC4ViewInputControlCityScroll(); }
    void CityScrollSetMouseScroll(cSC4ViewInputControl* tool, int x, int y) override {
        if (auto* scroll = dynamic_cast<cSC4ViewInputControlCityScroll*>(tool)) scroll->SetMouseScroll(x, y);
    }
    void PlayGlobalTerrainEffectSound() override {}
    int AddContourCallback() override { return {}; }
    void RemoveContourCallback(int handle) override {}
    bool ProjectPointToViewport(cS3DVector3& point) override { return {}; }
    void UpdateCostIndicator(float dt) override {}
    bool PopupModalDialogsSafe() override { return ActiveWinMgr() && ActiveWinMgr()->modalDialogs; }
    bool PopupModalDialogsEnabled() override { return ActiveWinMgr() && ActiveWinMgr()->modalDialogs; }
    std::uint32_t SimulationFrameCounter() override { return {}; }
    bool FilterUiCommandMessage(cGZMessage&) override { return true; }   // no tutorial running: every message is kept
    bool HandleUrl(cIGZString* link) override { return {}; }
    void HandleAdviceLink(cGZMessage& message) override {}
    void ProcessCheatCode(std::uint32_t id, const char* cmdLine) override {}
    void InvokeCatalogItemCommand(std::uint32_t, std::uint32_t, bool) override {}   // unused: the view's InvokeCatalogItemCommand is whole
    void ExecuteAcceleratorCommand(std::uint32_t command) override {}
    void Update24HourClockHover() override {}
    void UpdateCursorTextPositions(int x, int y) override {}
    void SetDrawContextFrameMarker(void* drawContext, std::uint32_t frame) override {}
    void OnView3DCreate(cISC4View3DWin* view) override {}
    void OnView3DShutdown() override {}
    void GetCityName(cIGZString& out) override { if (cSC4City* city = LoadedCity()) city->GetCityName(out); }
    int VisualTimeMode() override { return {}; }
    int CityPopulation() override { auto* r = CityComponentAs<cSC4ResidentialSimulator>(CityComponent::ResidentialSimulator, kGZCLSID_cSC4ResidentialSimulator); return r ? r->GetPopulation() : 0; }
    std::int64_t CityFunds() override { auto* b = CityComponentAs<cSC4BudgetSimulator>(CityComponent::BudgetSimulator, kGZCLSID_cSC4BudgetSimulator); return b ? b->GetTotalFunds() : 0; }
    void FormatNumber(std::int64_t value, cIGZString& out) override { out.AssignFromCString(GroupDigits(value).c_str()); }
    // SC4MakeMoneyString: the simoleon sign and the grouped amount ("§1,290,385", "-§500")
    void MakeMoneyString(std::int64_t value, cIGZString& out) override {
        const std::string grouped = GroupDigits(value < 0 ? -value : value);
        out.AssignFromCString(((value < 0 ? "-\xa7" : "\xa7") + grouped).c_str());
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
    bool BudgetDialogOpen(cIGZUnknown* dialog) override { return native::BudgetUiOpen(dialog); }
    void RefreshBudgetDialog(cIGZUnknown* dialog) override { native::BudgetUiRefresh(dialog); }
    void ShutdownNewsTickerWinProc(cIGZUnknown* proc) override {}
    void ShutdownMySimWinProc(cIGZUnknown* proc) override {}
    void ShutdownBudgetUIWinProc(cIGZUnknown* proc) override {}
    void ShutdownGraphsViewWinProc(cIGZUnknown* proc) override {}
    void ShutdownBuildingSelectWinProc(cIGZUnknown* proc) override {}
};
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
// region screen's. Its controls drive the running sound output (GameAudio) as they change.
void RunAudioOptionsDialog(std::function<std::uint8_t*()> preferences) {
    using sc4::cSC4AudioPreferencesWinProc;
    if (!ActiveWinMgr() || !ActiveWinMgr()->modalDialogs || cSC4AudioPreferencesWinProc::Displayed()) return;
    sc4::AudioPreferencesServices& services = sc4::GetAudioPreferencesServices();
    services.preferences = std::move(preferences);
    if (!services.windowManager) {
        services.windowManager = [] { return static_cast<cIGZWinMgr*>(ActiveWinMgr()); };
        services.savePreferences = [] { if (cSC4App* app = BootApplication()) app->SavePreferences(); };
        services.postMessage = [](std::uint32_t type) {
            if (!SL::spMS2) return;
            auto* message = new cRZMessage2Standard();
            message->AddRef();
            message->SetType(type);
            SL::spMS2->MessagePost(message, false);
            message->Release();
        };
        services.dataDirectory = [] { return platform::GameDataDir(); };
        services.applyAudio = [] { ApplyAudioPreferences(sc4::GetAudioPreferencesServices().preferences ? sc4::GetAudioPreferencesServices().preferences() : nullptr); };
        services.playSong = [](const std::string& path) { GameAudio* audio = CurrentGameAudio(); return audio && audio->PlaySong(path); };
        services.stopRadio = [] { if (GameAudio* audio = CurrentGameAudio()) audio->StopRadio(); };
        services.resumeRadio = [] { if (GameAudio* audio = CurrentGameAudio()) audio->ResumeRadio(); };
        services.currentSong = [] { GameAudio* audio = CurrentGameAudio(); return audio ? audio->CurrentSong() : std::string(); };
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

void InstallView3DWinStub() { static StubView3DWin s; sc4::SetView3DWinHost(&s); }

}  // namespace native
