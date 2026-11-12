// cSC4Pedestrian — see Pedestrian.h for the class, the layout, the vtable slots and the quirks. Every function here is a
// hand port checked against the disassembly (the decompile drops arguments and mixes up float lanes in several of them).
#include "sc4/Pedestrian.h"

#include <cmath>

#include "sc4/PathInfo.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {

cS3DVector3 MakeVector(float x, float y, float z) {
    cS3DVector3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}
cS3DVector3 Scale(const cS3DVector3& v, float s) { return MakeVector(v.x * s, v.y * s, v.z * s); }
// The compiled sums group the squares as (x*x + y*y) + z*z everywhere in this class.
float Length(const cS3DVector3& v) { return std::sqrt((v.x * v.x + v.y * v.y) + v.z * v.z); }

constexpr std::uint32_t kEdgeCrowdAnimationMinimum = 2;          // DoCrowdTick: fewer than two animation names never re-roll
constexpr float kPedestrianCrowdPush = 0.0027777778f;           // 1/360, DoCrowdTick (0x100a18410)
constexpr float kPedestrianCollisionInterval = 500.0f;          // TestCollisionExpensive (0x100a18418)
constexpr float kPedestrianCollisionRadius = 1.0f;
constexpr float kPedestrianCollisionOverlap = 0.01f;            // closer than this the pushed pedestrian gets a random direction (0x100a15928)
constexpr float kPedestrianStrayDistance = 10.0f;               // DoRiotFollowerTick: farther from the leader than this pulls towards it
constexpr float kPedestrianStrayPull = 0.015625f;               // 1/64 of the vector to the leader
constexpr float kPedestrianRedirectDistance = 20.0f;            // ChooseNewDestination (0x100a18408)
constexpr float kPedestrianOrientationEpsilon = 1e-05f;         // UpdateOrientation (0x100a18a70)
constexpr std::uint32_t kPedestrianStateIdleReturn = 1;         // DoIgniteTick: back to the idle state
constexpr std::uint32_t kPedestrianPathTypeWalk = 2;
constexpr std::uint8_t kPedestrianAnyEdge = 0xff;

// DoRiotLeaderTick direction tables (0x100a18440 x, 0x100a18450 z, 0x100a18460 the order the four turns are tried in).
constexpr int kRiotDirX[4] = {1, 0, -1, 0};
constexpr int kRiotDirZ[4] = {0, 1, 0, -1};
constexpr int kRiotTryOrder[4] = {0, 1, 3, 2};

// The riot leader and follower both steer by the orientation plus a sideways nudge: (-z, 0, x) of the orientation scaled by
// random / |(x, z)|. A zero horizontal orientation skips the scaling (and keeps the unscaled sideways terms), as compiled.
cS3DVector3 SidewaysNudge(const cS3DVector3& o, float random) {
    float x = -o.z, y = 0.0f, z = o.x;
    const float lengthSquared = o.x * o.x + (o.z * o.z + 0.0f);
    if (lengthSquared > 0.0f) {
        const float k = random / std::sqrt(lengthSquared);
        x = -o.z * k;
        y = 0.0f * k;
        z = k * o.x;
    }
    return MakeVector(x + o.x, y + o.y, z + o.z);
}

cS3DVector3 Normalized(const cS3DVector3& v) {
    const float inverse = 1.0f / Length(v);
    return MakeVector(v.x * inverse, v.y * inverse, v.z * inverse);
}

}  // namespace

const PedestrianTuning& cSC4Pedestrian::Tuning() const {
    static const PedestrianTuning kDefaults{};
    const PedestrianTuning* tuning = pedestrianHost_ ? pedestrianHost_->PedestrianTuningTable() : nullptr;
    return tuning ? *tuning : kDefaults;
}

// ---- construction / destruction ----------------------------------------------------------------------------------------
// 0x1000f1610: the traffic automaton constructor, then the pedestrian fields: the occupant type id 0x496e5783, the crowd
// reaction state/direction cleared, the animation speed 0.01, and the default speed is a random value in the manager's
// range * 1/3600 (the speed tween is then told to reach it with a zero duration).
cSC4Pedestrian::cSC4Pedestrian(IPedestrianHost* host, const cGZPersistResourceKey& modelKey)
    : cSC4TrafficAutomaton(host, AutomatonHost(), modelKey), pedestrianHost_(host ? host : nullptr) {
    type_ = kPedestrianOccupantType;
    crowdReactionState_ = 0;
    crowdReactionDirectionSet_ = false;
    crowdReactionDirection_ = MakeVector(0.0f, 0.0f, 0.0f);
    animationSpeed_ = kPedestrianAnimationSpeed;
    const PedestrianTuning& tuning = Tuning();
    const std::int32_t rolled = Host() ? Host()->RandomInt(tuning.speedMin, tuning.speedMax) : 0;
    const float speed = static_cast<float>(rolled) * kAutomatonSpeedScale;
    SetDefaultSpeed(speed);
    SpeedTween().SetDesired(speed, 0.0f);
}

