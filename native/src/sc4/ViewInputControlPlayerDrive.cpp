#include "sc4/ViewInputControlPlayerDrive.h"

#include "sc4/TrainJunctionView.h"
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
#include "gz/WinBtn.h"
#include "gz/WinGen.h"
#include "gz/WinInterfaces.h"
#include "gz/WinKnob.h"
#include "gz/WinText.h"
#include "gz/WinUtils.h"
#include "gz/cGZPersistResourceKey.h"
#include "gz/Buffer.h"
#include "sc4/SC4Utilities.h"
#include "sc4/WinMiniMap.h"

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
// The dashboard (ShowUI): script group 0x96a006b0, window ids 0x4bcb938a (dashboard) / 0xec1a5cbf (the key help panel,
// instance 0x8c1a5c9f), dial images in group 0x46a006b0; the minimap child 0x0bc3b559 gets SetFlags(0x11, 0x10).
constexpr std::uint32_t kDashboardGroup = 0x96a006b0, kDashboardWindowId = 0x4bcb938a;
constexpr std::uint32_t kDashboardHelpInstance = 0x8c1a5c9f, kDashboardHelpWindowId = 0xec1a5cbf;
constexpr std::uint32_t kDashboardMiniMapId = 0x0bc3b559;
constexpr std::uint32_t kDashboardZoomInButton = 0x4992046a, kDashboardZoomOutButton = 0xe9920494;
// Update's gauges: knobs (min, max, value) and texts.
constexpr std::uint32_t kGaugeSpeed = 0xebcb9403, kGaugeThrottle = 0x2bcb940b, kGaugeAltitude = 0x2bcbce68;
constexpr std::uint32_t kAltitudeText = 0xec0ea996, kMoneyText = 0xac0ea9db, kHeadlightButton = 0x0c16c9b9;
// The vehicle gauges: pitch (-30..30 degrees in radians, 0xbf060a92 / 0x3f060a92), the tank's barrel pitch / turret /
// velocity, the compass; the ammunition text and the fire button (both found recursively).
constexpr std::uint32_t kGaugePitch = 0x6bcbce73, kGaugeBarrelPitch = 0x2bf98d69, kGaugeBarrelTurn = 0xebf98d37;
constexpr std::uint32_t kGaugeCompass = 0x6c0ae7b1, kGaugeBarrelVelocity = 0x2c0c1c8c;
constexpr std::uint32_t kAmmoText = 0x8c3923ae, kFireButton = 0x6c3a0a6f;
constexpr float kPitchGaugeLimit = 0.52359879f;
constexpr std::uint32_t kPlayerDriveShowHelp = 0xac1f0449, kPlayerDriveShowDashboard = 0xcc1f0457, kPlayerDriveToggleHelp = 0xac1f045d;
constexpr std::uint32_t kWinProcButtonMessage = 3, kWinProcButtonSender = 0x287259f6;

