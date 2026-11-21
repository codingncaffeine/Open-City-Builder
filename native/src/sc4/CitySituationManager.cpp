// cSC4CitySituationManager — see sc4/CitySituationManager.h for the object layout and the quirks kept.
// The functions are grouped the way the original's symbol order groups them: lifetime, the situation value store the
// query windows read, the message routing, the tracking/target helpers, the coverage build and the script bridge.
#include "sc4/CitySituationManager.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "gz/CheatCodeManager.h"
#include "gz/StreamOps.h"
#include "rz/cRZCmdLine.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/GameDataRegistry.h"

namespace sc4 {

namespace {

ICitySituationManagerHost* g_host = nullptr;

// The managers' shared maximum CSI value (the original keeps it in a file-wide byte at 0x100b21a70, so two cities in
// one process share it — a quirk kept).
std::uint8_t g_maxCSI = 0;

constexpr std::uint32_t kCheatTriggerMission = 0x8c4136db;
constexpr const char* kCheatTriggerMissionText = "cISC4CitySituationManager";

// Clears a cRZString the way the original's getters do (an empty AssignFromChars).
void ClearString(cRZString& s) { s.AssignFromChars("", 0); }

}  // namespace

ICitySituationManagerHost* CitySituationManagerHost() { return g_host; }
void SetCitySituationManagerHost(ICitySituationManagerHost* host) { g_host = host; }

// ---------------------------------------------------------------------------------------------- situation track

// @ 0x1006e60e0: copies the advice and zeroes the automaton / track id.
SituationTrack::SituationTrack(cISC4CitySituationAdvice* situation) : advice(situation) {}

// @ 0x1006db496: compares the advice priority (AsAdvice()->vslot 0x38); a null advice sorts first.
bool SituationTrack::operator<(const SituationTrack& other) const {
    if (!advice) return other.advice != nullptr;
    if (!other.advice) return false;
    return advice->Priority() < other.advice->Priority();
}

// ---------------------------------------------------------------------------------------------- lifetime

// @ 0x1006d6d06: the five vtable views, then the date, the shared lua pointer, the random generator (seed 0xffffffff,
// which means "from the clock"), the visibility bytes, the containers and the status fields.
cSC4CitySituationManager::cSC4CitySituationManager() : random_(0xffffffff) {}

// @ 0x1006d6f86 (+ the 0x1006d732e / 0x1006d7338 destructors): releases every counted reference and the containers.
cSC4CitySituationManager::~cSC4CitySituationManager() = default;

// @ 0x1006e57be: the original compares the iid as a SIGNED value, so every id above 0x7fffffff takes the first branch;
// 0xe4fda3d4 (serializable) and 1 answer the +0x18 view, 0xe98b2f57 the +0x10 view, 0x452294aa (message target 2) the
// +0x08 view and 0x6bb1437a the base view. The port keeps one object for all five views, so every accepted id answers
// `this` (the thunks of the original only adjust the pointer).
bool cSC4CitySituationManager::QueryInterface(std::uint32_t iid, void** out) {
    if (out == nullptr) return false;
    const std::int32_t signedIid = static_cast<std::int32_t>(iid);
    if (signedIid < 1) {
        if (iid != kGZIID_cISC4CitySituationManagerSerializable && iid != 0xe98b2f57) return false;
    } else if (iid != 1 && iid != 0x452294aa && iid != kGZIID_cISC4CitySituationManager) {
        return false;
    }
    *out = this;
    AddRef();
    return true;
}

// @ 0x1006e5848: the refcount lives at +0x2c; the four thunks increment the same word.
std::uint32_t cSC4CitySituationManager::AddRef() { return ++refCount_; }

// @ 0x1006e588e: below two references the manager shuts itself down and destroys the object.
std::uint32_t cSC4CitySituationManager::Release() {
    if (refCount_ < 2) {
        Shutdown();
        delete this;
        return 0;
    }
    return --refCount_;
}

// ---------------------------------------------------------------------------------------------- value store

// @ 0x1006d8a60: the u16 mission-success counter of an advice id (0 when the mission never succeeded).
std::uint16_t cSC4CitySituationManager::GetMissionSuccessCount(std::uint32_t adviceId) const {
    const auto it = successCounts_.find(adviceId);
    return it == successCounts_.end() ? 0 : it->second;
}

// @ 0x1006d8a94: the current target's name. The original fills a hidden cRZString result: it starts empty and is only
// assigned when the target occupant answers QueryInterface(0x87dda3a9) and that view's vslot 0x60 returns a string.
void cSC4CitySituationManager::GetCurrentTargetName(cRZString& out) const {
    ClearString(out);
    if (!currentTarget_) return;
    if (cIGZString* name = currentTarget_->DisplayName()) out.AssignFromIGZString(*name);
}

// @ 0x1006d8b54: the aura of the active situation — the failure aura once the status is "failure" (2), else the
// success aura. False without an active situation.
bool cSC4CitySituationManager::GetSituationAura(float& radius, float& magnitude) const {
    if (!activeSituation_) return false;
    if (status_ == kSituationTimeout) {
        activeSituation_->GetFailureAura(radius, magnitude);
    } else {
        activeSituation_->GetSuccessAura(radius, magnitude);
    }
    return true;
}

// @ 0x1006d8b8a: the same selector for the money reward.
bool cSC4CitySituationManager::GetSituationMoney(std::int64_t& money) const {
    if (!activeSituation_) return false;
    long long amount = 0;   // the situation API takes long long& (int64_t is long on Linux)
    if (status_ == kSituationTimeout) {
        activeSituation_->GetFailureMoney(amount);
    } else {
        activeSituation_->GetSuccessMoney(amount);
    }
    money = amount;
    return true;
}

// @ 0x1006e5dbe: targets - current index; both through the virtuals, like the original.
std::uint32_t cSC4CitySituationManager::GetNumTargetsRemaining() const {
    const std::uint32_t remaining = GetNumTargets() - GetCurrentTargetIndex();
    return remaining;   // the original answers an int; a negative count wraps the same way
}

// @ 0x1006e5e02
float cSC4CitySituationManager::GetCurrentTimeRemainingSeconds() const {
    return GetCurrentTimeLimitSeconds() - GetCurrentSituationTime();
}

// @ 0x1006e5e20: reads the low half of the tracked-automata set's size word (+0xa70).
std::uint32_t cSC4CitySituationManager::GetTrackedAutomataCount() const {
    return static_cast<std::uint32_t>(trackedAutomata_.size());
}

// @ 0x1006db10e: the file-wide byte (0x100b21a70), not a member.
std::uint8_t cSC4CitySituationManager::GetMaxCSI() const { return g_maxCSI; }

// @ 0x1006db0de: store the byte, then tell the dispatch view (indicator type 4) when there is one.
void cSC4CitySituationManager::SetMaxCSI(std::uint8_t max) {
    g_maxCSI = max;
    if (dispatchView_) dispatchView_->SetIndicatorMax(4, max);
}

// @ 0x1006db122
void cSC4CitySituationManager::SetVisible(bool visible) {
    csiVisible_ = visible;
    if (dispatchView_) dispatchView_->ShowAllIndicators(4, visible);
}

// @ 0x1006db14a: re-applies the (shared) maximum to the dispatch view and answers true.
bool cSC4CitySituationManager::HandleTuningParametersChanged() {
    SetMaxCSI(GetMaxCSI());
    return true;
}

// @ 0x1006da36c: forget every mission success, wipe the completed-mission ids of the preferences (0x100 bytes = 64
// u32 ids at preferences + 0x1334) and save the preferences again.
void cSC4CitySituationManager::ResetTutorials() {
    successCounts_.clear();
    ISituationApp* app = g_host ? g_host->App() : nullptr;
    if (!app) return;
    std::uint32_t* missionIds = app->CompletedMissionIds();
    if (!missionIds) return;
    std::memset(missionIds, 0, 0x100);
    app->SavePreferences();
}

// @ 0x1006dae70: stores the status and, when it moved to success/failure, recomputes the status speed factor from the
// situation's "delay after disabled" (at least 1.0; the flag 0x8000000 test only applies when an automaton is active).
void cSC4CitySituationManager::HandleStatusChanged(std::uint32_t status) {
    if (status_ == status) return;
    status_ = status;
    if (status - 1 >= 2 || !activeSituation_) return;
    float factor = 1.0f;
    if (activeAutomaton_ && activeAutomaton_->CheckFlag(0x8000000)) {
        factor = activeSituation_->GetDelayAfterDisabled();
        if (factor <= 1.0f) factor = 1.0f;
    }
    statusSpeedFactor_ = factor;
}

// ---------------------------------------------------------------------------------------------- state clears

// @ 0x1006d8946: stops every coverage cell effect (vslot 0x20 with "immediately" true) and drops the map.
void cSC4CitySituationManager::FreeCoverageCells() {
    for (auto& entry : coverageEffects_) {
        if (entry.second) entry.second->Stop(true);
    }
    coverageEffects_.clear();
}

// @ 0x1006d899a: releases the automata of the tracked set (the original walks the nodes releasing +0x10 of each).
void cSC4CitySituationManager::ClearTrackedAutomata() {
    for (ISituationAutomaton* automaton : trackedAutomata_) {
        if (automaton) automaton->Release();
    }
    trackedAutomata_.clear();
}

// @ 0x1006d89d8: releases every track's advice/automaton, tells the dispatch view to untrack its indicator and clears
// the list.
void cSC4CitySituationManager::ClearSituationTrackList() {
    for (SituationTrack& track : situationTracks_) {
        track.advice = nullptr;
        if (dispatchView_ && track.trackId != 0) dispatchView_->UntrackOccupant(track.trackId, false);
        track.automaton = nullptr;
        track.trackId = 0;
    }
    situationTracks_.clear();
}

// ---------------------------------------------------------------------------------------------- messages

// @ 0x1006d8bc0 (the cGZMessage view at +0x10): always false; the manager only handles cIGZMessage2 messages.
bool cSC4CitySituationManager::DoMessage2(cGZMessage& message) {
    (void)message;
    return false;
}

// @ 0x1006dab92: builds a standard message, posts it (message server vslot 0x20 with "no tick") and releases it.
void cSC4CitySituationManager::PostMessageToSelf(std::uint32_t type, cISC4CitySituationAdvice* situation,
                                                 ISituationAutomaton* automaton) {
    cIGZMessageServer2* server = g_host ? g_host->MessageServer() : nullptr;
    if (!server) return;
    cRZMessage2Standard* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    if (situation) message->SetIGZUnknown(situation);
    if (automaton) message->SetVoid1(automaton);
    server->MessagePost(message, false);
    message->Release();
}

// @ 0x1006dcf04: the synchronous variant (message server vslot 0x18 = MessageSend) carrying one u32 in data1.
void cSC4CitySituationManager::SendSituationMessage(std::uint32_t type, cISC4CitySituationAdvice* situation,
                                                    std::uint32_t data) {
    cIGZMessageServer2* server = g_host ? g_host->MessageServer() : nullptr;
    if (!server) return;
    cRZMessage2Standard* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    if (situation) message->SetIGZUnknown(situation);
    message->SetData1(data);
    server->MessageSend(message);
    message->Release();
}

// ---------------------------------------------------------------------------------------------- serialization

// @ 0x1006e59ba: version 2, the mission-success map (u32 id, u16 count) and the CSI visibility byte; answers
// !stream.GetError() (the original returns the error word XOR 1).
std::uint32_t cSC4CitySituationManager::Write(cIGZOStream& stream) const {
    stream.SetUint16(2);
    stream.SetUint32(static_cast<std::uint32_t>(successCounts_.size()));
    for (const auto& entry : successCounts_) {
        stream.SetUint32(entry.first);
        stream.SetUint16(entry.second);
    }
    operator<<(stream, csiVisible_);
    return stream.GetError() ? 0 : 1;
}

// @ 0x1006e5a4c: accepts versions 0..2 only. Below 3 the save predates the tutorial state, so the "loaded" byte is set;
// from version 2 on the visibility byte follows the map.
std::uint32_t cSC4CitySituationManager::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version >= 3) return 0;
    tutorialsLoaded_ = true;
    successCounts_.clear();
    std::uint32_t count = 0;
    stream.GetUint32(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t id = 0;
        std::uint16_t value = 0;
        stream.GetUint32(id);
        stream.GetUint16(value);
        successCounts_[id] = value;
    }
    if (version > 1) operator>>(stream, csiVisible_);
    return stream.GetError() ? 0 : 1;
}

