// cSC4CitySituationManager — the city's "situation" (mission) system: which city situation advice is active, the target
// the player chases, the automata tracked for it, the countdown, the coverage cells and the mission status the query
// windows and the advisors read. Recovered from SimCity 4 Deluxe Edition 1.2.1 (object 0xbf8 bytes, class id 0x0bb14381,
// interface cISC4CitySituationManager 0x6bb1437a; created by cSC4AutomataCOMDirector::CreateSC4CitySituationManager
// @ 0x1004dc076, which is also where the 0xbf8 allocation size comes from).
//
// Original object layout (the five vtable views at +0..+0x27 are not modelled; the class only keeps one object):
//   +0x00 cISC4CitySituationManager vptr   +0x08 cIGZMessageTarget2 vptr (DoMessage(cIGZMessage2*))
//   +0x10 cIGZMessageTarget2 vptr (DoMessage(cGZMessage&))   +0x18 cIGZSerializable vptr   +0x20 cIGZWinProc vptr
//   +0x28 bool initialized      +0x2c u32 refcount        +0x30 cRZDate today (ConstructTodayWrapper)
//   +0x40 cRZAutoRefCount<cISCLua> lua (SL::spLua)        +0x48 cRZRandomSuper random (seed 0xffffffff, 0x9d0 bytes)
//   +0xa18 bool CSIVisible      +0xa19 bool tutorials were loaded from an old save (< version 3)
//   +0xa20 cRZAutoRefCount<cISC4CitySituationAdvice> activeSituation
//   +0xa28 vector<cRZAutoRefCount<cISC4AutomataManager>> automataManagers (vehicle, aircraft, pedestrian, watercraft)
//   +0xa40 list<tSituationTrack> situationTracks
//   +0xa58 unordered_set<cISC4Automaton*> trackedAutomata (count at +0xa70, max load 1.0 at +0xa78)
//   +0xa80 cRZAutoRefCount<cIGZWin> situation window     +0xa88 cRZAutoRefCount<cISC4LotManager>
//   +0xa90 cRZAutoRefCount<cISC4OccupantManager>         +0xa98 cRZAutoRefCount<cIGZWin> popup dialog
//   +0xaa0 cRZAutoRefCount<cSC4DispatchVehicleView>      +0xaa8 u32 (unused, 0)
//   +0xab0 unordered_map<u32, u16> mission success counts (size +0xac8, max load 1.0 at +0xad0)
//   +0xad8 eSuccessResult status (ctor 4 = none)         +0xae0 cRZAutoRefCount<cISC4Automaton> activeAutomaton
//   +0xae8 cRZAutoRefCount<cISC4Occupant> currentTarget  +0xaf0 cRZAutoRefCount<cISC4Occupant> (last target)
//   +0xaf8 cRZAutoRefCount<cISC4Lot> landfill lot        +0xb00 u32 numTargets   +0xb04 u32 currentTargetIndex
//   +0xb08 bool (target sequence reset)                  +0xb10 unordered_set<cRZAutoRefCount<cISC4Occupant>>
//   +0xb38 u32/u32 (unused, 0)  +0xb40 f32 pursuit timer  +0xb44 f32 status speed factor  +0xb48 f32 situation time
//   +0xb4c f32 countdown seconds  +0xb50 f32 time limit seconds  +0xb54 bool countdown finished  +0xb55 bool sound played
//   +0xb58 cRZAutoRefCount<cISC4MissionActor> mission actor  +0xb60 cRZAutoRefCount<cISC4VisualEffect> countdown effect
//   +0xb68 cRZAutoRefCount<cIGZUnknown> (unused)
//   +0xb70 unordered_set<SC4Point<u16>> coverage cells    +0xb98 u64 (unused, 0)
//   +0xba0 unordered_map<SC4Point<u16>, cRZAutoRefCount<cISC4VisualEffect>> coverage cell effects
//   +0xbcc..+0xbe4 int scriptRefs[7] (each -2 = no registry reference)
//   +0xbe8 cRZAutoRefCount<cISC4MySim> mySim             +0xbf0 u32 (unused, 0)
//
// Quirks kept: SetMaxCSI stores into a file-wide byte (0x100b21a70) shared by every instance, so GetMaxCSI is not
// per-city; the mission-success map keys are advice ids and its values are u16 counters; Read only accepts versions
// 0..2 and sets the "loaded" flag for versions below 3; Write always writes version 2 and tolerates a failed stream by
// answering !GetError(); QueryInterface rejects every id above 0x7fffffff except 0xe4fda3d4 / 0xe98b2f57 through the
// signed compare the original uses.
//
// Services the original reaches through SL::sp* globals and raw vslots come in through ICitySituationManagerHost
// (every method names the original call it replaces). The situation/automaton/occupant side speaks the interfaces of
// sc4/CitySituationEntities.h, which the advice class already uses.
#pragma once
#include <cstdint>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gz/cIGZIStream.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZOStream.h"
#include "gz/WinInterfaces.h"
#include "gz/cIGZUnknown.h"
#include "rz/cRZAutoRefCount.h"
#include "rz/cRZDate.h"
#include "rz/cRZRandomSuper.h"
#include "rz/cRZString.h"
#include "sc4/CitySituationAdvice.h"
#include "sc4/GameDataRegistry.h"
#include "sc4/CitySituationEntities.h"
#include "sc4/TutorialTask.h"

