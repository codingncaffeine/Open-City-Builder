// cSC4ViewInputControlPlayerDrive — the U-Drive-It tool against a recovered cSC4Automaton: taking over an automaton (the
// drive state, the camera follow, the zoom limits from the player-drive parameters, the resumed managers, the engine sound),
// the scripts' key map reaching the automaton's command history and the AEMS, the mission countdown blocking the keys, the
// choosing mode's pause, Escape ending the tool (the exit callback, the automaton leaving, the view dropping the tool) and the
// automaton's removal callback. The dashboard: its windows from the exemplar (dials, bottom-center placement, the hidden key
// help panel, the mission status panel), the tick's gauges (speed, throttle, altitude, money, headlights), the steering
// buttons, the panel toggles, the zoom buttons after a view change, its button procedure, and HideUI.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "rz/cRZMessage2Standard.h"
#include "sc4/AutomataDriveParams.h"
#include "sc4/AutomataStates.h"
#include "sc4/Automaton.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/ViewInputControlPlayerDrive.h"
#include "sc4/SC4Utilities.h"
#include "gz/Win.h"
#include "gz/WinBtn.h"
#include "gz/WinGen.h"
#include "gz/WinKnob.h"
#include "gz/WinText.h"
#include "gz/cGZPersistResourceKey.h"
#include <map>
#include "sc4audio/AEMSEvents.h"
#include "vic_fakes.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace sc4;
using namespace vic_fakes;