// 0x1000f3c22: the traffic automaton destructor (the pedestrian has no fields that need freeing).
cSC4Pedestrian::~cSC4Pedestrian() = default;

// ---- QueryInterface / GetGZCLSID ---------------------------------------------------------------------------------------
// 0x1000f3796: 0x896e75af and 0x8b99ee7c answer this object; everything else goes to the base.
bool cSC4Pedestrian::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    if (iid == kGZCLSID_cSC4Pedestrian) {
        *out = this;
        AddRef();
        return true;
    }
    if (iid == kGZIID_cISC4TrafficAutomaton) {
        *out = this;
        AddRef();
        return true;
    }
    return cSC4Automaton::QueryInterface(iid, out);
}

// ---- Init / Shutdown ----------------------------------------------------------------------------------------------------
// 0x1000f1792: roll the path offset (unless the occupant is already initialised), then the base Init.
bool cSC4Pedestrian::Init() {
    if (!IsInitialized()) RecalculatePathOffset();
    return cSC4TrafficAutomaton::Init();
}

// 0x1000f17f2: the base Shutdown.
bool cSC4Pedestrian::Shutdown() { return cSC4TrafficAutomaton::Shutdown(); }

// ---- movement -----------------------------------------------------------------------------------------------------------
// 0x1000f180a: a null direction moves without heading; otherwise the y of the direction is zeroed (the pedestrian walks on
// the ground plane) and the base moves.
void cSC4Pedestrian::MoveTo(const float* position, const float* direction) {
    if (!direction) {
        cSC4Automaton::MoveTo(position, nullptr);
        return;
    }
    float flattened[3] = {direction[0], 0.0f, direction[2]};
    cSC4Automaton::MoveTo(position, flattened);
}

// 0x1000f34d8: a ground-plane heading from the position to the target (the y delta is 0), or (0,0,1) when the target is on
// the position (the compiled code stores the 1.0 in the z lane; the decompile shows it in x).
void cSC4Pedestrian::UpdateOrientation(const cS3DVector3& target, float* out) const {
    const float dx = target.x - position_.x;
    const float dz = target.z - position_.z;
    const float length = std::sqrt(dz * dz + dx * dx);
    if (length <= kPedestrianOrientationEpsilon) {
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = 1.0f;
    } else {
        const float inverse = 1.0f / length;
        out[0] = dx * inverse;
        out[1] = 0.0f;
        out[2] = dz * inverse;
    }
}

// 0x1000f3568: roll the path offset in the tuning range.
void cSC4Pedestrian::RecalculatePathOffset() {
    const PedestrianTuning& tuning = Tuning();
    const double rolled = Host() ? Host()->RandomDouble(tuning.pathOffsetMin, tuning.pathOffsetMax) : 0.0;
    SetPathOffset(static_cast<float>(rolled));
}

// 0x1000f35a8: play the "idle" animation by name.
bool cSC4Pedestrian::HandleStopped() {
    cRZString name(kPedestrianIdleAnimation);
    PlayAnimByName(name);
    return true;
}

// 0x1000f3334: release the network occupant; the riot states (8/9) keep the rest of the state untouched, everything else
// runs the base handler.
void cSC4Pedestrian::HandleNetworkOccupantRemoved() {
    if (networkOccupant_) {
        ITrafficAutomatonOccupant* old = networkOccupant_;
        networkOccupant_ = nullptr;
        old->Release();
    }
    tAutomataState* state = GetState();
    if (state && (state->stateType & ~1u) == kPedestrianStateRiotLeader) return;
    cSC4TrafficAutomaton::HandleNetworkOccupantRemoved();
}

// 0x1000f117c: when the automaton does not carry flag 0x4000, record the crowd reaction state (1 or 2), go back to the
// default speed over a second and turn towards the event position: MoveTo its own position with the ground-plane direction
// to the event (the decompile drops this direction argument). Answers whether the reaction happened.
bool cSC4Pedestrian::HandleCrowdReactionEvent(const cS3DVector3& position, std::uint32_t event) {
    const bool flagged = CheckAutomataFlag(kAutomataFlagIgnoreAttractors);
    if (!flagged) {
        crowdReactionState_ = (event == 0) ? 1u : 2u;
        SetSpeed(GetDefaultSpeed(), 1.0f);
        const float direction[3] = {position.x - position_.x, 0.0f, position.z - position_.z};
        MoveTo(&position_.x, direction);
    }
    return !flagged;
}

// ---- the per-frame step -------------------------------------------------------------------------------------------------
// 0x1000f187a: the base tick, then when the automaton controls itself and the stopped flag just toggled, play the "idle"
// animation (HandleStopped) or the default animation (PlayAnim(0)).
bool cSC4Pedestrian::DoAnimationTick(std::uint32_t unknown, float milliseconds) {
    const bool wasStopped = CheckAutomataFlag(kTrafficFlagStopped);
    const bool result = cSC4TrafficAutomaton::DoAnimationTick(unknown, milliseconds);
    if (GetSelfControlling()) {
        if (CheckAutomataFlag(kTrafficFlagStopped) && !wasStopped) {
            HandleStopped();
        } else if (wasStopped) {
            PlayAnim(0);
        }
    }
    return result;
}

