// U-Drive-It on the native application: the services of cSC4ViewInputControlPlayerDrive (sc4::IPlayerDriveHost), the automata
// manager's view of the tool (IAutomataPlayerDriveControl: cSC4AutomataManager::EnterPlayerDriveView makes it and pushes it on the
// city view), the camera control the vehicle manager's camera follow moves, the player-drive parameters the vehicles read, and
// the kit's `udi` verbs (they call the tool's own entry points; nothing here sends input).
//   PARTIAL: the dashboard windows, the automaton pick of the choosing mode, the key effects' controllers and the dispatch
//   vehicle view answer neutrally; the AEMS calls reach SL::AEMS() only when an audio event system is registered.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

#include "app/NativeAutomataViews.h"
#include "app/NativeCity.h"
#include "app/NativeKeyboard.h"
#include "gz/WinKeyAccelerator.h"
#include "gz/WinKeyAcceleratorRes.h"
#include "app/NativeAnimationTicks.h"
#include "app/NativeUi.h"
#include "sc4/AutomataDriveParams.h"
#include "sc4/AutomataManager.h"
#include "sc4/AutomataScriptSystem.h"
#include "sc4/AutomataStates.h"
#include "sc4/Automaton.h"
#include "sc4/Disaster.h"
#include "sc4/OccupantGroupTemplate.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/ViewInputControlPlayerDrive.h"
#include "sc4/Vehicle.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"
#include "sc4audio/AEMSEvents.h"

namespace native {
bool NativePlayerDriveCameraPresent();   // app/NativePlayerDriveView.cpp
int NativePlayerDriveCameraZoom();
bool NativePlayerDriveCameraSetZoom(float zoom);
void NativePlayerDriveCameraViewport(int& width, int& height);
bool NativePlayerDriveCameraSetTarget(const float xyz[3]);
bool NativePlayerDriveCameraSetVelocity(const float xyz[3]);
bool NativePlayerDriveCameraTarget(float out[3]);
bool NativeSituationCountdownInProgress();
bool NativeSituationCSIVisible();
bool NativeSituationStatus(std::uint32_t& status);
void NativeDriveViewMinimizeUI(bool minimize);
void NativeDriveViewSetZoomLimits(int minimum, int maximum);
void NativeDriveViewSetZoom(int zoom);
bool NativeDriveViewCanZoom(bool in);
void NativeDriveViewClearCursorText(std::uint32_t id);
void NativeDriveViewMissionPanel(int op, std::uint32_t id);
void NativeDriveViewHideWindow(std::uint32_t id);
bool NativeDriveViewSwapAccelerator(cIGZWinKeyAccelerator* own, cIGZWinKeyAccelerator*& saved);
void NativeDriveViewRestoreAccelerator(cIGZWinKeyAccelerator* saved);
void NativeDriveViewPushTool(sc4::cSC4ViewInputControl* tool, int operation);
std::uint32_t NativeDriveViewCurrentToolId();
namespace {
using namespace sc4;

constexpr std::uint32_t kPlayerDriveKeyMap = 0x0c0eaad0;            // the tool's accelerator table {0xa2e3d533, 0x6a231eaa, ...}
constexpr std::uint32_t kPropertyDashboard = 0x2be8fbf3;
constexpr std::uint32_t kPlayerDriveFlags = 0x2010002;              // CreateAutomatonForPlayerDrive's creation flags
constexpr float kPlayerDriveCreationRadius = 64.0f;

template <class T> T* Concrete(CityComponent c) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    return object ? dynamic_cast<T*>(object) : nullptr;
}
template <class T> T* Plain(CityComponent c) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_PlainObject, &raw) || !raw) return nullptr;
    return static_cast<T*>(raw);
}
cSC4AutomataScriptSystem* ScriptSystem() {
    if (auto* s = Concrete<cSC4AutomataScriptSystem>(CityComponent::AutomataScriptSystem)) return s;
    return Plain<cSC4AutomataScriptSystem>(CityComponent::AutomataScriptSystem);
}
cSC4AutomataManager* VehicleManager() { return Concrete<cSC4AutomataManager>(CityComponent::VehicleManager); }