namespace {

struct FakeAutomatonHost final : IAutomatonHost {
    IAutomatonModel* CreateModel(const cGZPersistResourceKey&) override { return nullptr; }
    void DestroyModel(IAutomatonModel*) override {}
    IAutomatonEffects* CreateEffects() override { return nullptr; }
    void DestroyEffects(IAutomatonEffects*) override {}
    bool AddModelToRenderer(IAutomatonModel*) override { return true; }
    bool RemoveModelFromRenderer(IAutomatonModel*) override { return true; }
    void MoveModelInRenderer(IAutomatonModel*) override {}
    void ChangeModelInRenderer(IAutomatonModel*, int) override {}
    bool InsertOccupant(cISC4Occupant*) override { return true; }
    void RemoveOccupant(cISC4Occupant*) override {}
    bool RefreshOccupant(cISC4Occupant*) override { return true; }
    bool HasCity() override { return true; }
    float CitySizeX() override { return 4096.0f; }
    float CitySizeZ() override { return 4096.0f; }
    float TerrainHeight(float, float) override { return 0.0f; }
    bool NetworkAt(int, int) override { return false; }
    bool LotAt(int, int) override { return false; }
    void PlaceOnNetwork(const cS3DVector3&, cS3DVector3&, std::uint32_t) override {}
    cSC4AutomataController* FindAttraction(cISC4Occupant*, cS3DVector3*, float*, float*) override { return nullptr; }
    IAutomatonGroupTemplate* FindAutomataGroup(std::uint32_t) override { return nullptr; }
    bool AnimationUpdateAllowed() override { return true; }
    float SimSpeedModifier() override { return 1.0f; }
    float MaxSpeedUnits() override { return 3600.0f; }
    float FadeDurationSeconds() override { return 0.5f; }
    void PostMessage(cIGZMessage2*) override {}
    std::uint32_t GenerateSoundInstanceId() override { return 1; }
    void StopSoundLoop(std::uint32_t) override {}
    void PostAudioEvent(std::uint32_t) override {}
    std::uint32_t RandomBelow(std::uint32_t) override { return 0; }
    std::int32_t RandomInt(std::int32_t lo, std::int32_t) override { return lo; }
    double RandomDouble(double lo, double) override { return lo; }
    double RandomUnit() override { return 0.0; }
};

struct View final : IViewInputView {
    int refs = 1, endInputs = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    void EndInput(int) override { ++endInputs; }
    cSC4ViewInputControl* TopControl() override { return nullptr; }
};

struct Aems final : cISC4AEMSEvents {
    std::string log;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    void LoadSoundBank(const cISCPropertyHolder*, std::uint32_t) override {}
    void UnloadSoundBank(const cISCPropertyHolder*, std::uint32_t) override {}
    bool StartSound(AEMSEventType e) override { log += "+" + std::to_string(e); return true; }
    bool EndSound(AEMSEventType e) override { log += "-" + std::to_string(e); return true; }
    bool SetParamVelocity(SC4Percentage p) override { velocity = p.value; return true; }
    bool SetParamThrottle(SC4Percentage p) override { throttle = p.value; return true; }
    bool SetParamWobble(SC4Percentage) override { return true; }
    float velocity = -1.0f, throttle = -1.0f;
};

struct Callback final : cIGZMessageTarget2 {
    int refs = 1;
    std::vector<std::uint32_t> events;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool DoMessage(cIGZMessage2* m) override {
        auto* s = dynamic_cast<cIGZMessage2Standard*>(m);
        if (s && s->GetType() == kPlayerDriveCallbackMessage) events.push_back(static_cast<std::uint32_t>(s->GetData2()));
        return true;
    }
};

// The money text's formatter: the symbol and the plain number.
struct Formatter final : ILanguageFormatter {
    void FormatMoney(std::int64_t value, cIGZString& out, const cRZString& symbol) override {
        cRZString text;
        text.Sprintf("%s%lld", symbol.ToChar(), static_cast<long long>(value));
        out.Copy(text);
    }
    void FormatMoney(double, cIGZString&, const cIGZString&, const cRZString&) override {}
    void FormatNumber(std::int64_t, cRZString&) override {}
};
// A script-built generic window: takes the dashboard's window procedure.
struct FakeGenWindow final : cGZWin, gz::cIGZWinGen {
    explicit FakeGenWindow(cIGZWinMgr* mgr) : cGZWin(mgr) {}
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == gz::kGZIID_cIGZWinGen || iid == 0x5386d516) {
            *out = static_cast<gz::cIGZWinGen*>(this);
            AddRef();
            return true;
        }
        return cGZWin::QueryInterface(iid, out);
    }
    std::uint32_t AddRef() override { return cGZWin::AddRef(); }
    std::uint32_t Release() override { return cGZWin::Release(); }
    cIGZWin* AsIGZWin() override { return this; }
    std::uint32_t GetCLSID() const override { return 0; }
    bool GetWinGenFlag(int) const override { return false; }
    bool SetWinGenFlag(int, bool) override { return true; }
    bool SetWinProc(gz::cIGZWinProc* proc) override { winProc = proc; ++winProcs; return true; }
    gz::cIGZWinProc* GetWinProc() const override { return winProc; }
    bool GetWinProcAs(std::uint32_t, void**) const override { return false; }
    bool GetUserArea(cRZRect& out) const override { out = {}; return true; }
    bool SetUserArea(const cRZRect& area) override { userArea = area; ++userAreas; return true; }
    bool SetBackgroundBitmap(gz::BGBltType, cIGZBuffer*) override { return true; }
    bool GetBackgroundBitmap(gz::BGBltType&, cIGZBuffer*&) const override { return false; }
    bool SetBackgroundBitmap(cIGZBuffer*) override { return true; }
    bool GetBackgroundBitmap(cIGZBuffer** out) const override { *out = nullptr; return true; }
    bool SetBackgroundBlitType(gz::BGBltType) override { return true; }
    bool GetBackgroundBlitType(gz::BGBltType& type) const override { type = gz::kBGTiled; return true; }
    bool SetUserData(std::uint32_t, std::uint32_t) override { return true; }
    bool GetUserData(std::uint32_t, std::uint32_t& value) const override { value = 0; return true; }
    bool SetUserData(std::uint32_t) override { return true; }
    std::uint32_t GetUserData() const override { return 0; }
    void SetUserData(std::uint32_t, cIGZUnknown*) override {}
    bool GetUserData(std::uint32_t, std::uint32_t, void**) override { return false; }
    void RemoveUserData(std::uint32_t) override {}
    bool SetGutters(std::uint8_t, std::uint8_t) override { return true; }
    bool GetGutters(int& x, int& y) override { x = y = 0; return true; }
    void SetGutters(std::int8_t, std::int8_t, std::int8_t, std::int8_t) override {}
    bool GetGutters(int& l, int& t, int& r, int& b) const override { l = t = r = b = 0; return true; }
    bool AddColorBar(int, int, int, const cRZRect&) override { return true; }
    void AddDividerH(int, int) override {}
    bool RemoveColorBar(int) override { return true; }
    gz::cIGZWinProc* winProc = nullptr;
    cRZRect userArea;
    int winProcs = 0, userAreas = 0;
};

