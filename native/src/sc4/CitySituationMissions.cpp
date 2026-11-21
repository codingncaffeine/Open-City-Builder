// cSC4CitySituationManager — the mission flow: the message routing, starting a situation (InitiateSituation* /
// SetActiveAutomaton / the script references), its popup and the popup's window procedure, the drive (ActivateSituation /
// StartJoyride / InitiatePlayerDriveMode), the target choice, the per-tick checks and the result handlers. The i386
// addresses of the Mac build are given per function; the field offsets in the comments are the 64-bit ones of the header.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "gz/WinGen.h"
#include "gz/WinProc.h"
#include "rz/cRZCmdLine.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZVariant.h"
#include "s3d/S3DMath.h"
#include "sc4/CitySituationManager.h"
#include "sc4/GeometryUtilities.h"

namespace sc4 {

namespace {

constexpr std::uint32_t kCheatTriggerMission = 0x8c4136db;
constexpr std::uint32_t kPlayerDriveCreationFlags = 0x2010002;   // CreateAutomatonForPlayerDrive / CreateTarget
constexpr float kPlayerDriveCreationRadius = 64.0f;              // 0x42800000: the distance from the source building
constexpr float kCountdownMilliseconds = 4000.0f;                // 0x457a0000
constexpr float kTimeWarningSeconds = 30.0f;                     // the warning sound plays 30 seconds before the limit
constexpr std::uint32_t kTimeWarningSound = 0x6c32871f;
constexpr std::uint32_t kNextTargetSound = 0x2c22d924;
constexpr float kDefaultTimeLimitSeconds = 60.0f;                // GetTimeLimitSecondsFromScript without a script answer
constexpr float kDefaultPlayerDriveSpeed = 22.2222252f;          // without player-drive parameters
constexpr float kPlayerDriveSpeedScale = 1000.0f;
constexpr float kArrowHeight = 10.0f;                            // the green arrow floats 10 m above the target
constexpr float kArrowTurn = 4.712389f;                          // RotateY(3 pi / 2)
constexpr std::uint32_t kOccupantGroupMySimResidence = 0x193a;
constexpr std::uint32_t kOccupantGroupLandfill = 0x1406;
constexpr std::uint32_t kOccupantGroupTargetVehicle1 = 0x5005;   // HandleTargetRemoved: a vehicle target is looked up again
constexpr std::uint32_t kOccupantGroupTargetVehicle2 = 0x5006;
constexpr std::uint32_t kDashboardProperty = 0x2be8fbf3;
constexpr std::uint32_t kOccupantTypeAutomaton = 0x480e6151;
constexpr std::uint32_t kDefaultDashboardAutomaton = 0x6be8846c;   // no dashboard property: automaton occupants
constexpr std::uint32_t kDefaultDashboardOther = 0x0be8824f;       // ... and everything else

// The script function names (InitScriptRefs) and the effect names.
constexpr const char* kOnSuccessFunc = "on_success";
constexpr const char* kOnFailureFunc = "on_failure";
constexpr const char* kCheckSuccessFunc = "check_success";
constexpr const char* kFindTargetFunc = "find_target";
constexpr const char* kCreateTargetFunc = "create_target";
constexpr const char* kInitFunc = "init";
constexpr const char* kGetTimeLimitFunc = "get_time_limit";
constexpr const char* kTargetHighlightEffect = "mission_selection_red";
constexpr const char* kGreenArrowEffect = "greenarrow";
constexpr const char* kCountdownEffect = "count_down";

// The indices of the seven script references (+0xbcc..+0xbe4).
enum ScriptRef { kRefOnSuccess, kRefOnFailure, kRefCheckSuccess, kRefFindTarget, kRefCreateTarget, kRefInit, kRefGetTimeLimit };

cIGZMessage2Standard& StandardMessage(cIGZMessage2* message) { return *static_cast<cIGZMessage2Standard*>(message); }

// The situation advice behind a message's cIGZUnknown (QueryInterface 0x6beb1e8a); a counted reference or null.
cISC4CitySituationAdvice* SituationOfMessage(cIGZMessage2* message) {
    cIGZUnknown* unknown = StandardMessage(message).GetIGZUnknown();
    cISC4CitySituationAdvice* situation = nullptr;
    if (!unknown || !unknown->QueryInterface(kGZIID_cISC4CitySituationAdvice, reinterpret_cast<void**>(&situation))) return nullptr;
    return situation;
}

// cS3DMatrix3::RotateY: this = this * R.
void RotateY(float m[9], float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    for (int row = 0; row < 3; ++row) {
        const float x = m[row * 3 + 0], z = m[row * 3 + 2];
        m[row * 3 + 0] = x * c - z * s;
        m[row * 3 + 2] = x * s + z * c;
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------- message routing

// @ 0x1006d8bd0 (i386 0x004a870a): the original compares the type in a tree; every listed message answers true, anything
// else false.
bool cSC4CitySituationManager::DoMessage(cIGZMessage2* message) {
    if (!message) return false;
    ICitySituationManagerHost* host = CitySituationManagerHost();
    cIGZMessage2Standard& standard = StandardMessage(message);
    switch (message->GetType()) {
        case kSituationMsgAdviceTriggered:
        case kSituationMsgAdviceUntriggered:
        case kSituationMsgSituationComplete: {
            cISC4CitySituationAdvice* situation = SituationOfMessage(message);
            if (situation) {
                if (message->GetType() == kSituationMsgAdviceTriggered) {
                    HandleSituationTriggered(situation);
                } else if (message->GetType() == kSituationMsgAdviceUntriggered) {
                    HandleSituationUntriggered(situation);
                } else {
                    HandleSituationComplete(situation);
                }
                situation->Release();
            }
            return true;
        }
        case kSituationMsgAdviceShown:
            return true;
        case kSituationMsgAdviceEnabled:
            // Re-trigger every city-situation advice (only after a load from an old save and with an advisor system).
            if (!tutorialsLoaded_ || !host) return true;
            for (cISC4CitySituationAdvice* situation : host->AdvisorSituations()) HandleSituationTriggered(situation);
            return true;
        case kSituationMsgTimeUp:
            if (!activeSituation_) return true;
            if (status_ - 1u < 2u) return true;   // success / failure keep their result dialog
            EndSituation();
            return true;
        case kSituationMsgCheatTyped: {
            // "cISC4CitySituationManager <hex advice id>": posts 0xac1045ef (guid, 1, 0), i.e. a situation for an occupant.
            if (static_cast<std::uint32_t>(standard.GetData1()) != kCheatTriggerMission) return true;
            auto* text = static_cast<cIGZString*>(standard.GetVoid2());
            if (!text) return true;
            cRZCmdLine commandLine(text->ToChar());
            if (commandLine.argc() < 2) return true;
            const std::uint32_t adviceId = static_cast<std::uint32_t>(std::strtoul(commandLine[1].ToChar(), nullptr, 16));
            if (adviceId == 0) return true;
            cIGZMessageServer2* server = host ? host->MessageServer() : nullptr;
            if (!server) return true;
            cRZMessage2Standard* activation = new cRZMessage2Standard();
            activation->AddRef();
            activation->SetType(kSituationMsgActivateAdvice);
            activation->SetData1(adviceId);
            activation->SetData2(1);
            activation->SetData3(0);
            server->MessagePost(activation, false);
            activation->Release();
            return true;
        }
        case kSituationMsgAutomatonStatus: {
            ISituationAutomaton* automaton = host ? host->AutomatonOfMessage(standard.GetVoid1()) : nullptr;
            const std::uint32_t event = static_cast<std::uint32_t>(standard.GetData2());
            if (event == 9) {
                HandleAutomatonMoved(automaton);
            } else if (event == 11 || (event == 12 && static_cast<std::uint32_t>(standard.GetData3()) == 0x14)) {
                HandleAutomatonDisabled(automaton);
            } else if (event == 2 && automaton && automaton == activeAutomaton_.get()) {
                HandleActiveAutoRemoved();
            }
            return true;
        }
        case kSituationMsgAnimationTick: {
            // data3 carries the bits of the elapsed milliseconds as a float (the animation tick manager stores them so).
            const std::uint32_t bits = static_cast<std::uint32_t>(standard.GetData3());
            float milliseconds = 0.0f;
            std::memcpy(&milliseconds, &bits, sizeof(milliseconds));
            DoAnimationTick(milliseconds);
            return true;
        }
        case kSituationMsgPopupWithoutSituation:
            if (ISituationView3D* view = host ? host->View3D() : nullptr) view->HideMissionStatusPanel();
            if (popupDialog_) EndDialog(popupDialog_.get());
            ShowSituationPopup(nullptr, nullptr);
            return true;
        case kSituationMsgOccupantRemoved:
            if (host) HandleRemoveOccupant(host->OccupantOfMessage(standard.GetVoid1()));
            return true;
        case kSituationMsgOccupantInserted:
            if (host) HandleInsertOccupant(host->OccupantOfMessage(standard.GetVoid1()));
            return true;
        case kSituationMsgMissionActorSet: {
            // Only service missions (flag 0x8) keep a mission actor.
            if (!activeSituation_ || !activeSituation_->CheckSituationFlag(kSituationFlagServiceMission)) return true;
            cIGZUnknown* unknown = standard.GetIGZUnknown();
            if (!unknown) return true;
            missionActor_ = static_cast<ISituationMissionActor*>(unknown);
            return true;
        }
        case kSituationMsgShowPopup: {
            // void1 is the automaton the manager posted itself (PostMessageToSelf), read only when data1 was set.
            cISC4CitySituationAdvice* situation = SituationOfMessage(message);
            auto* automaton = standard.GetHasData1() ? static_cast<ISituationAutomaton*>(standard.GetVoid1()) : nullptr;
            if (ISituationView3D* view = host ? host->View3D() : nullptr) view->HideMissionStatusPanel();
            if (popupDialog_) EndDialog(popupDialog_.get());
            ShowSituationPopup(situation, automaton);
            if (situation) situation->Release();
            return true;
        }
        case kSituationMsgActivateAdvice:
            InitiateSituationByGuid(static_cast<std::uint32_t>(standard.GetData1()), standard.GetData2() != 0,
                                    static_cast<std::uint32_t>(standard.GetData3()));
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------------------------- starting a situation

// i386 0x004a3f32: the "city situations" advisor's advice `guid`; the automata of its groups (or of `automataGroup`)
// are tried in turn (driveable ones only), else a new automaton is made near a source building. With `forOccupant` the
// situation goes through InitiateSituationForOccupant (popup first); without, it starts and activates at once.
bool cSC4CitySituationManager::InitiateSituationByGuid(std::uint32_t guid, bool forOccupant, std::uint32_t automataGroup) {
    ICitySituationManagerHost* host = CitySituationManagerHost();
    cRZAutoRefCount<cISC4CitySituationAdvice> situation;
    if (cISC4CitySituationAdvice* found = host ? host->AdvisorSituationByGuid(guid) : nullptr) {
        situation = found;
        found->Release();
    }
    if (!situation) return false;
    std::vector<std::uint32_t> groups;
    if (automataGroup == 0) {
        groups = situation->GetAutomataGroups();
    } else {
        groups.push_back(automataGroup);
    }
    auto start = [&](ISituationAutomaton* automaton) {
        if (forOccupant) return InitiateSituationForOccupant(automaton->Occupant(), situation.get());
        if (!InitiateSituation(situation.get(), automaton, false)) return false;
        ActivateSituation();
        return true;
    };
    std::unordered_set<ISituationAutomaton*> instances;
    bool started = false;
    if (GetInstancesOfGroup(groups, instances)) {
        for (ISituationAutomaton* automaton : instances) {
            if (!CanPlayerDrive(automaton)) continue;
            if (start(automaton)) {
                started = true;
                break;
            }
        }
        // The original releases the set through the first automata manager (+0xa28[0], vslot 0x108).
        if (!automataManagers_.empty() && automataManagers_[0]) automataManagers_[0]->ReleaseInstances(instances);
        if (started) return true;
    }
    ISituationAutomaton* created = CreateAutomatonForPlayerDrive(groups);
    if (!created) {
        InitiateSituation(situation.get(), nullptr, true);   // reports "not available" through the popup
        return false;
    }
    created->AddRef();
    started = start(created);
    if (!started) created->Remove(false);   // the new automaton is not used: it goes again
    created->Release();
    return started;
}

// i386 0x004a4278: the occupant must be an automaton; without a situation the latest track whose automaton is this one
// (or whose situation finds it relevant) gives it. The situation starts with its popup.
bool cSC4CitySituationManager::InitiateSituationForOccupant(ISituationOccupant* occupant, cISC4CitySituationAdvice* situation) {
    ISituationAutomaton* automaton = occupant ? occupant->AsAutomaton() : nullptr;
    if (!automaton) return false;
    cRZAutoRefCount<ISituationAutomaton> hold(automaton);
    if (!situation) {
        for (auto it = situationTracks_.rbegin(); it != situationTracks_.rend(); ++it) {
            if (!it->advice) continue;
            if (it->automaton.get() != automaton && !it->advice->IsAutomatonRelevant(automaton)) continue;
            situation = it->advice.get();
            break;
        }
        if (!situation) return false;
    }
    return InitiateSituation(situation, automaton, true);
}

// i386 0x004a92cc: makes `situation` the active one (ending the previous one), binds the automaton and the script
// references, sets up the targets (or the coverage cells) and posts the popup: with the automaton when `showPopup`
// and the setup worked, else as "not available" (status 5).
bool cSC4CitySituationManager::InitiateSituation(cISC4CitySituationAdvice* situation, ISituationAutomaton* automaton, bool showPopup) {
    if (activeSituation_.get() == situation) return false;
    DeactivateSituation();
    activeSituation_ = situation;
    if (!activeSituation_) return false;
    status_ = kSituationNone;
    resultDelay_ = 0.0f;
    missionSoundPlayed_ = false;
    // Recovered-partial: a MySim mission (flag 0x20) binds the sim the occupant manager's vslot 0x30 hands out; the
    // port has no MySim source for the situations (mySim_ stays null).
    SetActiveAutomaton(automaton);
    bool result = false;
    bool setUp = false;
    if (activeAutomaton_) {
        InitScriptRefs(activeSituation_.get());
        const std::uint32_t condition = activeSituation_->GetSuccessCondition();
        if (condition == 2) {
            result = setUp = true;
        } else if (condition == 5) {
            // Recovered-partial: InitCoverageCells (the zone / network / lot coverage cells) is not recovered; a coverage
            // mission is reported as not available.
            result = setUp = false;
        } else {
            numTargets_ = 1;
            if (activeSituation_->HasTargetSequence()) numTargets_ = static_cast<std::uint32_t>(activeSituation_->GetTargetSequence().size());
            currentTargetIndex_ = 0;
            const std::uint32_t flags = activeSituation_->GetSituationFlags();
            if ((flags & kSituationFlagAlwaysCreateTarget) != 0 ||
                (FindTarget(true) == nullptr && (activeSituation_->GetSituationFlags() & kSituationFlagCreateTarget) != 0)) {
                CreateTarget(true);
            }
            result = setUp = currentTarget_ != nullptr;
        }
    }
    if (setUp && showPopup) {
        PostMessageToSelf(kSituationMsgShowPopup, activeSituation_.get(), activeAutomaton_.get());
        return true;
    }
    status_ = kSituationUnavailable;
    PostMessageToSelf(kSituationMsgShowPopup, activeSituation_.get(), nullptr);
    return result;
}

// i386 0x0049e3aa: the previous automaton loses the manager's callback; the new one gets flag 0x4000, the callback and
// its game-data registry node.
void cSC4CitySituationManager::SetActiveAutomaton(ISituationAutomaton* automaton) {
    if (activeAutomaton_.get() == automaton) return;
    if (activeAutomaton_) activeAutomaton_->RemoveListener(this);
    activeAutomaton_ = automaton;
    if (!activeAutomaton_) return;
    activeAutomaton_->SetFlag(0x4000, true);
    activeAutomaton_->AddListener(this);
    activeAutomaton_->RegisterInGameDataRegistry();
}

// i386 0x0049e62c
void cSC4CitySituationManager::ShutdownScriptRef(int& ref) {
    if (lua_ && ref != -2) lua_->Unref(ref);
    ref = -2;
}

// i386 0x0049e666: without a situation every reference is dropped; with one, the seven functions of its script table are
// referenced, and "get_time_limit" falls back to the one of the global base_citysituation_advice table.
void cSC4CitySituationManager::InitScriptRefs(cISC4CitySituationAdvice* situation) {
    if (!situation) {
        for (int& ref : scriptRefs_) ShutdownScriptRef(ref);
        return;
    }
    if (!lua_) return;
    const int savedTop = lua_->GetTop();
    lua_->GetRef(situation->GetScriptTableRef());
    const int table = lua_->GetTop();
    if (lua_->IsTable(table)) {
        InitScriptFuncRef(kOnSuccessFunc, table, scriptRefs_[kRefOnSuccess]);
        InitScriptFuncRef(kOnFailureFunc, table, scriptRefs_[kRefOnFailure]);
        InitScriptFuncRef(kCheckSuccessFunc, table, scriptRefs_[kRefCheckSuccess]);
        InitScriptFuncRef(kFindTargetFunc, table, scriptRefs_[kRefFindTarget]);
        InitScriptFuncRef(kCreateTargetFunc, table, scriptRefs_[kRefCreateTarget]);
        InitScriptFuncRef(kInitFunc, table, scriptRefs_[kRefInit]);
        InitScriptFuncRef(kGetTimeLimitFunc, table, scriptRefs_[kRefGetTimeLimit]);
        if (scriptRefs_[kRefGetTimeLimit] == -2 && lua_->GetGlobalTable("base_citysituation_advice")) {
            const int base = lua_->GetTop();
            if (lua_->GetField(kGetTimeLimitFunc, base)) {
                if (lua_->IsFunction(-1)) {
                    scriptRefs_[kRefGetTimeLimit] = lua_->Ref(true);
                } else {
                    lua_->Pop(1);
                }
            }
            lua_->Pop(1);
        }
    }
    lua_->SetTop(savedTop);
}

// i386 0x004a6092: drops the coverage cells, the automaton and the target, resets the status, the script references and
// the flags, tells the listeners the situation ended (0xabbbca64, 0) and forgets the situation.
void cSC4CitySituationManager::DeactivateSituation() {
    if (!activeSituation_) return;
    FreeCoverageCells();
    SetActiveAutomaton(nullptr);
    SetTarget(nullptr);
    targetOccupants_.clear();
    status_ = kSituationNone;
    numTargets_ = 0;
    InitScriptRefs(nullptr);
    SendSituationMessage(kSituationMsgSituationActive, activeSituation_.get(), 0);
    startCountdown_ = false;
    activeSituation_ = nullptr;
    targetSequenceReset_ = false;
    mySim_ = nullptr;
}

// i386 0x004a67a4: drops the mission actor, the time limit and the countdown effect, returns the active automaton's
// indicator, deactivates the situation and shows the indicators again.
void cSC4CitySituationManager::EndSituation() {
    missionActor_ = nullptr;
    timeLimit_ = 0.0f;
    if (countdownEffect_) {
        countdownEffect_->Stop(true);
        countdownEffect_ = nullptr;
    }
    if (activeSituation_) {
        if (activeAutomaton_) {
            auto it = std::find_if(situationTracks_.begin(), situationTracks_.end(),
                                   [this](const SituationTrack& track) { return track.advice.get() == activeSituation_.get(); });
            if (it != situationTracks_.end()) {
                TrackAutomaton(activeAutomaton_.get(), it->trackId, false);
                it->automaton = nullptr;
            }
        }
        DeactivateSituation();
    }
    if (dispatchView_) dispatchView_->ShowAllIndicators(4, csiVisible_);
}

// i386 0x0049e366: leaves the drive, then ends the situation when it is the active one.
void cSC4CitySituationManager::HandleSituationComplete(cISC4CitySituationAdvice* situation) {
    InitiatePlayerDriveMode(false);
    if (activeSituation_.get() == situation) EndSituation();
}

// i386 0x0049ed2a / 0x0049ed5e
std::uint32_t cSC4CitySituationManager::HandleTimeLimitReached() {
    const std::uint32_t status = activeSituation_ ? static_cast<std::uint32_t>(kSituationFailure) : status_;
    HandleStatusChanged(status);
    return status;
}
std::uint32_t cSC4CitySituationManager::HandleActiveAutoRemoved() {
    const std::uint32_t status = (activeSituation_ && status_ == kSituationRunning) ? static_cast<std::uint32_t>(kSituationFailure) : status_;
    HandleStatusChanged(status);
    return status;
}

// ---------------------------------------------------------------------------------------------- the drive

// i386 0x004a0fc8: the situation runs (status 3): the script's "init", the advice's field update, the pursuit generator
// of an evade mission, the time limit, the "situation active" notification and the drive with its countdown.
void cSC4CitySituationManager::ActivateSituation() {
    if (!activeSituation_) return;
    ICitySituationManagerHost* host = CitySituationManagerHost();
    status_ = kSituationRunning;
    resultDelay_ = 0.0f;
    missionSoundPlayed_ = false;
    CallScriptFunction(activeSituation_->GetScriptTableRef(), scriptRefs_[kRefInit]);
    activeSituation_->UpdateSituationFields();
    if (activeSituation_->CheckSituationFlag(kSituationFlagEvade) && host) {
        ISituationAutomataManager* vehicles = host->VehicleManager();
        if (vehicles) {
            std::vector<std::uint32_t> groups;
            for (std::uint32_t group : activeSituation_->GetEvadeList())
                if (vehicles->HandlesGroup(group)) groups.push_back(group);
            if (!groups.empty() && activeAutomaton_) host->StartPursuitGenerator(activeAutomaton_->Occupant(), groups);
        }
    }
    timeLimit_ = GetTimeLimitSecondsFromScript();
    situationTime_ = 0.0f;
    SendSituationMessage(kSituationMsgSituationActive, activeSituation_.get(), 1);
    InitiatePlayerDriveMode(true);
    startCountdown_ = true;
}

// i386 0x0049ea32: a drive without a situation (the dialog's joyride button).
void cSC4CitySituationManager::StartJoyride(ISituationAutomaton* automaton, bool countdown, cIGZMessageTarget2* target) {
    startCountdown_ = countdown;
    SendSituationMessage(kSituationMsgSituationActive, nullptr, 1);
    if (ICitySituationManagerHost* host = CitySituationManagerHost()) host->EnterPlayerDriveView(automaton, target);
}

// i386 0x0049ea7e: entering hands the active automaton to the drive view (cSC4AutomataManager::EnterPlayerDriveView with
// the manager as the callback target); leaving takes the U-Drive-It tool off the city view when it is the current one.
void cSC4CitySituationManager::InitiatePlayerDriveMode(bool enter) {
    ICitySituationManagerHost* host = CitySituationManagerHost();
    if (!host) return;
    if (enter) {
        startCountdown_ = true;
        host->EnterPlayerDriveView(activeAutomaton_.get(), this);
        return;
    }
    if (ISituationView3D* view = host->View3D()) view->EndPlayerDriveTool();
}

// i386 0x0049ed98: a uniformly random entry of the list (the manager's own generator).
ISituationOccupant* cSC4CitySituationManager::ChooseRandomOccupant(const std::vector<ISituationOccupant*>& list) {
    if (list.empty()) return nullptr;
    const std::uint32_t pick = random_.RandomUint32Uniform(static_cast<std::uint32_t>(list.size()));
    return pick < list.size() ? list[pick] : nullptr;
}

// i386 0x004a3378: an occupant of `group`: the MySim's residence for group 0x193a, else a random one of the group inside
// the camera's frustum box, else one of the first ten in the whole city. Returns a counted reference.
ISituationOccupant* cSC4CitySituationManager::FindOccupantByGroup(std::uint32_t group) {
    ICitySituationManagerHost* host = CitySituationManagerHost();
    ISituationOccupant* result = nullptr;
    if (group == kOccupantGroupMySimResidence && mySim_) {
        if (ISituationOccupant* home = mySim_->FindMissionOccupant()) {
            home->AddRef();
            result = home;
        }
    }
    if (result) return result;
    ISituationOccupantManager* occupants = host ? host->OccupantManager() : nullptr;
    if (!occupants) return nullptr;
    const std::vector<std::uint32_t> groups{group};
    const float center[3] = {0.0f, 0.0f, 0.0f};
    float boxMin[3] = {0, 0, 0}, boxMax[3] = {0, 0, 0};
    std::vector<ISituationOccupant*> list;
    if (host->ViewFrustumBox(boxMin, boxMax)) list = occupants->OccupantsInBox(boxMin, boxMax, groups, center, 0.0f, 0xffffffff);
    if (list.empty()) list = occupants->OccupantsInBox(nullptr, nullptr, groups, center, 0.0f, 10);
    if (!list.empty()) {
        if (ISituationOccupant* chosen = ChooseRandomOccupant(list)) {
            chosen->AddRef();
            result = chosen;
        }
    }
    occupants->ReleaseOccupants(list);
    return result;
}

// i386 0x004a3d0c: for each group, the group template's source building group gives a building; the first automata
// manager that finds a creation point within 64 m of it makes the automaton (flags 0x2010002). The result is not counted
// (the automata manager keeps it). The new automaton gets no trip from here: the drive gives it its path.
ISituationAutomaton* cSC4CitySituationManager::CreateAutomatonForPlayerDrive(const std::vector<std::uint32_t>& groups) {
    ICitySituationManagerHost* host = CitySituationManagerHost();
    if (!host) return nullptr;
    cRZAutoRefCount<ISituationAutomaton> created;
    for (std::uint32_t group : groups) {
        const std::uint32_t sourceGroup = host->SourceBuildingGroup(group);
        ISituationOccupant* source = FindOccupantByGroup(sourceGroup);
        if (!source) continue;
        float origin[3] = {0, 0, 0}, where[3] = {0, 0, 0};
        source->Position(origin);
        for (auto& manager : automataManagers_) {
            if (!manager) continue;
            if (manager->FindSpawnPosition(0.0f, kPlayerDriveCreationRadius, group, origin, where, kPlayerDriveCreationFlags)) {
                if (ISituationAutomaton* automaton = manager->CreateAutomaton(group, where, kPlayerDriveCreationFlags)) {
                    created = automaton;
                    automaton->Release();
                }
            }
            if (created) break;
        }
        source->Release();
        if (created) break;
    }
    return created.get();
}

// ---------------------------------------------------------------------------------------------- targets

// i386 0x0049f2aa: the situation's target groups, or (with a target sequence) the one group of the current index, which
// wraps to 0 past the end.
void cSC4CitySituationManager::GetCurrentTargetGroups(std::vector<std::uint32_t>& out) {
    if (!activeSituation_) return;
    if (!activeSituation_->HasTargetSequence()) {
        out = activeSituation_->GetTargetGroups();
        return;
    }
    const std::vector<std::uint32_t>& sequence = activeSituation_->GetTargetSequence();
    if (currentTargetIndex_ >= sequence.size()) currentTargetIndex_ = 0;
    out.assign(1, 0);
    if (!sequence.empty()) out[0] = sequence[currentTargetIndex_];
}

// i386 0x004a461e: the target changes. The old one loses its indicator, its callback (an automaton) or its registry node
// (anything else) and its two effects; the new one gets an indicator, the green arrow, the callback or the registry
// node, the landfill zone or lot rectangle with the red selection over it, a fresh time limit, and joins the visited set.
void cSC4CitySituationManager::SetTarget(ISituationOccupant* target) {
    if (currentTarget_.get() == target) return;
    ICitySituationManagerHost* host = CitySituationManagerHost();
    ISituationEffectsManager* effects = host ? host->EffectsManager() : nullptr;
    if (currentTarget_) {
        if (dispatchView_) dispatchView_->UntrackOccupant(targetIndicator_, true);
        if (ISituationAutomaton* automaton = GetTargetAsAutomaton()) {
            automaton->RemoveListener(this);
        } else if (host) {
            host->RegisterTargetOccupant(currentTarget_.get(), false);
        }
    }
    if (targetHighlight_) {
        targetHighlight_->Stop(false);
        targetHighlight_ = nullptr;
    }
    if (greenArrow_) {
        greenArrow_->Stop(false);
        greenArrow_ = nullptr;
    }
    currentTarget_ = target;
    targetIndicator_ = 0;
    landfillLot_ = nullptr;
    if (!currentTarget_) return;
    if (dispatchView_) {
        targetIndicator_ = dispatchView_->TrackOccupant(currentTarget_.get(), 5, 0, 0);
        dispatchView_->FlashIndicator(targetIndicator_, true, 1.0f);
    }
    if (effects) {
        ISituationEffect* arrow = nullptr;
        if (effects->CreateEffect(kGreenArrowEffect, arrow) && arrow) {
            greenArrow_ = arrow;
            arrow->Release();
            float targetPosition[3] = {0, 0, 0}, activePosition[3] = {0, 0, 0};
            currentTarget_->Position(targetPosition);
            if (activeAutomaton_) {
                activeAutomaton_->Position(activePosition);
                cS3DVector3 direction{targetPosition[0] - activePosition[0], targetPosition[1] - activePosition[1],
                                      targetPosition[2] - activePosition[2]};
                const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
                if (length > 0.001f) {
                    nS3D::Normalize(direction);
                    nS3D::Matrix3 rotation;
                    nS3D::AlignZToDirWithYUp(direction, rotation);
                    RotateY(rotation.m, kArrowTurn);
                    SituationEffectTransform transform;
                    transform.header = 0x0503;
                    std::memcpy(transform.rotation, rotation.m, sizeof(transform.rotation));
                    transform.position[0] = activePosition[0];
                    transform.position[1] = activePosition[1] + kArrowHeight;
                    transform.position[2] = activePosition[2];
                    greenArrow_->SetTransform(transform);
                    greenArrow_->Start(false);
                }
            }
        }
    }
    if (ISituationAutomaton* automaton = GetTargetAsAutomaton()) {
        automaton->SetFlag(0x4000, true);
        automaton->AddListener(this);
        automaton->RegisterInGameDataRegistry();
    } else if (host) {
        host->RegisterTargetOccupant(currentTarget_.get(), true);
    }
    // The rectangle the red selection covers: the landfill zone of a landfill target, else the target's lot.
    int rect[4] = {0, 0, 0, 0};
    bool haveRect = false;
    if (currentTarget_->IsInGroup(kOccupantGroupLandfill) && host && host->LandfillDeveloper() && host->City()) {
        float position[3] = {0, 0, 0};
        currentTarget_->Position(position);
        int cellX = 0, cellZ = 0;
        host->City()->WorldToCell(position[0], position[2], cellX, cellZ);
        if (ISituationLandfillLot* lot = host->LandfillDeveloper()->LandfillLotAt(cellX, cellZ)) {
            landfillLot_ = lot;
            lot->Release();
        }
    }
    if (landfillLot_) {
        if (const int* r = landfillLot_->Rect()) {
            std::memcpy(rect, r, sizeof(rect));
            haveRect = true;
        }
    } else if (lotManager_) {
        if (ISituationLot* lot = lotManager_->LotOfOccupant(currentTarget_.get())) {
            lot->Bounds(rect[0], rect[1], rect[2], rect[3]);
            haveRect = true;
        }
    }
    if (haveRect && effects) {
        ISituationEffect* highlight = nullptr;
        if (effects->CreateEffect(kTargetHighlightEffect, highlight) && highlight) {
            targetHighlight_ = highlight;
            highlight->Release();
            const float cells[4] = {static_cast<float>(rect[0]), static_cast<float>(rect[1]), static_cast<float>(rect[2] + 1),
                                    static_cast<float>(rect[3] + 1)};
            targetHighlight_->SetCellRect(cells);
            targetHighlight_->Start(true);
        }
    }
    timeLimit_ = GetTimeLimitSecondsFromScript();
    situationTime_ = 0.0f;
    targetOccupants_.insert(currentTarget_);
}

// i386 0x004a5524: a target of the current target groups between the minimum and maximum target distance of the active
// automaton (the minimum only for the first target). Reach-target and sequence missions (1, 3) take a driveable automaton
// of those groups, else a random occupant not visited yet; escape missions (2) the best neighbor connection of the
// automaton's road; script missions (4) the script's "find_target"; a MySim mission first the sim's residence.
ISituationOccupant* cSC4CitySituationManager::FindTarget(bool setTarget) {
    if (!activeSituation_ || !activeAutomaton_) return nullptr;
    ICitySituationManagerHost* host = CitySituationManagerHost();
    cRZAutoRefCount<ISituationOccupant> found;
    std::vector<std::uint32_t> groups;
    GetCurrentTargetGroups(groups);
    float origin[3] = {0, 0, 0};
    activeAutomaton_->Position(origin);
    if (activeSituation_->CheckSituationFlag(kSituationFlagMySimMission) &&
        std::find(groups.begin(), groups.end(), kOccupantGroupMySimResidence) != groups.end()) {
        if (ISituationOccupant* home = FindOccupantByGroup(kOccupantGroupMySimResidence)) {
            found = home;
            home->Release();
        }
    }
    if (!found) {
        const float maxDistance = activeSituation_->GetMaxTargetDistance();
        float minDistanceSquared = 0.0f;
        if (currentTargetIndex_ == 0) {
            const float minDistance = activeSituation_->GetMinTargetDistance();
            minDistanceSquared = minDistance * minDistance;
        }
        const std::uint32_t condition = activeSituation_->GetSuccessCondition();
        if (condition == 2) {
            // Recovered-partial: cSC4TrafficAutomaton::FindBestNeighborConnection (QueryInterface 0x8b99ee7c) is not
            // reachable through the situation seam; an escape mission finds no target occupant (its distance is the
            // city edge, GetDistanceToTarget).
        } else if (condition == 4) {
            // Recovered-partial: CallTargetScriptFunction (the script's find_target answering an occupant) is not ported.
        } else if (condition == 1 || condition == 3) {
            std::unordered_set<ISituationAutomaton*> instances;
            GetInstancesOfGroup(groups, instances);
            for (ISituationAutomaton* automaton : instances) {
                if (found) break;
                if (!CanPlayerDrive(automaton)) continue;
                float position[3] = {0, 0, 0};
                automaton->Position(position);
                position[1] = origin[1];
                const float distanceSquared = SC4GeometryUtilities::LengthOfLineSquared(position, origin);
                if (distanceSquared < minDistanceSquared || distanceSquared > maxDistance * maxDistance) continue;
                // Quirk kept: the original looks up the (still empty) candidate reference in the visited set, so an
                // automaton target is never filtered by it.
                if (targetOccupants_.count(cRZAutoRefCount<ISituationOccupant>()) != 0) continue;
                found = automaton->Occupant();
            }
            if (ISituationAutomataManager* vehicles = host ? host->VehicleManager() : nullptr) vehicles->ReleaseInstances(instances);
            if (!found) {
                if (ISituationOccupantManager* occupants = host ? host->OccupantManager() : nullptr) {
                    const float boxMin[3] = {origin[0] - maxDistance, -std::numeric_limits<float>::max(), origin[2] - maxDistance};
                    const float boxMax[3] = {origin[0] + maxDistance, std::numeric_limits<float>::max(), origin[2] + maxDistance};
                    std::vector<ISituationOccupant*> list = occupants->OccupantsInBox(boxMin, boxMax, groups, origin, minDistanceSquared, 0xffffffff);
                    std::vector<ISituationOccupant*> fresh;
                    for (ISituationOccupant* occupant : list) {
                        if (targetOccupants_.count(cRZAutoRefCount<ISituationOccupant>(occupant)) == 0) fresh.push_back(occupant);
                    }
                    if (ISituationOccupant* chosen = ChooseRandomOccupant(fresh)) found = chosen;
                    occupants->ReleaseOccupants(list);
                }
            }
        }
    }
    if (setTarget && found) SetTarget(found.get());
    return found.get();   // the target keeps its own reference (the original returns the released pointer too)
}

// i386 0x004a51e0: the script's "create_target", else a new automaton of the target groups made by the first manager
// that finds a creation point between the minimum and maximum target distance. A created target is removed again when
// the situation is cancelled (+0xb08).
ISituationOccupant* cSC4CitySituationManager::CreateTarget(bool setTarget) {
    if (!activeSituation_ || !activeAutomaton_) return nullptr;
    cRZAutoRefCount<ISituationOccupant> created;
    std::vector<std::uint32_t> groups;
    GetCurrentTargetGroups(groups);
    // Recovered-partial: the script's "create_target" (CallTargetScriptFunction) is not ported; the automaton path runs.
    if (!created) {
        cRZAutoRefCount<ISituationAutomaton> automaton;
        for (auto& manager : automataManagers_) {
            if (automaton) break;
            if (!manager) continue;
            float origin[3] = {0, 0, 0};
            activeAutomaton_->Position(origin);
            for (std::uint32_t group : groups) {
                if (automaton) break;
                float where[3] = {0, 0, 0};
                if (manager->FindSpawnPosition(activeSituation_->GetMinTargetDistance(), activeSituation_->GetMaxTargetDistance(), group, origin,
                                               where, kPlayerDriveCreationFlags)) {
                    if (ISituationAutomaton* made = manager->CreateAutomaton(group, where, kPlayerDriveCreationFlags)) {
                        automaton = made;
                        made->Release();
                    }
                }
            }
        }
        if (automaton) created = automaton->Occupant();
    }
    if (setTarget && created) SetTarget(created.get());
    if (created) targetSequenceReset_ = true;
    return created.get();
}

// i386 0x004a5b86: the next target of a target sequence: true when one was found (the automaton plays 0x2c22d924); the
// listeners hear the new index (0xac083d7d).
bool cSC4CitySituationManager::ChooseNextTarget() {
    if (!activeSituation_ || !activeSituation_->HasTargetSequence()) return false;
    const std::size_t count = activeSituation_->GetTargetSequence().size();
    ++currentTargetIndex_;
    if (currentTargetIndex_ >= count) return false;
    ISituationOccupant* next = FindTarget(true);
    if (next && activeAutomaton_) activeAutomaton_->StartSFX(kNextTargetSound);
    SendSituationMessage(kSituationMsgNextTarget, activeSituation_.get(), currentTargetIndex_);
    return next != nullptr;
}

// i386 0x004a09c0: the flat distance from the active automaton (or the active position) to the target; a lot-boundary
// mission (flag 0x10) measures to the edge of the target's lot (0 inside it). Without a target: the distance to the city
// edge (escape) or to the first coverage cell. 3.4e38 when there is nothing to measure.
float cSC4CitySituationManager::GetDistanceToTarget(bool fromActiveAutomaton) {
    constexpr float kFar = std::numeric_limits<float>::max();
    if (!activeAutomaton_) return kFar;
    ICitySituationManagerHost* host = CitySituationManagerHost();
    float from[3] = {0, 0, 0};
    if (fromActiveAutomaton) {
        activeAutomaton_->Position(from);
    } else if (!GetActivePosition(from)) {
        return kFar;
    }
    if (!currentTarget_) {
        const std::uint32_t condition = activeSituation_ ? activeSituation_->GetSuccessCondition() : 0;
        ISituationCity* city = host ? host->City() : nullptr;
        if (condition == 2 && city) {
            float dx = static_cast<float>(city->CellsX() << 4) - from[0];
            if (dx > from[0]) dx = from[0];
            float dz = static_cast<float>(city->CellsZ() << 4) - from[2];
            if (dz > from[2]) dz = from[2];
            return dz <= dx ? dz : dx;
        }
        if (condition == 5 && !coverageCells_.empty() && city) {
            const SituationCellPoint cell = *coverageCells_.begin();
            float to[3] = {0, 0, 0};
            city->CellToWorld(cell.x, cell.z, to);
            return std::sqrt((to[0] - from[0]) * (to[0] - from[0]) + (to[2] - from[2]) * (to[2] - from[2]));
        }
        return kFar;
    }
    float to[3] = {0, 0, 0};
    currentTarget_->Position(to);
    to[1] = from[1];
    const auto straight = [&] {
        return std::sqrt((to[0] - from[0]) * (to[0] - from[0]) + (to[1] - from[1]) * (to[1] - from[1]) + (to[2] - from[2]) * (to[2] - from[2]));
    };
    if (!activeSituation_->CheckSituationFlag(kSituationFlagUseLotBoundary) || !lotManager_) return straight();
    int rect[4] = {0, 0, 0, 0};
    if (landfillLot_) {
        const int* r = landfillLot_->Rect();
        if (!r) return straight();
        std::memcpy(rect, r, sizeof(rect));
    } else {
        ISituationLot* lot = lotManager_->LotOfOccupant(currentTarget_.get());
        if (!lot) return straight();
        lot->Bounds(rect[0], rect[1], rect[2], rect[3]);
    }
    const float a[2] = {from[0], from[2]};
    const float b[2] = {to[0], to[2]};
    const float lo[2] = {static_cast<float>(rect[0] << 4), static_cast<float>(rect[1] << 4)};
    const float hi[2] = {static_cast<float>(rect[2] * 16 + 16), static_cast<float>(rect[3] * 16 + 16)};
    if (SC4GeometryUtilities::PointInRectangle2D(a, lo, hi)) return 0.0f;
    const float c0[2] = {lo[0], lo[1]}, c1[2] = {hi[0], lo[1]}, c2[2] = {hi[0], hi[1]}, c3[2] = {lo[0], hi[1]};
    float hit[2] = {0, 0};
    if (SC4GeometryUtilities::LinesIntersect2D(a, b, c0, c1, hit) || SC4GeometryUtilities::LinesIntersect2D(a, b, c1, c2, hit) ||
        SC4GeometryUtilities::LinesIntersect2D(a, b, c2, c3, hit) || SC4GeometryUtilities::LinesIntersect2D(a, b, c3, c0, hit)) {
        return SC4GeometryUtilities::LengthOfLine2D(hit, a);
    }
    return kFar;
}

// i386 0x004a0da8: the script's get_time_limit(table, distance to the target, the automaton's top speed * 1000) in
// seconds; 60 without a script answer.
float cSC4CitySituationManager::GetTimeLimitSecondsFromScript() {
    if (!activeSituation_ || !activeAutomaton_ || scriptRefs_[kRefGetTimeLimit] == -2 || !lua_) return kDefaultTimeLimitSeconds;
    if (activeSituation_->CheckSituationFlag(kSituationFlagMySimMission) && mySim_) mySim_->PrepareScript();
    float result = kDefaultTimeLimitSeconds;
    const int savedTop = lua_->GetTop();
    lua_->GetRef(scriptRefs_[kRefGetTimeLimit]);
    if (lua_->IsFunction(-1)) {
        lua_->GetRef(activeSituation_->GetScriptTableRef());
        lua_->PushNumber(GetDistanceToTarget(true));
        float maxSpeed = 0.0f;
        const float speed = activeAutomaton_->PlayerDriveMaxSpeed(maxSpeed) ? kPlayerDriveSpeedScale * maxSpeed : kDefaultPlayerDriveSpeed;
        lua_->PushNumber(speed);
        lua_->CallProtected(3, 1);
        if (lua_->IsNumber(-1)) result = static_cast<float>(lua_->ToNumber(-1));
    }
    lua_->SetTop(savedTop);
    return result;
}

// ---------------------------------------------------------------------------------------------- the checks

// i386 0x004a5c46: inside the success distance (from the active position to the target, less the active radius) for the
// success timeout, or a success already decided, moves on to the next target: running while one remains, else success.
std::uint32_t cSC4CitySituationManager::CheckReachTargetSuccess(float elapsedMilliseconds) {
    std::uint32_t status = status_;
    const float distance = GetDistanceToTarget(false) - activeSituation_->GetActiveRadius();
    // Recovered-partial: GetFlashPeriod (the indicator's flash rate by distance) is not recovered; the dispatch view that
    // would flash is not either.
    bool next = false;
    if (activeSituation_->GetSuccessDistance() > distance) {
        reachTimer_ += elapsedMilliseconds;
        next = reachTimer_ * 0.001f >= activeSituation_->GetSuccessTimeout();
    } else {
        reachTimer_ = 0.0f;
    }
    if (next || status == kSituationSuccess) status = ChooseNextTarget() ? kSituationRunning : kSituationSuccess;
    HandleStatusChanged(status);
    return status;
}

// i386 0x004a5d68: the evade check, then the condition's own check (unless the script has "check_success", which then
// decides), then the time limit while the situation still runs.
std::uint32_t cSC4CitySituationManager::CheckForSuccess(float elapsedMilliseconds) {
    std::uint32_t status = kSituationNone;
    if (activeSituation_ && activeAutomaton_) {
        status = status_;
        // Recovered-partial: an evade mission (flag 2) first asks CheckEvadeListSuccess, which is not ported; its status
        // stays as it is here. The condition's check only runs while the situation runs.
        if (status == kSituationRunning && scriptRefs_[kRefCheckSuccess] == -2) {
            const std::uint32_t condition = activeSituation_->GetSuccessCondition();
            if (condition == 1 && currentTarget_) {
                status = CheckReachTargetSuccess(elapsedMilliseconds);
            } else {
                // Recovered-partial: CheckEscapeCitySuccess (2) and CheckCellCoverageSuccess (5) are not ported.
                status = kSituationRunning;
            }
        }
        if (scriptRefs_[kRefCheckSuccess] != -2 && lua_) {
            if (activeSituation_->CheckSituationFlag(kSituationFlagMySimMission) && mySim_) mySim_->PrepareScript();
            const int savedTop = lua_->GetTop();
            lua_->GetRef(scriptRefs_[kRefCheckSuccess]);
            if (lua_->IsFunction(-1)) {
                lua_->GetRef(activeSituation_->GetScriptTableRef());
                lua_->CallProtected(1, 1);
                if (lua_->IsNumber(-1)) {
                    // The original converts the double to an unsigned 32-bit status (clamped to 0..4294967295).
                    double value = lua_->ToNumber(-1);
                    value = std::min(std::max(value, 0.0), 4294967295.0);
                    status = static_cast<std::uint32_t>(value);
                }
            }
            lua_->SetTop(savedTop);
        }
        if (status == kSituationRunning) {
            situationTime_ += elapsedMilliseconds * 0.001f;
            const float limit = GetCurrentTimeLimitSeconds();
            if (limit >= 1.0f && situationTime_ >= limit) status = HandleTimeLimitReached();
        }
    }
    HandleStatusChanged(status);
    return status;
}

// i386 0x004a83b0: one animation tick. A drive that just started pauses the automata and runs the 4 s countdown (its
// effect at the drive view); once the countdown is over the automata resume and the checks run each tick. A decided
// result waits for its delay, then the success / failure handler runs and the result popup is posted.
void cSC4CitySituationManager::DoAnimationTick(float elapsedMilliseconds) {
    ICitySituationManagerHost* host = CitySituationManagerHost();
    bool skipChecks = true;
    if (startCountdown_) {
        for (auto& manager : automataManagers_)
            if (manager) manager->Pause();
        startCountdown_ = false;
        countdown_ = kCountdownMilliseconds;
        if (countdownEffect_) {
            countdownEffect_->Stop(false);
            countdownEffect_ = nullptr;
        }
        ISituationEffectsManager* effects = host ? host->EffectsManager() : nullptr;
        ISituationDriveView* driveView = host ? host->PlayerDriveView() : nullptr;
        if (effects && driveView) {
            ISituationEffect* effect = nullptr;
            if (effects->CreateEffect(kCountdownEffect, effect) && effect) {
                countdownEffect_ = effect;
                effect->Release();
                SituationEffectTransform transform;
                driveView->Position(transform.position);
                countdownEffect_->SetTransform(transform);
                countdownEffect_->Start(false);
            }
        }
    } else if (countdown_ > 0.0f) {
        countdown_ -= elapsedMilliseconds;
        if (countdown_ <= 0.0f) {
            countdownEffect_ = nullptr;
            for (auto& manager : automataManagers_)
                if (manager) manager->Resume();
            skipChecks = false;
        }
    } else if (activeSituation_ && resultDelay_ > 0.0f) {
        resultDelay_ += elapsedMilliseconds * -0.001f;
        if (resultDelay_ <= 0.0f) {
            if (status_ == kSituationSuccess) {
                HandleSituationSuccess(activeSituation_.get());
            } else if (status_ == kSituationFailure) {
                HandleSituationFailure(activeSituation_.get());
            }
            PostMessageToSelf(kSituationMsgShowPopup, activeSituation_.get(), nullptr);
        }
    } else {
        skipChecks = false;
    }
    if (missionActor_ && !missionActor_->IsValid()) missionActor_ = nullptr;
    if (!activeSituation_ || skipChecks) return;
    const float limit = GetCurrentTimeLimitSeconds();
    if (!missionSoundPlayed_ && limit >= kTimeWarningSeconds && limit - kTimeWarningSeconds <= situationTime_) {
        if (activeAutomaton_) activeAutomaton_->StartSFX(kTimeWarningSound);
        missionSoundPlayed_ = true;
    }
    HandleStatusChanged(CheckForSuccess(elapsedMilliseconds));
    UpdatePursuit(elapsedMilliseconds);
}

// i386 0x004a6038: the active automaton or the target moved: check at once.
void cSC4CitySituationManager::HandleAutomatonMoved(ISituationAutomaton* automaton) {
    if (!activeSituation_ || !automaton) return;
    if (activeAutomaton_.get() == automaton || currentTarget_.get() == automaton->Occupant()) {
        HandleStatusChanged(CheckForSuccess(0.0f));
    }
}

// i386 0x004a6c8a: the automaton is disabled (flag 0x10000000). The active automaton fails the situation; a target fails
// it too unless the mission moves on to its next target (reach / sequence missions), which succeeds when there is none.
// A tracked automaton gives up its track.
void cSC4CitySituationManager::HandleAutomatonDisabled(ISituationAutomaton* automaton) {
    if (automaton) automaton->SetFlag(0x10000000, true);
    if (activeSituation_) {
        std::uint32_t status = status_;
        if (activeAutomaton_.get() == automaton) {
            status = kSituationFailure;
        } else if (automaton && automaton->Occupant() == currentTarget_.get()) {
            const std::uint32_t condition = activeSituation_->GetSuccessCondition();
            if (condition != 1 && condition != 3) {
                status = kSituationFailure;
            } else if (!ChooseNextTarget()) {
                status = kSituationSuccess;
            }
        }
        HandleStatusChanged(status);
    }
    if (trackedAutomata_.count(automaton) != 0) HandleRemoveAutomaton(automaton);
}

// i386 0x004a6168: the target occupant went away. Without a "check_success" script a reach mission fails and a sequence
// mission moves on (success when none is left); with one, a vehicle target (groups 0x5005 / 0x5006) is looked up again.
std::uint32_t cSC4CitySituationManager::HandleTargetRemoved() {
    std::uint32_t status = status_;
    if (scriptRefs_[kRefCheckSuccess] == -2) {
        const std::uint32_t condition = activeSituation_->GetSuccessCondition();
        if (condition == 1) {
            status = kSituationFailure;
        } else if (condition == 3 && !ChooseNextTarget()) {
            status = kSituationSuccess;
        }
    } else if (currentTarget_ && (currentTarget_->IsInGroup(kOccupantGroupTargetVehicle1) || currentTarget_->IsInGroup(kOccupantGroupTargetVehicle2))) {
        if (!FindTarget(true)) SetTarget(nullptr);
    }
    HandleStatusChanged(status);
    return status;
}

// i386 0x004a6d4a: a removed occupant ends a running situation's target or automaton; an automaton also leaves its track.
void cSC4CitySituationManager::HandleRemoveOccupant(ISituationOccupant* occupant) {
    if (!occupant) return;
    if (activeSituation_) {
        std::uint32_t status = status_;
        if (occupant == currentTarget_.get() && status == kSituationRunning) status = HandleTargetRemoved();
        if (activeAutomaton_ && occupant == activeAutomaton_->Occupant() && status == kSituationRunning) status = HandleActiveAutoRemoved();
        HandleStatusChanged(status);
    }
    if (ISituationAutomaton* automaton = occupant->AsAutomaton()) HandleRemoveAutomaton(automaton);
}

// i386 0x004a622e: every 5 s of an evade mission the automata of the evade groups within the maximum target distance (at
// least half the city width) chase the active automaton (or, in a pursuit mission, the target).
void cSC4CitySituationManager::UpdatePursuit(float elapsedMilliseconds) {
    if (!activeSituation_ || !activeAutomaton_ || !activeSituation_->HasEvadeList()) return;
    pursuitTimer_ += elapsedMilliseconds;
    if (pursuitTimer_ < 5000.0f) return;
    pursuitTimer_ = 0.0f;
    ISituationAutomaton* chased = activeSituation_->CheckSituationFlag(kSituationFlagPursuit) ? GetTargetAsAutomaton() : activeAutomaton_.get();
    if (!chased) return;
    cRZAutoRefCount<ISituationAutomaton> hold(chased);
    float radius = activeSituation_->GetMaxTargetDistance();
    if (radius < 16.0f) {
        ICitySituationManagerHost* host = CitySituationManagerHost();
        ISituationCity* city = host ? host->City() : nullptr;
        radius = city ? static_cast<float>(city->CellsX() << 4) * 0.5f : radius;
    }
    for (auto& manager : automataManagers_) {
        if (!manager) continue;
        for (std::uint32_t group : activeSituation_->GetEvadeList())
            if (manager->HandlesGroup(group)) manager->AlertGroup(radius, group, chased, true);
    }
}

// ---------------------------------------------------------------------------------------------- results

// i386 0x004a7e4a: counts the success, records a training mission among the preferences' completed missions, plays the
// success effect at the automaton, adds the success aura and money, calls the script's "on_success" and retires the advice.
void cSC4CitySituationManager::HandleSituationSuccess(cISC4CitySituationAdvice* situation) {
    if (!situation || situation != activeSituation_.get()) return;
    ICitySituationManagerHost* host = CitySituationManagerHost();
    const std::uint32_t id = static_cast<std::uint32_t>(situation->GetAdviceId());
    auto it = successCounts_.find(id);
    if (it == successCounts_.end()) {
        successCounts_[id] = 1;
    } else if (it->second != 0xffff) {
        ++it->second;
    }
    if (situation->CheckSituationFlag(kSituationFlagTraining) && host && host->App()) {
        if (std::uint32_t* ids = host->App()->CompletedMissionIds()) {
            for (int i = 0; i < 64; ++i) {
                if (ids[i] == 0) {
                    ids[i] = id;
                    host->App()->SavePreferences();
                }
                if (ids[i] == id) break;
            }
        }
    }
    float position[3] = {0, 0, 0};
    if (activeAutomaton_) activeAutomaton_->Position(position);
    cRZString effectName;
    if (activeSituation_->GetSuccessEffectName(effectName)) {
        if (ISituationEffectsManager* effects = host ? host->EffectsManager() : nullptr) {
            ISituationEffect* effect = nullptr;
            if (effects->CreateEffect(effectName.ToChar(), effect) && effect) {
                SituationEffectTransform transform;
                std::memcpy(transform.position, position, sizeof(position));
                effect->SetTransform(transform);
                effect->Start(false);
                effect->Release();
            }
        }
    }
    float radius = 0.0f, magnitude = 0.0f;
    if (activeSituation_->GetSuccessAura(radius, magnitude)) {
        if (ISituationAuraSim* aura = host ? host->AuraSim() : nullptr) aura->AddAura(position[0], position[2], radius, magnitude);
    }
    long long money = 0;
    if (activeSituation_->GetSuccessMoney(money)) {
        if (ISituationBudgetSim* budget = host ? host->BudgetSim() : nullptr) budget->AddMoney(money);
    }
    CallScriptFunction(activeSituation_->GetScriptTableRef(), scriptRefs_[kRefOnSuccess]);
    RetireSituation(situation);
}

// i386 0x004a818a: plays the failure effect at the automaton (or the target), adds the failure aura and money and calls
// the script's "on_failure".
void cSC4CitySituationManager::HandleSituationFailure(cISC4CitySituationAdvice* situation) {
    if (!situation || situation != activeSituation_.get()) return;
    ICitySituationManagerHost* host = CitySituationManagerHost();
    float position[3] = {0, 0, 0};
    bool havePosition = true;
    if (activeAutomaton_) {
        activeAutomaton_->Position(position);
    } else if (currentTarget_) {
        currentTarget_->Position(position);
    } else {
        havePosition = false;
    }
    if (havePosition) {
        cRZString effectName;
        if (activeSituation_->GetFailureEffectName(effectName)) {
            if (ISituationEffectsManager* effects = host ? host->EffectsManager() : nullptr) {
                ISituationEffect* effect = nullptr;
                if (effects->CreateEffect(effectName.ToChar(), effect) && effect) {
                    SituationEffectTransform transform;
                    std::memcpy(transform.position, position, sizeof(position));
                    effect->SetTransform(transform);
                    effect->Start(false);
                    effect->Release();
                }
            }
        }
        float radius = 0.0f, magnitude = 0.0f;
        if (activeSituation_->GetFailureAura(radius, magnitude)) {
            if (ISituationAuraSim* aura = host ? host->AuraSim() : nullptr) aura->AddAura(position[0], position[2], radius, magnitude);
        }
        long long money = 0;
        if (activeSituation_->GetFailureMoney(money)) {
            if (ISituationBudgetSim* budget = host ? host->BudgetSim() : nullptr) budget->AddMoney(money);
        }
    }
    CallScriptFunction(activeSituation_->GetScriptTableRef(), scriptRefs_[kRefOnFailure]);
}

// ---------------------------------------------------------------------------------------------- the popup

// i386 0x004a05cc: the situation dialog: the briefing (0x2bb16d50) while a situation is set up, the result for success
// / not available (0x0bbc06b6) or failure (0x4bbc080f). It runs as a modal popup under a game pause; a briefing with an
// automaton shows the drive's mission status panel behind it.
void cSC4CitySituationManager::ShowSituationPopup(cISC4CitySituationAdvice* situation, ISituationAutomaton* automaton) {
    ICitySituationManagerHost* host = CitySituationManagerHost();
    if (dispatchView_) dispatchView_->ShowAllIndicators(4, false);
    if (!situation) status_ = kSituationUnavailable;
    std::uint32_t instance = 0x2bb16d50, windowId = 0xebb16d71;
    if (status_ == kSituationFailure) {
        instance = 0x4bbc080f;
        windowId = 0xebbc081e;
    } else if (status_ == kSituationUnavailable || status_ == kSituationSuccess) {
        instance = 0x0bbc06b6;
        windowId = 0xebbc081e;
    }
    cIGZWin* dialog = nullptr;
    if (!host || !host->CreateSituationWindow(instance, windowId, &dialog) || !dialog) return;
    host->BeginPopupPause();
    // The script window is asked for its cIGZWinGen view (iid 0x5386d516 in the original), which takes the procedure.
    gz::cIGZWinGen* generic = nullptr;
    if (dialog->QueryInterface(gz::kGZIID_cIGZWinGen, reinterpret_cast<void**>(&generic)) && generic) {
        generic->SetWinProc(this);
        generic->Release();
    }
    SetupDialog(dialog, situation, automaton);
    popupDialog_ = dialog;
    if (automaton) {
        std::uint32_t dashboard = 0;
        ISituationOccupant* occupant = automaton->Occupant();
        ISituationPropertyHolder* properties = occupant ? occupant->PropertyHolder() : nullptr;
        if (!properties || !properties->GetPropertyU32(kDashboardProperty, dashboard)) {
            dashboard = occupant && occupant->OccupantType() == kOccupantTypeAutomaton ? kDefaultDashboardAutomaton : kDefaultDashboardOther;
        }
        std::uint32_t panel = 0;
        if (host->DashboardMissionPanel(dashboard, panel)) {
            if (ISituationView3D* view = host->View3D()) view->ShowMissionStatusPanel(panel);
        }
    }
    host->RestoreWindowLocation(dialog);
    if (cIGZWinMgr* manager = host->WindowManager()) manager->RunPopup(dialog);
    host->SaveWindowLocation(dialog);
    host->FocusView3D();
    host->EndPopupPause();
    dialog->Release();
}

// i386 0x0049f374: fills the dialog. Recovered-partial: the original builds an HTML page (title and text of the advice
// or of the result, the time limit / money / aura lines, the reward texts) like cSC4AdvisorPopupTracker::SetupDialog,
// resizes the dialog to it and attaches cSC4SituationUIImageWinProc to the picture; only the window parameters the
// buttons read (the advice, the joyride automaton) are set here.
void cSC4CitySituationManager::SetupDialog(cIGZWin* dialog, cISC4CitySituationAdvice* situation, ISituationAutomaton* automaton) {
    if (!dialog) return;
    if (!situation) status_ = kSituationUnavailable;
    if (situation) dialog->SetParam(kSituationParamAdvice, new cRZVariant(static_cast<cIGZUnknown*>(situation)));
    if (automaton) dialog->SetParam(kSituationParamAutomaton, new cRZVariant(static_cast<cIGZUnknown*>(automaton)));
}

// i386 0x0049ee8c: disables the picture's window procedure, drops the two window parameters, forgets the dialog, ends it
// (window manager code 0xf) and destroys the window.
void cSC4CitySituationManager::EndDialog(cIGZWin* window) {
    if (!window) return;
    situationWindow_ = nullptr;   // the image window procedure (cSC4SituationUIImageWinProc, not recovered)
    window->SetParam(kSituationParamAdvice, nullptr);
    window->SetParam(kSituationParamAutomaton, nullptr);
    if (window == popupDialog_.get()) popupDialog_ = nullptr;
    ICitySituationManagerHost* host = CitySituationManagerHost();
    if (cIGZWinMgr* manager = host ? host->WindowManager() : nullptr) {
        manager->EndDialog(0xf, window, 0);
        manager->DestroyWindow(window);
    }
}

// i386 0x004a7020: a created target goes again, the advice is retired and the situation ends.
void cSC4CitySituationManager::HandleSituationCancelled() {
    if (targetSequenceReset_) {
        if (ISituationAutomaton* automaton = GetTargetAsAutomaton()) {
            automaton->AddRef();
            automaton->Remove(false);
            automaton->Release();
        }
    }
    RetireSituation(activeSituation_.get());
    EndSituation();
}

// i386 0x004a709c: the joyride button: the automaton kept in the dialog's parameter drives without the situation.
void cSC4CitySituationManager::HandleJoyride(cIGZWin* window) {
    cRZAutoRefCount<ISituationAutomaton> automaton;
    cIGZVariant* variant = nullptr;
    if (window && window->GetParam(kSituationParamAutomaton, &variant) && variant) {
        automaton = static_cast<ISituationAutomaton*>(variant->GetValUnknown());
        variant->Release();
    }
    HandleSituationCancelled();
    if (automaton) StartJoyride(automaton.get(), true, nullptr);
}

// i386 0x004a714e: the dialog's buttons (command 3 from sender 0x287259f6): 6 accepts the situation, 7 is the joyride, 1
// closes (a result dialog first completes the situation); every button ends the dialog. A followed HTML link ends it too.
bool cSC4CitySituationManager::DoWinProcMessage(cIGZWin* window, cGZMessage& message) {
    ICitySituationManagerHost* host = CitySituationManagerHost();
    if (message.type == gz::kWinProcInit || message.type == gz::kWinProcShutdown) return true;
    if (message.type == kGZMsgCommand && message.param1 == kSituationButtonSender) {
        const std::uint64_t button = message.param2;
        if (button == kSituationButtonAccept) {
            ActivateSituation();
        } else if (button == kSituationButtonJoyride) {
            HandleJoyride(window);
        } else if (button == kSituationButtonClose) {
            if (ISituationView3D* view = host ? host->View3D() : nullptr) view->HideMissionStatusPanel();
            if (activeSituation_ && status_ - 1u < 2u) {
                EndDialog(window);
                PostMessageToSelf(kSituationMsgSituationComplete, activeSituation_.get(), nullptr);
            }
            HandleSituationCancelled();
        }
        EndDialog(window);
        return true;
    }
    if (message.type == kSituationWinProcLinkJump) {
        const bool followed = host && host->DoURL(reinterpret_cast<const void*>(message.param2));
        if (ISituationView3D* view = host ? host->View3D() : nullptr) view->HideMissionStatusPanel();
        if (!activeSituation_ || status_ - 1u >= 2u) {
            HandleSituationCancelled();
            EndDialog(window);
        } else {
            EndDialog(window);
            PostMessageToSelf(kSituationMsgSituationComplete, activeSituation_.get(), nullptr);
        }
        return !followed;
    }
    return false;
}

}  // namespace sc4
