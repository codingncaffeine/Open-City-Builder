#include "sc4/ViewInputControlPlayerDrive.h"
#include "platform/Unwired.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "rz/cRZMessage2Standard.h"
#include "sc4/AutomataDriveParams.h"
#include "sc4/AutomataStates.h"
#include "sc4/Automaton.h"
#include "sc4/OccupantGroupTemplate.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4audio/AEMSEvents.h"

namespace sc4 {

namespace {
IPlayerDriveHost* g_host = nullptr;
constexpr std::uint32_t kGZIID_cIGZMessageTarget2Id = 0x452294aa;
constexpr std::uint32_t kPlayerDriveTickMessage = 0x86ad10ee;   // the animation tick (SL::ATM)
constexpr std::uint32_t kOccupantGroupLeftClickCommand = 0x4006;   // EnterDrivingMode's mouse command (command 7) group
constexpr std::uint32_t kOccupantGroupKeepAfterDrive = 0x4003;     // StopTracking: such automata pop the drive state instead of leaving
constexpr std::uint32_t kAutomatonEventRemoved = 3;
constexpr std::uint32_t kAutomatonEventFinalCrash = 0xb;
constexpr std::uint32_t kDashboardLeftButton = 0x8bcb93fc;
constexpr std::uint32_t kDashboardRightButton = 0x6bcb93f5;
// The AEMS events the drive commands start / end (HandleDriveCommandKeyDown / Up).
constexpr std::uint32_t kAemsEngine = 0, kAemsForward = 1, kAemsReverse = 2, kAemsTurn = 3, kAemsMouse = 4, kAemsCommand5 = 6;

using AemsEvent = cISC4AEMSEvents::AEMSEventType;
cISC4AEMSEvents* Aems() { return g_host ? g_host->AEMS() : nullptr; }
void AemsStart(std::uint32_t event) { if (cISC4AEMSEvents* a = Aems()) a->StartSound(static_cast<AemsEvent>(event)); }
void AemsEnd(std::uint32_t event) { if (cISC4AEMSEvents* a = Aems()) a->EndSound(static_cast<AemsEvent>(event)); }

tAutomataStatePlayerDrive* DriveState(cSC4Automaton* automaton) {
    return automaton ? static_cast<tAutomataStatePlayerDrive*>(automaton->FindState(kStatePlayerDrive)) : nullptr;
}
}  // namespace

IPlayerDriveHost* PlayerDriveHost() { return g_host; }
void SetPlayerDriveHost(IPlayerDriveHost* host) { g_host = host; }

// 0x004b70fa: the tool id 0x2b709676 and the default cursor; everything else starts empty.
cSC4ViewInputControlPlayerDrive::cSC4ViewInputControlPlayerDrive() {
    id_ = kPlayerDriveToolId;
    cursorId_ = kPlayerDriveDefaultCursor;
}

cSC4ViewInputControlPlayerDrive::~cSC4ViewInputControlPlayerDrive() {
    if (automaton_) automaton_->Release();
    if (rollover_) rollover_->Release();
    for (cIGZMessageTarget2* target : callbacks_)
        if (target) target->Release();
}

// 0x004b37e8: the message-target id answers the +0x28 view; the base answers the rest.
bool cSC4ViewInputControlPlayerDrive::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cIGZMessageTarget2Id) {
        *out = static_cast<cIGZMessageTarget2*>(this);
        AddRef();
        return true;
    }
    return cSC4ViewInputControl::QueryInterface(iid, out);
}

std::uint32_t cSC4ViewInputControlPlayerDrive::Release() {
    if (refCount_ > 1) return --refCount_;
    FinalRelease();
    return 0;
}

// 0x004b398a: an automaton that is neither disabled (0x8000000) nor marked for removal (1).
bool cSC4ViewInputControlPlayerDrive::IsDriveAutoValid() const {
    return automaton_ && !automaton_->CheckAutomataFlag(kAutomataFlagDisabled) &&
           !automaton_->CheckAutomataFlag(kAutomataFlagMarkedForRemove);
}

bool cSC4ViewInputControlPlayerDrive::InputBlocked() { return g_host && g_host->CountdownInProgress(); }