// ---------------------------------------------------------------------------------------------- script bridge

// @ 0x1006e5774: resolves `name` in the Lua table at `tableIndex` and keeps a registry reference when it is a function.
void cSC4CitySituationManager::InitScriptFuncRef(const char* name, int tableIndex, int& ref) {
    if (!lua_) return;
    if (!lua_->GetField(name, tableIndex)) return;
    if (!lua_->IsFunction(-1)) return;
    ref = lua_->Ref(true);
}

// @ 0x1006dcfa2: calls registry function `functionRef` with the value of registry reference `argumentRef` (either may be
// -2 = none). MySim missions prepare the sim's script first (mysim flag 0x20).
void cSC4CitySituationManager::CallScriptFunction(int functionRef, int argumentRef) {
    if (argumentRef == -2 || !lua_) return;
    if (activeSituation_ && activeSituation_->CheckSituationFlag(kSituationFlagMySimMission) && mySim_) {
        mySim_->PrepareScript();
    }
    const int savedTop = lua_->GetTop();
    lua_->GetRef(argumentRef);
    if (lua_->IsFunction(-1)) {
        lua_->GetRef(functionRef);
        lua_->CallProtected(1, 0);
    }
    lua_->SetTop(savedTop);
}

// ---------------------------------------------------------------------------------------------- targets