// The four managers the tool pauses (+0x8c vehicle, +0x90 pedestrian, +0x94 aircraft, +0x98 watercraft). PARTIAL: the native
// watercraft manager is a class of its own and is not paused.
cSC4AutomataManager* ManagerAt(int index) {
    static constexpr CityComponent kManagers[3] = {CityComponent::VehicleManager, CityComponent::PedestrianManager, CityComponent::AircraftManager};
    return index >= 0 && index < 3 ? Concrete<cSC4AutomataManager>(kManagers[index]) : nullptr;
}

const cSC4OccupantGroupTemplate* TemplateOf(cSC4Automaton* automaton) {
    return automaton ? OccupantGroupTemplateOf(automaton->GetAutomataGroup()) : nullptr;
}

// cSC4OccupantGroupTemplate::GetPlayerDriveParams (i386 0x0062ddb6): the template's own block, else its parent's, up the chain.
const tAutomataPlayerDriveParams* ParamsOf(const cSC4OccupantGroupTemplate* group) {
    for (int depth = 0; group && depth < 64; ++depth, group = group->parent)
        if (group->playerDriveParams) return group->playerDriveParams.get();
    return nullptr;
}

// The drive key map (ImportDriveCommandKeyMap's accelerator), made once.
cIGZWinKeyAccelerator* DriveKeyMap() {
    static cIGZWinKeyAccelerator* map = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        std::vector<std::uint8_t> bytes;
        if (LoadResourceBytes(gz::kKeyAcceleratorResType, gz::kKeyAcceleratorResGroup, kPlayerDriveKeyMap, bytes))
            map = CreateGameKeyAccelerator(kPlayerDriveKeyMap);
    }
    return map;
}


// ---- the camera control the camera follow moves (SL::spRenderer vslot 0x80) --------------------------------------------
class CameraView final : public IAutomataCameraControl {
public:
    int VirtualZoom() override { return NativePlayerDriveCameraZoom(); }
    bool SetZoom(float zoom) override { return NativePlayerDriveCameraSetZoom(zoom); }
    void GetViewportSize(int& width, int& height) override { NativePlayerDriveCameraViewport(width, height); }
    bool SetViewTargetPosition(const cS3DVector3& p) override {
        const float xyz[3] = {p.x, p.y, p.z};
        static const bool trace = std::getenv("SC4_UDIDEBUG") != nullptr;
        if (trace) std::fprintf(stderr, "udi: camera target (%.1f, %.1f, %.1f)\n", p.x, p.y, p.z);
        return NativePlayerDriveCameraSetTarget(xyz);
    }
    bool SetViewTargetVelocity(const cS3DVector3& v) override { const float xyz[3] = {v.x, v.y, v.z}; return NativePlayerDriveCameraSetVelocity(xyz); }
    void TargetVelocity(float out[3]) override { NativePlayerDriveCameraTarget(out); }
} g_camera;