// 0x004b72f4: the base Init, the animation ticks (channel 0xff), the terrain scale, the four debug strings, the managers,
// the junction view on the renderer, the dispatch view (its indicators hidden), the view-changed message and the command
// table, the tool's key accelerator. PARTIAL: the two pick filters and SC4TrainJunctionView (the train switch picks of
// OnMouseDownL / OnMouseMove) are not recovered; the debug strings had no reader.
bool cSC4ViewInputControlPlayerDrive::Init() {
    if (initialized_) return true;
    SC4_UNWIRED("cSC4ViewInputControlPlayerDrive::Init: the two pick filters and SC4TrainJunctionView not ported");
    cSC4ViewInputControl::Init();
    if (!g_host) return true;
    auto* target = static_cast<cIGZMessageTarget2*>(this);
    g_host->SetTickTarget(target, true);
    if (!hasTerrain_) {
        hasTerrain_ = true;
        terrainScale_ = g_host->TerrainScale();
    }
    if (automaton_) {
        automaton_->Release();
        automaton_ = nullptr;
    }
    dispatchView_ = g_host->HasDispatchView();
    g_host->SetMessageTarget(target, kPlayerDriveViewChanged, true);
    for (std::uint32_t command : kPlayerDriveCommandTable) g_host->SetMessageTarget(target, command, true);
    if (dispatchView_) g_host->ShowAllIndicators(false);
    g_host->InstallKeyAccelerator(true);
    g_host->SetCommandTable(true);
    return true;
}

// 0x004b446e: undoes Init: the command table, the key accelerator, the dispatch indicators (back as the CSI visibility says),
// the callback targets, the tracking, the pause, the UI, the minimized view, the engine sound, the messages, the ticks; last
// the automata manager leaves the drive view.
bool cSC4ViewInputControlPlayerDrive::Shutdown() {
    if (!initialized_) return true;
    initialized_ = false;
    if (g_host) {
        g_host->SetCommandTable(false);
        g_host->InstallKeyAccelerator(false);
        if (dispatchView_) g_host->ShowAllIndicators(g_host->CSIVisible());
    }
    for (cIGZMessageTarget2* callback : callbacks_)
        if (callback) callback->Release();
    callbacks_.clear();
    StopTracking();
    StopRollover();
    PauseEverything(false);
    HideUI();
    if (g_host) {
        g_host->MinimizeUI(false);
        AemsEnd(kAemsEngine);
        auto* target = static_cast<cIGZMessageTarget2*>(this);
        g_host->SetMessageTarget(target, kPlayerDriveViewChanged, false);
        for (std::uint32_t command : kPlayerDriveCommandTable) g_host->SetMessageTarget(target, command, false);
        g_host->ClearCursorText(kPlayerDriveRolloverText);
        g_host->SetTickTarget(target, false);
    }
    dispatchView_ = false;
    if (g_host) g_host->ExitPlayerDriveView();
    // The original calls the base Shutdown with the initialized byte already cleared, so it does nothing; the port releases
    // the base's references here instead of leaking them.
    initialized_ = true;
    cSC4ViewInputControl::Shutdown();
    return true;
}

// 0x004b487c: each target once, counted.
void cSC4ViewInputControlPlayerDrive::AddCallbackTarget(cIGZMessageTarget2* target) {
    if (!target) return;
    if (std::find(callbacks_.begin(), callbacks_.end(), target) != callbacks_.end()) return;
    target->AddRef();
    callbacks_.push_back(target);
}

// 0x004b3e82: message 0x0bb324bd (void1 = the tool, data2 = event, data3 = data) to every callback target.
void cSC4ViewInputControlPlayerDrive::SendCallbackMessage(std::uint32_t event, std::uint32_t data) {
    if (callbacks_.empty()) return;
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(kPlayerDriveCallbackMessage);
    message->SetVoid1(static_cast<cSC4ViewInputControl*>(this));
    message->SetData2(event);
    message->SetData3(data);
    for (auto it = callbacks_.begin(); it != callbacks_.end();) {
        cIGZMessageTarget2* target = *it;
        ++it;
        if (target) target->DoMessage(message);
    }
    message->Release();
}