struct Host final : IViewInputHost, IPlayerDriveHost {
    View view;
    Mgr mgr;
    Aems aems;
    tAutomataPlayerDriveParams params;
    bool haveParams = true, countdown = false, simPaused = false;
    int ticks = 0, messages = 0, commandTable = 0, accelerator = 0, minimized = -1, zoomMin = -9, zoomMax = -9, zoom = -9;
    int exits = 0, bankLoads = 0, disasterLayer = -1;
    int paused[4] = {0, 0, 0, 0};
    cSC4Automaton* followed = nullptr;
    int follows = 0;
    // IViewInputHost
    IViewInputView* View3D() override { return &view; }
    cIGZWinMgr* WindowManager() override { return &mgr; }
    void PlaySound(int, std::uint32_t) override {}
    // IPlayerDriveHost
    void SetTickTarget(cIGZMessageTarget2*, bool add) override { ticks += add ? 1 : -1; }
    void SetMessageTarget(cIGZMessageTarget2*, std::uint32_t, bool add) override { messages += add ? 1 : -1; }
    void SetCommandTable(bool add) override { commandTable += add ? 1 : -1; }
    void PostCommand(std::uint32_t command) override { commands.push_back(command); }
    float TerrainScale() override { return 16.0f; }
    void MinimizeUI(bool m) override { minimized = m ? 1 : 0; }
    void SetZoomLimits(int lo, int hi) override { zoomMin = lo; zoomMax = hi; }
    void SetZoom(int z) override { zoom = z; }
    bool CanZoomIn() override { return true; }
    bool CanZoomOut() override { return canZoomOut; }
    void ClearCursorText(std::uint32_t) override {}
    void ShowMissionStatusPanel(std::uint32_t id) override { missionShown.push_back(id); }
    void HideMissionStatusPanel() override { ++missionHides; }
    void UpdateMissionStatusPanel() override { ++missionUpdates; }
    void HideDriveWindow() override {}
    void InstallKeyAccelerator(bool install) override { accelerator += install ? 1 : -1; }
    bool AllowMaxZoom() override { return false; }
    void SetDisasterLayerActive(bool active) override { disasterLayer = active ? 1 : 0; }
    int AutomataManagerCount() override { return 4; }
    void PauseAutomataManager(int index, bool pause) override { paused[index] = pause ? 1 : 0; }
    void FollowAutomaton(cSC4Automaton* a) override { followed = a; ++follows; }
    bool HasSimulator() override { return true; }
    bool SimulatorPaused() override { return simPaused; }
    void PauseSimulator(bool pause) override { simPaused = pause; }
    const tAutomataPlayerDriveParams* PlayerDriveParams(cSC4Automaton*) override { return haveParams ? &params : nullptr; }
    const cSC4OccupantGroupTemplate* GroupTemplate(cSC4Automaton*) override { return nullptr; }
    bool DashboardId(cSC4Automaton*, std::uint32_t&) override { return false; }
    int DriveCommandForKey(std::uint32_t key) override {
        switch (key) { case 'W': return 0; case 'S': return 1; case 'A': return 2; case 'D': return 3; default: return 0xd; }
    }
    bool HasScriptSystem() override { return true; }
    bool CountdownInProgress() override { return countdown; }
    bool CSIVisible() override { return true; }
    void SetKeyController(cSC4Automaton*, const cRZString&, bool, int) override {}
    cSC4Automaton* PickAutomaton(int, int) override { return nullptr; }
    bool HasDispatchView() override { return false; }
    std::uint32_t TrackOccupant(cSC4Automaton*) override { return 0; }
    void UntrackOccupant(std::uint32_t) override {}
    void ShowAllIndicators(bool) override {}
    void UseAlternateModel(std::uint32_t, bool) override {}
    // the dashboard
    cGZWin* viewWin = nullptr;
    std::map<std::uint32_t, cIGZWin*> scripts;             // by window id
    std::vector<std::pair<cGZPersistResourceKey, std::uint32_t>> scriptRequests;
    Dashboard dashboardSpec;
    std::vector<std::uint32_t> exemplarRequests, imageRequests, missionShown;
    int missionUpdates = 0, missionHides = 0;
    long long money = 1234567;
    float ground = -250.0f;
    bool canZoomOut = true;
    std::vector<std::uint32_t> commands;
    cIGZWin* ViewWindow() override { return viewWin; }
    bool DashboardExemplar(std::uint32_t id, Dashboard& out) override { exemplarRequests.push_back(id); out = dashboardSpec; return true; }
    cIGZWin* CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id) override {
        scriptRequests.emplace_back(key, id);
        auto it = scripts.find(id);
        if (it == scripts.end()) return nullptr;
        if (parent) parent->ChildAdd(it->second);
        it->second->AddRef();
        return it->second;
    }
    cIGZBuffer* DashboardImage(std::uint32_t instance) override { imageRequests.push_back(instance); return nullptr; }
    long long CityMoney() override { return money; }
    float DriveGroundHeight(cSC4Automaton*, float, float) override { return ground; }
    cISC4AEMSEvents* AEMS() override { return &aems; }
    void LoadAutomatonSoundBank(cSC4Automaton*, bool load) override { bankLoads += load ? 1 : -1; }
    void ExitPlayerDriveView() override { ++exits; }
};