class cIGZWin;
class cIGZVariant;

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4CitySituationManager = 0x0bb14381;
constexpr std::uint32_t kGZIID_cISC4CitySituationManager = 0x6bb1437a;
constexpr std::uint32_t kGZIID_cISC4CitySituationManagerSerializable = 0xe4fda3d4;

// The message ids DoMessage(cIGZMessage2*) routes (compared as signed 32-bit values, like the original's jump table).
enum SituationMessage : std::uint32_t {
    kSituationMsgAdviceTriggered = 0x8a09fa64,      // HandleSituationTriggered(advice)
    kSituationMsgAdviceUntriggered = 0x8a09fa65,    // HandleSituationUntriggered(advice)
    kSituationMsgAdviceShown = 0x8a09fa67,          // ShowSituationPopup (ignored: one-shot notification)
    kSituationMsgActivateAdvice = 0xac1045ef,       // InitiateSituationByGuid(guid, hasAuto, autoId)
    kSituationMsgSituationPopup = 0x8a09fa66,       // ShowSituationPopup(advice, automaton)
    kSituationMsgMissionActorSet = 0xcc0d905f,      // keeps the mission actor when it carries flag 0x8
    kSituationMsgConsumeMissionActor = 0xebb67ef7,  // EndDialog + ShowSituationPopup(null, null)
    kSituationMsgAnimationTick = 0x86ad10ee,        // DoAnimationTick(data3 as float)
    kSituationMsgAutomatonStatus = 0x09d92fc3,      // automaton notification: status 2/9/11/12
    kSituationMsgCheatTyped = 0x230e27ac,           // the "cISC4CitySituationManager" cheat
    kSituationMsgAdviceEnabled = 0x26d31ec1,        // re-trigger every city-situation advice
    kSituationMsgCitySituationSelected = 0x2c16bde7,// the player opened a situation from the advisor: popup
    kSituationMsgTimeUp = 0xbb324bd,                // countdown finished: EndSituation
    kSituationMsgTargetMessage = 0x6bb350e4,        // the target automaton reported a mission event
    kSituationMsgOccupantInserted = 0x99ef1142,     // HandleInsertOccupant
    kSituationMsgOccupantRemoved = 0x99ef1143,      // HandleRemoveOccupant
    kSituationMsgSituationReloaded = 0x26d31ec1,
};

// cISC4CitySituationManager::eSuccessResult (the value kept at +0xad8; the constructor sets 4).
enum SituationStatus : std::uint32_t {
    kSituationSuccess = 0, kSituationFailure = 1, kSituationTimeout = 2, kSituationRunning = 3, kSituationNone = 4
};

// SC4Point<unsigned short>: a city cell.
struct SituationCellPoint {
    std::uint16_t x = 0;
    std::uint16_t z = 0;
    bool operator==(const SituationCellPoint& other) const { return x == other.x && z == other.z; }
    bool operator!=(const SituationCellPoint& other) const { return !(*this == other); }
};