// 0x004b3c1a
void cSC4ViewInputControlPlayerDrive::StopRollover() {
    if (!rollover_) return;
    SetRollover(nullptr);
    if (g_host) g_host->ClearCursorText(kPlayerDriveRolloverText);
}

void cSC4ViewInputControlPlayerDrive::SetRollover(cSC4Automaton* automaton) {
    if (rollover_ == automaton) return;
    if (automaton) automaton->AddRef();
    if (rollover_) rollover_->Release();
    rollover_ = automaton;
}

// 0x004b3a96: the automaton stops calling back, the dispatch view and the camera stop following it, the zoom limits go
// back to 0..5, the engine sound and its bank go. An automaton still in the drive state either leaves the city (not
// disabled and not of group 0x4003) or pops the drive state.
void cSC4ViewInputControlPlayerDrive::StopTracking() {
    if (!automaton_) return;
    automaton_->RemoveCallback(static_cast<cIGZMessageTarget2*>(this));
    if (g_host) {
        if (dispatchView_) g_host->UntrackOccupant(trackId_);
        g_host->FollowAutomaton(nullptr);
        g_host->SetZoomLimits(0, 5);
        AemsEnd(kAemsEngine);
        g_host->LoadAutomatonSoundBank(automaton_, false);
    }
    if (automaton_->GetBehavior() == kStatePlayerDrive) {
        if (!automaton_->CheckAutomataFlag(kAutomataFlagDisabled) &&
            !automaton_->AsOccupant()->IsOccupantGroup(kOccupantGroupKeepAfterDrive)) {
            automaton_->Remove(false);
        } else {
            automaton_->PopState();
        }
    }
    cSC4Automaton* old = automaton_;
    automaton_ = nullptr;
    old->Release();
}

// 0x004b3bea: the driven automaton went away: the tool ends.
void cSC4ViewInputControlPlayerDrive::AutomatonRemoved(cSC4Automaton* automaton) {
    if (automaton_ != automaton) return;
    StopTracking();
    EndInput();
}

// 0x004b3fc2: pausing pauses the simulator (unless it already is) and the four automata managers; resuming resumes the
// simulator (when it is paused) and the managers.
void cSC4ViewInputControlPlayerDrive::PauseEverything(bool pause) {
    if (!g_host) return;
    if (g_host->HasSimulator()) {
        const bool paused = g_host->SimulatorPaused();
        if (pause && !paused) g_host->PauseSimulator(true);
        if (!pause && paused) g_host->PauseSimulator(false);
    }
    const int count = g_host->AutomataManagerCount();
    for (int i = 0; i < count; ++i) g_host->PauseAutomataManager(i, pause);
}

// 0x004b4100
void cSC4ViewInputControlPlayerDrive::EnterChoosingMode() {
    StopTracking();
    StopRollover();
    PauseEverything(true);
}

// 0x004b3f32: the exit callback, a disabled automaton is removed, the tracking ends, the disaster layer comes back and the
// view drops the tool.
void cSC4ViewInputControlPlayerDrive::EndInput() {
    SendCallbackMessage(static_cast<std::uint32_t>(ePlayerDriveCallback::kExit), 0);
    if (automaton_ && automaton_->GetDisabled()) automaton_->Remove(false);
    StopTracking();
    StopRollover();
    if (g_host) g_host->SetDisasterLayerActive(true);
    cSC4ViewInputControl::EndInput();
}