// @ 0x1006db27c: collects the automata of every group from every registered manager (deduplicated by the shared set).
bool cSC4CitySituationManager::GetInstancesOfGroup(
        const std::vector<std::uint32_t>& groups,
        std::unordered_set<ISituationAutomaton*>& out) {
    for (std::uint32_t group : groups) {
        for (auto& manager : automataManagers_) {
            if (!manager) continue;
            if (manager->HandlesGroup(group)) manager->AppendInstancesOfGroup(group, out);
        }
    }
    return !out.empty();
}

// @ 0x1006db16a: the first automaton of the situation's automata groups that the player may drive and that is not
// already tracked. The temporary set is released through the vehicle manager (the original always calls it).
bool cSC4CitySituationManager::FindAutomatonForSituation(cISC4CitySituationAdvice* situation,
                                                         ISituationAutomaton** out) {
    *out = nullptr;
    if (!situation) return false;
    std::unordered_set<ISituationAutomaton*> instances;
    GetInstancesOfGroup(situation->GetAutomataGroups(), instances);
    for (ISituationAutomaton* automaton : instances) {
        if (!CanPlayerDrive(automaton)) continue;
        if (trackedAutomata_.count(automaton) != 0) continue;
        automaton->AddRef();
        *out = automaton;
        break;
    }
    if (ISituationAutomataManager* vehicles = g_host ? g_host->VehicleManager() : nullptr) {
        vehicles->ReleaseInstances(instances);
    }
    return *out != nullptr;
}