struct SituationCellPointHash {        // cSC4CitySituationManager::hash_tCellPoint
    std::size_t operator()(const SituationCellPoint& p) const noexcept {
        return static_cast<std::size_t>(p.x) * 0x10000u + p.z;
    }
};

// cISC4AutomataManager::tAutomatonPtrHash — the automaton pointers are hashed on the pointer bits shifted down.
struct SituationAutomatonPtrHash {
    std::size_t operator()(const ISituationAutomaton* p) const noexcept {
        return reinterpret_cast<std::size_t>(p) >> 4;
    }
};

struct SituationOccupantRefHash {      // hash_cISC4Occupant: hashes the cRZAutoRefCount's pointer
    std::size_t operator()(const cRZAutoRefCount<ISituationOccupant>& p) const noexcept {
        return reinterpret_cast<std::size_t>(p.get()) >> 4;
    }
};

// cSC4CitySituationManager::tSituationTrack (0x18 bytes): one situation's automaton plus its indicator handle.
struct SituationTrack {
    cRZAutoRefCount<cISC4CitySituationAdvice> advice;   // +0x00
    cRZAutoRefCount<ISituationAutomaton> automaton;     // +0x08
    std::uint32_t trackId = 0;                          // +0x10 (0 = not tracked by the dispatch view)

    SituationTrack() = default;
    explicit SituationTrack(cISC4CitySituationAdvice* situation);   // @ 0x1006e60e0
    // @ 0x1006db496: sorts on the advice priority (nullptr advice sorts first).
    bool operator<(const SituationTrack& other) const;
};

// --- host seams ---------------------------------------------------------------------------------------------------
// The situation popup/dialog state of the 3D view (SL::spView3D): the window that owns the popups and the currently
// picked automaton used by the popup commands.
class ISituationView3D {
public:
    virtual ~ISituationView3D() = default;
    // SL::spView3D vslot 0x1c8: the view cancels a pending situation popup.
    virtual void CancelSituationPopup() = 0;
};

// SL::spSC4 (cISC4App) — only the three slots the situation manager uses.
class ISituationApp {
public:
    virtual ~ISituationApp() = default;
    virtual bool DebugFunctionalityEnabled() = 0;         // vslot 0xa8 (GetDebugFunctionalityEnabled)
    // vslot 0x130 (GetPreferences): the preferences block; +0x1334 holds 64 mission ids (zero terminated) whose
    // situations count as already succeeded. Null when there are no preferences.
    virtual std::uint32_t* CompletedMissionIds() = 0;
    virtual void SavePreferences() = 0;                   // vslot 0x88
};

// SL::spCCM (cGZCheatCodeManager) — the cheat code the manager registers for mission testing.
class ISituationCheatCodes {
public:
    virtual ~ISituationCheatCodes() = default;
    virtual void RegisterCheatCode(std::uint32_t id, const char* text) = 0;              // vslot 0x18
    virtual void UnregisterCheatCode(std::uint32_t id) = 0;                              // vslot 0x20
    virtual void AddNotification(cIGZMessageTarget2* target, std::uint32_t id) = 0;       // vslot 0x38
    virtual void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t id) = 0;    // vslot 0x40
};

// The object cSC4AutomataManager::spPlayerDriveView points at through +0x50: it only has to give its position.
// SL::ATM() — only the notification list the manager subscribes to.
class ISituationAutomatonNotifier {
public:
    virtual ~ISituationAutomatonNotifier() = default;
    virtual void AddNotification(cIGZMessageTarget2* target, std::uint32_t mask) = 0;      // vslot 0x28
    virtual void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t mask) = 0;   // vslot 0x30
};

class ISituationDriveView {
public:
    virtual ~ISituationDriveView() = default;
    virtual void Position(float xyz[3]) = 0;              // vslot 0x38
};