// ---- the tool's services ---------------------------------------------------------------------------------------------------
class PlayerDriveHost final : public IPlayerDriveHost {
public:
    void SetTickTarget(cIGZMessageTarget2* target, bool add) override {
        if (cIGZMessageServer2* atm = AnimationTicks()) {
            if (add) atm->AddNotification(target, 0xff);
            else atm->RemoveNotification(target, 0xff);
        }
    }
    void SetMessageTarget(cIGZMessageTarget2* target, std::uint32_t type, bool add) override {
        if (!SL::spMS2) return;
        if (add) SL::spMS2->AddNotification(target, type);
        else SL::spMS2->RemoveNotification(target, type);
    }
    void SetCommandTable(bool) override {}   // the three panel commands only post through the dashboard's buttons
    void PostCommand(std::uint32_t command) override { ExecuteGameCommand(command); }
    float TerrainScale() override { return 16.0f; }   // cSTETerrain vslot 0x18: the meters per cell
    void MinimizeUI(bool minimize) override { NativeDriveViewMinimizeUI(minimize); }
    void SetZoomLimits(int minimum, int maximum) override { NativeDriveViewSetZoomLimits(minimum, maximum); }
    void SetZoom(int zoom) override { NativeDriveViewSetZoom(zoom); }
    bool CanZoomIn() override { return NativeDriveViewCanZoom(true); }
    bool CanZoomOut() override { return NativeDriveViewCanZoom(false); }
    void ClearCursorText(std::uint32_t id) override { NativeDriveViewClearCursorText(id); }
    void ShowMissionStatusPanel(std::uint32_t id) override { NativeDriveViewMissionPanel(1, id); }
    void HideMissionStatusPanel() override { NativeDriveViewMissionPanel(0, 0); }
    void UpdateMissionStatusPanel() override { NativeDriveViewMissionPanel(2, 0); }
    void HideDriveWindow() override { NativeDriveViewHideWindow(0x43); }
    void InstallKeyAccelerator(bool install) override {
        if (install) {
            if (cIGZWinKeyAccelerator* own = DriveKeyMap()) installed_ = NativeDriveViewSwapAccelerator(own, saved_);
        } else if (installed_) {
            NativeDriveViewRestoreAccelerator(saved_);
            saved_ = nullptr;
            installed_ = false;
        }
    }
    bool AllowMaxZoom() override { return false; }   // SL::spSC4 +0xf10 (the "allow zoom 6" preference, off by default)
    void SetDisasterLayerActive(bool active) override {
        cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(CityComponent::DisasterLayer) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kGZIID_cISC4DisasterLayer, &raw) || !raw) return;
        static_cast<cISC4DisasterLayer*>(raw)->EnableDisasters(active);
        static_cast<cISC4DisasterLayer*>(raw)->Release();
    }
    int AutomataManagerCount() override { return 4; }
    void PauseAutomataManager(int index, bool pause) override {
        if (cSC4AutomataManager* m = ManagerAt(index)) {
            if (pause) m->Pause();
            else m->Resume();
        }
    }
    void FollowAutomaton(cSC4Automaton* automaton) override {
        if (cSC4AutomataManager* m = VehicleManager()) m->SetCameraFollow(automaton ? AutomatonView(automaton) : nullptr);
    }
    bool HasSimulator() override { return CitySimulatorObject() != nullptr; }
    bool SimulatorPaused() override { cSC4Simulator* s = CitySimulatorObject(); return s && s->IsPaused(); }
    void PauseSimulator(bool pause) override {
        if (cSC4Simulator* s = CitySimulatorObject()) {
            if (pause) s->Pause();
            else s->Resume();
        }
    }
    const tAutomataPlayerDriveParams* PlayerDriveParams(cSC4Automaton* automaton) override { return ParamsOf(TemplateOf(automaton)); }
    const cSC4OccupantGroupTemplate* GroupTemplate(cSC4Automaton* automaton) override { return TemplateOf(automaton); }
    bool DashboardId(cSC4Automaton* automaton, std::uint32_t& id) override {
        return automaton && cSCPropertyHelp::GetPropertyValue(automaton->AsPropertyHolder(), kPropertyDashboard, id);
    }
    int DriveCommandForKey(std::uint32_t key) override {
        cSC4AutomataScriptSystem* s = ScriptSystem();
        return s ? s->GetDriveCommand(key) : kPlayerDriveCommandNone;
    }
    bool HasScriptSystem() override { return ScriptSystem() != nullptr; }
    bool CountdownInProgress() override { return NativeSituationCountdownInProgress(); }
    bool CSIVisible() override { return NativeSituationCSIVisible(); }
    void SetKeyController(cSC4Automaton*, const cRZString&, bool, int) override {}
    cSC4Automaton* PickAutomaton(int, int) override { return nullptr; }
    bool HasDispatchView() override { return false; }
    std::uint32_t TrackOccupant(cSC4Automaton*) override { return 0; }
    void UntrackOccupant(std::uint32_t) override {}
    void ShowAllIndicators(bool) override {}
    void UseAlternateModel(std::uint32_t, bool) override {}
    bool ShowDashboard(std::uint32_t, std::uint32_t& missionPanel) override { missionPanel = 0; return false; }
    void HideDashboard() override {}
    void SetDashboardButton(std::uint32_t, bool) override {}
    cISC4AEMSEvents* AEMS() override {
        cIGZUnknown* service = SL::AEMS();
        void* raw = nullptr;
        if (!service || !service->QueryInterface(kGZIID_cISC4AEMSEvents, &raw) || !raw) return nullptr;
        auto* events = static_cast<cISC4AEMSEvents*>(raw);
        events->Release();   // SL::AEMS() keeps the service
        return events;
    }
    void LoadAutomatonSoundBank(cSC4Automaton* automaton, bool load) override {
        cISC4AEMSEvents* events = AEMS();
        if (!events || !automaton) return;
        if (load) events->LoadSoundBank(automaton->AsPropertyHolder(), 0xcbbba52f);
        else events->UnloadSoundBank(automaton->AsPropertyHolder(), 0xcbbba52f);
    }
    void ExitPlayerDriveView() override { if (cSC4AutomataManager* m = VehicleManager()) m->ExitPlayerDriveView(); }