// 0x1000f338c: age the animation timer; when the flags allow and the simulator is not paused, reset the animation speed
// to 0.01 and step every model; when the first model reached its last frame, post the animation-done callback.
void cSC4Pedestrian::UpdateAnimation(float milliseconds) {
    SetAnimationTimer(AnimationTimer() + milliseconds);
    const bool blocked = CheckAutomataFlag(kAutomataFlagIgnorePaths) || CheckAutomataFlag(kAutomataFlagSelfControlling) ||
                         !CheckAutomataFlag(kTrafficFlagStopped);
    const bool allowed = Host() && Host()->AnimationUpdateAllowed();
    if (blocked && allowed) {
        animationSpeed_ = kPedestrianAnimationSpeed;
        for (IAutomatonModel* model : Models()) {
            if (!model) continue;
            model->SetAutoFrameRate(animationSpeed_);
            model->UpdateAnimation(milliseconds);
        }
        if (!Callbacks().empty() && !Models().empty()) {
            IAutomatonModel* model = Models().front();
            if (model && model->GetFrameCount() - 1 <= model->GetFrame()) SendCallbackMessage(kAutomataEventAnimationDone, 0);
        }
    }
}

// ---- the state machine --------------------------------------------------------------------------------------------------
// 0x1000f201a: the base state tick first; when it did not handle the state, dispatch on the pedestrian's own state ids.
bool cSC4Pedestrian::DoStateTick(float milliseconds) {
    if (StateStack().empty()) return false;
    if (cSC4TrafficAutomaton::DoStateTick(milliseconds)) return true;
    tAutomataState* state = GetState();
    if (!state) return false;
    switch (state->stateType) {
        case kPedestrianStateBeeLine:
            DoBeeLineTick(milliseconds);
            return true;
        case kPedestrianStateCrowd:
            return DoCrowdTick(milliseconds);
        case kPedestrianStateQueue:
            DoQueueTick(milliseconds);
            return true;
        case kPedestrianStateStrike:
            return DoStrikeTick(milliseconds);
        case kPedestrianStateRiotLeader:
            return DoRiotLeaderTick(milliseconds, false);
        case kPedestrianStateRiotFollower:
            return DoRiotFollowerTick(milliseconds);
        case kPedestrianStateIgnite:
            DoIgniteTick(milliseconds);
            return true;
        default:
            return false;
    }
}

// 0x1000f1a98: state 1 marks the automaton to ignore paths/attractors/roads; 6 self-controls and snaps to the path; 8/9/4/2/3
// run the crowd/riot flag setup; 7 is a no-op; anything else clears the ignore flags and snaps to the path.
bool cSC4Pedestrian::HandleEnterState(tAutomataState* state) {
    if (cSC4TrafficAutomaton::HandleEnterState(state)) return true;
    if (!state) {
        SetAutomataFlag(kAutomataFlagIgnorePaths, false);
        SetAutomataFlag(kAutomataFlagIgnoreAttractors, false);
        return true;
    }
    switch (state->stateType) {
        case kStateIdle:   // 1
            SetAutomataFlag(kAutomataFlagIgnorePaths, true);
            SetAutomataFlag(kAutomataFlagIgnoreAttractors, true);
            SetAutomataFlag(kAutomataFlagIgnoreRoads, true);
            return true;
        case kStateDispatched:   // 6
            SetAutomataFlag(kAutomataFlagSelfControlling, true);
            SetAutomataFlag(kAutomataFlagIgnorePaths, false);
            SnapToPath();
            return true;
        case kStateIdleTick:   // 7
            return true;
        case kPedestrianStateRiotLeader:   // 8
            SpeedTween().SetDesired(kPedestrianCrowdSpeed, 0.0f);
            SetAutomataFlag(kAutomataFlagSelfControlling, true);
            SetAutomataFlag(kAutomataFlagIgnorePaths, true);
            SetAutomataFlag(kAutomataFlagIgnoreAttractors, false);
            SetAutomataFlag(kAutomataFlagNoCollision, true);
            SetAutomataFlag(kAutomataFlagKeepAfterRoute, true);
            SetAutomataFlag(kAutomataFlagPlayerDrive, true);
            SetAutomataFlag(kAutomataFlagRemoved, true);
            DoRiotLeaderTick(0.0f, true);
            return true;
        case kPedestrianStateRiotFollower:   // 9
            SpeedTween().SetDesired(kPedestrianCrowdSpeed, 0.0f);
            [[fallthrough]];
        case kPedestrianStateQueue:   // 4
            SetAutomataFlag(kAutomataFlagSelfControlling, true);
            [[fallthrough]];
        case kPedestrianStateBeeLine:   // 2
        case kPedestrianStateCrowd:    // 3
            SetAutomataFlag(kAutomataFlagIgnorePaths, true);
            SetAutomataFlag(kAutomataFlagIgnoreAttractors, false);
            return true;
        default:
            SetAutomataFlag(kAutomataFlagIgnoreAttractors, false);
            SetAutomataFlag(kAutomataFlagIgnorePaths, false);
            SnapToPath();
            return true;
    }
}