// GetChildAs on the dashboard: the child's `iid` view (counted) or null.
template <class T> T* DashboardChild(cIGZWin* dashboard, std::uint32_t id, std::uint32_t iid, bool recursive = false) {
    void* out = nullptr;
    if (!dashboard) return nullptr;
    const bool ok = recursive ? dashboard->GetChildAsRecursive(id, iid, &out) : dashboard->GetChildAs(id, iid, &out);
    return ok ? static_cast<T*>(out) : nullptr;
}
void SetKnob(cIGZWin* dashboard, std::uint32_t id, float minimum, float maximum, float value) {
    if (auto* knob = DashboardChild<gz::cIGZWinKnob>(dashboard, id, gz::kGZIID_cIGZWinKnob)) {
        knob->SetMinValue(minimum);
        knob->SetMaxValue(maximum);
        knob->SetValue(value);
        knob->Release();
    }
}
void SetText(cIGZWin* dashboard, std::uint32_t id, const cRZString& text, bool recursive = false) {
    if (auto* label = DashboardChild<gz::cIGZWinText>(dashboard, id, gz::kGZIID_cIGZWinText, recursive)) {
        if (cIGZWin* window = label->AsIGZWin()) window->SetCaption(text);
        label->Release();
    }
}
void SetDashboardButtonChecked(cIGZWin* dashboard, std::uint32_t id, bool checked) {
    if (auto* button = DashboardChild<gz::cIGZWinBtn>(dashboard, id, gz::kGZIID_cIGZWinBtn)) {
        button->SetChecked(checked);
        button->Release();
    }
}
// Bottom center of the view.
void PlaceAtBottomCenter(cIGZWin* window, cIGZWin* view) {
    if (window && view) window->GZWinMoveTo((view->GetW() - window->GetW()) / 2, view->GetH() - window->GetH());
}
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
    if (dashboard_) dashboard_->Release();
    if (dashboardHelp_) dashboardHelp_->Release();
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
// table, the tool's key accelerator. The two pick filters live with the host's picks (PickAutomaton, ToggleTrainJunctionAt);
// the junction view's cells are mirrored in the tool for the tests; the debug strings had no reader.
bool cSC4ViewInputControlPlayerDrive::Init() {
    if (initialized_) return true;
    junctionHoverX_ = junctionHoverZ_ = junctionSelectedX_ = junctionSelectedZ_ = -1;   // a fresh SC4TrainJunctionView
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
    if (junctionView_) junctionView_->Release();
    junctionView_ = new SC4TrainJunctionView();
    junctionView_->AddRef();
    junctionView_->Init();
    g_host->AddViewObject(junctionView_, 5, 1000);
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
    if (junctionView_) {
        if (g_host) g_host->RemoveViewObject(junctionView_);
        junctionView_->Shutdown();
        junctionView_->Release();
        junctionView_ = nullptr;
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

// 0x004b4b1c: the dashboard exemplar {0x6534284a, 0x690f693f, id} names the dashboard window (script group 0x96a006b0, its
// 0x2be8e61b instance; window 0x4bcb938a) and the mission status panel (0xec1b2361). Each dial child gets its image (group
// 0x46a006b0) and frame count, the window the tool as its procedure, the minimap child SetFlags(0x11, 0x10); it sits at the
// view's bottom center. The key help panel (instance 0x8c1a5c9f, window 0xec1a5cbf) starts hidden at the same place. Then
// the view shows the mission status panel.
void cSC4ViewInputControlPlayerDrive::ShowUI(std::uint32_t id) {
    if (!g_host) return;
    std::uint32_t missionPanel = 0;
    IPlayerDriveHost::Dashboard spec;
    if (!dashboard_ && g_host->DashboardExemplar(id, spec)) {
        missionPanel = spec.missionPanel;
        cIGZWin* view = g_host->ViewWindow();
        dashboard_ = g_host->CreateWindowFromScript(cGZPersistResourceKey{0, kDashboardGroup, spec.windowInstance}, view, kDashboardWindowId);
        if (dashboard_) {
            for (std::size_t i = 0; i < spec.dialIds.size() && i < spec.dialImages.size() && i < spec.dialFrames.size(); ++i) {
                auto* dial = DashboardChild<gz::cIGZWinKnob>(dashboard_, spec.dialIds[i], gz::kGZIID_cIGZWinKnob);
                if (!dial) continue;
                cIGZBuffer* image = g_host->DashboardImage(spec.dialImages[i]);
                dial->SetKnobImage(image);
                if (image) image->Release();
                dial->SetFrameCount(spec.dialFrames[i]);
                dial->Release();
            }
            void* gen = nullptr;
            if (dashboard_->QueryInterface(gz::kGZIID_cIGZWinGen, &gen) && gen) {
                static_cast<gz::cIGZWinGen*>(gen)->SetWinProc(&dashboardProc_);
                static_cast<gz::cIGZWinGen*>(gen)->Release();
            }
            if (auto* map = DashboardChild<cISC4WinMiniMap>(dashboard_, kDashboardMiniMapId, kMiniMapIID)) {
                map->SetFlags(0x11, 0x10);
                map->Release();
            }
            PlaceAtBottomCenter(dashboard_, view);
            dashboardHelp_ = g_host->CreateWindowFromScript(cGZPersistResourceKey{0, kDashboardGroup, kDashboardHelpInstance}, view,
                                                            kDashboardHelpWindowId);
            if (dashboardHelp_) {
                dashboardHelp_->HideWindow();
                PlaceAtBottomCenter(dashboardHelp_, view);
                if (dashboardHelp_->QueryInterface(gz::kGZIID_cIGZWinGen, &gen) && gen) {
                    static_cast<gz::cIGZWinGen*>(gen)->SetWinProc(&dashboardProc_);
                    static_cast<gz::cIGZWinGen*>(gen)->Release();
                }
            }
        }
    }
    if (missionPanel != 0) g_host->ShowMissionStatusPanel(missionPanel);
}

// 0x004b38ee: both dashboard windows leave the view; the view hides the mission status panel.
void cSC4ViewInputControlPlayerDrive::HideUI() {
    if (!g_host) return;
    cIGZWin* view = g_host->ViewWindow();
    for (cIGZWin** window : {&dashboard_, &dashboardHelp_}) {
        if (!*window) continue;
        if (view) view->ChildRemove(*window);
        cIGZWin* old = *window;
        *window = nullptr;
        old->Release();
    }
    g_host->HideMissionStatusPanel();
}

bool cSC4ViewInputControlPlayerDrive::DashboardProc::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == gz::kGZIID_cIGZWinProc || iid == 1) {
        *out = static_cast<gz::cIGZWinProc*>(this);
        AddRef();
        return true;
    }
    return owner_->QueryInterface(iid, out);
}