// Everything the manager reaches outside of itself. The application installs one host at startup; a missing host is
// tolerated until the affected feature is used.
class ICitySituationManagerHost {
public:
    virtual ~ICitySituationManagerHost() = default;
    virtual ISituationLua* Lua() = 0;                                       // SL::spLua (may be null)
    virtual cIGZMessageServer2* MessageServer() = 0;                        // SL::spMS2 (may be null)
    virtual ISituationApp* App() = 0;                                       // SL::spSC4 (may be null)
    virtual ISituationCheatCodes* CheatCodes() = 0;                         // SL::spCCM (may be null)
    virtual ISituationView3D* View3D() = 0;                                 // SL::spView3D (may be null)
    virtual ISituationDispatchView* DispatchView() = 0;                     // SL::spDispatchManager vslot 0xb0
    virtual ISituationCity* City() = 0;                                     // SL::spCity (may be null)
    virtual ISituationLotManager* LotManager() = 0;                         // SL::spLotManager
    virtual ISituationEffectsManager* EffectsManager() = 0;                 // SL::spCity vslot 0x2a8
    // The four cISC4AutomataManager instances Init collects, in the original order (vehicle, aircraft, pedestrian,
    // watercraft); `index` past the end answers null.
    virtual int AutomataManagerCount() = 0;
    virtual ISituationAutomataManager* AutomataManager(int index) = 0;
    virtual ISituationAutomataManager* VehicleManager() = 0;                // SL::spVehicleManager (ReleaseInstances)
    virtual ISituationDriveView* PlayerDriveView() = 0;                     // cSC4AutomataManager::spPlayerDriveView[0x50]
    virtual ISituationMissionActor* MySimActor() = 0;                       // SL::spMySim (may be null)
    virtual ISituationOccupantManager* OccupantManager() = 0;
    virtual ISituationZoneManager* ZoneManager() = 0;
    virtual ISituationTerrain* Terrain() = 0;
    virtual ISituationTrafficNetwork* TrafficNetwork() = 0;
    virtual ISituationAuraSim* AuraSim() = 0;
    virtual ISituationBudgetSim* BudgetSim() = 0;
    virtual ISituationLandfillDeveloper* LandfillDeveloper() = 0;
    // cSC4AutomataManager::EnterPlayerDriveView(automaton, target) @ 0x1000ff652: the view takes over the drive.
    virtual void EnterPlayerDriveView(ISituationAutomaton* automaton, cIGZMessageTarget2* target) = 0;
    // RZ::WM(): the window manager EndDialog talks to (vslot 0x48 = EndDialog(0xf, window, 0), 0xb8 = DestroyWindow).
    virtual cIGZWinMgr* WindowManager() = 0;
    // The objects the situation dialog keeps in its window params. The original reads the param (id 0xea9f13c0 for
    // the situation window, 0x8bb696c1 for the joyride panel), asks the param object's vslot 0x2f8 for a reference,
    // releases it and clears the param; HandleJoyride keeps the object and drives with it.
    virtual void ReleaseWindowParamObject(cIGZWin* window, std::uint32_t paramId) = 0;
    virtual cIGZWin* WindowParamWindow(cIGZWin* window, std::uint32_t paramId) = 0;   // counted, may be null
    // cSC4HTMLLinkHandler::DoURL(window param) @ 0x1001f22b6: true when the link was followed.
    virtual bool DoURL(const void* text) = 0;
    // SL::spAdvisorSystem vslot 0x38(0x1d) = the "city situations" advisor, vslot 0x58 = its advice list; every entry is
    // QueryInterface'd for 0x6beb1e8a (DoMessage 0x26d31ec1 re-triggers them all).
    virtual std::vector<cISC4CitySituationAdvice*> AdvisorSituations() = 0;
    // SL::spAdvisorSystem vslot 0x38(0x1d) -> vslot 0xa0(advice->AsAdvice()): removes one advice from that list.
    virtual void RetireAdvisorSituation(cISC4CitySituationAdvice* situation) = 0;
    // RZGetFramework() vslot 0x28(0xebe0e860, 0xe898ed03): the cSC4GameDataRegistry service (may be null).
    virtual cISC4GameDataRegistry* GameDataRegistry() = 0;
    // SL::ATM(): the automata manager that broadcasts automaton notifications (vslot 0x28 = add, 0x30 = remove).
    virtual ISituationAutomatonNotifier* AutomataNotifier() = 0;
    // The messages carry the engine's own objects (void1 = the cISC4Occupant / cISC4Automaton the occupant manager and the
    // automata post); the host answers the situation view of that object, or null when it has none.
    virtual ISituationOccupant* OccupantOfMessage(void* engineOccupant) = 0;
    virtual ISituationAutomaton* AutomatonOfMessage(void* engineAutomaton) = 0;
};
ICitySituationManagerHost* CitySituationManagerHost();
void SetCitySituationManagerHost(ICitySituationManagerHost* host);