// @ 0x1006db35c: driveable means: not flag 2, not flag 1, not flag 0x8000000, not flag 0x10000000, and the automaton type
// is not a pursuit car ((type | 0x20) != 0x34).
bool cSC4CitySituationManager::CanPlayerDrive(ISituationAutomaton* automaton) const {
    if (!automaton) return false;
    if (!automaton->CheckFlag(2)) return false;
    if (automaton->CheckFlag(1)) return false;
    if (automaton->CheckFlag(0x8000000)) return false;
    if (automaton->CheckFlag(0x10000000)) return false;
    return (static_cast<std::uint32_t>(automaton->AutomatonType()) | 0x20u) != 0x34u;
}

// @ 0x1006db912: puts the automaton's occupant into `group` and tracks the automaton.
void cSC4CitySituationManager::TreatAutomatonAsGroup(ISituationAutomaton* automaton, std::uint32_t group) {
    if (!automaton) return;
    if (ISituationOccupant* occupant = automaton->Occupant()) occupant->SetGroup(group);
    HandleInsertAutomaton(automaton);
}

// @ 0x1006e2e08: the current target viewed as an automaton. The original QueryInterfaces without keeping the reference
// and releases the reference QueryInterface took, so the net refcount does not change.
ISituationAutomaton* cSC4CitySituationManager::GetTargetAsAutomaton() const {
    if (!currentTarget_) return nullptr;
    return currentTarget_->AsAutomaton();
}

// @ 0x1006e3c40: the active position: the flag-8 (service) situations take the mission actor's translation when the
// actor is valid and visible, everything else the active automaton's position.
bool cSC4CitySituationManager::GetActivePosition(float xyz[3]) {
    if (!activeSituation_) return false;
    if (activeSituation_->CheckSituationFlag(kSituationFlagServiceMission)) {
        if (!missionActor_ || !missionActor_->IsValid() || missionActor_->IsHidden()) return false;
        missionActor_->Position(xyz);
        return true;
    }
    if (!activeAutomaton_) return false;
    activeAutomaton_->Position(xyz);
    return true;
}

// @ 0x1006dd074: takes the situation off the advisor list (advisor vslot 0x38(0x1d) -> vslot 0xa0) and handles the
// untrigger.
bool cSC4CitySituationManager::RetireSituation(cISC4CitySituationAdvice* situation) {
    if (!situation) return false;
    if (g_host) g_host->RetireAdvisorSituation(situation);
    HandleSituationUntriggered(situation);
    return true;
}

}  // namespace sc4