// 0x1000f1fe0: leaving the riot-leader state clears the no-collision flag, then the base exit handler.
bool cSC4Pedestrian::HandleExitState(const tAutomataState* state) {
    if (state && state->stateType == kPedestrianStateRiotLeader) SetAutomataFlag(kAutomataFlagNoCollision, false);
    return cSC4TrafficAutomaton::HandleExitState(state);
}

// ---- the tick helpers ---------------------------------------------------------------------------------------------------
std::uint32_t cSC4Pedestrian::RandomBelow(std::uint32_t range) { return Host() ? Host()->RandomBelow(range) : 0; }
float cSC4Pedestrian::RandomRange(double low, double high) {
    return Host() ? static_cast<float>(Host()->RandomDouble(low, high)) : 0.0f;
}

// 0x1000f2124: move along the (normalised) attraction vector, then update the attraction.
bool cSC4Pedestrian::DoBeeLineTick(float milliseconds) {
    const float length = Length(attraction_);
    if (length > 0.0f) MoveForwardWithCollision(Scale(attraction_, 1.0f / length), milliseconds);
    UpdateAttraction();
    return true;
}

// 0x1000f2de8: advance by direction * (speed * modifier * ms / |direction|), stay on the network (unless roads are ignored)
// and refuse the move when the target collides; a pending crowd-reaction push is then tried as a fallback. Answers whether
// the automaton moved.
bool cSC4Pedestrian::MoveForwardWithCollision(const cS3DVector3& direction, float milliseconds) {
    const float magnitude = Length(direction);
    const float speed = GetCurrentSpeed();
    cS3DVector3 delta = direction;
    if (magnitude > 0.0f) {
        const float scale = (speed * GetSpeedModifier() * milliseconds) / magnitude;
        delta = Scale(direction, scale);
    }
    cS3DVector3 next = MakeVector(delta.x + position_.x, delta.y + position_.y, delta.z + position_.z);
    if (!CheckAutomataFlag(kAutomataFlagIgnoreRoads) && Host())
        Host()->PlaceOnNetwork(position_, next, kAutomataMoveNetworkMask);
    bool moved = !TestCollisionExpensive(next, milliseconds);
    if (!moved && crowdReactionDirectionSet_ && Length(crowdReactionDirection_) > 0.0f) {
        next = MakeVector(crowdReactionDirection_.x + next.x, next.y + crowdReactionDirection_.y,
                          crowdReactionDirection_.z + next.z);
        if (Host()) Host()->PlaceOnNetwork(position_, next, kAutomataMoveNetworkMask);
        moved = !TestCollisionExpensive(next, 0.0f);
    }
    crowdReactionDirectionSet_ = false;
    if (moved) MoveTo(&next.x, &direction.x);
    return moved;
}

// 0x1000f2ffa: every 500 ms look for another pedestrian within a metre of the target position (the occupant manager's
// box query for type 0x496e5783, at most 10). The first one found is pushed away: its crowd-reaction direction is set to
// the vector from the target to it, or to a random ground-plane direction in [-0.5, 0.5] when it is nearly on top of the
// target. Answers whether anybody was close enough. The query results are released.
bool cSC4Pedestrian::TestCollisionExpensive(const cS3DVector3& position, float milliseconds) {
    const float timer = milliseconds + collisionTimer_;
    collisionTimer_ = timer;
    if (timer < kPedestrianCollisionInterval) return false;
    collisionTimer_ = 0.0f;
    bool collided = false;
    std::vector<cISC4Occupant*> occupants;
    if (pedestrianHost_) {
        const float minXyz[3] = {position.x + -1.0f, 0.0f + position.y, position.z + -1.0f};
        const float maxXyz[3] = {position.x + 1.0f, position.y + 2.0f, position.z + 1.0f};
        pedestrianHost_->OccupantsInBox(minXyz, maxXyz, kPedestrianOccupantType, 10, occupants);
    }
    cISC4Occupant* self = AsOccupant();
    for (cISC4Occupant* occupant : occupants) {
        if (collided) break;
        if (occupant == self) continue;
        float other[3] = {0.0f, 0.0f, 0.0f};
        occupant->GetPosition(other);
        const float dx = other[0] - position.x;
        const float dy = other[1] - position.y;
        const float dz = other[2] - position.z;
        const float distance = std::sqrt(dz * dz + (dx * dx + dy * dy));
        if (!(distance < kPedestrianCollisionRadius)) continue;
        cS3DVector3 push = MakeVector(dx, dy, dz);
        if (distance < kPedestrianCollisionOverlap) {
            push.x = RandomRange(-0.5, 0.5);
            push.z = RandomRange(-0.5, 0.5);
            push.y = 0.0f;
        }
        // The occupant is another pedestrian: the original writes its crowd-reaction fields (+0x39c / +0x3a0) through the
        // occupant sub-object without any type check.
        auto* pushed = static_cast<cSC4Pedestrian*>(occupant);
        pushed->crowdReactionDirectionSet_ = true;
        pushed->crowdReactionDirection_ = push;
        collided = true;
    }
    for (cISC4Occupant* occupant : occupants) occupant->Release();
    return collided;
}