bool cSC4ViewInputControlPlayerDrive::DashboardProc::DoWinProcMessage(cIGZWin*, cGZMessage& message) {
    if (message.type != kWinProcButtonMessage || message.param1 != kWinProcButtonSender) return false;
    return owner_->DoWinProcMessage(static_cast<std::uint32_t>(message.param2));
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
            SetDashboardButtonChecked(dashboard_, command == 2 ? kDashboardLeftButton : kDashboardRightButton, true);
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
            SetDashboardButtonChecked(dashboard_, command == 2 ? kDashboardLeftButton : kDashboardRightButton, false);
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

// 0x004b5dc4: a click on a rail junction switches it (the junction view keeps its cell) and does nothing else. Otherwise, in
// choosing mode a click on an automaton drives it; in driving mode, outside a countdown, a vehicle of group 0x4006 gets
// command 7 (its horn / siren).
bool cSC4ViewInputControlPlayerDrive::OnMouseDownL(int x, int y, std::uint32_t modifiers) {
    (void)modifiers;
    int cellX = 0, cellZ = 0;
    if (g_host && g_host->ToggleTrainJunctionAt(x, y, cellX, cellZ)) {
        junctionSelectedX_ = cellX;
        junctionSelectedZ_ = cellZ;
        if (junctionView_) junctionView_->SetMouseDownCell(cellX, cellZ);
        return true;
    }
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
// state (+0x214 / +0x218, the tank's turret aim); the junction view gets the cell under the pointer.
bool cSC4ViewInputControlPlayerDrive::OnMouseMove(int x, int y, std::uint32_t modifiers) {
    (void)modifiers;
    int cellX = -1, cellZ = -1;
    if (!g_host || !g_host->TerrainCellAt(x, y, cellX, cellZ)) cellX = cellZ = -1;
    junctionHoverX_ = cellX;   // the junction view's hovered cell
    junctionHoverZ_ = cellZ;
    if (junctionView_) junctionView_->SetHighlightCell(cellX, cellZ);
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

// Update's dashboard block (0x004b63ca): with a drive state, the speed knob (0..MaxSpeed, the current speed), the throttle
// knob (the state's minimum..maximum, its speed clamped), the altitude (height above the ground under the automaton: text
// "%.0f" and a knob 0..100 on the height's last hundred), the vehicle knobs the vehicle classes fill in the drive state
// (pitch, the tank's barrel pitch / turret / velocity, the compass), the headlight button (flag 1 = toggle 4), the city's
// money, and where the dashboard has it the ammunition (shells - shots fired) with the fire button checked while armed;
// then, with or without a state, the view's mission status panel.
void cSC4ViewInputControlPlayerDrive::UpdateDashboard() {
    if (!dashboard_ || !g_host) return;
    if (tAutomataStatePlayerDrive* state = DriveState(automaton_)) {
        SetKnob(dashboard_, kGaugeSpeed, 0.0f, speedReference_, static_cast<float>(automaton_->GetCurrentSpeed()));
        float minimum = 0.0f;
        std::memcpy(&minimum, &state->value40, sizeof minimum);
        const float maximum = state->groupSpeed;
        const float speed = state->value3c;
        const float throttle = minimum <= speed ? (speed <= maximum ? speed : maximum) : minimum;
        SetKnob(dashboard_, kGaugeThrottle, minimum, maximum, throttle);
        float position[3] = {0.0f, 0.0f, 0.0f};                  // automaton vslot 0x1c (i386)
        automaton_->GetPosition(position);
        float altitude = position[1] - g_host->DriveGroundHeight(automaton_, position[0], position[2]);
        cRZString text;
        text.Sprintf("%.0f", static_cast<double>(altitude));
        SetText(dashboard_, kAltitudeText, text);
        while (100.0f < altitude) altitude -= 100.0f;
        SetKnob(dashboard_, kGaugeAltitude, 0.0f, 100.0f, altitude);
        SetKnob(dashboard_, kGaugePitch, -kPitchGaugeLimit, kPitchGaugeLimit, state->pitchGauge);
        SetKnob(dashboard_, kGaugeBarrelPitch, state->barrelPitch[0], state->barrelPitch[1], state->barrelPitch[2]);
        SetKnob(dashboard_, kGaugeBarrelTurn, state->barrelTurn[0], state->barrelTurn[1], state->barrelTurn[2]);
        SetKnob(dashboard_, kGaugeCompass, state->compassMin, state->compassMax, state->compassValue);
        SetKnob(dashboard_, kGaugeBarrelVelocity, state->barrelVelocity[0], state->barrelVelocity[1], state->barrelVelocity[2]);
        if (auto* lights = DashboardChild<gz::cIGZWinBtn>(dashboard_, kHeadlightButton, gz::kGZIID_cIGZWinBtn)) {
            lights->SetBtnFlag(1, toggle44_);
            lights->Release();
        }
        cRZString money;
        SC4MakeMoneyString(g_host->CityMoney(), money);
        SetText(dashboard_, kMoneyText, money);
        if (auto* ammo = DashboardChild<gz::cIGZWinText>(dashboard_, kAmmoText, gz::kGZIID_cIGZWinText, true)) {
            cRZString rounds;
            rounds.Sprintf("%d", state->limit[tAutomataStatePlayerDrive::kFireCommand] - state->used[tAutomataStatePlayerDrive::kFireCommand]);
            if (cIGZWin* window = ammo->AsIGZWin()) window->SetCaption(rounds);
            ammo->Release();
            if (auto* fire = DashboardChild<gz::cIGZWinBtn>(dashboard_, kFireButton, gz::kGZIID_cIGZWinBtn, true)) {
                fire->SetChecked(state->ready[tAutomataStatePlayerDrive::kFireCommand] != 0);
                fire->Release();
            }
        }
    }
    g_host->UpdateMissionStatusPanel();
}

// 0x004b63ca: per animation tick while the automaton is valid: the dashboard (UpdateDashboard), the AEMS velocity
// (|speed| / MaxSpeed) and throttle (the drive state's speed between its minimum and maximum); 9 seconds after the final
// crash the tool ends.
void cSC4ViewInputControlPlayerDrive::Update() {
    if (IsDriveAutoValid()) {
        // The drive state's cursor while command 8 is held, else the tool's default.
        if (tAutomataStatePlayerDrive* state = DriveState(automaton_)) {
            const std::uint32_t cursor = (state->history[8] == -1.0f || state->cursor == 0) ? kPlayerDriveDefaultCursor : state->cursor;
            cursorId_ = cursor;
            SetCursor(cursor);
        }
        UpdateDashboard();
        if (tAutomataStatePlayerDrive* state = DriveState(automaton_)) {
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
    if (type == kPlayerDriveViewChanged) {                       // the zoom buttons follow what the view can still do
        if (dashboard_ && g_host) {
            nsGZWinUtils::SetWindowEnabled(dashboard_, kDashboardZoomInButton, g_host->CanZoomIn());
            nsGZWinUtils::SetWindowEnabled(dashboard_, kDashboardZoomOutButton, g_host->CanZoomOut());
        }
        return true;
    }
    if (type == kPlayerDriveShowHelp || type == kPlayerDriveShowDashboard || type == kPlayerDriveToggleHelp) {
        if (!dashboard_) return true;
        const bool showDashboard = type == kPlayerDriveShowDashboard || (type == kPlayerDriveToggleHelp && !dashboard_->IsVisible());
        if (showDashboard) {
            dashboard_->ShowWindow();
            if (dashboardHelp_) dashboardHelp_->HideWindow();
        } else {
            dashboard_->HideWindow();
            if (dashboardHelp_) dashboardHelp_->ShowWindow();
        }
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
