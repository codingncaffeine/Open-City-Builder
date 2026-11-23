// cSC4ViewInputControlPlayerDrive — the U-Drive-It input tool (Mac i386 0x004b37e8..0x004b72f4, 31 functions; object 0xbc
// bytes, made by cSC4AutomataManager::EnterPlayerDriveView). In choosing mode every automata manager and the simulator pause
// and a click on an automaton takes it over; in driving mode the automaton runs its player-drive state (state 0x10), the
// vehicle manager's camera follows it, the keys the automata scripts map to drive commands (cSC4AutomataScriptSystem::
// GetDriveCommand) reach the automaton's HandlePlayerDriveCommand, and the AEMS gets the engine's velocity / throttle every
// animation tick. Escape, the automaton's removal, or 9 seconds after its final crash end the tool, which leaves the drive
// view (cSC4AutomataManager::ExitPlayerDriveView) from its Shutdown.
//
// Original layout (i386): +0x1c the 3D view (base), +0x28 cIGZMessageTarget2, +0x2c cIGZWinProc, +0x30 the driven automaton
// (counted), +0x34 the dispatch vehicle view, +0x38 its track id, +0x3c (1, unused), +0x40 the speed the AEMS velocity is
// relative to (0.042 until the automaton's MaxSpeed), +0x44 / +0x45 the toggles of commands 4 and 10, +0x48 cRZTimer (units
// 4, the crash timer), +0x5c the rolled-over automaton, +0x60 the callback targets, +0x68 the terrain, +0x6c its scale, +0x70 the
// stoplight distance over it, +0x74..+0x80 four debug strings, +0x84 cAutomataOccupantFilter, +0x88
// cTrafficNetworkFilter_PlayerDrive, +0x8c / +0x90 / +0x94 / +0x98 the vehicle, pedestrian, aircraft and watercraft managers,
// +0x9c SC4TrainJunctionView, +0xa0 the simulator, +0xa4 / +0xa8 the dashboard windows, +0xac the traffic network map, +0xb0
// the automata script system, +0xb4 / +0xb8 the view's key accelerator and the tool's own one.
//
// The services the original reaches through SL::sp* and raw vslots come in through IPlayerDriveHost (each method names the
// call it replaces). The automaton is the recovered cSC4Automaton itself.
#pragma once
#include <cstdint>
#include <list>

#include "gz/cIGZMessage2.h"
#include "rz/cRZString.h"
#include "rz/cRZTimer.h"
#include "sc4/ViewInputControl.h"
#include "gz/WinProc.h"
#include <vector>

class cISC4AEMSEvents;
class cIGZWin;
class cIGZBuffer;
struct cGZPersistResourceKey;