namespace sc4 {

// ---------------------------------------------------------------------------------------------- Init / Shutdown

// The two raw u32 registry values ("num_targets", "cur_target_index") use the generic exchange function the original
// passes (nsISC4GameDataRegistry::GenericValueExchnageFuncUint32 at 0x1006e6628): reading hands the value out, writing
// stores the variant's u32.
namespace {

// The manager's messages are always cRZMessage2Standard objects; the original reaches the payload accessors through
// the object's own vtable, so the port downcasts (the caller guarantees the type, as the original does).
cIGZMessage2Standard& StandardMessage(cIGZMessage2* message) {
    return *static_cast<cIGZMessage2Standard*>(message);
}

bool ExchangeUint32(void* context, cIGZVariant& value, bool write, cISC4GameDataRegistry* registry) {
    (void)registry;
    if (context == nullptr) return false;
    std::uint32_t& slot = *static_cast<std::uint32_t*>(context);
    if (write) {
        slot = value.GetValUint32();
    } else {
        value.SetValUint32(slot);
    }
    return true;
}

}  // namespace

// @ 0x1006d7366: subscribes to the messages and the automaton notifications, collects the four automata managers, binds
// the lot/occupant managers and the script state, registers the "sitmgr" game-data node and seeds the mission-success
// map from the preferences' completed-mission array. Answers true (also when already initialised).
//
// Recovered-partial: the six script command entries of the game-data registry (get_success_count, activate_situation,
// get_current_target, get_active_auto, get_target_name, show_CSI, num_targets_remaining) go through
// cSC4AutomataScriptSystem::GetHexFromVariant, which is not recovered, so only the node and the two raw u32 values are
// registered here.
bool cSC4CitySituationManager::Init() {
    if (initialized_) return true;
    ICitySituationManagerHost* host = g_host;
    ISituationApp* app = host ? host->App() : nullptr;
    const bool debug = app && app->DebugFunctionalityEnabled();
    ISituationCheatCodes* cheats = host ? host->CheatCodes() : nullptr;
    if (debug && cheats) {
        cheats->AddNotification(this, 0);
        cheats->RegisterCheatCode(kCheatTriggerMission, kCheatTriggerMissionText);
    }

    // Every message the manager listens to (the original registers each id with the message server, vslot 0x28).
    if (cIGZMessageServer2* server = host ? host->MessageServer() : nullptr) {
        static const std::uint32_t kMessages[] = {0x99ef1142, 0x99ef1143, 0x6bb350e4, 0xebb67ef7, 0x8a09fa64,
                                                  0x8a09fa65, 0x8a09fa67, 0xcc0d905f, 0xac1045ef, 0x26d31ec1};
        for (std::uint32_t type : kMessages) server->AddNotification(this, type);
    }

    if (host) dispatchView_ = host->DispatchView();
    if (ISituationAutomatonNotifier* notifier = host ? host->AutomataNotifier() : nullptr) {
        notifier->AddNotification(this, 0xff);
    }
    if (host) {
        const int managerCount = host->AutomataManagerCount();
        for (int i = 0; i < managerCount; ++i) {
            if (ISituationAutomataManager* manager = host->AutomataManager(i)) automataManagers_.emplace_back(manager);
        }
        lotManager_ = host->LotManager();
        occupantManager_ = host->OccupantManager();      // SL::spCity vslot 0x238
        mySim_ = nullptr;
        lua_ = host->Lua();
        if (sc4::cISC4GameDataRegistry* registry = host->GameDataRegistry()) {
            // nsISC4GameDataRegistry::AddNode with a null parent hangs the node under the root; the values hang under it.
            const auto node = registry->AddChildEntry(static_cast<cIGZUnknown*>(this), "sitmgr", registry->GetRootNodeEntry(), 0, nullptr);
            registry->AddChildEntry(&numTargets_, "num_targets", &ExchangeUint32, node, 2, nullptr);
            registry->AddChildEntry(&currentTargetIndex_, "cur_target_index", &ExchangeUint32, node, 2, nullptr);
        }
        if (ISituationMySim* mySim = host->MySimActor() ? nullptr : nullptr) mySim_ = mySim;   // (no MySim source yet)
    }

    HandleTuningParametersChanged();
    if (dispatchView_) dispatchView_->ShowAllIndicators(4, csiVisible_);

    // Situations listed as completed in the preferences count as succeeded (only when the first id is non-zero).
    if (app) {
        if (std::uint32_t* ids = app->CompletedMissionIds()) {
            if (ids[0] != 0) {
                for (std::uint32_t* id = ids; *id != 0; ++id) successCounts_[*id] = 1;
            }
        }
    }
    initialized_ = true;
    return true;
}

// @ 0x1006d85c6: releases every service reference, frees the coverage cells, drops the automata managers, unsubscribes
// from the message server and the automaton notifier, clears the tracking lists, removes the game-data node and
// unregisters the cheat code.
//
// Recovered-partial: the original also calls vslot 0x160 (DeactivateSituation) here, which is not recovered.
bool cSC4CitySituationManager::Shutdown() {
    if (!initialized_) return true;
    mySim_ = nullptr;
    occupantManager_ = nullptr;
    lua_ = nullptr;
    lotManager_ = nullptr;
    missionActor_ = nullptr;
    popupDialog_ = nullptr;
    countdownEffect_ = nullptr;
    FreeCoverageCells();
    automataManagers_.clear();
    if (ICitySituationManagerHost* host = g_host) {
        if (ISituationAutomatonNotifier* notifier = host->AutomataNotifier()) notifier->RemoveNotification(this, 0xff);
    }
    dispatchView_ = nullptr;
    if (cIGZMessageServer2* server = g_host ? g_host->MessageServer() : nullptr) {
        static const std::uint32_t kMessages[] = {0x99ef1142, 0x99ef1143, 0x6bb350e4, 0xebb67ef7, 0x8a09fa64,
                                                  0x8a09fa65, 0x8a09fa67, 0xcc0d905f, 0xac1045ef, 0x26d31ec1};
        for (std::uint32_t type : kMessages) server->RemoveNotification(this, type);
    }
    ClearTrackedAutomata();
    ClearSituationTrackList();
    if (sc4::cISC4GameDataRegistry* registry = g_host ? g_host->GameDataRegistry() : nullptr) {
        registry->RemoveChildEntry(static_cast<cIGZUnknown*>(this));
    }
    ISituationApp* app = g_host ? g_host->App() : nullptr;
    ISituationCheatCodes* cheats = g_host ? g_host->CheatCodes() : nullptr;
    if (app && app->DebugFunctionalityEnabled() && cheats) {
        cheats->RemoveNotification(this, 0);
        cheats->UnregisterCheatCode(kCheatTriggerMission);
    }
    initialized_ = false;
    return true;
}

// ---------------------------------------------------------------------------------------------- automaton tracking

// @ 0x1006db4ec: the one place that adds/removes an automaton from the tracked set and asks the dispatch view for (or
// returns) its indicator. Quirk kept: tracking an automaton that is already tracked still asks the view for a new
// indicator before the early return, so the previous indicator id is lost.
void cSC4CitySituationManager::TrackAutomaton(ISituationAutomaton* automaton, std::uint32_t& trackId, bool track) {
    if (!dispatchView_ || !automaton) return;
    if (!track) {
        if (trackId == 0) return;
        dispatchView_->UntrackOccupant(trackId, false);
        trackId = 0;
        if (trackedAutomata_.erase(automaton) == 0) return;
        automaton->RemoveListener(this);
        automaton->Release();
        return;
    }
    const int indicator = automaton->FirstIndicator();
    if (indicator == 0) return;
    trackId = dispatchView_->TrackOccupant(automaton->Occupant(), 4, indicator, 0);
    if (trackedAutomata_.count(automaton) != 0) return;
    trackedAutomata_.insert(automaton);
    automaton->AddListener(this);
    automaton->AddRef();
}

// @ 0x1006db962: gives a driveable automaton the first free situation track whose advice is relevant to it, or moves it
// into the track of its situation when that track sorts earlier. The scan stops after GetMaxCSI() occupied tracks.
void cSC4CitySituationManager::HandleInsertAutomaton(ISituationAutomaton* automaton) {
    if (!CanPlayerDrive(automaton)) return;
    const bool alreadyTracked = trackedAutomata_.count(automaton) != 0;
    const std::uint32_t maxTracks = GetMaxCSI();
    std::uint32_t occupied = 0;
    for (auto it = situationTracks_.begin(); it != situationTracks_.end(); ++it) {
        if (it->automaton == automaton) return;
        if (it->automaton) {
            if (maxTracks <= ++occupied) return;
            continue;
        }
        if (!it->advice) continue;
        if (!it->advice->IsAutomatonRelevant(automaton)) continue;
        if (alreadyTracked) {
            // The automaton already has a track: move its indicator into this slot when this slot's situation sorts first.
            for (auto later = std::next(it); later != situationTracks_.end(); ++later) {
                if (later->automaton != automaton) continue;
                if (!(*later < *it)) continue;
                it->automaton = automaton;
                it->trackId = later->trackId;
                later->automaton = nullptr;
                later->trackId = 0;
                return;
            }
            return;
        }
        TrackAutomaton(automaton, it->trackId, true);
        it->automaton = automaton;
        return;
    }
}

// @ 0x1006dbade: the automaton disappeared: return its indicator, clear its track and let that track's situation look
// for a replacement automaton.
void cSC4CitySituationManager::HandleRemoveAutomaton(ISituationAutomaton* automaton) {
    if (!automaton) return;
    if (trackedAutomata_.count(automaton) == 0) return;
    for (SituationTrack& track : situationTracks_) {
        if (track.automaton != automaton) continue;
        TrackAutomaton(automaton, track.trackId, false);
        track.automaton = nullptr;
        if (track.advice) HandleSituationTriggered(track.advice);
        return;
    }
}

// ---------------------------------------------------------------------------------------------- occupant routing

// @ 0x1006d9c1c: an occupant that may not be a situation automaton (property 0x029244db) and that answers
// QueryInterface(0xa9b40f05) is fed to HandleInsertAutomaton as the automaton it is.
void cSC4CitySituationManager::HandleInsertOccupant(ISituationOccupant* occupant) {
    if (!occupant) return;
    if (ISituationPropertyHolder* holder = occupant->PropertyHolder()) {
        if (holder->HasProperty(kPropertyNoSituationAutomaton)) return;
    }
    if (ISituationAutomaton* automaton = occupant->AsAutomaton()) HandleInsertAutomaton(automaton);
}

// ------------------------------------------------------------------------------- situation tracks (trigger/untrigger)

// @ 0x1006d9318: adds a track for the situation (sorted by advice priority), then either gives it the first driveable
// automaton of its groups, or moves an already-tracked relevant automaton into it when that automaton's current track
// sorts later. New automata are only taken while fewer than GetMaxCSI() automata are tracked.
void cSC4CitySituationManager::HandleSituationTriggered(cISC4CitySituationAdvice* situation) {
    if (!situation) return;
    auto findTrack = [this](const cISC4CitySituationAdvice* advice) {
        return std::find_if(situationTracks_.begin(), situationTracks_.end(),
                            [advice](const SituationTrack& track) { return track.advice.get() == advice; });
    };
    auto it = findTrack(situation);
    if (it == situationTracks_.end()) {
        situationTracks_.push_back(SituationTrack(situation));
        situationTracks_.sort();
        it = findTrack(situation);
    }
    if (it == situationTracks_.end()) return;
    if (it->automaton) return;

    ISituationAutomaton* found = nullptr;
    FindAutomatonForSituation(situation, &found);
    if (!found) {
        // Re-use an automaton that is already tracked and relevant for this situation.
        for (ISituationAutomaton* automaton : trackedAutomata_) {
            if (!situation->IsAutomatonRelevant(automaton)) continue;
            for (auto later = std::next(it); later != situationTracks_.end(); ++later) {
                if (later->automaton != automaton) continue;
                if (!(*later < *it)) continue;
                it->automaton = automaton;
                it->trackId = later->trackId;
                later->automaton = nullptr;
                later->trackId = 0;
                return;
            }
        }
        return;
    }
    if (trackedAutomata_.size() >= GetMaxCSI()) {
        // No indicator budget left for a new automaton: take over one from a later, lower-priority track instead.
        for (auto later = std::next(it); later != situationTracks_.end(); ++later) {
            if (!later->automaton) continue;
            if (!(*later < *it)) continue;
            it->automaton = later->automaton;
            it->trackId = later->trackId;
            later->automaton = nullptr;
            later->trackId = 0;
            break;
        }
        found->Release();
        return;
    }
    TrackAutomaton(found, it->trackId, true);
    it->automaton = found;
    found->Release();
}

// @ 0x1006d96c4: removes the situation's track (returning its automaton) and then tries to spend the freed indicators on
// the situations that are still waiting — the original looks up automata for the UNTRIGGERED situation here.
void cSC4CitySituationManager::HandleSituationUntriggered(cISC4CitySituationAdvice* situation) {
    if (!situation) return;
    auto it = std::find_if(situationTracks_.begin(), situationTracks_.end(),
                           [situation](const SituationTrack& track) { return track.advice.get() == situation; });
    if (it != situationTracks_.end()) {
        if (it->automaton) {
            TrackAutomaton(it->automaton, it->trackId, false);
            it->automaton = nullptr;
        }
        it->advice = nullptr;
        situationTracks_.erase(it);
    }
    std::size_t tracked = trackedAutomata_.size();
    const std::uint32_t maxTracks = GetMaxCSI();
    for (SituationTrack& track : situationTracks_) {
        if (tracked >= maxTracks) break;
        if (track.advice.get() == situation) continue;
        if (track.automaton) continue;
        ISituationAutomaton* found = nullptr;
        FindAutomatonForSituation(situation, &found);
        if (!found) continue;
        TrackAutomaton(found, track.trackId, true);
        track.automaton = found;
        ++tracked;
        found->Release();
    }
}

// ---------------------------------------------------------------------------------------------- message routing

// @ 0x1006d8bd0: routes the notifications the manager subscribed to. The original's switch is a signed compare tree;
// the ids are listed in SituationMessage (sc4/CitySituationManager.h). Answers true for a handled message.
//
// Recovered-partial: the routes whose handlers are not recovered answer false (they are the target selection core
// 0x9d92fc3/0xbb324bd, the situation start 0xac1045ef/0x6bb350e4, the popup 0x8a09fa66/0x2c16bde7/0xebb67ef7 and the
// per-tick update 0x86ad10ee).
bool cSC4CitySituationManager::DoMessage(cIGZMessage2* message) {
    if (!message) return false;
    switch (message->GetType()) {
        case kSituationMsgAdviceTriggered:
        case kSituationMsgAdviceUntriggered: {
            cIGZUnknown* unknown = StandardMessage(message).GetIGZUnknown();
            if (!unknown) return true;
            cISC4CitySituationAdvice* situation = nullptr;
            if (!unknown->QueryInterface(kGZIID_cISC4CitySituationAdvice, reinterpret_cast<void**>(&situation)) || !situation) {
                return true;
            }
            if (message->GetType() == kSituationMsgAdviceTriggered) {
                HandleSituationTriggered(situation);
            } else {
                HandleSituationUntriggered(situation);
            }
            situation->Release();
            return true;
        }
        case kSituationMsgAdviceShown:
            return true;                     // one-shot notification, nothing to do
        case kSituationMsgMissionActorSet: {
            // Only service missions (flag 0x8) keep a mission actor.
            if (!activeSituation_ || !activeSituation_->CheckSituationFlag(kSituationFlagServiceMission)) return true;
            cIGZUnknown* unknown = StandardMessage(message).GetIGZUnknown();
            if (!unknown) return true;
            if (missionActor_.get() == unknown) return true;
            unknown->AddRef();
            missionActor_ = static_cast<ISituationMissionActor*>(unknown);
            return true;
        }
        case kSituationMsgOccupantInserted:
            if (g_host) HandleInsertOccupant(g_host->OccupantOfMessage(StandardMessage(message).GetVoid1()));
            return true;
        case kSituationMsgAdviceEnabled: {
            // Re-trigger every city-situation advice (only after a load from an old save and with an advisor system).
            if (!tutorialsLoaded_ || !g_host) return true;
            for (cISC4CitySituationAdvice* situation : g_host->AdvisorSituations()) HandleSituationTriggered(situation);
            return true;
        }
        case kSituationMsgAutomatonStatus: {
            // Status 2 = the active automaton got stuck: it fails unless the situation is already in a final state.
            ISituationAutomaton* automaton = g_host ? g_host->AutomatonOfMessage(StandardMessage(message).GetVoid1()) : nullptr;
            const std::uint32_t code = static_cast<std::uint32_t>(StandardMessage(message).GetData2());
            if (code == 2 && activeAutomaton_ && activeAutomaton_.get() == automaton) {
                const std::uint32_t status = (activeSituation_ && status_ != kSituationRunning) ? kSituationTimeout : status_;
                HandleStatusChanged(status);
            }
            return true;
        }
        case kSituationMsgCheatTyped: {
            // "cISC4CitySituationManager <hex advice id>" activates that situation.
            if (static_cast<std::uint32_t>(StandardMessage(message).GetData1()) != kCheatTriggerMission) return false;
            auto* text = static_cast<cIGZString*>(StandardMessage(message).GetVoid2());
            if (!text) return true;
            cRZCmdLine commandLine(text->ToChar());
            if (commandLine.argc() < 2) return true;
            const std::uint32_t adviceId = static_cast<std::uint32_t>(std::strtoul(commandLine[1].ToChar(), nullptr, 16));
            if (adviceId == 0) return true;
            cIGZMessageServer2* server = g_host ? g_host->MessageServer() : nullptr;
            if (!server) return true;
            cRZMessage2Standard* activation = new cRZMessage2Standard();
            activation->AddRef();
            activation->SetType(kSituationMsgActivateAdvice);
            activation->SetData1(adviceId);
            activation->SetData2(1);
            server->MessagePost(activation, false);
            activation->Release();
            return true;
        }
        default:
            return false;
    }
}

}  // namespace sc4