// 0x1000f328e: true when the tile under the automaton (position / 16, the safe lookup) carries a network occupant.
bool cSC4Pedestrian::VerifyNetworkOccupant() {
    cSC4TrafficNetworkMap* map = TrafficHost() ? TrafficHost()->TrafficNetworkMap() : nullptr;
    if (!map) return false;
    const int cellX = static_cast<int>(position_.x * 0.0625f);
    const int cellZ = static_cast<int>(position_.z * 0.0625f);
    TrafficTileInfo* tile = map->SafeGetTileInfoAtCell(cellX, cellZ);
    return tile != nullptr && tile->occupant != nullptr;
}

// 0x1000f190a: the base destination choice, then re-route towards the attraction when it is far enough and the walk is not
// already on the edge that faces it. Answers the base result, or StartTrip's when a better path was found.
bool cSC4Pedestrian::ChooseNewDestination() {
    if (!cSC4TrafficAutomaton::ChooseNewDestination()) return false;
    bool result = true;
    if (attractionController_ && networkOccupant_ && !(std::fabs(attractionStrength_) < kPedestrianRedirectDistance)) {
        // normalised in place (no zero check, as compiled)
        const float inverse = 1.0f / std::sqrt((attraction_.z * attraction_.z + attraction_.y * attraction_.y) + attraction_.x * attraction_.x);
        attraction_ = MakeVector(attraction_.x * inverse, attraction_.y * inverse, attraction_.z * inverse);
        std::uint8_t edge = 0;
        GetEdgeFromVector(networkOccupant_, attraction_, edge, true);
        if (edge != static_cast<std::uint8_t>(GetPathKey())) {
            cISC4PathInfo* pathInfo = networkOccupant_->GetPathInfoObject();
            std::uint32_t key = 0;
            if (pathInfo && pathInfo->GetBestPath(kPedestrianPathTypeWalk, kPedestrianAnyEdge, edge, position_, &key, nullptr))
                result = StartTrip(networkOccupant_, key, true);
        }
    }
    return result;
}

// ---- the behaviour ticks -------------------------------------------------------------------------------------------------
// 0x1000f21c2: walk to the state's controller (the attraction) in a crowd. A pending push from a collision is added to the
// position first (its length scaled to ms/360); the move is refused when the target collides. When the first model has
// reached its last frame and the behaviour lists at least two animations, play a random one.
bool cSC4Pedestrian::DoCrowdTick(float milliseconds) {
    tAutomataState* state = GetState();
    if (!state || !state->controller) return false;
    float target[3] = {0.0f, 0.0f, 0.0f};
    cS3DVector3 next = position_;
    state->controller->GetPosition(target);
    const cS3DVector3 toTarget = MakeVector(target[0] - position_.x, target[1] - position_.y, target[2] - position_.z);
    if (crowdReactionDirectionSet_) {
        const float lengthSquared = (crowdReactionDirection_.x * crowdReactionDirection_.x +
                                     crowdReactionDirection_.y * crowdReactionDirection_.y) +
                                    crowdReactionDirection_.z * crowdReactionDirection_.z;
        if (lengthSquared > 0.0f) {
            const float scale = (kPedestrianCrowdPush * milliseconds) / std::sqrt(lengthSquared);
            crowdReactionDirection_ = Scale(crowdReactionDirection_, scale);
        }
        next = MakeVector(crowdReactionDirection_.x + position_.x, crowdReactionDirection_.y + position_.y,
                          crowdReactionDirection_.z + position_.z);
        crowdReactionDirectionSet_ = false;
    }
    if (Host()) Host()->PlaceOnNetwork(position_, next, kAutomataMoveNetworkMask);
    if (!TestCollisionExpensive(next, milliseconds)) {
        const float inverse = 1.0f / std::sqrt((toTarget.x * toTarget.x + toTarget.y * toTarget.y) + toTarget.z * toTarget.z);
        const float direction[3] = {toTarget.x * inverse, 0.0f, toTarget.z * inverse};
        MoveTo(&next.x, direction);
    }
    const auto* behavior = reinterpret_cast<const tBehaviorTemplate*>(state->runtime30);
    if (!Models().empty() && behavior && Models().front() != nullptr) {
        IAutomatonModel* model = Models().front();
        if (model->GetFrameCount() - 1 <= model->GetFrame() && behavior->anims.size() >= kEdgeCrowdAnimationMinimum)
            PlayAnimByName(behavior->anims[RandomBelow(static_cast<std::uint32_t>(behavior->anims.size()))]);
    }
    UpdateAttraction();
    return true;
}