namespace sc4 {

class cSC4Automaton;
class cSC4OccupantGroupTemplate;
class tAutomataStatePlayerDrive;
struct tAutomataPlayerDriveParams;

constexpr std::uint32_t kPlayerDriveToolId = 0x2b709676;          // the tool id the views compare (CurrentInputControlIsPlayerDrive)
constexpr std::uint32_t kPlayerDriveDefaultCursor = 0xc16c4e1b;
constexpr std::uint32_t kPlayerDriveCallbackMessage = 0x0bb324bd; // SendCallbackMessage: void1 = the tool, data2 = event, data3 = data
constexpr std::uint32_t kPlayerDriveRolloverText = 0xab73510c;
constexpr std::uint32_t kPlayerDriveCommandCount = 0xd;
constexpr float kPlayerDriveCrashEndMilliseconds = 9000.0f;       // Update: FLOAT_00a70968
constexpr std::uint32_t kPlayerDriveViewChanged = 0xa6b79602;
constexpr std::uint32_t kPlayerDriveCommandTable[3] = {0xac1f0449, 0xcc1f0457, 0xac1f045d};   // _commandTable (0x00abc300)

enum class ePlayerDriveCallback : std::uint32_t { kExit = 1 };

class IPlayerDriveHost {
public:
    virtual ~IPlayerDriveHost() = default;
    // ---- framework
    virtual void SetTickTarget(cIGZMessageTarget2* target, bool add) = 0;                    // SL::ATM() vslot 0x14 / 0x18 (0xff)
    virtual void SetMessageTarget(cIGZMessageTarget2* target, std::uint32_t type, bool add) = 0;   // SL::spMS2 vslot 0x14 / 0x18
    virtual void SetCommandTable(bool add) = 0;                                              // cIGZCommandServer 0x1c / 0x28 (_commandTable, 3)
    virtual void PostCommand(std::uint32_t command) = 0;                                     // SL::spCommandServer vslot 0x4c(command, 0, 0)
    virtual float TerrainScale() = 0;                                                        // SL::spTerrain vslot 0x18
    // ---- the 3D view (+0x1c)
    virtual void MinimizeUI(bool minimize) = 0;                                              // view vslot 0xdc
    virtual void SetZoomLimits(int minimum, int maximum) = 0;                                // view vslot 0x2c
    virtual void SetZoom(int zoom) = 0;                                                      // view vslot 0x20
    virtual bool CanZoomIn() = 0;                                                            // view vslot 0x30
    virtual bool CanZoomOut() = 0;                                                           // view vslot 0x34
    virtual void ClearCursorText(std::uint32_t id) = 0;                                      // view vslot 0xa4
    virtual void ShowMissionStatusPanel(std::uint32_t id) = 0;                               // view vslot 0xe0
    virtual void HideMissionStatusPanel() = 0;                                               // view vslot 0xe4
    virtual void UpdateMissionStatusPanel() = 0;                                             // view vslot 0xe8
    virtual void HideDriveWindow() = 0;                                                      // nsGZWinUtils::SetWindowVisible(view, 0x43, false)
    // Init: the view's accelerator (vslot 0x7c) is kept and the tool's own one (resource {0xa2e3d533, 0x6a231eaa, 0x0c0eaad0},
    // class 0x42e967be) installed with vslot 0x80; Shutdown puts the kept one back.
    virtual void InstallKeyAccelerator(bool install) = 0;
    virtual bool AllowMaxZoom() = 0;                                                         // SL::spSC4 vslot 0x98 -> +0xf10 (zoom 5, else 4)
    virtual void SetDisasterLayerActive(bool active) = 0;                                    // city vslot 0x120 (GetDisasterLayer) -> vslot 0x14
    // ---- the automata
    virtual int AutomataManagerCount() = 0;                                                  // +0x8c..+0x98 (vehicle, pedestrian, aircraft, watercraft)
    virtual void PauseAutomataManager(int index, bool pause) = 0;                            // manager vslot 0x24 Pause / 0x28 Resume
    virtual void FollowAutomaton(cSC4Automaton* automaton) = 0;                              // vehicle manager vslot 0x88 SetCameraFollow
    virtual bool HasSimulator() = 0;                                                         // +0xa0
    virtual bool SimulatorPaused() = 0;                                                      // simulator vslot 0x3c
    virtual void PauseSimulator(bool pause) = 0;                                             // simulator vslot 0x24 Pause / 0x30 Resume
    // GetAutomataGroup() +0x74, else its template's GetPlayerDriveParams (the parent chain): null when none.
    virtual const tAutomataPlayerDriveParams* PlayerDriveParams(cSC4Automaton* automaton) = 0;
    virtual const cSC4OccupantGroupTemplate* GroupTemplate(cSC4Automaton* automaton) = 0;    // GetAutomataGroup() +0xc
    virtual bool DashboardId(cSC4Automaton* automaton, std::uint32_t& id) = 0;               // exemplar property 0x2be8fbf3
    virtual int DriveCommandForKey(std::uint32_t key) = 0;                                   // script system vslot 0x38 (0xd = none)
    virtual bool HasScriptSystem() = 0;                                                      // +0xb0
    virtual bool CountdownInProgress() = 0;                                                  // city situation manager vslot 0x80
    virtual bool CSIVisible() = 0;                                                           // city situation manager vslot 0x78 (true without one)
    // A key effect's controller (city vslot 0xbc, the controller manager): find (vslot 0x44) or, when `create`, create (vslot
    // 0x14) the named controller of the automaton's occupant; `active` 1 / 0 sets it active / inactive (controller vslot
    // 0x48), -1 leaves it.
    virtual void SetKeyController(cSC4Automaton* automaton, const cRZString& name, bool create, int active) = 0;
    virtual cSC4Automaton* PickAutomaton(int x, int y) = 0;                                  // spRenderer vslot 0x100 with +0x84, QI 0xa9b40f05 (counted)
    // ---- the dispatch vehicle view (+0x34)
    virtual bool HasDispatchView() = 0;
    virtual std::uint32_t TrackOccupant(cSC4Automaton* automaton) = 0;                       // TrackOccupant(occupant, 6, 0, 0)
    virtual void UntrackOccupant(std::uint32_t id) = 0;                                      // UntrackOccupant(id, false)
    virtual void ShowAllIndicators(bool show) = 0;                                           // ShowAllIndicators(4, show)
    virtual void UseAlternateModel(std::uint32_t id, bool alternate) = 0;
    // ---- the dashboard (ShowUI / HideUI / Update's gauges)
    virtual cIGZWin* ViewWindow() = 0;                                                       // view (+0x1c) vslot 0xc (borrowed)
    // spRM vslot 0xc: the exemplar {0x6534284a, 0x690f693f, id} (iid 0xa52160f5); false when there is none. Its properties:
    // 0x2be8e61b the dashboard window's script instance (0 when missing), 0xec1b2361 the mission status panel (0 when
    // missing), and, only when all three are present, the dials: 0x2be8e834 child ids, 0x2be8e6cb image instances,
    // 0xabe8e6cc frame counts (one entry each, the first list's length).
    struct Dashboard {
        std::uint32_t windowInstance = 0;
        std::uint32_t missionPanel = 0;
        std::vector<std::uint32_t> dialIds, dialImages, dialFrames;
    };
    virtual bool DashboardExemplar(std::uint32_t id, Dashboard& out) = 0;
    // SC4CreateWindowFromScript(key, parent, id, cIGZWin iid): one reference, null on failure.
    virtual cIGZWin* CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id) = 0;
    virtual cIGZBuffer* DashboardImage(std::uint32_t instance) = 0;                          // cSLBuffer(0, 0x46a006b0, instance, 1, 0): one reference
    virtual long long CityMoney() = 0;                                                       // spCity vslot 0xd0 (budget) vslot 0x18
    // The altitude gauge's ground under (x, z): an automaton in occupant group 0x4004 (occupant vslot 0x58) reads the terrain's
    // altitude (spTerrain 0x30) where the point is in bounds (0x2c), else the sea level (0x74); any other reads the terrain
    // map's cell height (spCity 0x17c -> spTerrainMap 0x48) in bounds, else the sea level.
    virtual float DriveGroundHeight(cSC4Automaton* automaton, float x, float z) = 0;
    // ---- SL::AEMS() (null when there is no audio event system)
    virtual cISC4AEMSEvents* AEMS() = 0;
    virtual void LoadAutomatonSoundBank(cSC4Automaton* automaton, bool load) = 0;            // AEMS vslot 0xc / 0x10(holder, 0xcbbba52f)
    // cSC4AutomataManager::ExitPlayerDriveView (the last call of Shutdown).
    virtual void ExitPlayerDriveView() = 0;
};
IPlayerDriveHost* PlayerDriveHost();
void SetPlayerDriveHost(IPlayerDriveHost* host);