float History(cSC4Automaton* a, int command) {
    auto* state = static_cast<tAutomataStatePlayerDrive*>(a->FindState(kStatePlayerDrive));
    return state ? state->history[command] : -99.0f;
}

}  // namespace

int main() {
    Host host;
    Formatter formatter;
    SetLanguageFormatter(&formatter);
    SetViewInputHost(&host);
    SetPlayerDriveHost(&host);
    FakeAutomatonHost automatonHost;
    host.params.SetI(0x0, 2);        // MinZoom
    host.params.SetI(0x4, 4);        // StartingZoom
    host.params.SetF(0x8, 0.5f);     // MaxSpeed

    // ---- choosing mode, then taking over an automaton
    auto* tool = new cSC4ViewInputControlPlayerDrive();
    tool->AddRef();
    CHECK(tool->GetID() == kPlayerDriveToolId);
    CHECK(tool->Init());
    CHECK(host.ticks == 1 && host.messages == 4 && host.commandTable == 1 && host.accelerator == 1);
    tool->EnterChoosingMode();
    CHECK(host.simPaused);
    CHECK(host.paused[0] == 1 && host.paused[1] == 1 && host.paused[2] == 1 && host.paused[3] == 1);

    // The dashboard windows the scripts build: the view 800 x 600; the dashboard 474 x 136 with its dials, texts and
    // buttons; the key help panel 460 x 132.
    cGZWin viewWin(&host.mgr);
    viewWin.AddRef();
    viewWin.SetArea(0, 0, 800, 600);
    host.viewWin = &viewWin;
    auto* dash = new FakeGenWindow(&host.mgr);
    dash->SetArea(0, 0, 474, 136);
    auto addChild = [&](cGZWin* child, std::uint32_t id) { child->SetID(id); child->SetArea(0, 0, 70, 70); dash->ChildAdd(child); return child; };
    auto* speedKnob = static_cast<gz::cGZWinKnob*>(addChild(new gz::cGZWinKnob(&host.mgr), 0xebcb9403));
    auto* throttleKnob = static_cast<gz::cGZWinKnob*>(addChild(new gz::cGZWinKnob(&host.mgr), 0x2bcb940b));
    auto* altitudeKnob = static_cast<gz::cGZWinKnob*>(addChild(new gz::cGZWinKnob(&host.mgr), 0x2bcbce68));
    auto* altitudeText = static_cast<gz::cGZWinText*>(addChild(new gz::cGZWinText(&host.mgr), 0xec0ea996));
    auto* moneyText = static_cast<gz::cGZWinText*>(addChild(new gz::cGZWinText(&host.mgr), 0xac0ea9db));
    auto* lights = static_cast<gz::cGZWinBtn*>(addChild(new gz::cGZWinBtn(&host.mgr), 0x0c16c9b9));
    auto* rightButton = static_cast<gz::cGZWinBtn*>(addChild(new gz::cGZWinBtn(&host.mgr), 0x6bcb93f5));
    auto* zoomOut = addChild(new cGZWin(&host.mgr), 0xe9920494);
    auto* pitchKnob = static_cast<gz::cGZWinKnob*>(addChild(new gz::cGZWinKnob(&host.mgr), 0x6bcbce73));
    auto* barrelPitchKnob = static_cast<gz::cGZWinKnob*>(addChild(new gz::cGZWinKnob(&host.mgr), 0x2bf98d69));
    auto* turretKnob = static_cast<gz::cGZWinKnob*>(addChild(new gz::cGZWinKnob(&host.mgr), 0xebf98d37));
    auto* compassKnob = static_cast<gz::cGZWinKnob*>(addChild(new gz::cGZWinKnob(&host.mgr), 0x6c0ae7b1));
    auto* barrelSpeedKnob = static_cast<gz::cGZWinKnob*>(addChild(new gz::cGZWinKnob(&host.mgr), 0x2c0c1c8c));
    // the ammunition text and the fire button sit one level down (found recursively)
    auto* weaponPanel = addChild(new cGZWin(&host.mgr), 0x11112222);
    auto* ammoText = new gz::cGZWinText(&host.mgr);
    ammoText->SetID(0x8c3923ae);
    weaponPanel->ChildAdd(ammoText);
    auto* fireButton = new gz::cGZWinBtn(&host.mgr);
    fireButton->SetID(0x6c3a0a6f);
    weaponPanel->ChildAdd(fireButton);
    auto* help = new FakeGenWindow(&host.mgr);
    help->SetArea(0, 0, 460, 132);
    host.scripts[0x4bcb938a] = dash;
    host.scripts[0xec1a5cbf] = help;
    host.dashboardSpec.windowInstance = 0xabcb937d;
    host.dashboardSpec.missionPanel = 0xac1d544d;
    host.dashboardSpec.dialIds = {0xebcb9403, 0x2bcb940b};
    host.dashboardSpec.dialImages = {0xcbcba952, 0xcbcb6e9f};
    host.dashboardSpec.dialFrames = {55, 54};

    auto* car = new cSC4Automaton(&automatonHost);
    car->AddRef();
    Callback exitWatcher;
    tool->AddCallbackTarget(&exitWatcher);
    tool->AddCallbackTarget(&exitWatcher);   // once only
    CHECK(exitWatcher.refs == 2);
    tool->EnterDrivingMode(car, true);
    CHECK(tool->DrivenAutomaton() == car);
    CHECK(car->GetBehavior() == kStatePlayerDrive);
    CHECK(car->CheckAutomataFlag(kAutomataFlagPlayerDrive));
    CHECK(host.followed == car);
    CHECK(host.zoomMin == 1 && host.zoomMax == 4 && host.zoom == 3);
    CHECK(tool->SpeedReference() == 0.5f);
    CHECK(host.minimized == 1 && host.disasterLayer == 0 && host.bankLoads == 1);
    CHECK(!host.simPaused && host.paused[0] == 0 && host.paused[3] == 0);
    CHECK(host.aems.log == "+0");
    CHECK(host.aems.velocity == 0.0f && host.aems.throttle == 0.0f);
    CHECK(tool->IsDriveAutoValid());
    // ShowUI: the car's default dashboard exemplar 0x0be8824f; the dashboard (script 0x96a006b0 / 0xabcb937d, window
    // 0x4bcb938a) at the view's bottom center with its dial images and frame counts; the help panel (0x8c1a5c9f, window
    // 0xec1a5cbf) hidden at the bottom center; the mission status panel 0xac1d544d.
    CHECK(host.exemplarRequests.size() == 1 && host.exemplarRequests[0] == 0x0be8824f);
    CHECK(host.scriptRequests.size() == 2);
    CHECK(host.scriptRequests[0].first.group == 0x96a006b0 && host.scriptRequests[0].first.instance == 0xabcb937d &&
          host.scriptRequests[0].second == 0x4bcb938a);
    CHECK(host.scriptRequests[1].first.group == 0x96a006b0 && host.scriptRequests[1].first.instance == 0x8c1a5c9f &&
          host.scriptRequests[1].second == 0xec1a5cbf);
    CHECK(tool->DashboardWindow() == dash && tool->DashboardHelpWindow() == help);
    CHECK(dash->GetL() == (800 - 474) / 2 && dash->GetT() == 600 - 136);
    CHECK(help->GetL() == (800 - 460) / 2 && help->GetT() == 600 - 132 && !help->IsVisible());
    CHECK(host.imageRequests == (std::vector<std::uint32_t>{0xcbcba952, 0xcbcb6e9f}));
    CHECK(speedKnob->GetFrameCount() == 55 && throttleKnob->GetFrameCount() == 54);
    CHECK(host.missionShown == (std::vector<std::uint32_t>{0xac1d544d}));

    // ---- the key map drives the automaton's command history and the AEMS
    CHECK(tool->OnKeyDown('W', 0));
    CHECK(History(car, 0) == 0.0f);
    CHECK(host.aems.log == "+0+1");
    CHECK(tool->OnKeyDown('D', 0));
    CHECK(History(car, 3) == 0.0f);
    CHECK(host.aems.log == "+0+1+3");
    CHECK(rightButton->IsChecked());                           // the dashboard's right-turn button pressed
    CHECK(tool->OnKeyDown('W', 0x40000000));   // a repeat is swallowed
    CHECK(host.aems.log == "+0+1+3");
    CHECK(!tool->OnKeyDown('Q', 0));           // not in the key map
    CHECK(tool->OnKeyUp('W', 0));
    CHECK(History(car, 0) == -1.0f);
    CHECK(History(car, 3) == 0.0f);
    CHECK(host.aems.log == "+0+1+3-1");
    // the mission countdown blocks the keys
    host.countdown = true;
    CHECK(tool->OnKeyDown('S', 0));
    CHECK(History(car, 1) == -1.0f);
    host.countdown = false;
    CHECK(tool->OnKeyDown('S', 0));
    CHECK(History(car, 1) == 0.0f);
    // the right mouse button is command 8
    CHECK(tool->OnMouseDownR(0, 0, 0));
    CHECK(History(car, 8) == 0.0f);
    CHECK(tool->OnMouseUpR(0, 0, 0));
    CHECK(History(car, 8) == -1.0f);

    // ---- the animation tick feeds the AEMS: the drive state's speed between its minimum and maximum
    auto* state = static_cast<tAutomataStatePlayerDrive*>(car->FindState(kStatePlayerDrive));
    state->value3c = -0.25f;
    state->groupSpeed = 0.5f;
    auto* tick = new cRZMessage2Standard();
    tick->AddRef();
    tick->SetType(0x86ad10ee);
    tool->DoMessage(tick);
    CHECK(std::fabs(host.aems.throttle - 0.5f) < 1e-6f);
    CHECK(host.aems.velocity == 0.0f);
    // The tick's gauges: speed 0..MaxSpeed; throttle min..max with the speed clamped (-0.25 -> the minimum 0); the altitude
    // above the ground (0 - -250 = 250: text "250", knob 0..100 on 50); the city's money; headlights = toggle 4 (on).
    CHECK(speedKnob->GetMinValue() == 0.0f && speedKnob->GetMaxValue() == 0.5f && speedKnob->GetValue() == 0.0f);
    CHECK(throttleKnob->GetMinValue() == 0.0f && throttleKnob->GetMaxValue() == 0.5f && throttleKnob->GetValue() == 0.0f);
    CHECK(std::strcmp(altitudeText->GetCaption().ToChar(), "250") == 0);
    CHECK(altitudeKnob->GetMaxValue() == 100.0f && altitudeKnob->GetValue() == 50.0f);
    CHECK(std::strcmp(moneyText->GetCaption().ToChar(), "\xc2\xa7" "1234567") == 0);
    CHECK(lights->GetBtnFlag(1));
    CHECK(host.missionUpdates == 1);
    // The vehicle gauges start empty (no vehicle filled them): pitch on -30..30 degrees; entering the drive state leaves the
    // fire command unlimited (-1 uses, none used) and ready, so the ammunition reads -1 and fire is on; the cursor is the
    // tool's default while command 8 is not held.
    CHECK(std::fabs(pitchKnob->GetMinValue() + 0.52359879f) < 1e-7f && std::fabs(pitchKnob->GetMaxValue() - 0.52359879f) < 1e-7f);
    CHECK(pitchKnob->GetValue() == 0.0f && compassKnob->GetMaxValue() == 6.2831855f);
    CHECK(std::strcmp(ammoText->GetCaption().ToChar(), "-1") == 0 && fireButton->IsChecked());
    CHECK(tool->CursorId() == 0xc16c4e1b);
    state->value3c = 0.75f;                                    // above the maximum: the maximum
    // A tank's record: barrel pitch 0..0.4 on 0.2, turret 0..2 pi on 1, velocity 30..100 on 60, compass on 2; 12 shells,
    // 3 fired, armed; its cursor 0xac0ece77 while command 8 is held.
    state->pitchGauge = 0.25f;
    state->barrelPitch[0] = 0.0f, state->barrelPitch[1] = 0.4f, state->barrelPitch[2] = 0.2f;
    state->barrelTurn[0] = 0.0f, state->barrelTurn[1] = 6.2831855f, state->barrelTurn[2] = 1.0f;
    state->barrelVelocity[0] = 30.0f, state->barrelVelocity[1] = 100.0f, state->barrelVelocity[2] = 60.0f;
    state->compassValue = 2.0f;
    state->limit[7] = 12;
    state->used[7] = 3;
    state->ready[7] = 1;
    state->cursor = 0xac0ece77;
    state->history[8] = 1.0f;
    tool->DoMessage(tick);
    CHECK(throttleKnob->GetValue() == 0.5f && host.missionUpdates == 2);
    CHECK(pitchKnob->GetValue() == 0.25f);
    CHECK(barrelPitchKnob->GetMaxValue() == 0.4f && barrelPitchKnob->GetValue() == 0.2f);
    CHECK(turretKnob->GetMaxValue() == 6.2831855f && turretKnob->GetValue() == 1.0f);
    CHECK(barrelSpeedKnob->GetMinValue() == 30.0f && barrelSpeedKnob->GetMaxValue() == 100.0f && barrelSpeedKnob->GetValue() == 60.0f);
    CHECK(compassKnob->GetValue() == 2.0f);
    CHECK(std::strcmp(ammoText->GetCaption().ToChar(), "9") == 0 && fireButton->IsChecked());
    CHECK(tool->CursorId() == 0xac0ece77);
    state->history[8] = -1.0f;                                 // released: the default cursor
    state->ready[7] = 0;
    tool->DoMessage(tick);
    CHECK(tool->CursorId() == 0xc16c4e1b && !fireButton->IsChecked());
    // The panel toggles: 0xac1f0449 the help panel, 0xcc1f0457 the dashboard, 0xac1f045d flips between them.
    auto* panelCommand = new cRZMessage2Standard();
    panelCommand->AddRef();
    panelCommand->SetType(0xac1f0449);
    tool->DoMessage(panelCommand);
    CHECK(!dash->IsVisible() && help->IsVisible());
    panelCommand->SetType(0xac1f045d);
    tool->DoMessage(panelCommand);
    CHECK(dash->IsVisible() && !help->IsVisible());
    tool->DoMessage(panelCommand);
    CHECK(!dash->IsVisible() && help->IsVisible());
    panelCommand->SetType(0xcc1f0457);
    tool->DoMessage(panelCommand);
    CHECK(dash->IsVisible() && !help->IsVisible());
    // A view change: the zoom buttons follow CanZoomIn / CanZoomOut.
    host.canZoomOut = false;
    panelCommand->SetType(0xa6b79602);
    tool->DoMessage(panelCommand);
    CHECK(!zoomOut->IsEnabled());
    panelCommand->Release();
    // The dashboard's procedure: a button activation posts its command (zoom in -> 0x6a935cb7); other messages pass.
    {
        void* raw = nullptr;
        CHECK(dash->GetWinProc() != nullptr);
        cGZMessage click;
        click.type = 3;
        click.param1 = 0x287259f6;
        click.param2 = 0x4992046a;
        CHECK(dash->GetWinProc()->DoWinProcMessage(dash, click));
        CHECK(host.commands.size() == 1 && host.commands[0] == 0x6a935cb7);
        click.param1 = 0x12345678;
        CHECK(!dash->GetWinProc()->DoWinProcMessage(dash, click));
        CHECK(host.commands.size() == 1);
        (void)raw;
    }

    // ---- Escape: the exit callback, the automaton leaves the city, the view drops the tool
    CHECK(tool->OnKeyDown(0x1b, 0));
    CHECK(exitWatcher.events.size() == 1 && exitWatcher.events[0] == 1);
    CHECK(tool->DrivenAutomaton() == nullptr);
    CHECK(car->CheckAutomataFlag(kAutomataFlagMarkedForRemove));
    CHECK(host.view.endInputs == 1);
    CHECK(host.followed == nullptr);
    CHECK(host.zoomMin == 0 && host.zoomMax == 5);
    CHECK(host.disasterLayer == 1 && host.bankLoads == 0);
    CHECK(host.aems.log == "+0+1+3-1+2-0");
    // driving a removed automaton does nothing
    CHECK(!tool->IsDriveAutoValid());
    CHECK(tool->OnKeyDown('W', 0));

    // ---- a second automaton: its removal callback ends the tool
    auto* bus = new cSC4Automaton(&automatonHost);
    bus->AddRef();
    tool->EnterDrivingMode(bus, true);
    CHECK(tool->DrivenAutomaton() == bus);
    auto* removed = new cRZMessage2Standard();
    removed->AddRef();
    removed->SetType(kAutomatonMsgAutomaton);
    removed->SetVoid1(bus);
    removed->SetData2(3);
    tool->DoMessage(removed);
    CHECK(tool->DrivenAutomaton() == nullptr);
    CHECK(host.view.endInputs == 2);
    CHECK(exitWatcher.events.size() == 2);

    // ---- Shutdown undoes Init and leaves the drive view
    CHECK(tool->Shutdown());
    CHECK(host.ticks == 0 && host.messages == 0 && host.commandTable == 0 && host.accelerator == 0);
    CHECK(host.exits == 1 && host.minimized == 0);
    // HideUI (from Shutdown): both windows leave the view, the view hides the mission status panel.
    CHECK(tool->DashboardWindow() == nullptr && tool->DashboardHelpWindow() == nullptr);
    CHECK(viewWin.GetChildCount() == 0 && host.missionHides >= 1);
    CHECK(exitWatcher.refs == 1);

    tick->Release();
    removed->Release();
    tool->Release();
    car->Release();
    bus->Release();
    if (failures == 0) std::printf("sc4_viewinputcontrol_playerdrive: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