// 0x1000f2470: queue behind the controller. The controller's state tick runs, the automaton drives straight; once stopped it
// faces the controller's queue point and plays "idle". No controller pops the state.
bool cSC4Pedestrian::DoQueueTick(float milliseconds) {
    tAutomataState* state = GetState();
    if (!state || !state->controller) {
        PopState();
        return true;
    }
    state->controller->HandleStateTick(ControllerLinkFor(), state->stateType, milliseconds);
    DoStraightLineTick(milliseconds);
    if (CheckAutomataFlag(kTrafficFlagStopped)) {
        float target[3] = {0.0f, 0.0f, 0.0f};
        state->controller->GetPosition(target);
        const float dx = target[0] - position_.x;
        const float dy = target[1] - position_.y;
        const float dz = target[2] - position_.z;
        const float inverse = 1.0f / std::sqrt(dz * dz + (dy * dy + dx * dx));
        const float facing[3] = {dx * inverse, 0.0f, dz * inverse};
        SetOrientation(facing);
        PlayAnimByName(cRZString(kPedestrianIdleAnimation));
    }
    UpdateAttraction();
    return true;
}

// 0x1000f2602: the strike walks its path (snapping onto it first) and, at the end of the path, either picks a way along the
// controller's lot or turns back. Answers false when the walk cannot go on (no path, no controller, no free edge).
bool cSC4Pedestrian::DoStrikeTick(float milliseconds) {
    if (path_ == nullptr && !SnapToPath()) return false;
    DoPathTick(milliseconds);
    if (!GetReachedDestination()) {
        UpdateAttraction();
        return true;
    }
    tAutomataState* state = GetState();
    if (!state || !state->controller) return false;
    cISC4Occupant* controllerOccupant = state->controller->GetOccupant();
    if (!controllerOccupant) return false;

    float controllerPosition[3] = {0.0f, 0.0f, 0.0f};
    state->controller->GetPosition(controllerPosition);
    int lotCellX = 0, lotCellZ = 0;
    ITrafficAutomatonHost* trafficHost = TrafficHost();
    if (trafficHost) trafficHost->PositionToCell(controllerPosition[0], controllerPosition[2], lotCellX, lotCellZ);
    ITrafficNetworkMapLot* lot = pedestrianHost_ ? pedestrianHost_->LotAt(lotCellX, lotCellZ) : nullptr;
    std::int32_t rect[4];
    if (lot) {
        lot->GetBoundingRect(rect);
    } else {
        SC4CellRect cells{};
        controllerOccupant->GetBoundingCityCells(cells);
        rect[0] = cells.x0; rect[1] = cells.z0; rect[2] = cells.x1; rect[3] = cells.z1;
    }
    rect[0] += -1; rect[1] += -1; rect[2] += 1; rect[3] += 1;

    bool failed = true;
    cSC4TrafficNetworkMap* map = trafficHost ? trafficHost->TrafficNetworkMap() : nullptr;
    if (GetTrafficAutomataManager() && networkOccupant_ && map && map->GetTileInfoByNetwork(networkOccupant_)) {
        const std::uint32_t key = GetPathKey();
        TrafficTileInfo* tile = map->GetTileInfoAtNeighborEdge(networkOccupant_, static_cast<std::uint8_t>(key), false, 0);
        if (tile && tile->occupant) {
            ITrafficNetworkMapOccupant* neighbour = tile->occupant;
            const std::uint32_t nibble = (key >> 24) & 0xf;
            float neighbourPosition[3] = {0.0f, 0.0f, 0.0f};
            neighbour->AsOccupant()->GetPosition(neighbourPosition);
            int cellX = 0, cellZ = 0;
            trafficHost->PositionToCell(neighbourPosition[0], neighbourPosition[2], cellX, cellZ);
            if (rect[3] < cellZ || rect[0] > cellX || rect[1] > cellZ || rect[2] < cellX) {
                // the next tile leaves the lot's surroundings: swap the entry and exit edges and turn back
                const std::uint32_t turned = ((key >> 8) & 0xff) | ((key << 8) & 0xffff) | (key & 0xff0000) | (nibble << 24);
                StartTrip(networkOccupant_, turned, true);
                RecalculatePathOffset();
                failed = false;
            } else {
                // still inside: try the three other exits, in turn, from the neighbour tile
                const std::uint8_t entry = static_cast<std::uint8_t>(key);
                const std::uint8_t opposite = static_cast<std::uint8_t>((entry + 2) & 3);
                const std::uint32_t keep = (nibble << 24) | (key & 0xff0000) | (static_cast<std::uint32_t>(opposite) << 8);
                std::uint32_t attempt = key;
                std::uint8_t edge = entry;
                bool started = false;
                for (;;) {
                    if (edge != opposite) started = StartTrip(static_cast<ITrafficAutomatonOccupant*>(neighbour), attempt, true);
                    edge = static_cast<std::uint8_t>((edge + 1) & 3);
                    if (edge == entry) break;
                    attempt = edge | keep;
                    if (started) break;
                }
                failed = !started;
            }
        }
    }
    if (failed) return false;
    UpdateAttraction();
    return true;
}