// --- cSC4CitySituationManager -----------------------------------------------------------------------------------
// The class implements the message target the message server registers (message group +0x08 of the original) and the
// two-slot visibility view the tutorial tasks already use (ITutorialCSIVisibility, vslots 0xf0 / 0xe8).
class cSC4CitySituationManager final : public cIGZMessageTarget2, public ITutorialCSIVisibility {
public:
    cSC4CitySituationManager();                                   // @ 0x1006d6d06
    ~cSC4CitySituationManager() override;                         // @ 0x1006d6f86 (+ 0x1006d732e / 0x1006d7338)

    // cISC4CitySituationManager view (+0): identity / lifetime
    bool QueryInterface(std::uint32_t iid, void** out) override;  // @ 0x1006e57be (+ 4 thunks)
    std::uint32_t AddRef() override;                              // @ 0x1006e5848 (+ 4 thunks)
    std::uint32_t Release() override;                             // @ 0x1006e588e (+ 4 thunks)
    std::uint32_t GetGZCLSID() const { return kGZCLSID_cSC4CitySituationManager; }   // @ 0x1006e5e50
    bool Init();                                                  // @ 0x1006d7366
    bool Shutdown();                                              // @ 0x1006d85c6

    // Accessors (the "situation value store" the query windows read).
    bool GetVisible() override { return csiVisible_; }            // vslot 0xf0 GetCSIVisible @ 0x1006e5e2c (ITutorialCSIVisibility)
    void SetVisible(bool visible) override;                       // vslot 0xe8 SetCSIVisible @ 0x1006db122
    std::uint8_t GetMaxCSI() const;                               // @ 0x1006db10e
    void SetMaxCSI(std::uint8_t max);                             // @ 0x1006db0de
    std::uint16_t GetMissionSuccessCount(std::uint32_t adviceId) const;   // @ 0x1006d8a60
    const cISC4CitySituationAdvice* GetActiveSituation() const { return activeSituation_.get(); }   // @ 0x1006e5d70
    std::uint32_t GetCurrentStatus() const { return status_; }    // @ 0x1006e5d7e
    ISituationAutomaton* GetActiveAuto() const { return activeAutomaton_.get(); }   // @ 0x1006e5d8a
    ISituationOccupant* GetCurrentTarget() const { return currentTarget_.get(); }   // @ 0x1006e5d98
    std::uint32_t GetCurrentTargetIndex() const { return currentTargetIndex_; }     // @ 0x1006e5da6
    std::uint32_t GetNumTargets() const { return numTargets_; }   // @ 0x1006e5db2
    std::uint32_t GetNumTargetsRemaining() const;                 // @ 0x1006e5dbe
    float GetCurrentTimeLimitSeconds() const { return timeLimit_; }   // @ 0x1006e5de6
    float GetCurrentSituationTime() const { return situationTime_; }  // @ 0x1006e5df4
    float GetCurrentTimeRemainingSeconds() const;                 // @ 0x1006e5e02
    std::uint32_t GetTrackedAutomataCount() const;                // @ 0x1006e5e20
    const std::list<SituationTrack>& GetSituationTracks() const { return situationTracks_; }   // port: read-only status listing
    bool IsInitialized() const { return initialized_; }           // port: read-only status listing
    bool CountdownIsInProgress() const { return countdown_ > 0.0f; }   // @ 0x1006e5e38
    void GetCurrentTargetName(cRZString& out) const;               // @ 0x1006d8a94 (returns through a hidden arg)
    bool GetSituationAura(float& radius, float& magnitude) const;  // @ 0x1006d8b54
    bool GetSituationMoney(std::int64_t& money) const;             // @ 0x1006d8b8a
    void ResetTutorials();                                         // @ 0x1006da36c
    bool HandleTuningParametersChanged();                          // @ 0x1006db14a
    void HandleStatusChanged(std::uint32_t status);                // @ 0x1006dae70