// 0x004b5184: takes over the driving automaton of `automaton` (FindDrivingAutomaton: the engine of a train): the disaster
// layer pauses, the previous drive ends, the view minimizes its UI and hides window 0x43, the dashboard of the automaton's
// exemplar (property 0x2be8fbf3; else 0x6be8846c for occupant type 0x480e6151 and 0x0be8824f for the rest) appears, the
// engine sound bank loads and the engine event starts at zero. The drive state (0x10) is pushed, the camera follows, the
// zoom limits come from the player-drive parameters (MinZoom - 1 .. 4, or 5 with the preference; StartingZoom - 1), the
// dispatch view tracks the automaton, the tool listens to it and everything resumes.
void cSC4ViewInputControlPlayerDrive::EnterDrivingMode(cSC4Automaton* automaton, bool flag) {
    (void)flag;   // not read by the original
    if (!automaton || !g_host) return;
    g_host->SetDisasterLayerActive(false);
    cSC4Automaton* driver = automaton->FindDrivingAutomaton();
    if (!driver) return;
    StopTracking();
    StopRollover();
    g_host->MinimizeUI(true);
    g_host->HideDriveWindow();
    std::uint32_t dashboard = 0;
    if (!g_host->DashboardId(driver, dashboard)) dashboard = driver->GetType() == 0x480e6151 ? 0x6be8846c : 0x0be8824f;
    ShowUI(dashboard);
    g_host->LoadAutomatonSoundBank(driver, true);
    if (cISC4AEMSEvents* aems = Aems()) {
        aems->StartSound(static_cast<AemsEvent>(kAemsEngine));
        aems->SetParamVelocity(SC4Percentage(0.0f));
        aems->SetParamThrottle(SC4Percentage(0.0f));
        aems->SetParamWobble(SC4Percentage(0.0f));
    }
    driver->AddRef();
    if (automaton_) automaton_->Release();
    automaton_ = driver;
    automaton_->PushState(kStatePlayerDrive, nullptr, -1.0f, false, 0, 0, nullptr);
    g_host->FollowAutomaton(automaton_);
    speedReference_ = 0.042f;
    int minZoom = 0;
    int startZoom = 4;
    if (const tAutomataPlayerDriveParams* params = g_host->PlayerDriveParams(automaton_)) {
        speedReference_ = params->F(0x8);
        minZoom = params->I(0x0) - 1;
        startZoom = params->I(0x4) - 1;
    }
    g_host->SetZoomLimits(minZoom, g_host->AllowMaxZoom() ? 5 : 4);
    g_host->SetZoom(startZoom);
    toggle44_ = true;
    toggle45_ = false;
    if (dispatchView_) trackId_ = g_host->TrackOccupant(automaton_);
    automaton_->AddCallback(static_cast<cIGZMessageTarget2*>(this));
    crashTimer_.Stop();
    crashTimer_.Reset();
    PauseEverything(false);
}

// 0x004b4b1c / 0x004b38ee: the dashboard windows and the view's mission status panel. PARTIAL: the windows themselves
// (SC4CreateWindowFromScript of the exemplar's 0x2be8e61b instance, its dials from 0x2be8e834 / 0x2be8e6cb / 0xabe8e6cc and the
// second panel 0xec1a5cbf) are the host's.
void cSC4ViewInputControlPlayerDrive::ShowUI(std::uint32_t id) {
    std::uint32_t missionPanel = 0;
    if (!dashboard_ && g_host) dashboard_ = g_host->ShowDashboard(id, missionPanel);
    if (!dashboard_) SC4_UNWIRED("cSC4ViewInputControlPlayerDrive::ShowUI: the dashboard window (exemplar 0x2be8e61b and its dials) was not created");
    if (missionPanel != 0 && g_host) g_host->ShowMissionStatusPanel(missionPanel);
}

void cSC4ViewInputControlPlayerDrive::HideUI() {
    if (!g_host) return;
    if (dashboard_) {
        g_host->HideDashboard();
        dashboard_ = false;
    }
    g_host->HideMissionStatusPanel();
}

// 0x004b5aca: commands 0, 1, 2, 3, 5, 7 reach the automaton and start their AEMS events (1, 2, 3, 3, 6, 4); 2 and 3 press
// the dashboard's steering buttons first; 6, 11 and 12 only reach the automaton; 4 and 10 toggle (the toggled value is what
// the automaton gets); 9 also flips the drive state's byte +0x229. Other commands are not handled.
bool cSC4ViewInputControlPlayerDrive::HandleDriveCommandKeyDown(std::uint32_t command) {
    std::uint32_t event = 0;
    switch (command) {
        case 0: event = kAemsForward; break;
        case 1: event = kAemsReverse; break;
        case 2:
        case 3:
            if (dashboard_ && g_host) g_host->SetDashboardButton(command == 2 ? kDashboardLeftButton : kDashboardRightButton, true);
            event = kAemsTurn;
            break;
        case 4:
            toggle44_ = !toggle44_;
            if (IsDriveAutoValid()) automaton_->HandlePlayerDriveCommand(command, toggle44_);
            return true;
        case 10:
            toggle45_ = !toggle45_;
            if (IsDriveAutoValid()) automaton_->HandlePlayerDriveCommand(command, toggle45_);
            return true;
        case 5: event = kAemsCommand5; break;
        case 6:
        case 11:
        case 12:
            if (IsDriveAutoValid()) automaton_->HandlePlayerDriveCommand(command, true);
            return true;
        case 7: event = kAemsMouse; break;
        case 9:
            if (IsDriveAutoValid()) {
                automaton_->HandlePlayerDriveCommand(command, true);
                if (tAutomataStatePlayerDrive* state = DriveState(automaton_)) state->toggle229 ^= 1;
            }
            return true;
        default: return false;
    }
    if (!IsDriveAutoValid()) return true;
    automaton_->HandlePlayerDriveCommand(command, true);
    AemsStart(event);
    return true;
}

