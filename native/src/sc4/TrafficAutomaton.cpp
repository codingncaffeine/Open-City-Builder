// cSC4TrafficAutomaton — see TrafficAutomaton.h for the class, the layout, the vtable slots and the quirks. Every
// function here is a hand port of the exported decompile; the disassembly was used where the decompiler dropped SIMD
// lanes, arguments or the float/int rounding sequence (the comments name the address).
//
// The driving state record (state 0x10) is the shared tAutomataStatePlayerDrive; the port keeps its traffic run-time
// pointers on the automaton, because cSC4Watercraft adds its own fields to the same record (see the header note).
#include "sc4/TrafficAutomaton.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <list>

#include "sc4/PathInfo.h"
#include "sc4/SimStreamOps.h"
#include "sc4/TrafficEffectManager.h"
#include "rz/cRZRandomSuper.h"

namespace sc4 {

namespace {

constexpr float kPathEpsilon = 1e-05f;
constexpr float kDestinationEpsilon = 0.001f;   // DoPathTick's "the interpolated point is the position" test
constexpr float kSafeDistance = 16.0f;          // GetSafeDistance's default
constexpr std::uint32_t kAutomataFlagWaitBit = 0x10;
constexpr std::uint32_t kAutomataFlagNoJunctionBit = 0x800;
constexpr std::uint32_t kAutomataFlagTunnelOccupantFlags = 0x403c1004;   // HandleEnterState's HasAnyNetworkFlag
constexpr std::uint32_t kAudioEventTunnel = 0xe;
constexpr std::uint32_t kTurnTableSign[2] = {0xbf800000u, 0x3f800000u};   // 0x100a18a60: {-1, 1}
constexpr float kDiagonal = 0.70710678f;   // 1 / sqrt(2), OffsetPathPoint's diagonal test

// cSC4Automaton::sRandom (the process-wide cRZRandomSuper the path-info queries take). The port's base automaton routes
// its randomness through IAutomatonHost, so the traffic path queries keep their own generator.
cRZRandomSuper& TrafficRandom() {
    static cRZRandomSuper random{0xffffffffu};
    return random;
}

float FloatFromBits(std::uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

cS3DVector3 MakeVector(float x, float y, float z) {
    cS3DVector3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

cS3DVector3 Add(const cS3DVector3& a, const cS3DVector3& b) { return MakeVector(a.x + b.x, a.y + b.y, a.z + b.z); }
cS3DVector3 Sub(const cS3DVector3& a, const cS3DVector3& b) { return MakeVector(a.x - b.x, a.y - b.y, a.z - b.z); }
cS3DVector3 Scale(const cS3DVector3& v, float s) { return MakeVector(v.x * s, v.y * s, v.z * s); }
float Length3(const cS3DVector3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
float Distance3(const cS3DVector3& a, const cS3DVector3& b) { return Length3(Sub(a, b)); }

ITrafficAutomatonOccupant* AsTrafficOccupant(ITrafficNetworkMapOccupant* occupant) {
    return static_cast<ITrafficAutomatonOccupant*>(occupant);
}

ITrafficAutomatonPeer* AsTrafficPeer(ITrafficNetworkMapAutomaton* automaton) {
    return static_cast<ITrafficAutomatonPeer*>(automaton);
}

constexpr std::uint32_t kEdgeDirection[4][3] = {{0xbf800000u, 0, 0}, {0, 0, 0xbf800000u}, {0x3f800000u, 0, 0},
                                                {0, 0, 0x3f800000u}};

}  // namespace

ITrafficAutomataManager* cSC4TrafficAutomaton::Manager() const {
    return trafficHost_ ? trafficHost_->TrafficAutomataManager() : nullptr;
}

const TrafficAutomatonTuning& cSC4TrafficAutomaton::Tuning() const {
    static const TrafficAutomatonTuning kDefaults{};
    const TrafficAutomatonTuning* tuning = trafficHost_ ? trafficHost_->Tuning() : nullptr;
    return tuning ? *tuning : kDefaults;
}

// ---- construction / destruction --------------------------------------------------------------------------------------
// 0x100116440: the base constructor, then the traffic fields; the original also ORs 0x10 into the occupant flags (the
// base constructor already does), zeroes +0x308..+0x397 and sets +0x320 = 0xffff, +0x308 = 7.
cSC4TrafficAutomaton::cSC4TrafficAutomaton(ITrafficAutomatonHost* host, IAutomatonHost* automatonHost,
                                           const cGZPersistResourceKey& modelKey)
    : cSC4Automaton(automatonHost ? automatonHost : AutomatonHost(), modelKey), trafficHost_(host) {}

// 0x1006f3b10: the generated destructor destroys the two deques, then releases +0x318 and +0x310, then the base. The
// lookahead deque's counted occupants are released by the node records.
cSC4TrafficAutomaton::~cSC4TrafficAutomaton() {
    lookahead_.clear();
    route_.clear();
    if (pathInfo_) {
        cISC4PathInfo* old = pathInfo_;
        pathInfo_ = nullptr;
        old->Release();
    }
    if (networkOccupant_) {
        ITrafficAutomatonOccupant* old = networkOccupant_;
        networkOccupant_ = nullptr;
        old->Release();
    }
}

bool cSC4TrafficAutomaton::QueryInterface(std::uint32_t iid, void** out) {
    // 0x100116706: 0x8b99ee7c answers this object; everything else goes to the base (1 / 0xa9b40f05 -> this,
    // 0x26bb767e -> +8, 0xe4fda3d4 -> +0x10, 0x452294aa -> +0x68).
    if (!out) return false;
    *out = nullptr;
    if (iid == kGZIID_cISC4TrafficAutomaton) {
        *out = this;
        AddRef();
        return true;
    }
    return cSC4Automaton::QueryInterface(iid, out);
}

// ---- Init / Shutdown -------------------------------------------------------------------------------------------------
// 0x100116558: skip when already initialized; otherwise the base Init, then (unless the automaton came from a save)
// UpdateAttraction and SnapToPath unless the matching ignore flags are set. A failed base Init is answered.
bool cSC4TrafficAutomaton::Init() {
    if (IsInitialized()) return true;
    if (!cSC4Automaton::Init()) return false;
    if (!saved_) {
        if (!CheckAutomataFlag(kAutomataFlagIgnoreAttractors)) UpdateAttraction();
        if (!CheckAutomataFlag(kAutomataFlagIgnorePaths)) SnapToPath();
    }
    return true;
}

// 0x1001165ee: when initialized, leave the path (SetPath(nullptr, 0, nullptr, false)) and clear the lookahead deque,
// then the base Shutdown.
bool cSC4TrafficAutomaton::Shutdown() {
    if (IsInitialized()) {
        SetPath(nullptr, 0, nullptr, false);
        ClearLookaheadDeque();
    }
    return cSC4Automaton::Shutdown();
}

// 0x100116632: release each node's occupant (the counted first field) and clear the deque.
void cSC4TrafficAutomaton::ClearLookaheadDeque() { lookahead_.clear(); }

// 0x100114798: temporarily drop the network occupant, switch the populate layer (which SetPath re-resolves through the
// manager) and put the occupant back. The old occupant is kept alive across the two SetPath calls.
void cSC4TrafficAutomaton::SetPopulateLayer(std::uint32_t layer) {
    ITrafficAutomatonOccupant* old = networkOccupant_;
    if (old) old->AddRef();
    const std::uint32_t key = pathKey_;
    SetPath(nullptr, key, nullptr, true);
    populateLayer_ = layer;
    SetPath(old, key, nullptr, true);
    if (old) old->Release();
}

// ---- SetDestination --------------------------------------------------------------------------------------------------
// 0x100117cc4: clear "reached", store the point, then turn on the x/z direction only (the y delta is dropped: the
// original computes dest.y - position.y and then subtracts it from itself). A zero-length direction answers the
// reached event instead. MoveTo gets a null position (heading change only).
void cSC4TrafficAutomaton::SetDestination(const float* xyz) {
    SetAutomataFlag(kAutomataFlagReachedDestination, false);
    if (!xyz) return;
    destination_ = MakeVector(xyz[0], xyz[1], xyz[2]);
    const float dx = destination_.x - position_.x;
    const float dz = destination_.z - position_.z;
    const float length = std::sqrt(dz * dz + dx * dx);
    if (length > 0.0f) {
        const float inverse = 1.0f / length;
        orientation_ = MakeVector(dx * inverse, 0.0f, dz * inverse);
        MoveTo(nullptr, &orientation_.x);
    } else {
        SetAutomataFlag(kAutomataFlagReachedDestination, true);
        SendCallbackMessage(kAutomataEventDestinationReached, 0);
    }
}

// ---- the per-frame step ----------------------------------------------------------------------------------------------
// 0x1001167f2: ease the speed tween, update opacity / animation and, while self controlling, either run the state
// machine or drive (DoPathTick / DoStraightLineTick). When the state machine says the destination is reached the next
// destination is chosen (ChooseNewDestination) or the automaton removes itself.
bool cSC4TrafficAutomaton::DoAnimationTick(std::uint32_t unknown, float milliseconds) {
    (void)unknown;
    if (speedTween_.elapsed < speedTween_.duration) {
        speedTween_.elapsed += milliseconds / 1000.0f;
        if (speedTween_.elapsed >= speedTween_.duration) {
            speedTween_.duration = 0.0f;
            speedTween_.current = speedTween_.desired;
        }
    }
    UpdateOpacity(milliseconds);
    UpdateAnimation(milliseconds);
    std::uint32_t result = 1;
    if (GetSelfControlling()) {
        if (!DoStateTick(milliseconds)) {
            if (!GetReachedDestination()) {
                if (!CheckAutomataFlag(kAutomataFlagIgnorePaths) && networkOccupant_ && pathInfo_ && path_) {
                    result = DoPathTick(milliseconds) ? 1u : 0u;
                } else {
                    result = DoStraightLineTick(milliseconds) ? 1u : 0u;
                }
            }
            if (GetReachedDestination() && !GetMarkedForRemove()) {
                if (!ChooseNewDestination()) Remove(false);
            }
            UpdateAttraction();
        }
    }
    SendCallbackMessage(kAutomataEventTick, 0);
    return result != 0;
}

// 0x10011696a: the normal drive step. A special-route automaton with empty route / lookahead queues either reports the
// route destination or keeps driving. The step is speed * modifier * milliseconds; the safe distance caps it, and a
// leader within 16 units makes the automaton adopt the leader's speed (SetSpeed(leader, 0.5 s)). A non-positive step
// sets the stopped flag and can remove the automaton when it is on a special route. Otherwise the destination point is
// interpolated along the path, clamped to the city and (for non-elevated paths) snapped to the road altitude, then the
// automaton moves there through UpdateOrientation + MoveTo.
bool cSC4TrafficAutomaton::DoPathTick(float milliseconds) {
    cS3DVector3 direction{};
    const bool specialRouteDone = GetSpecialRouteFlag() && route_.empty() && lookahead_.empty();
    if (specialRouteDone) {
        if (CheckAutomataFlag(kTrafficFlagStopped)) {
            HandleReachedRouteDestination();
            return true;
        }
    }
    float distance = GetCurrentSpeed() * GetSpeedModifier() * milliseconds;
    UpdateCollision();
    float safe = GetSafeDistance();
    if (safe < kSafeDistance && leader_) {
        const float leaderSpeed = leader_->GetCurrentSpeed();
        SetSpeed(leaderSpeed, 0.5f);   // 0x100a158bc = 0.5f
    }
    float step = std::min(distance, safe);
    if (step <= 0.0f) {
        distance = step;
        SetAutomataFlag(kTrafficFlagStopped, true);
        const float radius = GetRadius();
        if (distance < -radius) {
            if (!CheckAutomataFlag(kAutomataFlagSpecialRoute)) {
                Remove(false);
                return false;
            }
        }
        return true;
    }
    // InterpolateAlongPath advances the destination point along the current path from the position.
    if (!InterpolateAlongPath(distance, position_, destination_, nullptr, nullptr, nullptr)) {
        SetAutomataFlag(kAutomataFlagReachedDestination, true);
        SendCallbackMessage(kAutomataEventDestinationReached, 0);
    }
    // Clamp the point to the city rectangle (the original clamps x against sizeX - 0.5 and z against sizeZ - 0.5,
    // keeping a negative coordinate at 0).
    cSC4TrafficNetworkMap* map = trafficHost_ ? trafficHost_->TrafficNetworkMap() : nullptr;
    const float sizeX = trafficHost_ ? trafficHost_->CitySizeX() : 0.0f;
    const float sizeZ = trafficHost_ ? trafficHost_->CitySizeZ() : 0.0f;
    if (destination_.x < 0.0f) destination_.x = 0.0f;
    else if (destination_.x > sizeX - 0.5f) destination_.x = sizeX - 0.5f;
    if (destination_.z < 0.0f) destination_.z = 0.0f;
    else if (destination_.z > sizeZ - 0.5f) destination_.z = sizeZ - 0.5f;
    const float dx = destination_.x - position_.x;
    const float dz = destination_.z - position_.z;
    if (std::sqrt(dz * dz + dx * dx) <= kDestinationEpsilon) {
        MoveTo(&destination_.x, nullptr);
        SetAutomataFlag(kTrafficFlagStopped, true);
    } else {
        SetAutomataFlag(kTrafficFlagStopped, false);
        if (pathInfo_ && networkOccupant_ && !pathInfo_->UseElevation()) {
            destination_.y = networkOccupant_->GetAltitude(destination_.x, destination_.z);
        }
        UpdateOrientation(destination_, &direction.x);
        MoveTo(&destination_.x, &direction.x);
    }
    (void)map;
    return true;
}

// 0x1001176d8: drive straight at the destination (a path-less automaton, or the ignore-paths flag). The original uses
// the 2D length of the destination delta, the collide-candidate / GetCollidingAutomaton pair to keep the speed at 0,
// and the city bounds; the move is constrained onto the network (ConstrainVectorToNetworkTile, flags 0xc49 or 2 for
// trains) unless ignore-roads (0x20000) is set.
bool cSC4TrafficAutomaton::DoStraightLineTick(float milliseconds) {
    cS3DVector3 next{};
    cS3DVector3 direction{};
    float targetX = destination_.x;
    float targetY = destination_.y;
    float targetZ = destination_.z;
    if (targetX < 0.0f || targetY < 0.0f || targetZ < 0.0f) {
        targetX = position_.x;
        targetY = position_.y;
        targetZ = position_.z;
        destination_ = position_;
    }
    const float dx = targetX - position_.x;
    const float dz = targetZ - position_.z;
    const float length = std::sqrt(dz * dz + dx * dx);
    float step = GetCurrentSpeed() * GetSpeedModifier() * milliseconds;
    if (length <= 0.0f || step <= 0.0f) {
        SetAutomataFlag(kTrafficFlagStopped, true);
        SetAutomataFlag(kAutomataFlagReachedDestination, true);
        SendCallbackMessage(kAutomataEventDestinationReached, 0);
        return true;
    }
    float limit = length;
    if (CheckAutomataFlag(kAutomataFlagCollideCandidate) && GetCollidingAutomaton(0.0f) != nullptr) limit = 0.0f;
    const float inverse = 1.0f / length;
    const float moveX = dx * inverse * step;
    const float moveZ = dz * inverse * step;
    const float moveLength = std::sqrt(moveZ * moveZ + moveX * moveX);
    if (limit <= moveLength) {
        SetAutomataFlag(kTrafficFlagStopped, true);
        SetAutomataFlag(kAutomataFlagReachedDestination, true);
        SendCallbackMessage(kAutomataEventDestinationReached, 0);
        return true;
    }
    SetAutomataFlag(kTrafficFlagStopped, false);
    next = MakeVector(position_.x + moveX, position_.y, position_.z + moveZ);
    if (next.x < 0.0f || next.z < 0.0f) return false;
    if (trafficHost_ && (trafficHost_->CitySizeX() <= next.x || trafficHost_->CitySizeZ() <= next.z)) return false;
    UpdateOrientation(next, &direction.x);
    cSC4TrafficNetworkMap* map = trafficHost_ ? trafficHost_->TrafficNetworkMap() : nullptr;
    if (map && !CheckAutomataFlag(kAutomataFlagIgnoreRoads)) {
        const std::uint32_t flags = IsTrain() ? 2u : 0xc49u;
        if (!map->ConstrainVectorToNetworkTile(position_, next, flags)) {
            Remove(false);
        } else {
            next.y = map->GetRoadAltitude(next.x, next.z);
        }
    }
    MoveTo(&next.x, &direction.x);
    return true;
}

// 0x1001172f8: the state tick. Only the station-stop (0x1e) and the stop-sign / stoplight wait (0xd) have traffic
// specific ticks; everything else is the base state machine.
bool cSC4TrafficAutomaton::DoStateTick(float milliseconds) {
    tAutomataState* state = GetState();
    if (state) {
        if (state->stateType == kTrafficStateStopAtStation) return DoStopAtStationTick(milliseconds);
        if (state->stateType == kTrafficStateStopSign) return DoStopSignTick(milliseconds);
    }
    return cSC4Automaton::DoStateTick(milliseconds);
}

// 0x10011733e: while the speed tween's current value is positive, keep driving; then the base state tick. Always true.
bool cSC4TrafficAutomaton::DoStopAtStationTick(float milliseconds) {
    float speed;
    if (speedTween_.duration <= 0.0f) {
        speed = speedTween_.desired;
    } else {
        const float ratio = speedTween_.elapsed / speedTween_.duration;
        float clamped = 1.0f;
        if (ratio <= 1.0f) clamped = ratio;
        clamped = ratio < 0.0f ? 0.0f : clamped;
        speed = clamped * (speedTween_.desired - speedTween_.current) + speedTween_.current;
    }
    if (speed > 0.0f) DoPathTick(milliseconds);
    cSC4Automaton::DoStateTick(milliseconds);
    return true;
}

// 0x1001173cc: wait for the stoplight. The state timer is decreased by the speed modifier * milliseconds while the
// stopped flag is set; when it runs out (or was never set) the default speed returns, the waiting / stopped flags clear
// and the intersection is queried again. A zero answer pops the state; the tick still answers false.
bool cSC4TrafficAutomaton::DoStopSignTick(float milliseconds) {
    tAutomataState* state = GetState();
    if (!state) return false;
    if (state->timer > 0.0f) {
        if (!CheckAutomataFlag(kTrafficFlagStopped)) return false;
        const float modifier = GetSpeedModifier();
        state->timer -= modifier * milliseconds;
        if (state->timer > 0.0f) return true;
    }
    ReturnToDefaultSpeed();
    SetAutomataFlag(kTrafficFlagStopped, false);
    SetAutomataFlag(kAutomataFlagWaitBit, false);
    cSC4TrafficNetworkMap* map = trafficHost_ ? trafficHost_->TrafficNetworkMap() : nullptr;
    float time = 0.0f;
    const int result = map ? map->CheckIntersection(networkOccupant_, this, pathKey_, time) : 0;
    if (result == 0) PopState();
    return false;
}

// ---- state entry / exit ----------------------------------------------------------------------------------------------
// 0x100116f3e. State 0x10 sets the driving state up, runs the base enter handler, then copies the tunnel occupant and
// clears the per-record hint; the base enter handler then runs a second time (quirk kept). State 0x1e slows to a stop
// over half a second. Every other state only runs the base handler.
bool cSC4TrafficAutomaton::HandleEnterState(tAutomataState* state) {
    if (state) {
        if (state->stateType == kTrafficStateDrive) {
            StartDrivingState();
            cSC4Automaton::HandleEnterState(state);
            ITrafficAutomatonOccupant* occupant = networkOccupant_;
            if (occupant && occupant->HasAnyNetworkFlag(kAutomataFlagTunnelOccupantFlags)) {
                if (occupant->HasNetworkFlag(kTrafficNetworkFlagTunnel) && tunnelOccupant_ != occupant) {
                    occupant->AddRef();
                    if (tunnelOccupant_) tunnelOccupant_->Release();
                    tunnelOccupant_ = occupant;
                }
                drivingFlag254_ = 0;
                if (auto* drive = dynamic_cast<tAutomataStatePlayerDrive*>(state)) drive->history[4] = -1.0f;
            }
        } else if (state->stateType == kTrafficStateStopAtStation) {
            SetSpeed(0.0f, 0.5f);
        }
    }
    return cSC4Automaton::HandleEnterState(state);
}

// 0x10011711e. 0x10 tears the driving state down (unless the automaton is being removed it first clears ignore-paths
// and snaps back onto the path) and then runs the base exit; 0xd and 0x1e clear the waiting / stopped flags and restore
// the default speed (0x1e also runs the base exit); 0x15 snaps back onto the path. The others go to the base.
bool cSC4TrafficAutomaton::HandleExitState(const tAutomataState* state) {
    if (!state) return cSC4Automaton::HandleExitState(nullptr);
    const std::uint32_t type = state->stateType;
    if (type < kTrafficStatePullOver) {
        if (type == kTrafficStateStopSign) {
            ReturnToDefaultSpeed();
            SetAutomataFlag(kAutomataFlagWaitBit, false);
            SetAutomataFlag(kTrafficFlagStopped, false);
            return true;
        }
        if (type == kTrafficStateDrive) {
            if (!CheckAutomataFlag(kAutomataFlagMarkedForRemove)) {
                SetAutomataFlag(kAutomataFlagIgnorePaths, false);
                SnapToPath();
            }
            StopDrivingState();
            return cSC4Automaton::HandleExitState(state);
        }
        return cSC4Automaton::HandleExitState(state);
    }
    if (type == kTrafficStatePullOver) {
        if (!CheckAutomataFlag(kAutomataFlagMarkedForRemove)) SnapToPath();
        return true;
    }
    if (type != kTrafficStateStopAtStation) return cSC4Automaton::HandleExitState(state);
    ReturnToDefaultSpeed();
    SetAutomataFlag(kAutomataFlagWaitBit, false);
    SetAutomataFlag(kTrafficFlagStopped, false);
    cSC4Automaton::HandleExitState(state);
    return true;
}

// The effect / particle / queue half of HandleEnterState for state 0x10 (0x100116f8c..0x10011707e).
void cSC4TrafficAutomaton::StartDrivingState() {
    ITrafficAutomataManager* manager = Manager();
    if (manager && manager->EffectManager()) {
        drivingEffect78_ = manager->EffectManager()->CreateEffect(0, 0, 5, AsOccupant());
    }
    if (trafficHost_) {
        particleSystem_ = trafficHost_->CreateParticleSystem();
        if (particleSystem_) particle_ = particleSystem_->AddParticle(position_);
    }
    SetAutomataFlag(kAutomataFlagSpecialRoute, false);
    route_.clear();
    lookahead_.clear();
}

// The cleanup half of HandleExitState for state 0x10 (0x1001171xx). The original keeps these pointers in the driving
// record; the record's destructor releases the tunnel occupant, so the port releases it here (see the header note).
void cSC4TrafficAutomaton::StopDrivingState() {
    ITrafficAutomataManager* manager = Manager();
    if (drivingEffect78_) {
        if (manager && manager->EffectManager()) manager->EffectManager()->StopEffect(drivingEffect78_);
        drivingEffect78_ = nullptr;
    }
    if (drivingEffect80_) {
        if (manager && manager->EffectManager()) manager->EffectManager()->StopEffect(drivingEffect80_);
        drivingEffect80_ = nullptr;
    }
    if (tunnelAttachment_) {
        tunnelAttachment_->Detach(false);
        tunnelAttachment_->Release();
        tunnelAttachment_ = nullptr;
    }
    if (particleSystem_) {
        if (particle_) {
            particleSystem_->DeleteParticle(particle_);
            particle_ = nullptr;
        }
        particleSystem_->Shutdown();
        particleSystem_ = nullptr;
    }
    if (tunnelOccupant_) {
        tunnelOccupant_->Release();
        tunnelOccupant_ = nullptr;
    }
}

// ---- collision / intersection ----------------------------------------------------------------------------------------
// 0x100116cb6. A collide candidate whose leader either is marked for removal or is being waited on re-acquires the
// leader (FindLeader). The 0x800 flag or an invisible occupant with the force-collision flag clears the wait flag and
// (without a leader) restores the default speed. Behaviour 0xd is the stoplight wait and returns at once. Otherwise the
// intersection is measured: an intersection path info answers 1.7 s, else the map's CheckIntersection does; a non-zero
// answer sets the wait flag and (for stoplights, while stopped) pushes the 0xd state with the junction duration in
// milliseconds before slowing down. A cleared / short wait restores the default speed, otherwise the automaton slows.
void cSC4TrafficAutomaton::UpdateCollision() {
    cSC4TrafficNetworkMap* map = trafficHost_ ? trafficHost_->TrafficNetworkMap() : nullptr;
    if (CheckAutomataFlag(kAutomataFlagCollideCandidate) && leader_ &&
        (leader_->CheckAutomataFlag(kAutomataFlagMarkedForRemove) ||
         (CheckAutomataFlag(kAutomataFlagWaitBit)))) {
        SetLeader(FindLeader());
    }
    if (CheckAutomataFlag(kAutomataFlagNoJunctionBit) ||
        ((cSC4Occupant::GetFlags() & kOccupantFlagVisible) == 0 &&
         CheckAutomataFlag(kAutomataFlagForceCollision))) {
        SetAutomataFlag(kAutomataFlagWaitBit, false);
        if (leader_) return;
        ReturnToDefaultSpeed();
        return;
    }
    if (GetBehavior() == kTrafficStateStopSign) return;
    float time = 0.0f;
    if (!IsTrain() && pathInfo_ && pathInfo_->IsIntersection((pathKey_ >> 24) & 0xf)) {
        time = 1.7f;
    } else if (map) {
        const int result = map->CheckIntersection(networkOccupant_, this, GetPathKey(), time);
        if (result != 0) {
            SetAutomataFlag(kAutomataFlagWaitBit, true);
            if (result == 1 && CheckAutomataFlag(kTrafficFlagStopped)) {
                PushState(kTrafficStateStopSign, nullptr, Tuning().junctionDuration * 1000.0f, false, 0, 0, nullptr);
            }
            SlowDownForStopLight();
            return;
        }
    }
    if (!CheckAutomataFlag(kAutomataFlagWaitBit) || 0.7f <= time) {
        SetAutomataFlag(kAutomataFlagWaitBit, false);
        ReturnToDefaultSpeed();
        return;
    }
    SlowDownForStopLight();
}

// 0x1001174e2. Walks the leader chain while every automaton is waiting and stopped, counting the ones that are not
// marked for removal and remembering the last traffic automaton. Fewer than three followers, no traffic automaton or a
// follower without a network occupant answer true. Otherwise the manager's layer density is scaled by the lane weight
// at (lane * 4 + edge) and by 10, rounded half away from zero with the float->int magic bias (12582912.0f), and the
// count must not exceed it.
bool cSC4TrafficAutomaton::CheckIntersectionDensity() {
    ITrafficAutomataManager* manager = Manager();
    bool result = true;
    if (!manager) return result;
    std::uint32_t count = 0;
    cSC4Automaton* traffic = nullptr;
    cSC4Automaton* current = this;
    while (true) {
        if (!current->CheckAutomataFlag(kAutomataFlagWaitBit)) break;
        if (!current->CheckAutomataFlag(kTrafficFlagStopped)) break;
        count += current->CheckAutomataFlag(kAutomataFlagMarkedForRemove) ? 0u : 1u;
        if (manager->IsTrafficAutomaton(current)) traffic = current;
        current = current->GetLeader();
        if (!current || count > 0x31) break;
    }
    if (!traffic || count < 3) return true;
    auto* follower = dynamic_cast<cSC4TrafficAutomaton*>(traffic);
    if (!follower) return true;
    const double density = manager->GetLayerDensity(populateLayer_);
    ITrafficAutomatonOccupant* occupant = follower->GetNetworkOccupant();
    if (!occupant) return true;
    TrafficTileInfo* tile = nullptr;
    cSC4TrafficNetworkMap* map = trafficHost_ ? trafficHost_->TrafficNetworkMap() : nullptr;
    if (!map) return true;
    tile = map->GetTileInfoByNetwork(occupant);
    manager->SetCurrentLayer(populateLayer_);
    float weights[16] = {};
    manager->ComputeEdgeWeights(occupant, tile, populateLayer_, weights);
    // The original indexes the weight table with the *entry* byte (in AH after the first GetPathKey) * 4 plus the
    // second GetPathKey's low byte (the exit edge).
    const std::uint32_t key = follower->GetPathKey();
    const std::uint32_t entry = (key >> 8) & 0xff;
    const std::uint32_t exit = key & 0xff;
    const float merged = static_cast<float>(density * static_cast<double>(weights[entry * 4 + exit])) * 10.0f;
    // 0x10011766f..0x1001176a7: the original adds -0.5f and 12582912.0f (1.5 * 2^23), then treats the float's bit
    // pattern as the integer k = floor(merged), adds 0xb4c00001 (wrapping to k + 1) and keeps k + 1 when its float
    // value equals merged (an integral merged), else k. NaN keeps k. The count compares as an unsigned >=.
    const float biased = merged + -0.5f + 12582912.0f;
    std::uint32_t bits = 0;
    std::memcpy(&bits, &biased, sizeof(bits));
    const std::int32_t low = static_cast<std::int32_t>(bits - 0x4b400000u);
    const std::int32_t high = static_cast<std::int32_t>(bits + 0xb4c00001u);
    const float roundTrip = static_cast<float>(high);
    std::int32_t rounded = low;
    if (!std::isnan(merged) && merged == roundTrip) rounded = high;
    result = static_cast<std::uint32_t>(rounded) >= count;   // `cmp %r12d(count), %eax(rounded)` + setae
    return result;
}

// ---- path / lane selection -------------------------------------------------------------------------------------------
// 0x100117dbe: (re)attach a network occupant, its path info and a path; leave the old tile and enter the new one in the
// traffic network map, and run the tunnel opacity / audio transition when the occupant changes.
void cSC4TrafficAutomaton::SetPath(ITrafficAutomatonOccupant* occupant, std::uint32_t pathKey,
                                   const std::vector<cS3DVector3>* path, bool keepDestination) {
    ITrafficAutomatonOccupant* old = networkOccupant_;
    if (old) old->AddRef();
    const std::uint32_t oldKey = GetPathKey();
    if (networkOccupant_ != occupant) {
        if (occupant) occupant->AddRef();
        ITrafficAutomatonOccupant* replaced = networkOccupant_;
        networkOccupant_ = occupant;
        if (replaced) replaced->Release();
    }
    cISC4PathInfo* pathInfo = nullptr;
    const std::vector<cS3DVector3>* points = path;
    if (occupant) {
        pathInfo = occupant->GetPathInfoObject();
        if (pathInfo) {
            pathInfo->AddRef();
            if (!points) {
                const tPath* found = pathInfo->GetPath(pathKey, nullptr);
                if (found) points = &found->points;
            }
        }
    }
    if (!points || !pathInfo) {
        path_ = nullptr;
        if (pathInfo_) {
            cISC4PathInfo* replaced = pathInfo_;
            pathInfo_ = nullptr;
            replaced->Release();
        }
        pathIndex_ = 0;
    } else {
        pathKey_ = pathKey;
        if (pathInfo_ != pathInfo) {
            pathInfo->AddRef();
            cISC4PathInfo* replaced = pathInfo_;
            pathInfo_ = pathInfo;
            if (replaced) replaced->Release();
        }
        path_ = points;
        pathIndex_ = 0;
        if (keepDestination) {
            SetAutomataFlag(kAutomataFlagReachedDestination, false);
        } else {
            // The original uses points[0] and points[1] (even when the path has a single point); guarded here.
            cS3DVector3 start = points->at(0);
            OffsetPathPoint(start);
            if (networkOccupant_ && !pathInfo_->UseElevation()) {
                start.y = networkOccupant_->GetAltitude(start.x, start.z);
            }
            cS3DVector3 end = points->size() > 1 ? points->at(1) : points->at(0);
            OffsetPathPoint(end);
            if (networkOccupant_ && !pathInfo_->UseElevation()) {
                end.y = networkOccupant_->GetAltitude(end.x, end.z);
            }
            MoveTo(&start.x, nullptr);
            SetDestination(&end.x);
        }
    }
    // The tunnel transition: the original tests the occupant's network flag 0x80000 and then queries the tunnel
    // interface (iid 0x8a4bd52b); the port treats the flag as the tunnel test.
    if (networkOccupant_ != old) {
        const bool newTunnel = networkOccupant_ && networkOccupant_->HasNetworkFlag(kTrafficNetworkFlagTunnel);
        const bool oldTunnel = old && old->HasNetworkFlag(kTrafficNetworkFlagTunnel);
        if (newTunnel && !oldTunnel) {
            SetOpacity(1.0f, 0.1f);
            if (FindState(kTrafficStateDrive) && trafficHost_) trafficHost_->StartAudioEvent(kAudioEventTunnel);
        } else if (!newTunnel && oldTunnel) {
            SetOpacity(0.0f, 0.8f);
            if (FindState(kTrafficStateDrive) && trafficHost_) trafficHost_->StopAudioEvent(kAudioEventTunnel);
        }
    }
    cSC4TrafficNetworkMap* map = trafficHost_ ? trafficHost_->TrafficNetworkMap() : nullptr;
    if (map) {
        if (old) map->ExitNetwork(this, old, oldKey);
        if (networkOccupant_) {
            ReturnToDefaultSpeed();
            map->EnterNetwork(this, networkOccupant_);
            if (GetDetectCollisions()) SetLeader(FindLeader());
        }
    }
    if (old) old->Release();
    if (pathInfo) pathInfo->Release();
}

// 0x1001182e4: snap the automaton onto the path of the network occupant under its position. Gives up on the
// "uninitialized" position sentinel (the original's kUninit3DVector words). The attraction vector (GetChaseVector, or
// the attraction controller's direction when the automaton is not ignoring attractors) picks the entry edge
// (GetEdgeFromVector); GetBestPath then finds the path key, SnapPointToPath the index, and the first / last segment
// gives the heading (the fallback is the current orientation).
bool cSC4TrafficAutomaton::SnapToPath() {
    const std::uint32_t sentinelX = kTrafficUninitPositionBits[0];
    const std::uint32_t sentinelY = kTrafficUninitPositionBits[1];
    const std::uint32_t sentinelZ = kTrafficUninitPositionBits[2];
    if (std::fabs(position_.x - FloatFromBits(sentinelX)) < 0.0001f &&
        std::fabs(position_.y - FloatFromBits(sentinelY)) < 0.0001f &&
        std::fabs(position_.z - FloatFromBits(sentinelZ)) < 0.0001f) {
        return false;
    }
    cSC4TrafficNetworkMap* map = trafficHost_ ? trafficHost_->TrafficNetworkMap() : nullptr;
    ITrafficAutomataManager* manager = Manager();
    if (!map || !manager) return false;
    int cellX = 0;
    int cellZ = 0;
    if (trafficHost_) trafficHost_->PositionToCell(position_.x, position_.z, cellX, cellZ);
    ITrafficAutomatonOccupant* occupant = AsTrafficOccupant(map->FindNetworkOccupant(cellX, cellZ));
    if (!occupant) return false;
    cISC4PathInfo* pathInfo = occupant->GetPathInfoObject();
    if (!pathInfo) return false;
    const std::uint32_t travelType = manager->GetTravelType(this);
    std::uint8_t entry = kPathEdgeWildcard;
    bool haveAttraction = false;
    if (GetChaseVector(attraction_)) {
        attraction_ = Scale(attraction_, 1.0f / Length3(attraction_));
        haveAttraction = true;
    } else if (attractionController_ && !CheckAutomataFlag(kAutomataFlagIgnoreAttractors)) {
        const float length = Length3(attraction_);
        if (length > kPathEpsilon) {
            attraction_ = Scale(attraction_, 1.0f / length);
            haveAttraction = true;
        }
    }
    if (haveAttraction) GetEdgeFromVector(occupant, attraction_, entry, true);
    std::uint32_t key = 0;
    const std::vector<cS3DVector3>* points = nullptr;
    {
        const tPath* found = pathInfo->GetBestPath(travelType, kPathEdgeWildcard, entry, position_, &key, nullptr);
        if (found) points = &found->points;
    }
    if (!points) return false;
    SetPath(occupant, key, points, true);
    cS3DVector3 snapped = position_;
    const int index = pathInfo->SnapPointToPath(snapped, points);
    pathIndex_ = index >= 0 ? static_cast<std::uint32_t>(index) : 0;
    cS3DVector3 direction = orientation_;
    if (points->size() > 1) {
        direction = Sub(points->at(1), points->at(0));
        if (Length3(direction) < kPathEpsilon) direction = Sub(points->back(), points->front());
        if (Length3(direction) < kPathEpsilon) direction = orientation_;
    }
    const float length = Length3(direction);
    if (length > 0.0f) direction = Scale(direction, 1.0f / length);
    MoveTo(&snapped.x, &direction.x);
    return true;
}

// 0x100117a96: walk the path from `from`, offsetting every point with OffsetPathPoint, and advance `out` by `distance`.
// The index (or the automaton's own path index) is advanced as segments are consumed. Answers true while the point
// stays inside a segment; at the last segment it writes the remaining distance (before the move) and answers false.
bool cSC4TrafficAutomaton::InterpolateAlongPath(float distance, const cS3DVector3& from, cS3DVector3& out,
                                                std::int32_t* index, const std::vector<cS3DVector3>* path,
                                                float* remaining) {
    if (remaining) *remaining = 0.0f;
    const std::vector<cS3DVector3>* points = path ? path : path_;
    if (!points || points->empty()) return false;
    std::int32_t cursor = index ? *index : static_cast<std::int32_t>(pathIndex_);
    out = from;
    const std::size_t count = points->size();
    if (cursor < 0) {
        cursor = 0;
    } else if (static_cast<std::size_t>(cursor) >= count - 1) {
        if (remaining) *remaining = distance;
        if (index) *index = cursor; else pathIndex_ = static_cast<std::uint32_t>(cursor);
        return false;
    }
    while (static_cast<std::size_t>(cursor) < count - 1) {
        cS3DVector3 target = points->at(static_cast<std::size_t>(cursor) + 1);
        OffsetPathPoint(target);
        cS3DVector3 delta = Sub(target, out);
        const float segment = Length3(delta);
        if (segment > kPathEpsilon) delta = Scale(delta, 1.0f / segment);
        if (distance <= segment) {
            out = Add(out, Scale(delta, distance));
            if (index) *index = cursor; else pathIndex_ = static_cast<std::uint32_t>(cursor);
            return true;
        }
        ++cursor;
        if (index) *index = cursor; else pathIndex_ = static_cast<std::uint32_t>(cursor);
        if (static_cast<std::size_t>(cursor) >= count - 1) {
            if (distance <= 0.0f) return false;
            if (segment > 0.0f) {
                if (remaining) *remaining = distance - segment;
                out = Add(out, Scale(delta, distance));
            }
            return false;
        }
        out = target;
        distance -= segment;
    }
    return false;
}

// 0x100119f70: shift a path point sideways by the path offset (plus the behavior template's +0x64 offset). The original
// builds a 4x3 basis from the path tangent and multiplies it; the disassembly of 0x100119fd0..0x10011a24c reduces to
// the perpendicular (cos(yaw) * sign, 0, -sin(yaw) * sign), where yaw = atan2(-dx, -dz) and the sign is the side of
// the diagonal (dx + dz) / sqrt(2). The tangent and the diagonal are not guarded in the original.
void cSC4TrafficAutomaton::OffsetPathPoint(cS3DVector3& point) {
    float offset = pathOffset_;
    tAutomataState* state = GetState();
    if (state && state->runtime30 != 0) {
        offset += reinterpret_cast<const tBehaviorTemplate*>(state->runtime30)->pathOffset;
    }
    if (!(std::fabs(offset) >= 0.0001f)) return;
    if (!path_ || path_->size() < 2) return;
    const cS3DVector3& first = path_->front();
    const cS3DVector3& last = path_->back();
    const float dx = last.x - first.x;
    const float dz = last.z - first.z;
    const float inverse = 1.0f / std::sqrt(dx * dx + dz * dz);
    const float nx = dx * inverse;
    const float nz = dz * inverse;
    const float yaw = std::atan2(-nx, -nz);   // SC4ViewUtilities::GetYawAngle2D @ 0x1001771b2
    const float sine = std::sin(yaw);
    const float cosine = std::cos(yaw);
    const float diagonal = kDiagonal * (nx + nz);
    const float sign = diagonal < 0.0f ? FloatFromBits(kTurnTableSign[0]) : FloatFromBits(kTurnTableSign[1]);
    point.x += cosine * sign * offset;
    point.z += -sine * sign * offset;
}

// ---- direction helpers -----------------------------------------------------------------------------------------------
// 0x100119eec: unit vector from the position to `target`; a zero-length delta answers (0, 0, 1).
void cSC4TrafficAutomaton::UpdateOrientation(const cS3DVector3& target, float* out) const {
    const float dx = target.x - position_.x;
    const float dy = target.y - position_.y;
    const float dz = target.z - position_.z;
    const float length = std::sqrt(dz * dz + dx * dx + dy * dy);
    if (length <= kPathEpsilon) {
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = 1.0f;
        return;
    }
    const float inverse = 1.0f / length;
    out[0] = dx * inverse;
    out[1] = dy * inverse;
    out[2] = dz * inverse;
}

// ---- speed -----------------------------------------------------------------------------------------------------------
// 0x10011a580: SetSpeed(0, (1 / modifier) * 1.5), or 0.1 s when there is no modifier.
void cSC4TrafficAutomaton::SlowDownForStopLight() {
    const float modifier = GetSpeedModifier();
    const float seconds = modifier <= 0.0f ? 0.1f : (1.0f / modifier) * 1.5f;
    SetSpeed(0.0f, seconds);
}

// 0x10011a5d0: SetSpeed(default speed, (1 / modifier) * 0.3), or 0.1 s when there is no modifier.
void cSC4TrafficAutomaton::ReturnToDefaultSpeed() {
    const float modifier = GetSpeedModifier();
    const float seconds = modifier <= 0.0f ? 0.1f : (1.0f / modifier) * 0.3f;
    SetSpeed(GetDefaultSpeed(), seconds);
}

// ---- leader / collision helpers --------------------------------------------------------------------------------------
// 0x10011a2e8: the distance the automaton may still travel safely. With a path and the waiting flag (0x10) it is the
// distance from the position to the path's last point minus the radius (floored at 0). With a leader and collision
// detection it is the leader's collide position distance minus the radius and 3 (the original adds -3.0f). The result
// is the smaller of the two; the original starts both at 16.
float cSC4TrafficAutomaton::GetSafeDistance() {
    cS3DVector3 position{};
    GetPosition(&position.x);
    // Both candidates start at the original's 16-unit default and the smaller one wins (the leader term therefore caps
    // a path-only answer at 16 when there is no leader).
    float pathSafe = kSafeDistance;
    if (path_ && CheckAutomataFlag(kAutomataFlagWaitBit)) {
        const cS3DVector3& last = path_->back();
        const float distance = Distance3(last, position) - GetRadius();
        pathSafe = distance < 1.0f ? 0.0f : distance;
    }
    float leaderSafe = kSafeDistance;
    if (leader_ && GetDetectCollisions()) {
        cS3DVector3 collide{};
        leader_->GetCollidePosition(collide);
        collide = Sub(collide, position);
        leaderSafe = Length3(collide) - GetRadius() - 3.0f;
    }
    return std::min(pathSafe, leaderSafe);
}

// 0x10011932e: keep the automaton alive while it has a cell lifespan, its layer is not "traffic" (the original's
// (layer - 7) > 1 test), it is forced to collide, or its occupant is a road (network type 2).
bool cSC4TrafficAutomaton::ShouldKeepAlive() const {
    if (cellLifespan_ == 0 && (populateLayer_ - 7u) > 1u) {
        if (!CheckAutomataFlag(kAutomataFlagForceCollision)) {
            if (networkOccupant_ && networkOccupant_->IsOfType(2)) return true;
            return false;
        }
    }
    return true;
}

cSC4Automaton* cSC4TrafficAutomaton::FindLeader() {
    if (lookahead_.size() < 2) {
        if (!networkOccupant_) return nullptr;
        cSC4Automaton* leader = FindLeaderInTile(networkOccupant_, pathKey_, 0xfff00ffu, 0xfffff00u);
        if (leader) return leader;
        cSC4TrafficNetworkMap* map = trafficHost_ ? trafficHost_->TrafficNetworkMap() : nullptr;
        if (!map) return nullptr;
        const std::uint32_t key = pathKey_;
        const std::uint32_t type = (key >> 24) & 0xf;
        TrafficTileInfo* tile = map->GetTileInfoAtNeighborEdge(networkOccupant_, static_cast<std::uint8_t>(key), false, type);
        if (!tile || !tile->occupant) return nullptr;
        return FindLeaderInTile(AsTrafficOccupant(tile->occupant),
                                type * 0x1000000u + 0xffu + ((key * 0x100u + 0x200u) & 0x300u), 0xf00ff00u,
                                0xf00ff00u);
    }
    for (std::deque<tLookaheadNode>::const_iterator it = lookahead_.begin(); it != lookahead_.end(); ++it) {
        if (!it->occupant) continue;
        cSC4Automaton* leader = FindLeaderInTile(it->occupant.get(), it->key, 0xfff00ffu, 0xfffff00u);
        if (leader) return leader;
    }
    return nullptr;
}

// 0x100115ce8: the nearest automaton in `occupant`'s tile that is not marked for removal, passes ShouldCollide and
// either ignores paths or shares (key & maskA) / (key & maskB) with `key`'s direction bits. When the tile belongs to
// the automaton's own occupant the scan starts at the automaton itself (quirk: the automata behind it are skipped).
// The distance is 2D (x/z) from the automaton's position, using the automaton's own y for every candidate.
cSC4Automaton* cSC4TrafficAutomaton::FindLeaderInTile(ITrafficAutomatonOccupant* occupant, std::uint32_t key,
                                                      std::uint32_t maskA, std::uint32_t maskB) {
    cSC4TrafficNetworkMap* map = trafficHost_ ? trafficHost_->TrafficNetworkMap() : nullptr;
    if (!map || !occupant) return nullptr;
    TrafficTileInfo* tile = map->GetTileInfoByNetwork(occupant);
    if (!tile) return nullptr;
    const cS3DVector3& position = GetPathPos();
    std::list<ITrafficNetworkMapAutomaton*>::const_iterator it = tile->automata.begin();
    if (networkOccupant_ == occupant) {
        while (it != tile->automata.end() && AsTrafficPeer(*it) != this) ++it;
        if (it == tile->automata.end()) return nullptr;
    }
    float best = FLT_MAX;
    cSC4Automaton* bestAutomaton = nullptr;
    for (; it != tile->automata.end(); ++it) {
        ITrafficAutomatonPeer* peer = AsTrafficPeer(*it);
        cSC4Automaton* automaton = peer->AsAutomaton();
        if (automaton->GetMarkedForRemove()) continue;
        if (!ShouldCollide(automaton)) continue;
        const std::uint32_t otherKey = peer->GetPathKey();
        if (!(automaton->CheckAutomataFlag(kAutomataFlagIgnorePaths) || ((otherKey ^ key) & maskA) == 0 ||
              ((otherKey ^ key) & maskB) == 0)) {
            continue;
        }
        float other[3] = {0.0f, 0.0f, 0.0f};
        automaton->GetPosition(other);
        other[1] = position.y;
        const float dx = other[0] - position.x;
        const float dz = other[2] - position.z;
        const float distance = std::sqrt(dz * dz + dx * dx);
        if (distance < best) {
            best = distance;
            bestAutomaton = automaton;
        }
    }
    return bestAutomaton;
}

// 0x10011a640: remove the neighbouring automata of the same occupant type in `occupant`'s tile (the signal-change
// cleanup) and recurse into the four neighbour tiles while `depth` is non-negative. The automaton itself, special-route
// automata, non-self-controlling ones, removals, driving-state automata and forced collisions are skipped.
void cSC4TrafficAutomaton::RemoveSurroundingAutomata(ITrafficAutomatonOccupant* occupant, int depth) {
    cSC4TrafficNetworkMap* map = trafficHost_ ? trafficHost_->TrafficNetworkMap() : nullptr;
    if (!map) return;
    ITrafficAutomatonOccupant* target = occupant;
    if (!target) {
        target = networkOccupant_;
        if (CheckAutomataFlag(kAutomataFlagIgnorePaths)) {
            int cellX = 0;
            int cellZ = 0;
            if (trafficHost_) trafficHost_->PositionToCell(position_.x, position_.z, cellX, cellZ);
            target = AsTrafficOccupant(map->FindNetworkOccupant(cellX, cellZ));
        }
        if (!target) return;
    }
    TrafficTileInfo* tile = map->GetTileInfoByNetwork(target);
    if (!tile) return;
    for (ITrafficNetworkMapAutomaton* mapAutomaton : tile->automata) {
        ITrafficAutomatonPeer* peer = AsTrafficPeer(mapAutomaton);
        cSC4Automaton* other = peer->AsAutomaton();
        if (other == this) continue;
        if (peer->CheckAutomataFlag(kAutomataFlagSpecialRoute)) continue;
        if (!peer->CheckAutomataFlag(kAutomataFlagSelfControlling)) continue;
        if (other->GetMarkedForRemove()) continue;
        if (peer->FindState(kTrafficStateDrive)) continue;
        if (peer->CheckAutomataFlag(kAutomataFlagForceCollision)) continue;
        if (other->GetType() == GetType()) {
            peer->Remove(false);
            peer->SetOpacity(0.0f, Tuning().fadeDuration * 0.25f);
        }
    }
    if (depth < 0) return;
    for (std::uint8_t edge = 0; edge < 4; ++edge) {
        TrafficTileInfo* neighbor = map->GetTileInfoAtNeighborEdge(target, edge, false, 0);
        if (neighbor && neighbor->occupant) RemoveSurroundingAutomata(AsTrafficOccupant(neighbor->occupant), depth - 1);
    }
}

// ---- lookahead / route selection -------------------------------------------------------------------------------------
// 0x1001187dc: choose the next destination when the current route ends. Requires the manager, a network occupant and
// no ignore-paths flag. The lookahead deque's front occupant reference is dropped, a dead end removes the automaton,
// then ChooseBestPath runs for every remaining node (rewriting the node keys) and the front node starts the next route.
// The cell lifespan is decremented. Answers whether any node produced a path.
bool cSC4TrafficAutomaton::ChooseNewDestination() {
    if (!Manager() || !networkOccupant_ || CheckAutomataFlag(kAutomataFlagIgnorePaths)) return false;
    ChooseLookaheadTiles();
    if (lookahead_.empty()) {
        Remove(false);
        return false;
    }
    tLookaheadNode& front = lookahead_.front();
    front.occupant = nullptr;
    if (front.deadEnd) {
        Remove(false);
        lookahead_.pop_front();
        return false;
    }
    lookahead_.pop_front();
    if (lookahead_.empty()) {
        Remove(false);
        return false;
    }
    cS3DVector3 startPoint = path_ ? path_->back() : position_;
    bool any = false;
    for (std::deque<tLookaheadNode>::iterator it = lookahead_.begin(); it != lookahead_.end(); ++it) {
        std::uint32_t key = 0;
        float length = 0.0f;
        if (ChooseBestPath(it, *it, startPoint, key, length)) {
            it->key = key;
            any = true;
            if (it->occupant) {
                cISC4PathInfo* pathInfo = it->occupant->GetPathInfoObject();
                if (pathInfo) {
                    const tPath* found = pathInfo->GetPath(key, nullptr);
                    if (found) startPoint = found->points.back();
                }
            }
        }
    }
    if (cellLifespan_ != 0) --cellLifespan_;
    if (lookahead_.empty()) return any;
    const tLookaheadNode& next = lookahead_.front();
    // 0x1001196d0: StartTrip(occupant, key, true) on the front node (the vtable slot 0x450 is StartTrip).
    StartTrip(next.occupant.get(), next.key, true);
    if (!next.deadEnd) return any;
    if (!GetSpecialRouteFlag()) {
        Remove(false);
        return any;
    }
    HandleReachedRouteDestination();
    return any;
}

// 0x100118b1c: grow the lookahead deque up to GetDesiredLookaheadCount() + 1 nodes from its back. The first node is
// the current occupant / path key. A node whose choose fails gets its dead-end flag; an occupant-less node stops the
// growth and answers false. ChooseLookaheadTileByRoute runs for special routes, ChooseLookaheadTileByDensity otherwise.
bool cSC4TrafficAutomaton::ChooseLookaheadTiles() {
    if (lookahead_.empty()) {
        tLookaheadNode node;
        node.occupant = networkOccupant_;
        node.key = pathKey_;
        node.deadEnd = 0;
        lookahead_.push_back(node);
    }
    const std::uint32_t desired = GetDesiredLookaheadCount();
    bool result = true;
    if (lookahead_.size() < static_cast<std::size_t>(desired) + 1) {
        while (true) {
            tLookaheadNode& back = lookahead_.back();
            tLookaheadNode next;
            bool alive = true;
            if (back.deadEnd) break;
            if (!back.occupant) {
                result = false;
            } else {
                const bool special = CheckAutomataFlag(kAutomataFlagSpecialRoute);
                const bool chosen = special ? ChooseLookaheadTileByRoute(back, next) : ChooseLookaheadTileByDensity(back, next);
                if (!chosen) {
                    back.deadEnd = 1;
                    alive = false;
                } else {
                    lookahead_.push_back(next);
                }
            }
            if (!alive || lookahead_.size() >= static_cast<std::size_t>(desired) + 1 || !result) break;
        }
    }
    return result;
}

// 0x1001190b6: pick the next tile from the free exits of the tile behind the node's exit edge (GetTileInfoAtNeighborEdge
// with the second direction flag). ChooseExitEdge produces the candidate exits (four, weighted); the first exit that the
// occupant connects to (and that is not the node's own entry for a train) becomes the next key. When no connection
// exists and the automaton should not be kept alive, a random wildcard path key is used and the node is marked a dead
// end. Answers whether a next node was produced.
bool cSC4TrafficAutomaton::ChooseLookaheadTileByDensity(const tLookaheadNode& from, tLookaheadNode& out) {
    cSC4TrafficNetworkMap* map = trafficHost_ ? trafficHost_->TrafficNetworkMap() : nullptr;
    if (!map || !from.occupant) return false;
    const std::uint32_t key = from.key;
    const std::uint32_t type = (key >> 24) & 0xf;
    TrafficTileInfo* tile = map->GetTileInfoAtNeighborEdge(from.occupant.get(), static_cast<std::uint8_t>(key), false, type);
    if (!tile) return false;
    out.occupant = tile->occupant ? AsTrafficOccupant(tile->occupant) : nullptr;
    if (!out.occupant) return false;
    const std::uint8_t entry = static_cast<std::uint8_t>((key + 2u) & 3u);
    cISC4PathInfo* pathInfo = out.occupant->GetPathInfoObject();
    if (!pathInfo) return false;
    std::uint8_t edges[4] = {0, 0, 0, 0};
    const std::uint8_t exitCount = ChooseExitEdge(out.occupant, tile, entry, edges);
    for (std::uint32_t i = 0; i < 4; ++i) {
        const std::uint8_t exit = edges[i];
        if (exit == static_cast<std::uint8_t>(key >> 8) && IsTrain()) continue;
        if (out.occupant->PathConnectionExists(type, entry, exit)) {
            out.key = (static_cast<std::uint32_t>(entry) << 8) | (type << 24) | exit;
            return true;
        }
        if (i < exitCount) {
            if (!ShouldKeepAlive()) {
                pathInfo->GetRandomPathWildcard(type, entry, kPathEdgeWildcard, &TrafficRandom(), &out.key, nullptr);
                out.deadEnd = 1;
                return true;
            }
        }
    }
    return false;
}

// 0x100119390: the route-driven variant. While the route deque has entries, the front entry is the next cell and the
// direction to the following route cell decides the exit edge (z first, then x). The tile chain at that cell is scanned
// for an occupant whose PathConnectionExists answers; with an empty route the neighbour tile of the node's exit edge is
// used and its occupant must connect in any direction (the first entry that works). Answers whether a next node exists.
bool cSC4TrafficAutomaton::ChooseLookaheadTileByRoute(const tLookaheadNode& from, tLookaheadNode& out) {
    cSC4TrafficNetworkMap* map = trafficHost_ ? trafficHost_->TrafficNetworkMap() : nullptr;
    if (!map || route_.empty()) return false;
    const std::uint32_t key = from.key;
    const std::uint32_t type = (key >> 24) & 0xf;
    const std::uint8_t entry = static_cast<std::uint8_t>((key + 2u) & 3u);
    const RouteData front = route_.front();
    route_.pop_front();
    if (route_.empty()) {
        out.deadEnd = 1;
        TrafficTileInfo* tile = map->GetTileInfoAtNeighborEdge(from.occupant.get(), static_cast<std::uint8_t>(key), true, type);
        if (!tile || !tile->occupant) return false;
        out.occupant = AsTrafficOccupant(tile->occupant);
        for (std::uint32_t i = 0; i < 4; ++i) {
            const std::uint8_t exit = static_cast<std::uint8_t>(((key & 3u) + i) & 3u);
            if (out.occupant->PathConnectionExists(type, entry, exit)) {
                out.key = (static_cast<std::uint32_t>(entry) << 8) | (type << 24) | exit;
                return true;
            }
        }
        return false;
    }
    const RouteData following = route_.front();
    std::uint8_t direction;
    if (static_cast<int>(following.z) - static_cast<int>(front.z) < 0) direction = 1;
    else if (following.z != front.z) direction = 3;
    else direction = front.x <= following.x ? 2 : 0;
    TrafficTileInfo* tile = map->SafeGetTileInfoAtCell(front.x, front.z);
    for (; tile; tile = tile->next) {
        if (!tile->occupant) continue;
        ITrafficAutomatonOccupant* occupant = AsTrafficOccupant(tile->occupant);
        if (occupant->PathConnectionExists(type, entry, direction)) {
            out.occupant = occupant;
            out.key = (static_cast<std::uint32_t>(entry) << 8) | (type << 24) | direction;
            out.deadEnd = 0;
            return true;
        }
    }
    return false;
}

// 0x100118d98: the best exit path of one lookahead node. GetPathsNearStartPoint (radius 4, or 32 for a special route)
// lists the candidate keys; a single candidate answers it directly (without the congestion penalty - a quirk); with
// several, each candidate's distance from the position is added to the map's per-lane automata count times the
// lookahead penalty and, when the deque has a following node, to that node's recursively computed remaining length from
// the candidate path's last point. The smallest non-negative score wins.
bool cSC4TrafficAutomaton::ChooseBestPath(std::deque<tLookaheadNode>::const_iterator node, const tLookaheadNode& value,
                                          const cS3DVector3& position, std::uint32_t& key, float& length) {
    if (!value.occupant) return false;
    cISC4PathInfo* pathInfo = value.occupant->GetPathInfoObject();
    if (!pathInfo) return false;
    const std::uint32_t word = value.key;
    const float radius = CheckAutomataFlag(kAutomataFlagSpecialRoute) ? 32.0f : 4.0f;   // 0x42000000 / 0x40800000
    const std::vector<std::uint32_t> candidates =
        pathInfo->GetPathsNearStartPoint((word >> 24) & 0xf, position, static_cast<std::uint8_t>(word >> 8),
                                         static_cast<std::uint8_t>(word), radius);
    if (candidates.empty()) return false;
    if (candidates.size() == 1) {
        key = candidates.front();
        const tPath* found = pathInfo->GetPath(key, nullptr);
        if (!found || found->points.empty()) return false;
        length = Distance3(found->points.front(), position);
        return true;
    }
    length = FLT_MAX;
    key = 0;
    bool result = false;
    const std::deque<tLookaheadNode>::const_iterator following = std::next(node);
    for (std::uint32_t candidate : candidates) {
        const tPath* found = pathInfo->GetPath(candidate, nullptr);
        if (!found || found->points.empty()) continue;
        float distance = Distance3(found->points.front(), position);
        if (following != lookahead_.end()) {
            std::uint32_t subKey = 0;
            float extra = 0.0f;
            if (ChooseBestPath(following, *following, found->points.back(), subKey, extra)) distance += extra;
        }
        cSC4TrafficNetworkMap* map = trafficHost_ ? trafficHost_->TrafficNetworkMap() : nullptr;
        if (!map) continue;
        const float penalty =
            static_cast<float>(static_cast<std::uint16_t>(map->GetAutomataCountForLane(value.occupant.get(), candidate))) *
            Tuning().lookaheadPenalty;
        const float score = distance + penalty;
        if (0.0f <= score && score < length) {
            length = score;
            key = candidate;
            result = true;
        }
    }
    return result;
}

// 0x100119256: fill the four weighted exit edges of `tile` for `entry` through the manager's edge weights (vslot
// 0x270), the attraction bias (ApplyAttractionWeight) and the map's ChooseWeightedEdge.
std::uint8_t cSC4TrafficAutomaton::ChooseExitEdge(ITrafficAutomatonOccupant* occupant, TrafficTileInfo* tile,
                                                  std::uint8_t entry, std::uint8_t* edges) {
    cSC4TrafficNetworkMap* map = trafficHost_ ? trafficHost_->TrafficNetworkMap() : nullptr;
    float weights[16] = {};
    ITrafficAutomataManager* manager = Manager();
    if (manager) {
        manager->SetCurrentLayer(populateLayer_);
        manager->ComputeEdgeWeights(occupant, tile, populateLayer_, weights);
    }
    ApplyAttractionWeight(occupant, weights);
    if (!map) return 0;
    const int count = map->ChooseWeightedEdge(weights + static_cast<std::uint32_t>(entry) * 4u, edges);
    return static_cast<std::uint8_t>(count);
}

// 0x10011971c: attract the automaton towards the attraction vector. GetChaseVector (or the attraction controller when
// the ignore-attractors flag is clear) must answer; then the vector is normalised (the original divides by sqrt
// without a zero guard), its edge comes from GetEdgeFromVector and every row whose entry connects to that exit shifts
// max(weight, 100) * |attraction distance| * 0.01 from the opposite exit's weight onto the chosen exit.
bool cSC4TrafficAutomaton::ApplyAttractionWeight(ITrafficAutomatonOccupant* occupant, float* weights) {
    if (!occupant) return false;
    if (GetChaseVector(attraction_)) {
        attractionStrength_ = 100.0f;
    } else {
        if (!attractionController_) return false;
        if (CheckAutomataFlag(kAutomataFlagIgnoreAttractors)) return false;
    }
    const float inverse = 1.0f / std::sqrt(attraction_.x * attraction_.x + attraction_.y * attraction_.y +
                                           attraction_.z * attraction_.z);
    attraction_ = Scale(attraction_, inverse);
    std::uint8_t entry = kPathEdgeWildcard;
    GetEdgeFromVector(occupant, attraction_, entry, true);
    if (entry == kPathEdgeWildcard) return true;
    const float distance = attractionStrength_;
    const std::uint8_t opposite = static_cast<std::uint8_t>((entry + 2) & 3);
    float* row = weights;
    for (std::uint32_t i = 0; i < 4; ++i, row += 4) {
        if (!occupant->PathConnectionExists(0, static_cast<std::uint8_t>(i), entry)) continue;
        float amount = row[opposite];
        if (amount <= 100.0f) amount = 100.0f;
        amount = amount * std::fabs(distance) * 0.01f;
        row[entry] += amount;
        float left = row[opposite] - amount;
        if (left <= 0.0f) left = 0.0f;
        row[opposite] = left;
    }
    return true;
}

// 0x100119906: the edge whose direction (west, north, east, south) has the largest dot product with `direction`.
// Directions below 0.6 (about 53 degrees) do not count. With checkConnection the occupant must have a path connection
// from any entry to the candidate exit (the node's path type nibble is used).
bool cSC4TrafficAutomaton::GetEdgeFromVector(ITrafficAutomatonOccupant* occupant, const cS3DVector3& direction,
                                             std::uint8_t& edge, bool checkConnection) {
    edge = kPathEdgeWildcard;
    float best = -2.0f;
    for (std::uint32_t i = 0; i < 4; ++i) {
        const float dot = direction.x * FloatFromBits(kEdgeDirection[i][0]) +
                          direction.y * FloatFromBits(kEdgeDirection[i][1]) +
                          direction.z * FloatFromBits(kEdgeDirection[i][2]);
        if (!(best < dot)) continue;
        if (checkConnection && occupant &&
            !occupant->PathConnectionExists((pathKey_ >> 24) & 0xf, kPathEdgeWildcard, static_cast<std::uint8_t>(i))) {
            continue;
        }
        edge = static_cast<std::uint8_t>(i);
        best = dot;
    }
    return best >= 0.6f && edge != kPathEdgeWildcard;
}

// 0x100119668: the network manager's "the automaton left the network" callback: choose a random path from the key and
// remove the automaton. The lot and rectangle arguments are unused.
bool cSC4TrafficAutomaton::HandleExitNetwork(ITrafficAutomatonOccupant* occupant, std::uint32_t key, void* lot,
                                             const SC4RectInt32& rect) {
    (void)lot;
    (void)rect;
    ChooseRandomPath(occupant, key);
    Remove(false);
    return false;
}

// 0x10011968e: attach a random wildcard path of the occupant for `key` (the type is the key's nibble, the entry its
// second byte, the exit any) and answer true when one exists.
bool cSC4TrafficAutomaton::ChooseRandomPath(ITrafficAutomatonOccupant* occupant, std::uint32_t key) {
    if (!occupant) return false;
    cISC4PathInfo* pathInfo = occupant->GetPathInfoObject();
    if (!pathInfo) return false;
    std::uint32_t foundKey = 0;
    const tPath* found = pathInfo->GetRandomPathWildcard((key >> 24) & 0xf, static_cast<std::uint8_t>(key >> 8),
                                                         kPathEdgeWildcard, &TrafficRandom(), &foundKey, nullptr);
    if (!found) return false;
    SetPath(occupant, foundKey, &found->points, true);
    return true;
}

// 0x100119aac: start a route. ClearLookaheadDeque, set the special-route flag and clear ignore-paths; `onlyStart`
// stops there. Otherwise the route deque's front cell selects the tile (SafeGetTileInfoAtCell) and the direction to the
// next route cell selects the exit edge; the first occupant of the cell chain with a random wildcard path for that exit
// starts the path and the move. When the route becomes empty the midpoint of the path is the destination and the
// reached flag is set.
bool cSC4TrafficAutomaton::BeginRoute(bool onlyStart) {
    cSC4TrafficNetworkMap* map = trafficHost_ ? trafficHost_->TrafficNetworkMap() : nullptr;
    ITrafficAutomataManager* manager = Manager();
    if (!manager) return false;
    ClearLookaheadDeque();
    SetSpecialRouteFlag(true);
    SetAutomataFlag(kAutomataFlagIgnorePaths, false);
    if (onlyStart) return true;
    if (!map || route_.empty()) return false;
    const RouteData front = route_.front();
    route_.pop_front();
    std::uint8_t exit = kPathEdgeWildcard;
    if (!route_.empty()) {
        const RouteData following = route_.front();
        if (static_cast<int>(following.z) - static_cast<int>(front.z) < 0) exit = 1;
        else if (following.z != front.z) exit = 3;
        else exit = front.x <= following.x ? 2 : 0;
    }
    TrafficTileInfo* tile = map->SafeGetTileInfoAtCell(front.x, front.z);
    for (; tile; tile = tile->next) {
        if (!tile->occupant) continue;
        ITrafficAutomatonOccupant* occupant = AsTrafficOccupant(tile->occupant);
        cISC4PathInfo* pathInfo = occupant->GetPathInfoObject();
        if (!pathInfo) continue;
        const std::uint32_t travelType = manager->GetTravelType(this);
        std::uint32_t key = 0;
        const tPath* found = pathInfo->GetRandomPathWildcard(travelType, kPathEdgeWildcard, exit, &TrafficRandom(), &key, nullptr);
        if (!found) continue;
        cS3DVector3 start{};
        cS3DVector3 direction = MakeVector(0.0f, 0.0f, 1.0f);
        SetPath(occupant, key, &found->points, true);
        if (found->points.size() > 1) {
            direction = Sub(found->points.back(), found->points.front());
            const float length = Length3(direction);
            if (length <= kPathEpsilon) {
                direction = MakeVector(0.0f, 0.0f, 1.0f);
            } else {
                direction = Scale(direction, 1.0f / length);
            }
        }
        if (route_.empty()) {
            pathInfo->GetPathMidpoint(&found->points, start);
            SetAutomataFlag(kAutomataFlagReachedDestination, true);
        } else {
            start = found->points.front();
        }
        MoveTo(&start.x, &direction.x);
        return true;
    }
    return false;
}

// 0x100119a46: replace the route deque; when `begin` is set, start it (only the first half when `onlyStart`). The
// default speed is restored on success.
bool cSC4TrafficAutomaton::SetRouteInfo(const std::deque<RouteData>& route, bool begin, bool onlyStart) {
    if (route.empty()) return false;
    route_ = route;
    if (begin && !BeginRoute(onlyStart)) return false;
    ReturnToDefaultSpeed();
    return true;
}

// 0x100119db2: start a trip on `occupant` with a path key. Uses the path for the key when one exists; with `best` it
// falls back to GetBestPath around the automaton's position. A path with fewer than two points is refused. SetPath
// attaches it, and the map's lane tick for the key is reset to 0.
bool cSC4TrafficAutomaton::StartTrip(ITrafficAutomatonOccupant* occupant, std::uint32_t key, bool best) {
    if (!occupant) return false;
    cISC4PathInfo* pathInfo = occupant->GetPathInfoObject();
    cSC4TrafficNetworkMap* map = trafficHost_ ? trafficHost_->TrafficNetworkMap() : nullptr;
    if (!pathInfo) return false;
    const std::uint32_t type = (key >> 24) & 0xf;
    std::uint32_t foundKey = key;
    const std::vector<cS3DVector3>* points = nullptr;
    const tPath* direct = pathInfo->GetPath(key, nullptr);
    if (direct) points = &direct->points;
    if (best && !points) {
        const tPath* found = pathInfo->GetBestPath(type, static_cast<std::uint8_t>(key >> 8),
                                                   static_cast<std::uint8_t>(key), position_, &foundKey, nullptr);
        if (found) points = &found->points;
    }
    if (!points) return false;
    if (points->size() < 2) return false;
    SetPath(occupant, foundKey, points, best);
    if (map) {
        std::uint32_t* laneTick = map->GetLaneTick(occupant, populateLayer_, type, static_cast<std::uint8_t>(key >> 8),
                                                   static_cast<std::uint8_t>(key));
        if (laneTick) *laneTick = 0;
    }
    return true;
}

// ---- queries ---------------------------------------------------------------------------------------------------------
std::uint32_t cSC4TrafficAutomaton::GetPathKey() const { return pathKey_; }

ITrafficAutomatonOccupant* cSC4TrafficAutomaton::GetNetworkOccupant() const { return networkOccupant_; }

float cSC4TrafficAutomaton::GetPathOffset() const { return pathOffset_; }

void cSC4TrafficAutomaton::SetPathOffset(float offset) { pathOffset_ = offset; }

const cS3DVector3& cSC4TrafficAutomaton::GetPathPos() const { return position_; }

bool cSC4TrafficAutomaton::IsTrain() const { return false; }

const cS3DVector3& cSC4TrafficAutomaton::GetDispatchDestination() const { return destination_; }

const std::deque<RouteData>& cSC4TrafficAutomaton::GetRouteInfo() const { return route_; }

bool cSC4TrafficAutomaton::CloseEnoughToDispatchDestination() const {
    const float limit = static_cast<float>(static_cast<int>(Tuning().dispatchDistance << 4));
    return Distance3(GetDispatchDestination(), position_) <= limit;
}

std::uint8_t cSC4TrafficAutomaton::GetCellLifespan() const { return cellLifespan_; }

void cSC4TrafficAutomaton::SetCellLifespan(std::uint8_t lifespan) { cellLifespan_ = lifespan; }

const std::deque<tLookaheadNode>& cSC4TrafficAutomaton::GetLookaheadDeque() const { return lookahead_; }

void cSC4TrafficAutomaton::RecalculatePathOffset() { pathOffset_ = 0.0f; }

bool cSC4TrafficAutomaton::GetDetectCollisions() const { return CheckAutomataFlag(kAutomataFlagCollideCandidate); }

// 0x10011a24c: toggle the collide-candidate flag; turning it on re-acquires the leader from FindLeader (the original
// does not clear the leader when it is turned off).
void cSC4TrafficAutomaton::SetDetectCollisions(bool on) {
    const bool current = GetDetectCollisions();
    if ((current ^ on) != true) return;
    SetAutomataFlag(kAutomataFlagCollideCandidate, on);
    if (on) SetLeader(FindLeader());
    RecalculateLeaders();
}

// 0x1001187cc: the network occupant was removed -> remove the automaton without a fade.
void cSC4TrafficAutomaton::HandleNetworkOccupantRemoved() { Remove(false); }

// 0x10011a4ac: the stop point of the current tile. The path info's GetStopPoint for (type, entry, exit 0xff, lane 0);
// failing that the neighbour tile behind the key's exit edge and its occupant's stop point with the opposite entry.
bool cSC4TrafficAutomaton::GetStopPoint(cS3DVector3& out) {
    if (!pathInfo_) return false;
    const std::uint32_t key = pathKey_;
    const std::uint32_t type = (key >> 24) & 0xf;
    if (pathInfo_->GetStopPoint(out, type, static_cast<std::uint8_t>(key >> 8), kPathEdgeWildcard, 0)) return true;
    cSC4TrafficNetworkMap* map = trafficHost_ ? trafficHost_->TrafficNetworkMap() : nullptr;
    if (!map) return false;
    TrafficTileInfo* tile = map->GetTileInfoAtNeighborEdge(networkOccupant_, static_cast<std::uint8_t>(key), true, type);
    if (!tile || !tile->occupant) return false;
    ITrafficAutomatonOccupant* occupant = AsTrafficOccupant(tile->occupant);
    cISC4PathInfo* pathInfo = occupant->GetPathInfoObject();
    if (!pathInfo) return false;
    return pathInfo->GetStopPoint(out, type, static_cast<std::uint8_t>((key + 2u) & 3u), kPathEdgeWildcard, 0);
}

ITrafficAutomataManager* cSC4TrafficAutomaton::GetTrafficAutomataManager() const { return Manager(); }

std::uint8_t cSC4TrafficAutomaton::GetDesiredLookaheadCount() const {
    return static_cast<std::uint8_t>(Tuning().desiredLookaheadCount);
}

// ---- persistence -----------------------------------------------------------------------------------------------------
// 0x10011aa6a: version 2, then the base record, the leader (again: the base record already has one), the populate
// layer, the cell lifespan, the network occupant, the path key, the path index and the route deque (three bytes per
// entry). The original answers the base Write result, not the later stream error.
bool cSC4TrafficAutomaton::Write(cIGZOStream& stream) const {
    stream.SetUint16(2);
    if (!cSC4Automaton::Write(stream)) return false;
    SC4WriteGZSerializable(stream, static_cast<const cIGZSerializable*>(leader_));
    stream.SetUint32(populateLayer_);
    stream.SetUint8(cellLifespan_);
    SC4WriteGZSerializable(stream, networkOccupant_);
    stream.SetUint32(pathKey_);
    stream.SetUint32(pathIndex_);
    stream.SetUint32(static_cast<std::uint32_t>(route_.size()));
    for (const RouteData& entry : route_) {
        stream.SetUint8(entry.x);
        stream.SetUint8(entry.z);
        stream.SetUint8(entry.travelType);
    }
    return true;
}

// 0x10011ac58: version 2 only. The base record is read first; the base's leader is then released and replaced by the
// traffic record's second leader reference. The occupant comes from the host (the original's SC4ReadGZSerializable with
// iid 0xa821ef94 needs the GZCOM factory). The route deque is refilled from three bytes per entry.
bool cSC4TrafficAutomaton::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 2) return false;
    if (!cSC4Automaton::Read(stream)) return false;
    if (leader_) {
        cSC4Automaton* old = leader_;
        leader_ = nullptr;
        old->Release();
    }
    void* rawLeader = nullptr;
    SC4ReadGZSerializable(stream, kAutomatonIid, &rawLeader, false);
    leader_ = static_cast<cSC4Automaton*>(rawLeader);
    std::uint32_t layer = 0;
    stream.GetUint32(layer);
    populateLayer_ = layer;
    stream.GetUint8(cellLifespan_);
    if (networkOccupant_) {
        ITrafficAutomatonOccupant* old = networkOccupant_;
        networkOccupant_ = nullptr;
        old->Release();
    }
    if (trafficHost_) networkOccupant_ = trafficHost_->ReadNetworkOccupant(stream);
    stream.GetUint32(pathKey_);
    std::uint32_t index = 0;
    stream.GetUint32(index);
    pathIndex_ = index;
    route_.clear();
    std::uint32_t count = 0;
    stream.GetUint32(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        RouteData entry;
        stream.GetUint8(entry.x);
        stream.GetUint8(entry.z);
        stream.GetUint8(entry.travelType);
        route_.push_back(entry);
    }
    return true;
}

// 0x10011a85e: "layer: %d  lifespan: %d  route: %d, %d edge: %d-%d <base>". The original builds cRZStrings and returns
// its static cRZString; the port formats into a static buffer (same text, no cRZString allocation).
const char* cSC4TrafficAutomaton::GetDebugExtraText() const {
    static char buffer[512];
    char routeText[64] = {0};
    if (!route_.empty()) {
        std::snprintf(routeText, sizeof(routeText), "route: %d, %d", route_.front().x, route_.front().z);
    }
    char edgeText[64];
    std::snprintf(edgeText, sizeof(edgeText), "edge: %d-%d", (pathKey_ >> 8) & 0xff, pathKey_ & 0xff);
    const char* base = cSC4Automaton::GetDebugExtraText();
    std::snprintf(buffer, sizeof(buffer), "layer: %d  lifespan: %d  %s %s %s", populateLayer_, cellLifespan_, routeText,
                  edgeText, base ? base : "");
    return buffer;
}

// 0x10011af24: PARTIAL. The function searches the neighbours simulator's connection lists (kinds 3, 0xc, 8, 2, 10),
// sets up the traffic simulator's path finder for each connection rectangle, calls its FindPath (vslot 0xb8, not
// recovered - the port's cISC4PathFinder has no such method), keeps the connection with the smallest arrived count and
// answers the network occupant of the best cell. Without cISC4NeighborConnection, FindPath and the connection
// rectangles the result cannot be produced; the seam is left in the host (NeighborConnectionCells).
ITrafficAutomatonOccupant* cSC4TrafficAutomaton::FindBestNeighborConnection() {
    // PARTIAL (see the traffic automaton to-do row): the connection list and the path finder's FindPath are not
    // recovered, so this stays a null answer behind the host seam.
    return nullptr;
}

}  // namespace sc4