class cSC4ViewInputControlPlayerDrive final : public cSC4ViewInputControl, public cIGZMessageTarget2 {
public:
    cSC4ViewInputControlPlayerDrive();                                            // 0x004b70fa
    ~cSC4ViewInputControlPlayerDrive() override;

    bool QueryInterface(std::uint32_t iid, void** out) override;                  // 0x004b37e8
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    bool DoMessage(cIGZMessage2* message) override;                               // 0x004b6ed4

    bool Init() override;                                                         // 0x004b72f4
    bool Shutdown() override;                                                     // 0x004b446e
    bool IsSelfScrollingView() override { return true; }                          // 0x00801932
    bool OnKeyDown(std::uint32_t key, std::uint32_t modifiers) override;          // 0x004b5fb2
    bool OnKeyUp(std::uint32_t key, std::uint32_t modifiers) override;            // 0x004b6344
    bool OnMouseDownL(int x, int y, std::uint32_t modifiers) override;            // 0x004b5dc4
    bool OnMouseDownR(int x, int y, std::uint32_t modifiers) override;            // 0x004b3a36
    bool OnMouseUpL(int x, int y, std::uint32_t modifiers) override;              // 0x004b62c8
    bool OnMouseUpR(int x, int y, std::uint32_t modifiers) override;              // 0x004b39d6
    bool OnMouseMove(int x, int y, std::uint32_t modifiers) override;             // 0x004b4310
    void EndInput() override;                                                     // 0x004b3f32
    bool DoWinProcMessage(std::uint32_t buttonId);                                // 0x004b3daa (the button-click branch)
    cIGZWin* DashboardWindow() const { return dashboard_; }
    cIGZWin* DashboardHelpWindow() const { return dashboardHelp_; }