// 0x004b604e: the key-up mirror (4 and 10 do nothing; 6, 9, 11, 12 only release the command).
bool cSC4ViewInputControlPlayerDrive::HandleDriveCommandKeyUp(std::uint32_t command) {
    std::uint32_t event = 0;
    switch (command) {
        case 0: event = kAemsForward; break;
        case 1: event = kAemsReverse; break;
        case 2:
        case 3:
            if (dashboard_ && g_host) g_host->SetDashboardButton(command == 2 ? kDashboardLeftButton : kDashboardRightButton, false);
            event = kAemsTurn;
            break;
        case 4:
        case 10: return true;
        case 5: event = kAemsCommand5; break;
        case 6:
        case 9:
        case 11:
        case 12:
            if (IsDriveAutoValid()) automaton_->HandlePlayerDriveCommand(command, false);
            return true;
        case 7: event = kAemsMouse; break;
        default: return false;
    }
    if (!IsDriveAutoValid()) return true;
    automaton_->HandlePlayerDriveCommand(command, false);
    AemsEnd(event);
    return true;
}

// 0x004b48ca: the key effect of the group template for the command (flags: 1 hold, 2 one shot, 4 hard transition, 8 override,
// 0x10 has an effect, 0x20 has a controller). The drive state keeps one on/off byte per command (+0x205): a hold key follows
// the key, a one-shot key fires on the press only, any other key toggles on the press. A change starts / stops (or restarts,
// for a one-shot) the effect and switches the controller; the override flag answers true so the command itself is skipped.
bool cSC4ViewInputControlPlayerDrive::HandleKeyEffects(std::uint32_t command, bool down) {
    if (!IsDriveAutoValid() || !g_host || command >= kPlayerDriveCommandCount) return false;
    tAutomataStatePlayerDrive* state = DriveState(automaton_);
    const cSC4OccupantGroupTemplate* group = g_host->GroupTemplate(automaton_);
    tKeyEffect effect;
    if (!state || !group || !group->GetKeyEffect(static_cast<std::uint8_t>(command), effect)) return false;
    const std::uint32_t flags = effect.value;
    bool on = state->keyEffectOn205[command] != 0;
    bool oneShot = false;
    bool changed = false;
    bool next = on;
    if (flags & 2) {
        oneShot = down;
        next = false;
        changed = on != next || oneShot;
    } else if (flags & 1) {
        next = down;
        changed = on != next;
    } else if (down) {
        next = !on;
        changed = true;
    }
    state->keyEffectOn205[command] = next ? 1 : 0;
    if (changed) {
        const int transition = (flags >> 2) & 1;
        if (flags & 0x10) {
            if (oneShot) automaton_->RecreateNamedEffects(effect.fx, transition, true);
            else if (next) automaton_->StartNamedEffects(effect.fx, transition, true);
            else automaton_->StopNamedEffects(effect.fx, transition);
        }
        if (flags & 0x20) g_host->SetKeyController(automaton_, effect.controller, next || oneShot, next ? 1 : (oneShot ? -1 : 0));
    }
    return (flags & 8) != 0;
}

