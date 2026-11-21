// cSC4ViewInputControlPlayerDrive — the U-Drive-It tool against a recovered cSC4Automaton: taking over an automaton (the
// drive state, the camera follow, the zoom limits from the player-drive parameters, the resumed managers, the engine sound),
// the scripts' key map reaching the automaton's command history and the AEMS, the mission countdown blocking the keys, the
// choosing mode's pause, Escape ending the tool (the exit callback, the automaton leaving, the view dropping the tool) and the
// automaton's removal callback.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "rz/cRZMessage2Standard.h"
#include "sc4/AutomataDriveParams.h"
#include "sc4/AutomataStates.h"
#include "sc4/Automaton.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/ViewInputControlPlayerDrive.h"
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
    void PostCommand(std::uint32_t) override {}
    float TerrainScale() override { return 16.0f; }
    void MinimizeUI(bool m) override { minimized = m ? 1 : 0; }
    void SetZoomLimits(int lo, int hi) override { zoomMin = lo; zoomMax = hi; }
    void SetZoom(int z) override { zoom = z; }
    bool CanZoomIn() override { return true; }
    bool CanZoomOut() override { return true; }
    void ClearCursorText(std::uint32_t) override {}
    void ShowMissionStatusPanel(std::uint32_t) override {}
    void HideMissionStatusPanel() override {}
    void UpdateMissionStatusPanel() override {}
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
    bool ShowDashboard(std::uint32_t, std::uint32_t&) override { return false; }
    void HideDashboard() override {}
    void SetDashboardButton(std::uint32_t, bool) override {}
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

    // ---- the key map drives the automaton's command history and the AEMS
    CHECK(tool->OnKeyDown('W', 0));
    CHECK(History(car, 0) == 0.0f);
    CHECK(host.aems.log == "+0+1");
    CHECK(tool->OnKeyDown('D', 0));
    CHECK(History(car, 3) == 0.0f);
    CHECK(host.aems.log == "+0+1+3");
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
    CHECK(exitWatcher.refs == 1);

    tick->Release();
    removed->Release();
    tool->Release();
    car->Release();
    bus->Release();
    if (failures == 0) std::printf("sc4_viewinputcontrol_playerdrive: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