private:
    static cIGZMessageServer2* AnimationTicks() { return AnimationTickServerView(); }
    cIGZWinKeyAccelerator* saved_ = nullptr;   // the view's own accelerator while the tool's is in (counted)
    bool installed_ = false;
} g_host;

// ---- the automata manager's view of the tool --------------------------------------------------------------------------------
class ControlView final : public IAutomataPlayerDriveControl {
public:
    ControlView() : tool_(new cSC4ViewInputControlPlayerDrive()) { tool_->AddRef(); }
    ~ControlView() { tool_->Release(); }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_ > 0) return refs_;
        delete this;
        return 0;
    }
    void EnterChoosingMode() override { tool_->EnterChoosingMode(); }
    void EnterDrivingMode(IAutomataAutomaton* automaton, bool flag) override { tool_->EnterDrivingMode(AutomatonOf(automaton), flag); }
    void AddCallbackTarget(cIGZMessageTarget2* target) override { tool_->AddCallbackTarget(target); }
    bool HandlesGroup(std::uint32_t) override { return true; }
    bool HasDrivenAutomaton() override { return tool_->DrivenAutomaton() != nullptr; }
    std::uint32_t DrivenGroup() override {
        const cSC4OccupantGroupTemplate* group = TemplateOf(tool_->DrivenAutomaton());
        return group ? group->groupId : 0;
    }
    cSC4ViewInputControlPlayerDrive* Tool() const { return tool_; }

private:
    std::uint32_t refs_ = 0;
    cSC4ViewInputControlPlayerDrive* tool_;
};
ControlView* g_control = nullptr;   // the view cSC4AutomataManager::spPlayerDriveView holds (SetPlayerDriveControl)

cSC4ViewInputControlPlayerDrive* Tool() { return g_control ? g_control->Tool() : nullptr; }

}  // namespace

// ---- the automata manager host's player-drive calls (NativeAutomataManagerHost.cpp) ---------------------------------------
IAutomataPlayerDriveControl* NativeCreatePlayerDriveControl() { return new ControlView(); }
void NativeSetPlayerDriveControl(IAutomataPlayerDriveControl* control) { g_control = static_cast<ControlView*>(control); }
IAutomataPlayerDriveControl* NativePlayerDriveControl() { return g_control; }
// SL::spView3D vslot 0xc8(tool, operation): the view takes the tool (and Inits it).
void NativeAddPlayerDriveControl(IAutomataPlayerDriveControl* control, int operation) {
    if (control) NativeDriveViewPushTool(static_cast<ControlView*>(control)->Tool(), operation);
}
// SL::spView3D vslot 0xc0 -> vslot 0x28 == 0x2b709676: the view's current tool is the U-Drive-It tool.
bool NativeCurrentInputControlIsPlayerDrive() {
    return NativeDriveViewCurrentToolId() == kPlayerDriveToolId;
}
IAutomataCameraControl* NativeAutomataCameraControl() { return NativePlayerDriveCameraPresent() ? &g_camera : nullptr; }
const tAutomataPlayerDriveParams* NativePlayerDriveParams(cSC4Automaton* automaton) { return ParamsOf(TemplateOf(automaton)); }
// The automata script system's drive key map (ImportDriveCommandKeyMap): GetKeysForAccelerator of each command.
bool NativeDriveKeyMapAvailable() { return DriveKeyMap() != nullptr; }
void NativeDriveKeyCodes(int command, std::uint32_t keys[8], std::uint32_t& count) {
    cIGZWinKeyAccelerator* map = DriveKeyMap();
    if (!map) {
        count = 0;
        return;
    }
    map->GetKeysForAccelerator(static_cast<std::uint32_t>(command), keys, count);
}

void InstallPlayerDrive() { SetPlayerDriveHost(&g_host); }