// 0x004b5fb2: a repeated key (bit 0x40000000) is swallowed; Escape ends the tool; outside a mission countdown the scripts'
// key map gives the command, its key effect runs and, unless that overrides it, the command.
bool cSC4ViewInputControlPlayerDrive::OnKeyDown(std::uint32_t key, std::uint32_t modifiers) {
    if (modifiers & 0x40000000) return true;
    if (key == 0x1b) {
        EndInput();
        return true;
    }
    if (InputBlocked()) return true;
    if (!g_host || !g_host->HasScriptSystem()) return false;
    const int command = g_host->DriveCommandForKey(key);
    if (command == static_cast<int>(kPlayerDriveCommandCount)) return false;
    if (!HandleKeyEffects(static_cast<std::uint32_t>(command), true)) HandleDriveCommandKeyDown(static_cast<std::uint32_t>(command));
    return true;
}

// 0x004b6344
bool cSC4ViewInputControlPlayerDrive::OnKeyUp(std::uint32_t key, std::uint32_t modifiers) {
    (void)modifiers;
    if (InputBlocked()) return true;
    if (!g_host || !g_host->HasScriptSystem()) return false;
    const int command = g_host->DriveCommandForKey(key);
    if (command == static_cast<int>(kPlayerDriveCommandCount)) return false;
    if (!HandleKeyEffects(static_cast<std::uint32_t>(command), false)) HandleDriveCommandKeyUp(static_cast<std::uint32_t>(command));
    return true;
}

// 0x004b5dc4: choosing mode: a click on an automaton drives it. Driving mode: outside a countdown, a vehicle of group 0x4006
// gets command 7 (its horn / siren). PARTIAL: the first branch (a click on a rail junction toggles it through the network map
// and the junction view, with sound 0x8be8d9ac) needs SC4TrainJunctionView.
bool cSC4ViewInputControlPlayerDrive::OnMouseDownL(int x, int y, std::uint32_t modifiers) {
    (void)modifiers;
    SC4_UNWIRED("cSC4ViewInputControlPlayerDrive::OnMouseDownL: rail junction toggle (SC4TrainJunctionView, sound 0x8be8d9ac) not ported");
    if (!IsDriveAutoValid()) {
        if (!automaton_ && g_host) {
            if (cSC4Automaton* picked = g_host->PickAutomaton(x, y)) {
                EnterDrivingMode(picked, true);
                picked->Release();
            }
        }
    } else if (!InputBlocked() && automaton_->AsOccupant()->IsOccupantGroup(kOccupantGroupLeftClickCommand)) {
        HandleDriveCommandKeyDown(7);
    }
    return true;
}

// 0x004b62c8
bool cSC4ViewInputControlPlayerDrive::OnMouseUpL(int x, int y, std::uint32_t modifiers) {
    (void)x;
    (void)y;
    (void)modifiers;
    if (IsDriveAutoValid() && !InputBlocked()) HandleDriveCommandKeyUp(7);
    return true;
}

// 0x004b3a36 / 0x004b39d6: the right button is command 8.
bool cSC4ViewInputControlPlayerDrive::OnMouseDownR(int x, int y, std::uint32_t modifiers) {
    (void)x;
    (void)y;
    (void)modifiers;
    if (IsDriveAutoValid() && !InputBlocked()) automaton_->HandlePlayerDriveCommand(8, true);
    return true;
}

bool cSC4ViewInputControlPlayerDrive::OnMouseUpR(int x, int y, std::uint32_t modifiers) {
    (void)x;
    (void)y;
    (void)modifiers;
    if (IsDriveAutoValid() && !InputBlocked()) automaton_->HandlePlayerDriveCommand(8, false);
    return true;
}

// 0x004b4310: choosing mode: the automaton under the mouse is the rollover; driving mode: the mouse position goes to the drive
// state (+0x214 / +0x218, the tank's turret aim). PARTIAL: the junction view's hovered cell needs SC4TrainJunctionView.
bool cSC4ViewInputControlPlayerDrive::OnMouseMove(int x, int y, std::uint32_t modifiers) {
    (void)modifiers;
    SC4_UNWIRED("cSC4ViewInputControlPlayerDrive::OnMouseMove: hovered rail junction cell (SC4TrainJunctionView) not ported");
    if (!IsDriveAutoValid()) {
        cSC4Automaton* picked = g_host ? g_host->PickAutomaton(x, y) : nullptr;
        if (!picked) {
            SetRollover(nullptr);
            if (g_host) g_host->ClearCursorText(kPlayerDriveRolloverText);
        } else {
            SetRollover(picked);
            picked->Release();
        }
    } else if (tAutomataStatePlayerDrive* state = DriveState(automaton_)) {
        state->mouseX214 = x;
        state->mouseY218 = y;
    }
    return true;
}