// 0x1000f1ca8: the riot leader. Initialising picks a random heading and clears the remembered cell; afterwards, each time it
// enters a new 16-metre cell it tries to keep its heading and then the other three turns (connection test on the road
// network, entry any, exit = the new heading, walk paths); with none free it picks a random heading. It walks along the
// orientation plus a sideways nudge and the heading's unit step. Losing the network removes it.
bool cSC4Pedestrian::DoRiotLeaderTick(float milliseconds, bool initialize) {
    auto* state = static_cast<tAutomataStateRiotLeader*>(GetState());
    if (!VerifyNetworkOccupant()) {
        Remove(false);
        return false;
    }
    if (initialize) {
        state->value40 = static_cast<std::uint8_t>(RandomBelow(4));
        state->value38 = -1;
        state->value3c = -1;
        SpeedTween().SetDesired(kPedestrianCrowdSpeed, 0.0f);
        return true;
    }
    const int cellX = static_cast<int>(position_.x * 0.0625f);
    const int cellZ = static_cast<int>(position_.z * 0.0625f);
    if (state->value38 != cellX || state->value3c != cellZ) {
        cSC4TrafficNetworkMap* map = TrafficHost() ? TrafficHost()->TrafficNetworkMap() : nullptr;
        bool found = false;
        for (int attempt = 0; attempt < 4 && !found; ++attempt) {
            const int heading = (state->value40 + kRiotTryOrder[attempt]) % 4;
            if (map && map->ConnectionExists(kRiotDirX[heading] + cellX, kRiotDirZ[heading] + cellZ, kPedestrianAnyEdge,
                                             static_cast<std::uint8_t>(heading), kPedestrianPathTypeWalk)) {
                state->value40 = static_cast<std::uint8_t>(heading);
                state->value38 = cellX;
                state->value3c = cellZ;
                found = true;
            }
        }
        if (!found) state->value40 = static_cast<std::uint8_t>(RandomBelow(4));
    }
    cS3DVector3 steer = SidewaysNudge(orientation_, RandomRange(-1.0, 1.0));
    float stepX = static_cast<float>(kRiotDirX[state->value40]);
    float stepZ = static_cast<float>(kRiotDirZ[state->value40]);
    float stepY = 0.0f;
    const float stepLengthSquared = stepZ * stepZ + (stepX * stepX + 0.0f);
    if (stepLengthSquared > 0.0f) {
        const float inverse = 0.25f / std::sqrt(stepLengthSquared);
        stepX *= inverse;
        stepY = 0.0f * inverse;
        stepZ *= inverse;
    }
    steer = MakeVector(steer.x + stepX, steer.y + stepY, steer.z + stepZ);
    MoveForwardWithCollision(Normalized(steer), milliseconds);
    UpdateAttraction();
    return true;
}

// 0x1000f293a: the riot follower. It first looks up its leader (the newest riot-follower state whose controller has an
// occupant exposing the automaton interface). It steers like the leader, from a random ground-plane vector in [-1, 1] when it
// has a pending push, otherwise from the orientation plus a sideways nudge in [-0.6, 0.6]; when more than 10 m behind the
// leader it adds 1/64 of the vector to it. Losing the network removes it.
bool cSC4Pedestrian::DoRiotFollowerTick(float milliseconds) {
    if (!leader_) {
        std::vector<tAutomataState*>& states = StateStack();
        for (int index = static_cast<int>(states.size()) - 1; index >= 0; --index) {
            tAutomataState* state = states[index];
            if (state->stateType != kPedestrianStateRiotFollower || !state->controller) continue;
            cISC4Occupant* occupant = state->controller->GetOccupant();
            if (!occupant) continue;
            if (leader_) {
                cSC4Automaton* previous = leader_;
                leader_ = nullptr;
                previous->Release();
            }
            void* found = nullptr;
            occupant->QueryInterface(kAutomatonIid, &found);
            leader_ = static_cast<cSC4Automaton*>(found);
            break;
        }
        if (!leader_) {
            UpdateAttraction();
            return true;
        }
    }
    if (!VerifyNetworkOccupant()) {
        Remove(false);
        return false;
    }
    cS3DVector3 steer;
    if (crowdReactionDirectionSet_) {
        const float x = RandomRange(-1.0, 1.0);
        const float z = RandomRange(-1.0, 1.0);
        steer = MakeVector(x, 0.0f, z);
    } else {
        steer = SidewaysNudge(orientation_, RandomRange(-0.6, 0.6));
    }
    float leaderPosition[3] = {0.0f, 0.0f, 0.0f};
    leader_->GetPosition(leaderPosition);
    float toLeaderX = leaderPosition[0] - position_.x;
    float toLeaderZ = leaderPosition[2] - position_.z;
    if (std::sqrt(toLeaderZ * toLeaderZ + (toLeaderX * toLeaderX + 0.0f)) > kPedestrianStrayDistance) {
        toLeaderX *= kPedestrianStrayPull;
        toLeaderZ *= kPedestrianStrayPull;
        steer = MakeVector(steer.x + toLeaderX, steer.y + 0.0f, steer.z + toLeaderZ);
    }
    steer = Normalized(steer);
    SpeedTween().SetDesired(kPedestrianCrowdPush, 0.0f);
    MoveForwardWithCollision(steer, milliseconds);
    UpdateAttraction();
    return true;
}