// ---- the kit's `udi` verbs ------------------------------------------------------------------------------------------------
namespace {
void Status(const char* label) {
    cSC4ViewInputControlPlayerDrive* tool = Tool();
    cSC4Automaton* automaton = tool ? tool->DrivenAutomaton() : nullptr;
    std::printf("udi %s: tool %s, current %s", label, tool ? "yes" : "no", NativeCurrentInputControlIsPlayerDrive() ? "drive" : "other");
    if (!automaton) {
        std::printf(", no vehicle\n");
        return;
    }
    float position[3] = {0, 0, 0}, heading[3] = {0, 0, 0};
    automaton->GetPosition(position);
    automaton->GetOrientation(heading);
    const cSC4OccupantGroupTemplate* group = TemplateOf(automaton);
    auto* state = static_cast<tAutomataStatePlayerDrive*>(automaton->FindState(kStatePlayerDrive));
    std::printf(", vehicle %p group 0x%x state 0x%x pos (%.1f, %.1f, %.1f) heading (%.2f, %.2f) speed %.4f actual %.4f",
                static_cast<void*>(automaton), group ? group->groupId : 0, automaton->GetBehavior(), position[0], position[1], position[2],
                heading[0], heading[2], automaton->GetCurrentSpeed(), automaton->GetActualSpeed());
    if (auto* vehicle = dynamic_cast<cSC4Vehicle*>(automaton)) {
        const cS3DVector3& path = vehicle->GetPathPos();
        std::printf(" path pos (%.1f, %.1f, %.1f) occupant %s", path.x, path.y, path.z, vehicle->GetNetworkOccupant() ? "yes" : "no");
    }
    std::printf(" valid %d removing %d disabled %d", tool->IsDriveAutoValid() ? 1 : 0, automaton->CheckAutomataFlag(kAutomataFlagMarkedForRemove) ? 1 : 0,
                automaton->CheckAutomataFlag(kAutomataFlagDisabled) ? 1 : 0);
    if (state)
        std::printf(" cmd fwd %.0f rev %.0f left %.0f right %.0f steer %.3f off-road %u", state->history[0], state->history[1], state->history[2],
                    state->history[3], state->steering6c, state->onRoad254);
    float target[3];
    if (NativePlayerDriveCameraTarget(target)) std::printf(" camera (%.1f, %.1f, %.1f)", target[0], target[1], target[2]);
    std::uint32_t status = 0;
    if (NativeSituationStatus(status)) std::printf(" mission status %u countdown %d", status, NativeSituationCountdownInProgress() ? 1 : 0);
    std::printf("\n");
}
}  // namespace