    void EnterChoosingMode();                                                     // 0x004b4100
    void EnterDrivingMode(cSC4Automaton* automaton, bool flag);                   // 0x004b5184
    void AddCallbackTarget(cIGZMessageTarget2* target);                           // 0x004b487c
    bool HandleDriveCommandKeyDown(std::uint32_t command);                        // 0x004b5aca
    bool HandleDriveCommandKeyUp(std::uint32_t command);                          // 0x004b604e
    bool HandleKeyEffects(std::uint32_t command, bool down);                      // 0x004b48ca
    void Update();                                                                // 0x004b63ca
    void AutomatonRemoved(cSC4Automaton* automaton);                              // 0x004b3bea

    cSC4Automaton* DrivenAutomaton() const { return automaton_; }
    bool IsDriveAutoValid() const;                                                // 0x004b398a
    float SpeedReference() const { return speedReference_; }
    bool Toggle4() const { return toggle44_; }
    bool CrashTimerRunning() const { return crashTimer_.IsTimerRunning(); }

private:
    void StopTracking();                                                          // 0x004b3a96
    void StopRollover();                                                          // 0x004b3c1a
    void PauseEverything(bool pause);                                             // 0x004b3fc2
    void ShowUI(std::uint32_t id);                                                // 0x004b4b1c
    void HideUI();                                                                // 0x004b38ee
    void SendCallbackMessage(std::uint32_t event, std::uint32_t data);            // 0x004b3e82
    void SetRollover(cSC4Automaton* automaton);
    bool InputBlocked();                                                          // city situation manager vslot 0x80

    cSC4Automaton* automaton_ = nullptr;          // +0x30 (counted)
    bool dispatchView_ = false;                   // +0x34
    std::uint32_t trackId_ = 0;                   // +0x38
    float speedReference_ = 0.042f;               // +0x40 (0x3d2c0831)
    bool toggle44_ = false;                       // +0x44
    bool toggle45_ = false;                       // +0x45
    cRZTimer crashTimer_{cRZTimer::kMilliseconds};   // +0x48 (units 4)
    cSC4Automaton* rollover_ = nullptr;           // +0x5c (counted)
    std::list<cIGZMessageTarget2*> callbacks_;    // +0x60 (counted)
    float terrainScale_ = 1.0f;                   // +0x6c
    bool hasTerrain_ = false;                     // +0x68
    // The dashboard windows' procedure (+0x2c in the original: the tool itself): a button activation (message 3 from
    // 0x287259f6) runs DoWinProcMessage.
    class DashboardProc final : public gz::cIGZWinProc {
    public:
        explicit DashboardProc(cSC4ViewInputControlPlayerDrive* owner) : owner_(owner) {}
        bool QueryInterface(std::uint32_t iid, void** out) override;
        std::uint32_t AddRef() override { return owner_->AddRef(); }
        std::uint32_t Release() override { return owner_->Release(); }
        bool DoWinProcMessage(cIGZWin* window, cGZMessage& message) override;
    private:
        cSC4ViewInputControlPlayerDrive* owner_;
    };
    void UpdateDashboard();                                                       // Update's gauge block (0x004b63ca)
    cIGZWin* dashboard_ = nullptr;                // +0xa4 (counted)
    cIGZWin* dashboardHelp_ = nullptr;            // +0xa8 (counted): the key help panel 0xec1a5cbf
    DashboardProc dashboardProc_{this};
};

}  // namespace sc4