    // Message routing
    bool DoMessage(cIGZMessage2* message) override;                // @ 0x1006d8bd0 (+ thunk 0x1006da35e)
    bool DoMessage2(cGZMessage& message);                          // @ 0x1006d8bc0 (cGZMessage view, always false)
    void PostMessageToSelf(std::uint32_t type, cISC4CitySituationAdvice* situation, ISituationAutomaton* automaton);   // @ 0x1006dab92

    // Serialization (cIGZSerializable view at +0x18)
    std::uint32_t Write(cIGZOStream& stream) const;                // @ 0x1006e59ba (+ thunk 0x1006e5a3e)
    std::uint32_t Read(cIGZIStream& stream);                       // @ 0x1006e5a4c (+ thunk 0x1006e5d62)

    // The state clears (Shutdown's helpers)
    void FreeCoverageCells();                                      // @ 0x1006d8946
    void ClearTrackedAutomata();                                   // @ 0x1006d899a
    void ClearSituationTrackList();                                // @ 0x1006d89d8

    // Tracking / target selection (slice 3 and 5)
    bool FindAutomatonForSituation(cISC4CitySituationAdvice* situation, ISituationAutomaton** out);   // @ 0x1006db16a
    bool GetInstancesOfGroup(const std::vector<std::uint32_t>& groups, std::unordered_set<ISituationAutomaton*>& out);   // @ 0x1006db27c
    bool CanPlayerDrive(ISituationAutomaton* automaton) const;      // @ 0x1006db35c
    void TrackAutomaton(ISituationAutomaton* automaton, std::uint32_t& trackId, bool track);   // @ 0x1006db4ec
    void TreatAutomatonAsGroup(ISituationAutomaton* automaton, std::uint32_t group);   // @ 0x1006db912
    void HandleInsertAutomaton(ISituationAutomaton* automaton);      // @ 0x1006db962
    void HandleRemoveAutomaton(ISituationAutomaton* automaton);      // @ 0x1006dbade
    void HandleInsertOccupant(ISituationOccupant* occupant);         // @ 0x1006d9c1c
    void HandleRemoveOccupant(ISituationOccupant* occupant);         // @ 0x1006d9cb0
    void HandleAutomatonMoved(ISituationAutomaton* automaton);       // @ 0x1006d9dac
    void HandleAutomatonDisabled(ISituationAutomaton* automaton);    // @ 0x1006d9dfc
    void SendSituationMessage(std::uint32_t type, cISC4CitySituationAdvice* situation, std::uint32_t data);   // @ 0x1006dcf04
    ISituationAutomaton* GetTargetAsAutomaton() const;               // @ 0x1006e2e08
    bool GetActivePosition(float xyz[3]);                            // @ 0x1006e3c40
    bool RetireSituation(cISC4CitySituationAdvice* situation);       // @ 0x1006dd074
    void EndDialog(cIGZWin* window);                                // @ 0x1006d9ece
    void HandleJoyride(cIGZWin* window);                            // @ 0x1006e5628
    bool DoWinProcMessage(cIGZWin* window, cGZMessage& message);    // @ 0x1006e5536
    bool HandleSituationCancelled();                                // @ 0x1006e56dc
    void InitScriptFuncRef(const char* name, int unused, int& ref); // @ 0x1006e5774
    void CallScriptFunction(int refIndex, int value);                // @ 0x1006dcfa2
    void DoAnimationTick(float elapsedSeconds);                      // @ 0x1006d988c
    std::uint32_t CheckForSuccess(float elapsedSeconds);             // @ 0x1006dac36
    void HandleSituationTriggered(cISC4CitySituationAdvice* situation);      // @ 0x1006d9318
    void HandleSituationUntriggered(cISC4CitySituationAdvice* situation);    // @ 0x1006d96c4
    void HandleSituationSuccess(cISC4CitySituationAdvice* situation);        // @ 0x1006da64e
    void HandleSituationFailure(cISC4CitySituationAdvice* situation);        // @ 0x1006da3ca
    void UpdatePursuit(float elapsedSeconds);                        // @ 0x1006daeec

private:
    bool initialized_ = false;                                   // +0x28
    std::uint32_t refCount_ = 0;                                 // +0x2c
    cRZDate today_;                                              // +0x30
    ISituationLua* lua_ = nullptr;                               // +0x40 (SL::spLua; the host owns the counted ref)
    cRZRandomSuper random_;                                      // +0x48 (seed 0xffffffff)
    bool csiVisible_ = true;                                     // +0xa18
    bool tutorialsLoaded_ = false;                               // +0xa19
    cRZAutoRefCount<cISC4CitySituationAdvice> activeSituation_;  // +0xa20
    std::vector<cRZAutoRefCount<ISituationAutomataManager>> automataManagers_;   // +0xa28
    std::list<SituationTrack> situationTracks_;                  // +0xa40
    // +0xa58. The original hashes the automaton pointers shifted down (tAutomatonPtrHash); that only changes the
    // iteration order of the set, which the port leaves to std::hash.
    std::unordered_set<ISituationAutomaton*> trackedAutomata_;
    cRZAutoRefCount<cIGZWin> situationWindow_;                   // +0xa80
    ISituationLotManager* lotManager_ = nullptr;                 // +0xa88 (borrowed from the host)
    ISituationOccupantManager* occupantManager_ = nullptr;       // +0xa90 (borrowed)
    cRZAutoRefCount<cIGZWin> popupDialog_;                       // +0xa98
    ISituationDispatchView* dispatchView_ = nullptr;             // +0xaa0 (borrowed)
    std::uint32_t unusedAa8_ = 0;                                // +0xaa8
    std::unordered_map<std::uint32_t, std::uint16_t> successCounts_;   // +0xab0
    std::uint32_t status_ = kSituationNone;                      // +0xad8
    cRZAutoRefCount<ISituationAutomaton> activeAutomaton_;       // +0xae0
    cRZAutoRefCount<ISituationOccupant> currentTarget_;          // +0xae8
    cRZAutoRefCount<ISituationOccupant> lastTarget_;             // +0xaf0
    cRZAutoRefCount<ISituationLandfillLot> landfillLot_;         // +0xaf8
    std::uint32_t numTargets_ = 0;                               // +0xb00
    std::uint32_t currentTargetIndex_ = 0;                       // +0xb04
    bool targetSequenceReset_ = false;                           // +0xb08
    std::unordered_set<cRZAutoRefCount<ISituationOccupant>, SituationOccupantRefHash> targetOccupants_;   // +0xb10
    float pursuitTimer_ = 0.0f;                                  // +0xb40
    float statusSpeedFactor_ = 0.0f;                             // +0xb44
    float situationTime_ = 0.0f;                                 // +0xb48
    float countdown_ = 0.0f;                                     // +0xb4c
    float timeLimit_ = 0.0f;                                     // +0xb50
    bool countdownFinished_ = false;                             // +0xb54
    bool missionSoundPlayed_ = false;                            // +0xb55
    cRZAutoRefCount<ISituationMissionActor> missionActor_;       // +0xb58
    cRZAutoRefCount<ISituationEffect> countdownEffect_;          // +0xb60
    cRZAutoRefCount<cIGZUnknown> unusedB68_;                     // +0xb68
    std::unordered_set<SituationCellPoint, SituationCellPointHash> coverageCells_;   // +0xb70
    std::unordered_map<SituationCellPoint, cRZAutoRefCount<ISituationEffect>, SituationCellPointHash> coverageEffects_;   // +0xba0
    int scriptRefs_[7] = {-2, -2, -2, -2, -2, -2, -2};           // +0xbcc..+0xbe4
    cRZAutoRefCount<ISituationMySim> mySim_;                     // +0xbe8
    std::uint32_t unusedBf0_ = 0;                                // +0xbf0
};

}  // namespace sc4