void NativePlayerDriveCommand(const std::string& line) {
    std::istringstream words(line);
    std::string verb;
    words >> verb;
    if (verb.empty() || verb == "status") {
        Status("status");
    } else if (verb == "groups") {   // the script groups with player-drive parameters (what can be driven)
        cSC4AutomataScriptSystem* s = ScriptSystem();
        int count = 0;
        if (s)
            for (const auto& [name, group] : s->GetAutomataGroupMap())
                if (group && group->playerDriveParams) {
                    const tAutomataPlayerDriveParams* p = group->playerDriveParams.get();
                    std::printf("udi group 0x%08x %s: max speed %.4f, zoom %d..%d\n", group->groupId, name.c_str(), p->F(0x8), p->I(0x0), p->I(0x4));
                    ++count;
                }
        std::printf("udi groups: %d (script system %s, key map %s)\n", count, s ? "yes" : "no", DriveKeyMap() ? "yes" : "no");
    } else if (verb == "start") {   // start GROUP X Z [RADIUS]: CreateAutomatonForPlayerDrive's creation near a point, then StartJoyride's drive view
        std::string groupText;
        float x = 0.0f, z = 0.0f;
        float radius = kPlayerDriveCreationRadius;   // the original's distance from the source building
        words >> groupText >> x >> z >> radius;
        const std::uint32_t group = static_cast<std::uint32_t>(std::stoul(groupText, nullptr, 0));
        cSC4AutomataManager* manager = nullptr;
        for (int i = 0; i < 3 && !manager; ++i)
            if (cSC4AutomataManager* m = ManagerAt(i); m && m->CanCreateGroup(group)) manager = m;
        if (!manager) {
            std::printf("udi start: no manager takes group 0x%x\n", group);
            return;
        }
        const cS3DVector3 origin{x, 0.0f, z};
        cS3DVector3 where{x, 0.0f, z};
        const bool placed = manager->GetRandomCreationPoint(group, origin, 0.0f, radius, origin, where, kPlayerDriveFlags);
        IAutomataAutomaton* made = manager->CreateAutomaton(group, where, kPlayerDriveFlags, cGZPersistResourceKey{}, nullptr, 0xff, nullptr);
        std::printf("udi start: group 0x%x near (%.0f, %.0f): point %s (%.1f, %.1f, %.1f), automaton %p\n", group, x, z, placed ? "found" : "none",
                    where.x, where.y, where.z, static_cast<void*>(made ? AutomatonOf(made) : nullptr));
        if (!made) return;
        if (cSC4AutomataManager* vehicles = VehicleManager()) vehicles->EnterPlayerDriveView(made, nullptr);
        Status("start");
    } else if (verb == "take") {   // take X Z [GROUP]: what the choosing mode's click does (EnterDrivingMode) with the nearest car on a road path
        float x = 0.0f, z = 0.0f;
        std::string groupText;
        words >> x >> z >> groupText;
        const std::uint32_t group = groupText.empty() ? 0u : static_cast<std::uint32_t>(std::stoul(groupText, nullptr, 0));
        cSC4AutomataManager* vehicles = VehicleManager();
        cSC4Vehicle* best = nullptr;
        float bestDistance = 0.0f;
        if (vehicles)
            for (IAutomataAutomaton* view : vehicles->Automata()) {
                auto* vehicle = dynamic_cast<cSC4Vehicle*>(AutomatonOf(view));
                if (!vehicle || !vehicle->GetNetworkOccupant() || !ParamsOf(TemplateOf(vehicle))) continue;
                if (!vehicle->CurrentPath() || vehicle->CurrentPath()->empty()) continue;   // a car driving along its path
                if (vehicle->CheckAutomataFlag(kAutomataFlagMarkedForRemove) || vehicle->GetBehavior() == kStatePlayerDrive) continue;
                if (group != 0 && !vehicle->AsOccupant()->IsOccupantGroup(group)) continue;
                float p[3];
                vehicle->GetPosition(p);
                const float d = (p[0] - x) * (p[0] - x) + (p[2] - z) * (p[2] - z);
                if (!best || d < bestDistance) {
                    best = vehicle;
                    bestDistance = d;
                }
            }
        int total = 0, onPath = 0;
        if (vehicles)
            for (IAutomataAutomaton* view : vehicles->Automata())
                if (auto* v = dynamic_cast<cSC4Vehicle*>(AutomatonOf(view))) {
                    ++total;
                    if (v->CurrentPath() && !v->CurrentPath()->empty()) ++onPath;
                }
        std::printf("udi take: near (%.0f, %.0f): %s (vehicles %d, on a path %d)\n", x, z, best ? "found" : "no drivable car", total, onPath);
        if (!best || !vehicles) return;
        if (Tool()) {   // the tool is up (choosing or driving): the click path
            Tool()->EnterDrivingMode(best, true);
        } else {
            vehicles->EnterPlayerDriveView(AutomatonView(best), nullptr);
        }
        Status("take");
    } else if (verb == "steer") {   // steer ACCEL TURN: the drive commands the arrow keys hold (1 / -1 / 0 each)
        int accel = 0, turn = 0;
        words >> accel >> turn;
        cSC4ViewInputControlPlayerDrive* tool = Tool();
        if (!tool) {
            std::printf("udi steer: no tool\n");
            return;
        }
        if (accel > 0) tool->HandleDriveCommandKeyDown(0); else tool->HandleDriveCommandKeyUp(0);
        if (accel < 0) tool->HandleDriveCommandKeyDown(1); else tool->HandleDriveCommandKeyUp(1);
        if (turn < 0) tool->HandleDriveCommandKeyDown(2); else tool->HandleDriveCommandKeyUp(2);
        if (turn > 0) tool->HandleDriveCommandKeyDown(3); else tool->HandleDriveCommandKeyUp(3);
        Status("steer");
    } else if (verb == "stop") {   // stop: what Escape does
        cSC4ViewInputControlPlayerDrive* tool = Tool();
        if (tool) tool->EndInput();
        Status("stop");
    } else {
        std::printf("udi: unknown verb %s\n", verb.c_str());
    }
}

}  // namespace native