// 0x004b3daa: the dashboard's buttons post the zoom-in / zoom-out commands and the two panel toggles.
bool cSC4ViewInputControlPlayerDrive::DoWinProcMessage(std::uint32_t buttonId) {
    std::uint32_t command = 0;
    switch (buttonId) {
        case 0x4992046a: command = 0x6a935cb7; break;
        case 0xe9920494: command = 0x6a935cba; break;
        case 0x2c16c9cb: command = 0xac1f0449; break;
        case 0x4c1a5cc9: command = 0xcc1f0457; break;
        default: return false;
    }
    if (g_host) g_host->PostCommand(command);
    return true;
}

// 0x004b63ca: per animation tick while the automaton is valid: the AEMS velocity (|speed| / MaxSpeed) and throttle (the drive
// state's speed between its minimum and maximum), and the view's mission status panel; 9 seconds after the final crash the
// tool ends. PARTIAL: the dashboard gauges (speed, throttle, altitude, heading, money, the passenger counts) and the
// cursor of the drive state (+0x114) are the dashboard's.
void cSC4ViewInputControlPlayerDrive::Update() {
    if (IsDriveAutoValid()) {
        if (tAutomataStatePlayerDrive* state = DriveState(automaton_)) {
            SC4_UNWIRED("cSC4ViewInputControlPlayerDrive::Update: dashboard gauges and the drive-state cursor (+0x114) not ported");
            if (dashboard_ && g_host) g_host->UpdateMissionStatusPanel();
            float velocity = 0.0f;
            if (speedReference_ > 0.0f) velocity = static_cast<float>(std::fabs(automaton_->GetCurrentSpeed()) / speedReference_);
            float minimum = 0.0f;
            std::memcpy(&minimum, &state->value40, sizeof minimum);
            const float maximum = state->groupSpeed;
            const float speed = std::fabs(state->value3c);
            float throttle = 0.0f;
            if (maximum > 0.0f) throttle = minimum < maximum ? (speed - minimum) / (maximum - minimum) : speed / maximum;
            if (cISC4AEMSEvents* aems = Aems()) {
                aems->SetParamVelocity(SC4Percentage(velocity));
                aems->SetParamThrottle(SC4Percentage(throttle));
            }
        }
    }
    if (crashTimer_.IsTimerRunning()) {
        float elapsed = 0.0f;
        crashTimer_.GetElapsedTimeFloat(elapsed);
        if (kPlayerDriveCrashEndMilliseconds <= elapsed) EndInput();
    }
}

// 0x004b6ed4: the view changed (zoom buttons), the animation tick (Update), the automaton's callbacks (removed: the tool ends;
// final crash: the engine stops, the dispatch view shows the wreck and the 9 s timer starts; the drive state left without
// one: the engine stops), the panel toggle commands.
bool cSC4ViewInputControlPlayerDrive::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    const std::uint32_t type = message->GetType();
    if (type == kPlayerDriveTickMessage) {
        Update();
        return true;
    }
    auto* standard = dynamic_cast<cIGZMessage2Standard*>(message);
    if (type == kAutomatonMsgAutomaton && standard) {
        auto* automaton = static_cast<cSC4Automaton*>(standard->GetVoid1());
        const std::uint32_t event = static_cast<std::uint32_t>(standard->GetData2());
        if (event == kAutomatonEventRemoved) {
            AutomatonRemoved(automaton);
        } else if (event == kAutomatonEventFinalCrash) {
            AemsEnd(kAemsEngine);
            if (g_host && dispatchView_) g_host->UseAlternateModel(trackId_, true);
            crashTimer_.SetUnits(cRZTimer::kMilliseconds);
            crashTimer_.Restart();
        } else if (event == kAutomataEventStateExited && static_cast<std::uint32_t>(standard->GetData3()) == kStatePlayerDrive) {
            if (!DriveState(automaton)) AemsEnd(kAemsEngine);
        }
        return true;
    }
    return true;
}

}  // namespace sc4