// 0x1000f2cb2: the ignite state asks the disaster layer to set the controller's occupant on fire (cause 2), then the
// pedestrian goes back to the idle state with a timer of -1. A state stack without such a controller only goes idle.
bool cSC4Pedestrian::DoIgniteTick(float milliseconds) {
    (void)milliseconds;
    std::vector<tAutomataState*>& states = StateStack();
    for (int index = static_cast<int>(states.size()) - 1; index >= 0; --index) {
        tAutomataState* state = states[index];
        if (state->stateType != kPedestrianStateIgnite || !state->controller) continue;
        cISC4Occupant* occupant = state->controller->GetOccupant();
        if (!occupant) continue;
        if (pedestrianHost_) pedestrianHost_->IgniteOccupant(occupant);
        break;
    }
    SetState(kPedestrianStateIdleReturn, nullptr, -1.0f, false, 0, 0, nullptr);
    return true;
}

// 0x1000f385e: the pedestrian manager (the traffic automaton's manager view).
ITrafficAutomataManager* cSC4Pedestrian::GetTrafficAutomataManager() const { return Manager(); }

// ---- persistence (stream version 5) -------------------------------------------------------------------------------------
// 0x1000f3626: version 5, then the base record, the crowd-reaction direction flag, the direction, the animation speed and
// the crowd-reaction state.
bool cSC4Pedestrian::Write(cIGZOStream& stream) const {
    stream.SetUint16(5);
    if (cSC4TrafficAutomaton::Write(stream)) {
        stream.SetUint8(crowdReactionDirectionSet_ ? 1u : 0u);
        stream << crowdReactionDirection_;
        stream.SetFloat32(animationSpeed_);
        stream.SetUint32(crowdReactionState_);
    }
    return !stream.GetError();   // a failing base record without a stream error still answers true, as compiled
}

// 0x1000f36ce: version 5 only; the base record, then the same four fields.
bool cSC4Pedestrian::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 5) return false;
    if (cSC4TrafficAutomaton::Read(stream)) {
        std::uint8_t flag = 0;
        stream.GetUint8(flag);
        crowdReactionDirectionSet_ = flag != 0;
        stream >> crowdReactionDirection_;
        stream.GetFloat32(animationSpeed_);
        stream.GetUint32(crowdReactionState_);
    }
    return !stream.GetError();
}

// ---- cSC4PedestrianOneShot ---------------------------------------------------------------------------------------------
bool cSC4PedestrianOneShot::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZCLSID_cSC4PedestrianOneShot && out) {
        *out = this;
        AddRef();
        return true;
    }
    return cSC4Pedestrian::QueryInterface(iid, out);
}

// i386 0x000b1f4c: the models play at the pedestrian's animation speed; the play count is a roll in the tuning's range, or
// in the exemplar's own when it has one of two or more bytes.
bool cSC4PedestrianOneShot::Init() {
    if (!cSC4Pedestrian::Init()) return false;
    for (IAutomatonModel* model : Models())
        if (model) model->SetAutoFrameRate(kPedestrianAnimationSpeed);
    const PedestrianTuning& tuning = Tuning();
    std::uint32_t low = tuning.oneShotAnimMin;
    std::uint8_t high = tuning.oneShotAnimMax;
    std::vector<std::uint8_t> own;
    if (cSCPropertyHelp::GetPropertyValue(AsPropertyHolder(), kPedestrianOneShotAnimCountProperty, own) && own.size() > 1) {
        low = own[0];
        high = own[1];
    }
    animCount_ = static_cast<std::uint8_t>(low + RandomBelow((high - low) + 1));
    return true;
}

// i386 0x000b1e66: no movement at all: the opacity tween, then every model's animation; when the first model stands on its
// last frame one play is used up, and the last one removes the figure (the tick then answers false).
bool cSC4PedestrianOneShot::DoAnimationTick(std::uint32_t unknown, float milliseconds) {
    (void)unknown;
    UpdateOpacity(milliseconds);
    bool alive = true, first = true;
    const std::vector<IAutomatonModel*> models = Models();   // Remove may empty the list
    for (IAutomatonModel* model : models) {
        if (!model) continue;
        model->UpdateAnimation(milliseconds);
        if (first && model->GetFrameCount() - 1 <= model->GetFrame()) {
            if (animCount_ == 0 || --animCount_ == 0) {
                Remove(false);
                alive = false;
            }
        }
        first = false;
    }
    return alive;
}

}  // namespace sc4
