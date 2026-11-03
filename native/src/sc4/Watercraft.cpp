#include "sc4/Watercraft.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "gz/cIGZMessage2.h"
#include "rz/cRZString.h"
#include "s3d/S3DMath.h"
#include "sc4/AutomataStates.h"

namespace sc4 {

const char* const kWatercraftAnimationNames[kWatercraftAnimationCount] = {
    "SMLmushroomcloud",   // +0xf58: the sinking explosion
    "boat_grounding",     // +0xf60: run aground
    "boat_splash",        // +0xf68: sinking splash
    "tugropethrow",       // +0xf70: tow rope thrown
    "tugropeconnection",  // +0xf78: tow rope connected
    "waterski",           // +0xf80: player-drive wake
};

namespace {

constexpr float kRadiansToDegrees = 57.2957795f;   // 0x100a23530 family: the original's *57.29578
constexpr float kHalfPi = 1.5707964f;
constexpr float kTwoPi = 6.2831855f;

// The original's min/max idiom: NaN falls through to the upper bound (its compares are false).
float Clamp(float value, float low, float high) {
    if (value < low) return low;
    if (high < value) return high;
    return value;
}

float Length(const cS3DVector3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

float Length2D(const cS3DVector3& v) {
    return std::sqrt(v.x * v.x + v.z * v.z);
}

cS3DVector3 Normalize(const cS3DVector3& v) {
    const float length = Length(v);
    if (length == 0.0f) return cS3DVector3{0.0f, 0.0f, 0.0f};
    const float inverse = 1.0f / length;
    return cS3DVector3{v.x * inverse, v.y * inverse, v.z * inverse};
}

cS3DVector3 Normalize2D(const cS3DVector3& v) {
    const float length = Length2D(v);
    if (length == 0.0f) return cS3DVector3{0.0f, 0.0f, 0.0f};
    const float inverse = 1.0f / length;
    return cS3DVector3{v.x * inverse, 0.0f, v.z * inverse};
}

// The rotation matrix of a quaternion (x, y, z, w), written into the transform's 3x3 block. This is the matrix the
// original's inlined quaternion code produces (Movement_Pitch / Movement_Rotation).
void SetQuaternionRotation(cS3DTransform& transform, float x, float y, float z, float w) {
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;
    transform.rotation[0][0] = 1.0f - 2.0f * (yy + zz);
    transform.rotation[0][1] = 2.0f * (xy - wz);
    transform.rotation[0][2] = 2.0f * (xz + wy);
    transform.rotation[1][0] = 2.0f * (xy + wz);
    transform.rotation[1][1] = 1.0f - 2.0f * (xx + zz);
    transform.rotation[1][2] = 2.0f * (yz - wx);
    transform.rotation[2][0] = 2.0f * (xz - wy);
    transform.rotation[2][1] = 2.0f * (yz + wx);
    transform.rotation[2][2] = 1.0f - 2.0f * (xx + yy);
    transform.flags = static_cast<std::uint8_t>(transform.flags | nS3D::kTransformRotation);
    ++transform.changeCounter;
}

// v' = M v for the transform's row-major 3x3 block.
cS3DVector3 TransformVector(const cS3DTransform& transform, const cS3DVector3& v) {
    return cS3DVector3{
        transform.rotation[0][0] * v.x + transform.rotation[0][1] * v.y + transform.rotation[0][2] * v.z,
        transform.rotation[1][0] * v.x + transform.rotation[1][1] * v.y + transform.rotation[1][2] * v.z,
        transform.rotation[2][0] * v.x + transform.rotation[2][1] * v.y + transform.rotation[2][2] * v.z,
    };
}

void SetForwardUpRotation(cS3DTransform& transform, const cS3DVector3& forward, const cS3DVector3& up,
                          bool withTranslation, const cS3DVector3& translation) {
    cS3DVector3 right = Normalize(cS3DVector3{
        up.y * forward.z - up.z * forward.y,
        up.z * forward.x - up.x * forward.z,
        up.x * forward.y - up.y * forward.x,
    });
    const cS3DVector3 up2 = cS3DVector3{
        forward.y * right.z - forward.z * right.y,
        forward.z * right.x - forward.x * right.z,
        forward.x * right.y - forward.y * right.x,
    };
    transform.rotation[0][0] = right.x;
    transform.rotation[1][0] = right.y;
    transform.rotation[2][0] = right.z;
    transform.rotation[0][1] = up2.x;
    transform.rotation[1][1] = up2.y;
    transform.rotation[2][1] = up2.z;
    transform.rotation[0][2] = forward.x;
    transform.rotation[1][2] = forward.y;
    transform.rotation[2][2] = forward.z;
    if (withTranslation) {
        transform.translation[0] = translation.x;
        transform.translation[1] = translation.y;
        transform.translation[2] = translation.z;
        transform.flags = static_cast<std::uint8_t>(transform.flags | nS3D::kTransformTranslation);
    }
    transform.flags = static_cast<std::uint8_t>(transform.flags | nS3D::kTransformRotation);
    ++transform.changeCounter;
}

}  // namespace

// ---- construction / destruction -------------------------------------------------------------------------------------
// @ 0x1004ef26a (the C2/C1 alias) and @ 0x1004f4b2a (the body). The original ctor takes no host; the port passes the
// seam through to the automaton base.
cSC4Watercraft::cSC4Watercraft(IWatercraftHost* host, const cGZPersistResourceKey& modelKey)
    : cSC4Automaton(host, modelKey), watercraftHost_(host), random_(0xffffffffu) {
    type_ = kWatercraftOccupantType;                                     // +0x50
    flags_ = static_cast<std::uint8_t>(flags_ | 0x10);                   // +0x5b
    // The original constructs the two compressed route readers in the ctor; the port asks the host for them.
    if (watercraftHost_) {
        currentRoute_.reset(watercraftHost_->CreateRouteReader());
        neighbourRouteReader_.reset(watercraftHost_->CreateRouteReader());
    }
    previousPosition_ = position_;
    simulatedPosition_ = position_;
    previousOrientation_ = orientation_;
    simulatedOrientation_ = orientation_;
}

// @ 0x1004fdf0a / 0x1004fdf14 (thunks), body @ 0x1004fe50e. The members are RAII in the port (auto-refcounts and
// unique_ptr), so the body only releases the named animations that the original frees explicitly.
cSC4Watercraft::~cSC4Watercraft() = default;

// ---- COM -------------------------------------------------------------------------------------------------------------
// @ 0x1004f604e. The four base ids (1, 0xa9b40f05, 0x26bb767e, 0xe4fda3d4, 0x452294aa) fall through to the automaton.
bool cSC4Watercraft::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    if (iid == kWatercraftIid) {
        *out = this;
        AddRef();
        return true;
    }
    return cSC4Automaton::QueryInterface(iid, out);
}

bool cSC4Watercraft::Write(cIGZOStream& stream) const {
    stream.SetUint16(2);   // the watercraft record version
    return cSC4Automaton::Write(stream);
}

bool cSC4Watercraft::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 2) return false;
    return cSC4Automaton::Read(stream);
}

// ---- drive parameters ------------------------------------------------------------------------------------------------
IWatercraftGroupTemplate* cSC4Watercraft::WatercraftGroup() const {
    // The original walks the group's parent chain; only nodes whose +0xe0 property is non-null are used.
    for (IAutomatonGroupTemplate* group = GetAutomataGroup(); group; group = group->Parent()) {
        if (auto* watercraftGroup = dynamic_cast<IWatercraftGroupTemplate*>(group)) {
            if (watercraftGroup->DriveParameters()) return watercraftGroup;
        }
    }
    return nullptr;
}

// @ 0x1004f54c6. The property array is 40 floats; a zero entry means "default" (quirky but verified in the
// original: each store is `value != 0 ? value : default`). f8/f17/f19 are derived from their neighbours when zero.
void cSC4Watercraft::LoadDriveParameters() {
    const float* property = nullptr;
    for (IAutomatonGroupTemplate* group = GetAutomataGroup(); group; group = group->Parent()) {
        if (auto* watercraftGroup = dynamic_cast<IWatercraftGroupTemplate*>(group)) {
            property = watercraftGroup->DriveParameters();
            if (property) break;
        }
    }
    WatercraftDriveParameters& p = driveParameters_;
    if (!property) {
        p = WatercraftDriveParameters{};
        return;
    }
    auto pick = [property](int index, float fallback) {
        const float value = property[index];
        return value != 0.0f ? value : fallback;
    };
    p.reverseAcceleration = pick(0, 0.1f);
    p.acceleration = pick(1, 0.001f);
    p.reverseRate = pick(2, 1.0f);
    p.brakeRate = pick(3, 0.05f);
    p.movementScale = pick(4, 0.2f);
    p.reverseBoost = pick(5, 5.0f);
    p.minSpeed = pick(6, -0.1f);
    p.maxSpeed = pick(7, 0.3f);
    const float maxTurn = pick(9, 2.0f);
    p.minTurn = property[8] != 0.0f ? property[8] : -maxTurn;
    p.maxTurn = maxTurn;
    p.timeScale = pick(10, 0.001f);
    p.turnStride = pick(11, 50.0f);
    p.reverseTurnStride = pick(12, 5.0f);
    p.aiTimeScale = pick(13, 0.001f);
    p.turnRampScale = pick(14, 0.001f);
    p.boatCollisionThreshold = pick(15, 0.1f);
    p.staticCollisionThreshold = pick(16, 0.5f);
    const float maxRoll = pick(18, 0.31415927f);
    const float maxSpeed = p.maxSpeed;
    p.rollRate = property[17] != 0.0f ? property[17] : maxRoll / maxSpeed;
    p.maxRoll = maxRoll;
    const float maxPitch = pick(20, 0.26179940f);
    p.pitchRate = property[19] != 0.0f ? property[19] : maxPitch / maxTurn;
    p.maxPitch = maxPitch;
    p.crashStateTimer = pick(21, 8000.0f);
    p.sinkMaxProgress = pick(22, 0.78539753f);
    p.agroundSpeedScale = pick(23, 0.95f);
    p.agroundProgressRate = pick(24, 2.1816616f);
    p.crashAngleDegrees = pick(25, 1.0471976f);
    p.groundHeightTolerance = pick(26, 10.0f);
    p.playerBoatCollisionThreshold = pick(27, 0.5f);
    p.playerStaticCollisionThreshold = pick(28, 0.25f);
    p.collisionSpeedScale = pick(29, 0.75f);
    p.speedMin = pick(30, -2000.0f);
    p.speedMax = pick(31, 3000.0f);
    p.leftTriggerThreshold = pick(32, 150.0f);
    p.rightTriggerThreshold = property[33];   // zero stays zero (the original's asymmetric idiom)
    p.value568 = pick(34, 2.0f);
    p.maxStep = pick(35, 100.0f);
    p.dockRequestRadius = pick(36, 300.0f);
    p.dockDistance = pick(37, 90.0f);
    p.dockSpeedScale = pick(38, 1.0f);
    p.dockTurnScale = pick(39, 1.0f);
}

// ---- Init / Shutdown -------------------------------------------------------------------------------------------------
// @ 0x1004f4e06. Only runs the first time: the occupant's IsInitialized (vslot 0x28) gates it.
bool cSC4Watercraft::Init() {
    if (IsInitialized()) return true;
    cSC4Automaton::Init();
    LoadDriveParameters();
    towTarget_ = nullptr;
    tower_ = nullptr;
    for (auto& animation : animations_) animation = nullptr;
    animationLoadCount_ = 0;
    if (watercraftHost_) {
        for (int i = 0; i < kWatercraftAnimationCount; ++i) {
            IWatercraftAnimation* animation = watercraftHost_->CreateNamedAnimation(kWatercraftAnimationNames[i]);
            animations_[i] = animation;
            if (animation) {
                ++animationLoadCount_;
                animation->Release();   // the creation reference; the auto-refcount keeps the animation alive
            }
        }
    }
    SetAutomataFlag(kAutomataFlagWatercraft0x8000, true);
    SetAutomataFlag(kAutomataFlagIgnorePaths, true);
    SetAutomataFlag(kAutomataFlagIgnoreRoads, true);
    SetAutomataFlag(kAutomataFlagReachedDestination, true);
    if (watercraftHost_) {
        watercraftHost_->AddNotification(static_cast<cIGZMessageTarget2*>(this), kWatercraftMessageTow);
    }
    towSearchActive_ = false;
    stationA_.SetQueued(false);
    stationA_.lot = nullptr;
    stationB_.SetQueued(false);
    stationB_.lot = nullptr;
    currentStation_.SetQueued(false);   // +0x3d9 / current station's route flag
    currentStation_.lot = nullptr;
    lookahead_.clear();
    driveMode_ = 0;
    dockingFlag_ = false;
    queueingFlag_ = false;
    dockingActive_ = false;
    turningFlag_ = false;
    crashMarked_ = false;
    following_ = false;
    animationHold_ = false;
    neighbourRouteSet_ = false;
    neighbour_ = nullptr;
    towWaitThreshold_ = 0.0f;
    towWaitElapsed_ = 0.0f;
    stillTimer_ = 0.0f;
    reverseTimer_ = 0.0f;
    speed_ = 0.0f;
    turn_ = 0.0f;
    field464_ = 0;
    crashTimer_ = 0.0f;
    crashProgress_ = 0.0f;
    previousPosition_ = position_;
    simulatedPosition_ = position_;
    previousOrientation_ = orientation_;
    simulatedOrientation_ = orientation_;
    crashRollEnabled_ = true;
    idleTimer_ = 0.0f;
    EnableActiveSound(true);
    return true;
}

// @ 0x1004f5b7a.
bool cSC4Watercraft::Shutdown() {
    if (IsInitialized()) {
        if (towTarget_) {
            cSC4Watercraft* target = towTarget_.get();
            target->PushState(kWatercraftStateRemoveFade, nullptr, -1.0f, false, 0, 0, nullptr);
            towTarget_ = nullptr;
        }
        tower_ = nullptr;
        if (watercraftHost_) {
            if (stationA_.Queued()) {
                watercraftHost_->DoneQueueing(stationA_.lot.get(), this);
                watercraftHost_->DoneDocking(stationA_.lot.get(), this);
            }
            if (stationB_.Queued()) {
                watercraftHost_->DoneQueueing(stationB_.lot.get(), this);
                watercraftHost_->DoneDocking(stationB_.lot.get(), this);
            }
            watercraftHost_->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kWatercraftMessageTow);
        }
        stationA_.SetQueued(false);
        stationA_.lot = nullptr;
        stationB_.SetQueued(false);
        stationB_.lot = nullptr;
        currentStation_.SetQueued(false);
        currentStation_.lot = nullptr;
        neighbour_ = nullptr;
        for (auto& animation : animations_) {
            if (animation) animation->SetPaused(true);
            animation = nullptr;
        }
        animationLoadCount_ = 0;
    }
    return cSC4Automaton::Shutdown();
}

// ---- messages --------------------------------------------------------------------------------------------------------
// @ 0x1004f6116. The 0x26d31ec1 "reload drive parameters" message is fully recovered. The 0xac400a31 tow-request
// message only clears the flag here: its payload (the position of the boat looking for a tow and its occupant groups)
// is read through cIGZMessage2Standard's GetIGZUnknown and the 0x78/get-position slots of that unknown, which the
// port does not model yet (see PARTIAL in the ledger).
bool cSC4Watercraft::DoMessage(cIGZMessage2* message) {
    bool handled = false;
    if (message) {
        const std::uint32_t type = message->GetType();
        if (type == kWatercraftMessageTow) {
            if (towSearchActive_) towSearchActive_ = false;
        } else if (type == kWatercraftMessageReloadDriveParams) {
            LoadDriveParameters();
            handled = true;
        }
    }
    const bool base = cSC4Automaton::DoMessage(message);
    return base || handled;
}

// ---- state helpers ---------------------------------------------------------------------------------------------------
// @ 0x1004f1050. The original's eW_ChangeState: 0 idle, 1 remove, 2 sink, 3 run aground, 4 run aground with roll.
void cSC4Watercraft::SetStateInternal(eW_ChangeState change) {
    switch (change) {
        case kChangeIdle:
            PushState(kWatercraftStateRemoveNow, nullptr, -1.0f, false, 0, 0, nullptr);
            break;
        case kChangeRemove:
            PushState(kWatercraftStateRemoveFade, nullptr, -1.0f, false, 0, 0, nullptr);
            break;
        case kChangeRunAground:
            SendCallbackMessage(0xb, 0);
            PushState(kWatercraftStateRunAground, nullptr, driveParameters_.crashStateTimer, true, 0, 0, nullptr);
            break;
        case kChangeSinkRoll:
            crashMarked_ = true;   // quirk: the original sets the crash-marked byte before falling through
            [[fallthrough]];
        case kChangeSink:
            SendCallbackMessage(0xb, 0);
            PushState(kWatercraftStateSink, nullptr, driveParameters_.crashStateTimer, true, 0, 0, nullptr);
            break;
        default:
            break;
    }
}

// @ 0x1004f6414: only a player-driven boat can enter an actual crash state; everyone else is removed.
void cSC4Watercraft::Crash_SetStateInternal(eW_ChangeState change, bool force) {
    if (!force && GetBehavior() != kStatePlayerDrive) {
        PushState(kWatercraftStateRemoveFade, nullptr, -1.0f, false, 0, 0, nullptr);
        return;
    }
    SetStateInternal(change);
}

// @ 0x1004f63b0: remembers the requested step in the simulation fields, then moves through the base automaton.
void cSC4Watercraft::MoveTo(const float* position, const float* direction) {
    if (position) {
        simulatedPosition_ = cS3DVector3{position[0], position[1], position[2]};
    }
    if (direction) {
        simulatedOrientation_ = cS3DVector3{direction[0], direction[1], direction[2]};
    }
    cSC4Automaton::MoveTo(position, direction);
    UpdateEffects();
    UpdateLighting();
}

// ---- ticks -----------------------------------------------------------------------------------------------------------
// @ 0x1004f6484. Dispatch on the behaviour; the idle-timeout path deliberately falls through into the crash handler
// and adds the frame to the crash timer a second time (quirk kept).
bool cSC4Watercraft::DoStateTick(float milliseconds) {
    switch (GetBehavior()) {
        case kWatercraftStateRunAground:
        case kWatercraftStateSink:
            crashTimer_ += milliseconds;
            DoCrashSequenceTick(milliseconds);
            break;
        case kWatercraftStateRemoveNow:
            Remove(true);
            return true;
        case kWatercraftStateRemoveFade:
            Remove(false);
            return true;
        case kWatercraftStateDocking:
            if (!DoDockingTick(milliseconds)) PopState();
            break;
        case kWatercraftStateQueueing:
            Ferry_DockRequests();
            break;
        case kWatercraftStateWaiting:
            break;
        case kWatercraftStateFollowing:
            DoFollowingTick(milliseconds);
            break;
        case kWatercraftStateUndocking:
            if (!DoUndockingTick(milliseconds)) PopState();
            break;
        default:
            if (GetBehavior() == kStateIdleTick) {
                const float previous = idleTimer_;
                idleTimer_ += milliseconds;
                if (previous + milliseconds <= 60000.0f) return cSC4Automaton::DoStateTick(milliseconds);
                PushState(kWatercraftStateRemoveFade, nullptr, -1.0f, false, 0, 0, nullptr);
                crashTimer_ += milliseconds;
                DoCrashSequenceTick(milliseconds);
                break;
            }
            break;
    }
    return cSC4Automaton::DoStateTick(milliseconds);
}

// @ 0x1004f65e0.
void cSC4Watercraft::DoCrashSequenceTick(float milliseconds) {
    if (OutOfBounds()) {
        PushState(kWatercraftStateRemoveNow, nullptr, -1.0f, false, 0, 0, nullptr);
        return;
    }
    cS3DTransform transform;
    GetTransform(transform);
    const std::uint32_t behavior = GetBehavior();
    if (behavior == kWatercraftStateSink) {
        crashTimer_ += milliseconds;
        StopRegularEffects();
        SpeedTween().SetDesired(0.0f, 0.0f);
        if (crashMarked_ && animations_[0]) {
            const bool playing = animations_[0]->IsPlaying();
            animations_[0]->SetTransform(transform);
            if (!playing) animations_[0]->Play(false);
        }
        if (animations_[2]) {
            if (!animations_[2]->IsPlaying()) {
                animations_[2]->SetTransform(transform);
                animations_[2]->Play(false);
            }
        }
        Crash_SequenceSink();
    } else if (behavior == kWatercraftStateRunAground) {
        crashTimer_ += milliseconds;
        StopRegularEffects();
        SpeedTween().SetDesired(0.0f, 0.0f);
        GetTransform(transform);
        if (animations_[1]) {
            const bool playing = animations_[1]->IsPlaying();
            animations_[1]->SetTransform(transform);
            if (!playing) animations_[1]->Play(false);
        }
        Crash_SequenceRunAground(milliseconds);
    }
}

// @ 0x1004f9194. Return false only through the 5000 ms idle path (the original removes the boat there).
bool cSC4Watercraft::DoAnimationTick(std::uint32_t unknown, float milliseconds) {
    (void)unknown;
    if (GetSpeedModifier() < 1e-05f) return true;
    if (animationHold_) {
        idleTimer_ += milliseconds;
        if (60000.0f < idleTimer_) {
            PushState(kWatercraftStateRemoveFade, nullptr, -1.0f, false, 0, 0, nullptr);
        }
    }
    if (speed_ != 0.0f || std::isnan(speed_)) UpdateActiveSound(milliseconds);
    if (!towTarget_) {
        if (animations_[4] && animations_[4]->IsPlaying()) animations_[4]->SetPaused(false);
    } else {
        cS3DTransform transform;
        GetTransform(transform);
        if (animations_[4]) {
            animations_[4]->SetTransform(transform);
            if (!animations_[4]->IsPlaying()) animations_[4]->Play(false);
        }
    }
    const bool base = cSC4Automaton::DoAnimationTick(unknown, milliseconds);
    if (!base) return false;
    reverseTimer_ = speed_ < 0.0f && speed_ != 0.0f ? reverseTimer_ + milliseconds : 0.0f;
    static bool noticeableInitialized = false;
    static float noticeableDistance = 0.0f;
    if (!noticeableInitialized) {
        noticeableInitialized = true;
        noticeableDistance = (watercraftHost_ ? watercraftHost_->CityExtent() : 0.0f) / 5.0f;
    }
    const cS3DVector3 orientationDelta = simulatedOrientation_ - previousOrientation_;
    const cS3DVector3 positionDelta = simulatedPosition_ - previousPosition_;
    if (Length(orientationDelta) >= 0.01f || speed_ != 0.0f || std::isnan(speed_)) {
        StartRegularEffects();
        stillTimer_ = 0.0f;
    } else if (Length(positionDelta) >= noticeableDistance) {
        StartRegularEffects();
        stillTimer_ = 0.0f;
    } else {
        stillTimer_ += milliseconds;
        if (250.0f < stillTimer_) StopRegularEffects();
        if (5000.0f < stillTimer_) {
            if (!FindState(kStateIdleTick) && !FindState(kWatercraftStateFollowing) &&
                !FindState(kWatercraftStateWaiting) && !FindState(kWatercraftStateQueueing) &&
                !FindState(kWatercraftStateUndocking) && !FindState(kStatePlayerDrive)) {
                return false;
            }
        }
    }
    if (!FindState(kStatePlayerDrive) && !FindState(kWatercraftStateQueueing) && !FindState(kWatercraftStateDocking) &&
        !FindState(kWatercraftStateUndocking) && !FindState(kWatercraftStateFollowing) &&
        !neighbourRouteSet_ && !animationHold_) {
        if (towWaitThreshold_ == 0.0f || towWaitElapsed_ <= towWaitThreshold_) {
            if (!FindState(kWatercraftStateWaiting) && GetBehavior() != kStateIdleTick) {
                towWaitElapsed_ += milliseconds;
                if (!Engine_MovementPossible()) {
                    if (OnLand()) PushState(kWatercraftStateRemoveNow, nullptr, -1.0f, false, 0, 0, nullptr);
                } else {
                    float step = milliseconds <= driveParameters_.maxStep ? milliseconds : driveParameters_.maxStep;
                    step = milliseconds < 0.0f ? 0.0f : step;
                    Engine_AIBoatMotion(step);
                    Movement_PositionNew(step);
                    Engine_DetectCollision();
                    Ferry_DockRequests();
                    Ferry_ClearDockRequests();
                }
            }
        } else {
            if (watercraftHost_ && watercraftHost_->CanDockNow(this)) {
                if (!FindState(kWatercraftStateWaiting)) {
                    SetAutomataFlag(kAutomataFlagNoCollision, false);
                    towWaitThreshold_ = 0.0f;
                    UpdateEffects();
                    UpdateLighting();
                    return true;
                }
            }
            PushState(kWatercraftStateRemoveFade, nullptr, -1.0f, false, 0, 0, nullptr);
        }
    }
    UpdateEffects();
    UpdateLighting();
    return true;
}

// @ 0x1004f9ce0.
bool cSC4Watercraft::DoPlayerDriveTick(float milliseconds) {
    cSC4Automaton::DoPlayerDriveTick(milliseconds);
    if (towTarget_) {
        const cS3DVector3 delta = towTarget_->simulatedPosition_ - simulatedPosition_;
        const float inverse = 1.0f / Length(delta);
        if (0.0f < simulatedOrientation_.x * delta.x * inverse + simulatedOrientation_.y * delta.y * inverse +
                        simulatedOrientation_.z * delta.z * inverse) {
            EndTowing();
        }
    }
    tAutomataState* current = GetState();
    if (current && current->stateType == kStatePlayerDrive) {
        auto* drive = static_cast<tAutomataStatePlayerDrive*>(current);
        if (drive->history[0] == -1.0f && drive->history[1] == -1.0f) {
            if (speed_ != 0.0f || std::isnan(speed_)) {
                const float sign = speed_ < 0.0f ? -1.0f : 1.0f;
                float updated = (milliseconds / -1000.0f) * driveParameters_.brakeRate * sign + speed_;
                const float updatedSign = updated < 0.0f ? -1.0f : 1.0f;
                speed_ = 0.0f < updatedSign == 0.0f < sign ? updated : 0.0f;
            }
        }
    }
    if (Engine_MovementPossible()) {
        float step = milliseconds <= driveParameters_.maxStep ? milliseconds : driveParameters_.maxStep;
        step = milliseconds < 0.0f ? 0.0f : step;
        Movement_PositionNew(step);
        Engine_DetectCollision();
    }
    return true;
}

// @ 0x1004f9e90. The four corners of the occupant's bounding cell rect; inside only when all four are city cells.
bool cSC4Watercraft::OutOfBounds() {
    SC4CellRect rect;
    GetBoundingCityCells(rect);
    if (!watercraftHost_) return false;
    if (!watercraftHost_->CellInCity(rect.x0, rect.z0)) return true;
    if (!watercraftHost_->CellInCity(rect.x0, rect.z1)) return true;
    if (!watercraftHost_->CellInCity(rect.x1, rect.z0)) return true;
    if (!watercraftHost_->CellInCity(rect.x1, rect.z1)) return true;
    return false;
}

// @ 0x1004f7e06: aground when the terrain at the boat's cell is above the boat.
bool cSC4Watercraft::OnLand() {
    if (!watercraftHost_) return false;
    int cellX = 0, cellZ = 0;
    watercraftHost_->WorldToCell(simulatedPosition_.x, simulatedPosition_.z, cellX, cellZ);
    if (!watercraftHost_->CellInCity(cellX, cellZ)) return false;
    const float cellHeight = watercraftHost_->TerrainHeightAtCell(cellX, cellZ);
    const float height = watercraftHost_->TerrainHeight(simulatedPosition_.x, simulatedPosition_.z);
    return height < cellHeight;
}

// @ 0x1004f910e.
bool cSC4Watercraft::Engine_MovementPossible() {
    if (CheckAutomataFlag(kAutomataFlagMarkedForRemove)) return false;
    if (OutOfBounds()) {
        PushState(kWatercraftStateRemoveNow, nullptr, -1.0f, false, 0, 0, nullptr);
        return false;
    }
    const std::uint32_t behavior = GetBehavior();
    return behavior != kWatercraftStateRunAground && behavior != kWatercraftStateSink;
}

// ---- effects ---------------------------------------------------------------------------------------------------------
// @ 0x1004f976a: stop the six regular wake/wash effects.
void cSC4Watercraft::StopRegularEffects() {
    static const char* const names[] = {"SMLWake", "SMLWash", "SMLMedWake", "SMLMedWash", "LargeWake", "LargeWash"};
    for (const char* name : names) StopNamedEffects(cRZString(name), 0);
}

// @ 0x1004f99aa.
void cSC4Watercraft::StartRegularEffects() {
    static const char* const names[] = {"SMLWake", "SMLWash", "SMLMedWake", "SMLMedWash", "LargeWake", "LargeWash"};
    for (const char* name : names) StartNamedEffects(cRZString(name), 0, false);
}

// ---- ferry / stations ------------------------------------------------------------------------------------------------
// @ 0x1004f1cc8. Copies both station records; with `initializeRoute` the current station becomes station A and its
// route is loaded. The original AddRefs station A's lot around the copy (a temporary cRZAutoRefCount) - RAII here.
void cSC4Watercraft::Ferry_SetStationInformation(const FerryStationInformation& stationA,
                                                 const FerryStationInformation& stationB, bool initializeRoute) {
    stationA_ = stationA;
    stationB_ = stationB;
    if (!initializeRoute) return;
    cRZAutoRefCount<cSC4Lot> keepAlive = stationA_.lot;
    Ferry_InitializeCurrentRoute(stationA_);
    dockingActive_ = true;
}

// @ 0x1004f27aa: groups 0x440f / 0x440a / 0x4411.
bool cSC4Watercraft::IsNeighborTravelBoat() {
    cISC4Occupant* occupant = AsOccupant();
    if (occupant->IsOccupantGroup(kWatercraftGroupTravelBoat2)) return true;
    if (occupant->IsOccupantGroup(kWatercraftGroupTravelBoat1)) return true;
    return occupant->IsOccupantGroup(kWatercraftGroupTravelBoat3) != 0;
}

// @ 0x1004f2818. Only stores the route when the neighbour's points stay within 3 radii of the neighbour (the route
// describes the neighbour's path; a distant point invalidates it).
void cSC4Watercraft::SetNbrRouteData(const std::vector<std::uint8_t>& route, cISC4Occupant* occupant) {
    neighbour_ = occupant;
    neighbourRouteSet_ = true;
    neighbourRoute_ = route;
    if (!neighbourRouteReader_ && watercraftHost_) {
        neighbourRouteReader_.reset(watercraftHost_->CreateRouteReader());
    }
    if (neighbourRouteReader_) neighbourRouteReader_->SetData(neighbourRoute_);
    if (!neighbour_) return;
    const float limit = GetRadius() * 3.0f;
    std::int16_t cellX = 0, cellZ = 0;
    while (neighbourRouteReader_ && neighbourRouteReader_->GetNext(cellX, cellZ)) {
        float neighbourPosition[3] = {0.0f, 0.0f, 0.0f};
        neighbour_->GetPosition(neighbourPosition);
        float worldX = 0.0f, worldZ = 0.0f;
        if (watercraftHost_) watercraftHost_->CellToWorld(cellX, cellZ, worldX, worldZ);
        const float height = watercraftHost_ ? watercraftHost_->TerrainHeight(worldX, worldZ) : 0.0f;
        const cS3DVector3 point{worldX, height, worldZ};
        const cS3DVector3 neighbourPoint{neighbourPosition[0], neighbourPosition[1], neighbourPosition[2]};
        if (limit < Length(point - neighbourPoint)) return;
        if (!neighbour_) return;
    }
}

// @ 0x1004f3894: mark direction `index` used and drop the points after index+1.
bool cSC4Watercraft::InvalidateLookaheadCell(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= lookahead_.size()) return false;
    WatercraftTravelPoint& point = lookahead_[static_cast<std::size_t>(index)];
    point.flags |= 1u << (static_cast<std::uint32_t>(Assist_DirLookup(index)) & 0x1f);
    if (static_cast<std::size_t>(index) + 1 < lookahead_.size()) lookahead_.resize(static_cast<std::size_t>(index) + 1);
    return true;
}

// @ 0x1004f7c50: the "other" ferry station. Quirk: it compares station A with station B (never with the current
// station) and returns B when they are at the same position, A otherwise.
FerryStationInformation cSC4Watercraft::Ferry_OtherStationInformation() const {
    const cS3DVector3 delta = stationB_.position - stationA_.position;
    const bool same = std::fabs(delta.x) < 0.0001f && std::fabs(delta.y) < 0.0001f && std::fabs(delta.z) < 0.0001f;
    return same ? stationB_ : stationA_;
}

// @ 0x1004f7720: switch to the other station; without two stations the boat removes itself.
void cSC4Watercraft::Ferry_SwitchActiveStation() {
    if (stationA_.Queued() && stationB_.Queued()) {
        FerryStationInformation other = Ferry_OtherStationInformation();
        Ferry_InitializeCurrentRoute(other);
        return;
    }
    Remove(true);
}

// @ 0x1004f7a24: release the current dock once the boat is far enough away.
void cSC4Watercraft::EndDocking() {
    if (!currentStation_.lot || !currentStation_.dockRequested) return;
    dockingActive_ = false;
    if (watercraftHost_) {
        watercraftHost_->DoneDocking(currentStation_.lot.get(), this);
        currentStation_.dockRequested = 0;
        SetAutomataFlag(kAutomataFlagRemoved, false);
    }
}

// @ 0x1004f7a9c: approach the dock, ask the manager for permission and push docking/queueing states.
void cSC4Watercraft::Ferry_DockRequests() {
    FerryStationInformation other = Ferry_OtherStationInformation();
    if (!other.lot) return;
    float center[3] = {0.0f, 0.0f, 0.0f};
    other.lot->GetCenter(center);
    const cS3DVector3 delta = simulatedPosition_ - cS3DVector3{center[0], center[1], center[2]};
    if (Length(delta) >= driveParameters_.dockRequestRadius) return;
    if (!watercraftHost_) return;
    const std::uint32_t behavior = GetBehavior();
    const bool dock = watercraftHost_->RequestPermissionToDock(other.lot.get(), this);
    if (behavior == kWatercraftStateQueueing) {
        if (!dock) return;
        PopState();
    } else if (!dock) {
        const bool wait = watercraftHost_->RequestPermissionToWait(other.lot.get(), this);
        PushState(wait ? kWatercraftStateQueueing : kWatercraftStateRemoveFade, nullptr, 0.0f, false, 0, 0, nullptr);
        return;
    }
    PushState(kWatercraftStateDocking, nullptr, 0.0f, false, 0, 0, nullptr);
}

// @ 0x1004f9bf6: give the dock back when the boat has moved past the request radius.
void cSC4Watercraft::Ferry_ClearDockRequests() {
    if (!currentStation_.lot || !currentStation_.dockRequested) return;
    float center[3] = {0.0f, 0.0f, 0.0f};
    currentStation_.lot->GetCenter(center);
    const float distance = Length(simulatedPosition_ - cS3DVector3{center[0], center[1], center[2]});
    if (distance < driveParameters_.dockRequestRadius) return;
    dockingActive_ = false;
    if (watercraftHost_) {
        watercraftHost_->DoneDocking(currentStation_.lot.get(), this);
        currentStation_.dockRequested = 0;
        SetAutomataFlag(kAutomataFlagRemoved, false);
    }
}

// @ 0x1004fdbf0. Copies the station into the current-station record, primes the route reader and resets the
// lookahead. The original sets routeIndex to 2 when it is zero and clears the "primed" byte.
void cSC4Watercraft::Ferry_InitializeCurrentRoute(const FerryStationInformation& station) {
    currentStation_ = station;
    currentStation_.dockRequested = 1;
    if (currentStation_.routeIndex == 0) currentStation_.routeIndex = 2;
    currentStation_.flags = static_cast<std::uint16_t>(currentStation_.flags & ~1u);   // +0x3d8 primed byte
    if (currentRoute_) currentRoute_->SetData(currentStation_.route);
    lookahead_.clear();
}

// @ 0x1004f76ba: drop the tow target (pop its state) and the tower.
void cSC4Watercraft::EndTowing() {
    if (towTarget_) {
        towTarget_->PopState();
        towTarget_ = nullptr;
    }
    tower_ = nullptr;
}

// ---- state entry / exit ----------------------------------------------------------------------------------------------
// @ 0x1004f77d2.
bool cSC4Watercraft::HandleEnterState(tAutomataState* state) {
    const bool result = cSC4Automaton::HandleEnterState(state);
    if (!state) return result;
    switch (state->stateType) {
        case kWatercraftStateRunAground:
        case kWatercraftStateSink:
            SetAutomataFlag(kAutomataFlagRemoved, true);
            SetAutomataFlag(kAutomataFlagDisabled, true);
            SendCallbackMessage(0xb, 0);
            crashTimer_ = 0.0f;
            break;
        case kWatercraftStateRemoveNow:
            SetAutomataFlag(kAutomataFlagRemoved, true);
            SetAutomataFlag(kAutomataFlagDisabled, true);
            Remove(true);
            break;
        case kWatercraftStateRemoveFade:
            SetAutomataFlag(kAutomataFlagRemoved, true);
            SetAutomataFlag(kAutomataFlagDisabled, true);
            Remove(false);
            break;
        case kWatercraftStateDocking:
            SetAutomataFlag(kAutomataFlagRemoved, true);
            dockingFlag_ = true;
            dockingActive_ = true;
            EndTowing();
            return result;
        case kWatercraftStateQueueing:
            SetAutomataFlag(kAutomataFlagRemoved, true);
            queueingFlag_ = true;
            speed_ = 0.0f;
            turn_ = 0.0f;
            field464_ = 0;
            EndTowing();
            return result;
        case kWatercraftStateWaiting:
            speed_ = 0.0f;
            turn_ = 0.0f;
            field464_ = 0;
            break;
        case kWatercraftStateFollowing:
            following_ = true;
            SetAutomataFlag(kAutomataFlagRemoved, true);
            EndDocking();
            return result;
        default: {
            if (state->stateType != kStatePlayerDrive) return result;
            EnableActiveSound(false);
            auto* drive = static_cast<tAutomataStatePlayerDrive*>(state);
            drive->value40 = 0;
            drive->value3c = driveParameters_.maxPitch;
            drive->groupSpeed = 1.0f;
            if (IAutomatonGroupTemplate* group = GetAutomataGroup()) {
                for (IAutomatonGroupTemplate* node = group; node; node = node->Parent()) {
                    if (auto* watercraftNode = dynamic_cast<IWatercraftGroupTemplate*>(node)) {
                        drive->groupSpeed = watercraftNode->DriveSpeed();
                        break;
                    }
                }
            }
            drive->value170 = 0xf;
            drive->animStage1a4 = 0;
            drive->timeout1d8 = 0x44fa0000;   // 2000.0f
            break;
        }
    }
    EndTowing();
    EndDocking();
    return result;
}

// @ 0x1004f75c0.
bool cSC4Watercraft::HandleExitState(const tAutomataState* state) {
    if (state) {
        switch (state->stateType) {
            case kStatePlayerDrive:
                EnableActiveSound(true);
                EndTowing();
                break;
            case kWatercraftStateDocking:
                dockingFlag_ = false;
                Ferry_SwitchActiveStation();
                break;
            case kWatercraftStateQueueing:
                SetAutomataFlag(kAutomataFlagRemoved, false);
                queueingFlag_ = false;
                break;
            case kWatercraftStateFollowing:
                SetAutomataFlag(kAutomataFlagRemoved, false);
                towTarget_ = nullptr;
                tower_ = nullptr;
                break;
            default:
                break;
        }
    }
    return cSC4Automaton::HandleExitState(state);
}

// ---- movement helpers ------------------------------------------------------------------------------------------------
// @ 0x1004fcea0. PARTIAL: the original composes the sideways step with a rotation of the caller's transform and then
// steps along the composed basis; the port keeps the sideways step, the yaw composition and the resulting position,
// but approximates the basis composition (the decompiler's SSE register pairing for the matrix rows is ambiguous).
void cSC4Watercraft::Movement_PositionTurning(float milliseconds, cS3DTransform& transform, cS3DVector3& position,
                                              cS3DTransform& extra) {
    const float horizontal = Length2D(simulatedOrientation_);
    const float inverseHorizontal = horizontal != 0.0f ? 1.0f / horizontal : 0.0f;
    const bool reverse = speed_ <= 0.0f;
    const float strideBase = reverse ? driveParameters_.reverseTurnStride : driveParameters_.turnStride;
    const float speedRatio = reverse ? std::fabs(speed_) / std::fabs(driveParameters_.minSpeed)
                                     : speed_ / driveParameters_.maxSpeed;
    const float turnRatio = std::fabs(turn_) / (reverse ? std::fabs(driveParameters_.minTurn)
                                                       : driveParameters_.maxTurn);
    const float stride = ((1.0f - turnRatio) + speedRatio + 0.5f) * strideBase;
    if (stride == 0.0f) return;
    const float side = (turn_ > 0.0f ? simulatedOrientation_.z : -simulatedOrientation_.z) * inverseHorizontal;
    const float forward = (turn_ > 0.0f ? -simulatedOrientation_.x : simulatedOrientation_.x) * inverseHorizontal;
    const float newX = simulatedPosition_.x + side * stride;
    const float newZ = simulatedPosition_.z + forward * stride;
    const float newY = watercraftHost_ ? watercraftHost_->TerrainHeight(newX, newZ) : simulatedPosition_.y;
    cS3DVector3 back{simulatedPosition_.x - newX, simulatedPosition_.y - newY, simulatedPosition_.z - newZ};
    const float backLength = Length(back);
    if (backLength == 0.0f) return;
    back = back * (1.0f / backLength);
    const float angle = (milliseconds * speed_ * driveParameters_.movementScale) / stride;
    // Rotate the caller's transform around its Y axis; a zeroed transform is reset to identity first.
    if (extra.changeCounter != 0 || extra.flags != 0) {
        extra = cS3DTransform{};
    }
    extra.flags = 0;
    const float half = -0.5f * angle;
    const float s = std::sin(half), c = std::cos(half);
    SetQuaternionRotation(extra, 0.0f, s, 0.0f, c);
    // Compose the boat's heading with that rotation, then take a step back along the new basis.
    simulatedOrientation_ = Normalize(TransformVector(extra, simulatedOrientation_));
    const float stepScale = std::fabs(milliseconds * speed_) * driveParameters_.movementScale;
    const float step = stepScale < backLength ? stepScale : backLength;
    position = cS3DVector3{newX + simulatedOrientation_.x * step, newY + simulatedOrientation_.y * step,
                           newZ + simulatedOrientation_.z * step};
    transform.translation[0] = position.x;
    transform.translation[1] = position.y;
    transform.translation[2] = position.z;
    transform.flags = static_cast<std::uint8_t>(transform.flags | nS3D::kTransformTranslation);
    ++transform.changeCounter;
}

// @ 0x1004fd4c4.
void cSC4Watercraft::Movement_PositionStraight(float milliseconds, cS3DTransform& transform, cS3DVector3& position) {
    const float horizontal = Length2D(simulatedOrientation_);
    const float inverse = horizontal != 0.0f ? 1.0f / horizontal : 0.0f;
    const float step = milliseconds * speed_ * driveParameters_.movementScale;
    position.x = simulatedOrientation_.x * inverse * step + simulatedPosition_.x;
    position.y = inverse * 0.0f * step + simulatedPosition_.y;
    position.z = step * inverse * simulatedOrientation_.z + simulatedPosition_.z;
    transform.translation[0] = position.x;
    transform.translation[1] = position.y;
    transform.translation[2] = position.z;
    transform.flags = static_cast<std::uint8_t>(transform.flags | nS3D::kTransformTranslation);
    ++transform.changeCounter;
}

// @ 0x1004fda16: pitch about the heading axis by speed/maxSpeed * turn * pitchRate, clamped to +-maxPitch.
void cSC4Watercraft::Movement_Pitch(cS3DTransform& transform) {
    if (!(0.0f < speed_)) return;
    float amount = (speed_ / driveParameters_.maxSpeed) * turn_ * driveParameters_.pitchRate;
    amount = Clamp(amount, -driveParameters_.maxPitch, driveParameters_.maxPitch);
    const cS3DVector3 axis = Normalize(simulatedOrientation_);
    const float half = amount * 0.5f;
    const float s = std::sin(half), c = std::cos(half);
    SetQuaternionRotation(transform, axis.x * s, axis.y * s, axis.z * s, c);
}

// @ 0x1004fd56c. Applies the pitch, then the roll about (dir.x, 0, -dir.z), then writes the basis
// (right, up, forward) and adds the planing lift to the translation.
void cSC4Watercraft::Movement_Rotation(cS3DTransform& transform) {
    if (speed_ == 0.0f && !std::isnan(speed_)) return;
    Movement_Pitch(transform);
    cS3DVector3 up{transform.rotation[0][1] * transform.scale, transform.rotation[1][1] * transform.scale,
                   transform.rotation[2][1] * transform.scale};
    up = Normalize(up);
    const float rollAmount = speed_ * driveParameters_.rollRate;
    const float clampedRoll = Clamp(rollAmount, -driveParameters_.maxRoll, driveParameters_.maxRoll);
    const float half = -0.5f * clampedRoll;
    const float s = std::sin(half), c = std::cos(half);
    cS3DVector3 axis{simulatedOrientation_.x, 0.0f, -simulatedOrientation_.z};
    axis = Normalize(axis);
    cS3DTransform roll;
    SetQuaternionRotation(roll, axis.x * s, axis.y * s, axis.z * s, c);
    cS3DVector3 forward = Normalize(TransformVector(roll, simulatedOrientation_));
    const float lift = speed_ != 0.0f || std::isnan(speed_) ? driveParameters_.maxRoll * 0.5f : 0.0f;
    SetForwardUpRotation(transform, forward, up, false, cS3DVector3{});
    transform.translation[1] += lift;
    transform.flags = static_cast<std::uint8_t>(transform.flags | nS3D::kTransformTranslation |
                                                nS3D::kTransformRotation);
    ++transform.changeCounter;
}

// @ 0x1004f89f0. PARTIAL: the straight path is exact; the turning path calls Movement_PositionTurning (partial) and
// the final heading composition from the extra transform is approximated.
void cSC4Watercraft::Movement_PositionNew(const float& milliseconds) {
    cS3DTransform transform;
    cS3DVector3 position = simulatedPosition_;
    GetTransform(transform);
    turn_ = Clamp(turn_, driveParameters_.minTurn, driveParameters_.maxTurn);
    speed_ = Clamp(speed_, driveParameters_.minSpeed, driveParameters_.maxSpeed);
    previousOrientation_ = simulatedOrientation_;   // the original copies the pre-move heading first
    if (speed_ == 0.0f || turn_ == 0.0f) {
        if (speed_ != 0.0f || std::isnan(speed_)) {
            Movement_PositionStraight(milliseconds, transform, position);
        } else {
            FindState(kStatePlayerDrive);   // the original looks the state up and ignores the answer
        }
    } else {
        cS3DTransform extra;
        Movement_PositionTurning(milliseconds, transform, position, extra);
        simulatedOrientation_ = Normalize(TransformVector(extra, simulatedOrientation_));
    }
    previousPosition_ = simulatedPosition_;
    simulatedPosition_ = position;
    if (speed_ != 0.0f || turn_ != 0.0f || std::isnan(turn_)) Movement_Rotation(transform);
    SetTransform(transform);
    position_ = simulatedPosition_;
    orientation_ = simulatedOrientation_;
    FinishMove(true, true);
    UpdateEffects();
    UpdateLighting();
}

// @ 0x1004fbf50: put the boat back on its previous position and rebuild the transform.
void cSC4Watercraft::Crash_RegressToPreviousLocation() {
    simulatedPosition_ = previousPosition_;
    cS3DTransform transform;
    GetTransform(transform);
    transform.translation[0] = simulatedPosition_.x;
    transform.translation[1] = simulatedPosition_.y;
    transform.translation[2] = simulatedPosition_.z;
    transform.flags = static_cast<std::uint8_t>(transform.flags | nS3D::kTransformTranslation);
    ++transform.changeCounter;
    SetTransform(transform);
    position_ = simulatedPosition_;
    FinishMove(true, true);
    UpdateEffects();
    UpdateLighting();
}

// @ 0x1004fc3e2: true when the boat's heading is within 2 degrees of `direction` (both normalized in place in the
// original; the port normalizes copies).
bool cSC4Watercraft::Helper_OrientationCloseEnough(cS3DVector3 direction) {
    direction = Normalize(direction);
    simulatedOrientation_ = Normalize(simulatedOrientation_);
    float dot = simulatedOrientation_.x * direction.x + simulatedOrientation_.y * direction.y +
                simulatedOrientation_.z * direction.z;
    dot = Clamp(dot, -1.0f, 1.0f);
    return std::acos(dot) * kRadiansToDegrees < 2.0f;
}

// ---- AI location picking ---------------------------------------------------------------------------------------------
// @ 0x1004fca96: turn the heading into one of the eight directions (0 = north-west is the unused one). The original
// computes the two acos angles from +x and -x; reproduced exactly.
int cSC4Watercraft::Assist_PickInitialDir() {
    const cS3DVector3 direction = Normalize(simulatedOrientation_);
    const float anglePlusX = std::acos(Clamp(direction.x, -1.0f, 1.0f)) * kRadiansToDegrees;
    const float angleMinusX = std::acos(Clamp(-direction.x, -1.0f, 1.0f)) * kRadiansToDegrees;
    if (direction.x > 0.0f) {
        int result = 3;
        if (direction.z != 0.0f || std::isnan(direction.z)) {
            if (direction.z <= 0.0f) {
                if (anglePlusX >= 22.5f) result = (anglePlusX < 67.5f) + 1;
            } else if (anglePlusX >= 22.5f) {
                result = 5 - (anglePlusX < 67.5f);
            }
        }
        return result;
    }
    if (direction.x < 0.0f) {
        int result = 7;
        if (direction.z != 0.0f || std::isnan(direction.z)) {
            if (direction.z <= 0.0f) {
                if (angleMinusX >= 22.5f) result = angleMinusX >= 67.5f ? 1 : 0;
            } else if (angleMinusX >= 22.5f) {
                result = (angleMinusX < 67.5f) + 5;
            }
        }
        return result;
    }
    return (direction.z > 0.0f ? 4 : 0) + 1;
}

// @ 0x1004fcc3c: rotate `direction` by the offset of `option` and wrap modulo 8. Option >= 8 leaves the output
// pointer's value untouched (the original overwrites it with its own current value first).
void cSC4Watercraft::Assist_PickNewDir(int option, int direction, int* out) {
    int result = direction;
    switch (option) {
        case 0: break;
        case 1: result = direction + 1; break;
        case 2: result = direction - 1; break;
        case 3: result = direction + 2; break;
        case 4: result = direction - 2; break;
        case 5: result = direction + 3; break;
        case 6: result = direction - 3; break;
        case 7: result = direction + 4; break;
        default: result = *out; break;
    }
    *out = result % 8;
}

// @ 0x1004fccb4: move the cell one step in `direction` (the eight compass directions of Assist_DirLookup).
void cSC4Watercraft::Assist_DirIntoCell(int direction, int& cellX, int& cellZ) {
    int deltaZ = -1;
    switch (direction) {
        case 0: --cellX; break;
        case 1: break;
        case 2: ++cellX; break;
        case 3: ++cellX; deltaZ = 0; break;
        case 4: ++cellX; deltaZ = 1; break;
        case 5: deltaZ = 1; break;
        case 6: --cellX; deltaZ = 1; break;
        case 7: --cellX; deltaZ = 0; break;
        default: return;
    }
    cellZ += deltaZ;
}

// @ 0x1004fdcc8: the direction from lookahead[index] to lookahead[index+1]. Quirk: the last two points are refused.
int cSC4Watercraft::Assist_DirLookup(int index) const {
    if (index < 0 || lookahead_.size() < 2 ||
        static_cast<std::size_t>(index) >= lookahead_.size() - 2) {
        return 0;
    }
    const WatercraftTravelPoint& from = lookahead_[static_cast<std::size_t>(index)];
    const WatercraftTravelPoint& to = lookahead_[static_cast<std::size_t>(index) + 1];
    const std::uint32_t dx = static_cast<std::uint32_t>(to.x - from.x);
    const std::uint32_t dz = static_cast<std::uint32_t>(to.z - from.z);
    if (dx == 0 && dz == 0xffffffffu) return 1;
    if (dx == 1 && dz == 0xffffffffu) return 2;
    if (dx == 1 && dz == 0) return 3;
    if (dx == 1 && dz == 1) return 4;
    if (dx == 0 && dz == 1) return 5;
    if (dx == 0xffffffffu && dz == 1) return 6;
    if (dx == 0xffffffffu && dz == 0) return 7;
    return 0;
}

// @ 0x1004fbefe: 3 for the large boat, 2 for the medium one, 1 otherwise.
int cSC4Watercraft::BoatSize() {
    cISC4Occupant* occupant = AsOccupant();
    if (occupant->IsOccupantGroup(kWatercraftGroupLargeBoat)) return 3;
    return (occupant->IsOccupantGroup(kWatercraftGroupMediumBoat) ? 1 : 0) + 1;
}

// ---- acceleration curve ----------------------------------------------------------------------------------------------
// @ 0x1004fbbfa. The original caps are -0.455 / 7.0263715 for the negative/positive half and uses tan (drive mode 2)
// or atan for the inverse mapping. The decompiler dropped the argument of the atan branch; the port uses the same
// (value * cap / range) argument as the tan branch, which is the only shape consistent with the inverse function.
float cSC4Watercraft::AccelerationFunction(const float& value) {
    constexpr float xMinRangeCap = -0.455f;
    constexpr float yMaxRangeCap = 7.0263715f;
    const float yMinRangeCap = std::atan(xMinRangeCap * kHalfPi);
    float speed = Clamp(value, driveParameters_.speedMin, driveParameters_.speedMax);
    const float cap = 0.0f < speed ? 0.91f : xMinRangeCap;
    const float range = 0.0f < speed ? driveParameters_.speedMax : driveParameters_.speedMin;
    const float ratio = (speed * cap) / range * kHalfPi;
    const float curve = driveMode_ == 2 ? std::tan(ratio) : std::atan(ratio);
    const float scale = 0.0f < curve ? driveParameters_.maxSpeed : driveParameters_.minSpeed;
    const float reference = 0.0f < curve ? yMaxRangeCap : yMinRangeCap;
    return Clamp((curve * scale) / reference, driveParameters_.minSpeed, driveParameters_.maxSpeed);
}

// @ 0x1004fbd80.
float cSC4Watercraft::AccelerationFunctionInverse(const float& value) {
    constexpr float xMinRangeCap = -0.455f;
    constexpr float yMaxRangeCap = 7.0263715f;
    const float yMinRangeCap = std::tan(xMinRangeCap * kHalfPi);
    float speed = Clamp(value, driveParameters_.minSpeed, driveParameters_.maxSpeed);
    const float reference = 0.0f < speed ? yMaxRangeCap : yMinRangeCap;
    const float range = 0.0f < speed ? driveParameters_.maxSpeed : driveParameters_.minSpeed;
    float angle = std::atan((reference * speed) / range) * 0.63661975f;   // 2/pi
    const float cap = 0.0f < angle ? 0.91f : xMinRangeCap;
    const float result = (angle * range) / cap;
    return Clamp(result, driveParameters_.speedMin, driveParameters_.speedMax);
}

// ---- collision -------------------------------------------------------------------------------------------------------
// @ 0x1004f8d06.
bool cSC4Watercraft::Engine_DetectCollision() {
    if (OutOfBounds()) {
        PushState(kWatercraftStateRemoveNow, nullptr, -1.0f, false, 0, 0, nullptr);
        return true;
    }
    const bool playerDrive = GetBehavior() == kStatePlayerDrive;
    {   // the original's AITestEnabled debug byte is off, so the manager query always runs
        cSC4Watercraft* other = watercraftHost_ ? watercraftHost_->CollisionDetection(1.0f, this, nullptr) : nullptr;
        if (other) {
            const float ratio = std::fabs(speed_ - other->speed_) / driveParameters_.maxSpeed;
            const float threshold = playerDrive ? driveParameters_.playerBoatCollisionThreshold
                                                : driveParameters_.boatCollisionThreshold;
            if (threshold <= ratio && ratio != threshold) {
                if (playerDrive && watercraftHost_) watercraftHost_->PostAudioEvent(7);
                if (playerDrive || other->GetBehavior() == kStatePlayerDrive) {
                    const bool selfPlayer = GetBehavior() == kStatePlayerDrive;
                    const int selfSize = BoatSize();
                    const int otherSize = other->BoatSize();
                    if (selfPlayer) {
                        if (selfSize <= otherSize) {
                            crashMarked_ = true;
                            SendCallbackMessage(0xb, 0);
                            PushState(kWatercraftStateSink, nullptr, driveParameters_.crashStateTimer, true, 0, 0,
                                      nullptr);
                        }
                        if (BoatSize() < other->BoatSize()) return true;
                    } else {
                        if (selfSize <= otherSize) {
                            SendCallbackMessage(0xb, 0);
                            PushState(kWatercraftStateSink, nullptr, driveParameters_.crashStateTimer, true, 0, 0,
                                      nullptr);
                        }
                        if (BoatSize() < other->BoatSize()) return true;
                        other->crashMarked_ = true;
                    }
                    other->SendCallbackMessage(0xb, 0);
                    other->PushState(kWatercraftStateSink, nullptr, other->driveParameters_.crashStateTimer, true, 0,
                                     0, nullptr);
                } else {
                    if (BoatSize() <= other->BoatSize()) Crash_SetStateInternal(kChangeRunAground, false);
                    if (other->BoatSize() <= BoatSize()) other->Crash_SetStateInternal(kChangeRunAground, false);
                }
                return true;
            }
            Crash_RegressToPreviousLocation();
            const float savedSpeed = speed_;
            if (other->Engine_MovementPossible()) other->speed_ = savedSpeed;
            speed_ = driveParameters_.collisionSpeedScale * speed_;   // quirk: the boat keeps 75% of its speed
        }
    }
    if (Crash_InanimateCollisionDetection()) {
        const float threshold = playerDrive ? driveParameters_.playerStaticCollisionThreshold
                                            : driveParameters_.staticCollisionThreshold;
        const float ratio = std::fabs(speed_) / driveParameters_.maxSpeed;
        if (threshold <= ratio && ratio != threshold) {
            if (playerDrive && watercraftHost_) watercraftHost_->PostAudioEvent(7);
            Crash_DetermineCase();
            return true;
        }
        Crash_RegressToPreviousLocation();
    }
    return false;
}

// @ 0x1004fc036. Tests the four bounding-box corners against the lot manager (a static object under the boat) and
// against the surface water; returns true (and sets the crash pitch flags) when the bow or stern is over land.
bool cSC4Watercraft::Crash_InanimateCollisionDetection() {
    SC4CellRect rect;
    GetBoundingCityCells(rect);
    if (!watercraftHost_) return false;
    const int cornersX[4] = {rect.x0, rect.x0, rect.x1, rect.x1};
    const int cornersZ[4] = {rect.z0, rect.z1, rect.z0, rect.z1};
    int index = -1;
    if (!watercraftHost_->LotAtCell(cornersX[0], cornersZ[0])) {
        for (int i = 1; i < 4; ++i) {
            if (watercraftHost_->LotAtCell(cornersX[i], cornersZ[i])) {
                index = i;
                break;
            }
        }
    } else {
        index = 0;
    }
    if (index == -1 && !dockingActive_) {
        if (watercraftHost_->IsWaterCell(cornersX[0], cornersZ[0])) {
            for (int i = 1; i < 4; ++i) {
                if (!watercraftHost_->IsWaterCell(cornersX[i], cornersZ[i])) {
                    index = i;
                    break;
                }
            }
        } else {
            index = 0;
        }
    }
    bool first = false;
    bool second = false;
    switch (index) {
        case 0: first = true; second = true; break;
        case 1: first = false; second = true; break;
        case 2: first = true; second = false; break;
        case 3: first = false; second = false; break;
        case -1: return false;
        default: return false;
    }
    Crash_PitchDetermination(first, second);
    return true;
}

// @ 0x1004fc1ec: choose sink (2) or run aground (3) from the terrain slope relative to the heading.
void cSC4Watercraft::Crash_DetermineCase() {
    SC4CellRect rect;
    GetBoundingCityCells(rect);
    bool anyLand = false;
    if (watercraftHost_) {
        anyLand = !watercraftHost_->IsWaterCell(rect.x0, rect.z0);
        if (!watercraftHost_->IsWaterCell(rect.x0, rect.z1)) anyLand = true;
        if (!watercraftHost_->IsWaterCell(rect.x1, rect.z0)) anyLand = true;
        if (!watercraftHost_->IsWaterCell(rect.x1, rect.z1)) anyLand = true;
    }
    std::uint32_t change = kChangeSink;
    if (anyLand && watercraftHost_) {
        cS3DVector3 normal{0.0f, 1.0f, 0.0f};
        watercraftHost_->TerrainNormal(simulatedPosition_.x, simulatedPosition_.z, normal);
        normal = Normalize(normal);
        const cS3DVector3 orientation = Normalize(simulatedOrientation_);
        const float dot = Clamp(-(normal.x * orientation.x + normal.y * orientation.y + normal.z * orientation.z),
                                -1.0f, 1.0f);
        if (driveParameters_.crashAngleDegrees <= std::acos(dot)) change = kChangeRunAground;
    }
    Crash_SetStateInternal(static_cast<eW_ChangeState>(change), false);
}

// @ 0x1004fdd82. The original's compass-direction switch that decides the sink pitch/roll signs; ported literally.
void cSC4Watercraft::Crash_PitchDetermination(bool first, bool second) {
    const float x = simulatedOrientation_.x;
    const float z = simulatedOrientation_.z;
    std::uint32_t direction = 0;
    if (std::fabs(x) <= std::fabs(z)) {
        if (0.0f < x) {
            direction = static_cast<std::uint32_t>(5 - (z < 0.0f ? 1 : 0));
        } else {
            direction = static_cast<std::uint32_t>((z < 0.0f) ^ 7);
        }
    } else if (x <= 0.0f) {
        direction = static_cast<std::uint32_t>(3 - (z < 0.0f ? 1 : 0));
    } else {
        direction = 0.0f <= z ? 1u : 0u;
    }
    switch (direction & 7) {
        default:
            first = !first;
            break;
        case 2:
        case 3:
            second = !second;
            break;
        case 4:
        case 6:
            crashRollSign_ = first ? 0 : 1;
            second = !second;
            crashPitchSign_ = second ? 1 : 0;
            return;
        case 5:
        case 7:
            crashRollSign_ = first ? 1 : 0;
            crashPitchSign_ = second ? 1 : 0;
            return;
    }
    crashRollSign_ = second ? 1 : 0;
    crashPitchSign_ = first ? 1 : 0;
}

// ---- AI boat motion --------------------------------------------------------------------------------------------------
// @ 0x1004fc4f0. Walk the lookahead: every cell is one step in a compass direction; a candidate is accepted when the
// water test passes and the manager reports no collision; a failed pass trims the tail of the lookahead and retries
// from the new last point. The route reader of the neighbour boat feeds the points when a neighbour route exists.
bool cSC4Watercraft::Engine_AIPickLocation() {
    if (!watercraftHost_) return false;
    int direction = -1;
    int attempt = 0;
    int cellX = 0, cellZ = 0;
    while (true) {
        if (lookahead_.empty()) {
            direction = Assist_PickInitialDir();
            watercraftHost_->WorldToCell(simulatedPosition_.x, simulatedPosition_.z, cellX, cellZ);
        } else {
            const WatercraftTravelPoint& last = lookahead_.back();
            cellX = last.x;
            cellZ = last.z;
            direction = last.direction;
        }
        int sizeX = 0, sizeZ = 0;
        watercraftHost_->CityCellCount(sizeX, sizeZ);
        if (cellX < 0 || sizeX <= cellX || cellZ < 0 || sizeZ <= cellZ) break;
        const int remaining = 10 - static_cast<int>(lookahead_.size());
        if (remaining < 1) break;
        bool first = true;
        bool stop = false;
        bool producedAny = false;
        for (int i = 0; i < remaining; ++i) {
            if (neighbourRouteSet_) {
                std::int16_t px = 0, pz = 0;
                if (!neighbourRouteReader_ || !neighbourRouteReader_->GetNext(px, pz)) {
                    PushState(kWatercraftStateRemoveFade, nullptr, -1.0f, false, 0, 0, nullptr);
                    stop = true;
                    break;
                }
                lookahead_.push_back(WatercraftTravelPoint{px, pz, 0, -1});
                producedAny = true;
                continue;
            }
            if (!currentStation_.HasRoute()) {
                const int boatSize = BoatSize();
                bool found = false;
                for (int option = 0; option < 8; ++option) {
                    int candidate = direction;
                    Assist_PickNewDir(option, direction, &candidate);
                    if (direction == (candidate + 4) % 8) continue;
                    if (!lookahead_.empty() &&
                        (lookahead_.back().flags & (1u << (static_cast<std::uint32_t>(candidate) & 0x1f))) != 0) {
                        continue;
                    }
                    int nextX = cellX, nextZ = cellZ;
                    Assist_DirIntoCell(candidate, nextX, nextZ);
                    float out[3] = {0.0f, 0.0f, 0.0f};
                    if (!watercraftHost_->IsWaterAtCell(nextX, nextZ, boatSize)) continue;
                    if (watercraftHost_->CollisionDetection(static_cast<float>(boatSize), this, out)) continue;
                    lookahead_.push_back(WatercraftTravelPoint{nextX, nextZ, 0, candidate});
                    producedAny = true;
                    cellX = nextX;
                    cellZ = nextZ;
                    direction = candidate;
                    found = true;
                    break;
                }
                if (!found) {
                    if (first) {
                        first = false;
                        if (lookahead_.size() >= 6) {
                            lookahead_.resize(lookahead_.size() - 5);
                        } else if (lookahead_.size() >= 4) {
                            lookahead_.resize(lookahead_.size() - 3);
                        } else {
                            lookahead_.clear();
                        }
                        if (!lookahead_.empty()) {
                            WatercraftTravelPoint& last = lookahead_.back();
                            last.flags |= 1u << (static_cast<std::uint32_t>(last.direction) & 0x1f);
                            cellX = last.x;
                            cellZ = last.z;
                            direction = last.direction;
                        }
                    }
                    stop = true;
                    break;
                }
            } else {
                if ((currentStation_.flags & 1) != 0) {   // +0x3d8: the route priming byte
                    // already primed
                } else {
                    currentStation_.flags |= 1;
                    for (std::int32_t skip = 0; skip < currentStation_.routeIndex; ++skip) {
                        std::int16_t px = 0, pz = 0;
                        if (currentRoute_) currentRoute_->GetNext(px, pz);
                    }
                }
                std::int16_t px = 0, pz = 0;
                if (!currentRoute_ || !currentRoute_->GetNext(px, pz)) {
                    stop = !lookahead_.empty();
                    break;
                }
                lookahead_.push_back(WatercraftTravelPoint{px, pz, 0, -1});
                producedAny = true;
                ++currentStation_.routeIndex;
            }
        }
        if (stop) break;
        if (!producedAny) break;
        if (13 < attempt) break;
        ++attempt;
    }
    return !lookahead_.empty();
}

// @ 0x1004f7ea0. PARTIAL: the point-advance/erase logic and the speed/turn integration are recovered, but the
// original's two-midpoint steering (the SSE vector pairs at 0x1004f7ea0..0x1004f89ee) is approximated by steering
// at the second lookahead midpoint; the travel-point flags/invalidations are kept.
void cSC4Watercraft::Engine_AIBoatMotion(float milliseconds) {
    if (!watercraftHost_) return;
    if (lookahead_.size() < 10) {
        const bool picked = Engine_AIPickLocation();
        speed_ = 0.0f;
        if (!picked) return;
    }
    if (lookahead_.empty()) return;
    if (lookahead_.size() < 2) return;
    auto midpoint = [this](std::size_t index, float& x, float& z) {
        float x0 = 0.0f, z0 = 0.0f, x1 = 0.0f, z1 = 0.0f;
        watercraftHost_->CellToWorld(lookahead_[index].x, lookahead_[index].z, x0, z0);
        watercraftHost_->CellToWorld(lookahead_[index + 1].x, lookahead_[index + 1].z, x1, z1);
        x = (x0 + x1) * 0.5f;
        z = (z0 + z1) * 0.5f;
    };
    float firstX = 0.0f, firstZ = 0.0f;
    midpoint(0, firstX, firstZ);
    cS3DVector3 toFirst{firstX - simulatedPosition_.x, 0.0f, firstZ - simulatedPosition_.z};
    const float firstDistance = Length(toFirst);
    if (firstDistance < 0.1f) {
        lookahead_.clear();
        PushState(kWatercraftStateRemoveNow, nullptr, -1.0f, false, 0, 0, nullptr);
        return;
    }
    if (lookahead_.size() < 3) return;
    float secondX = 0.0f, secondZ = 0.0f;
    midpoint(1, secondX, secondZ);
    cS3DVector3 toSecond{secondX - simulatedPosition_.x, 0.0f, secondZ - simulatedPosition_.z};
    const float secondDistance = Length(toSecond);
    if (secondDistance < 0.1f) {
        lookahead_.clear();
        PushState(kWatercraftStateRemoveNow, nullptr, -1.0f, false, 0, 0, nullptr);
        return;
    }
    cS3DVector3 steerDirection = Normalize(toSecond);
    if (firstDistance < 7.0f && Helper_OrientationCloseEnough(toFirst)) {
        turningFlag_ = false;
        lookahead_.erase(lookahead_.begin());
        UpdateEffects();
        UpdateLighting();
        return;
    }
    simulatedOrientation_ = Normalize(simulatedOrientation_);
    if (!turningFlag_) {
        const float angle = std::acos(Clamp(simulatedOrientation_.x * steerDirection.x +
                                                simulatedOrientation_.y * steerDirection.y +
                                                simulatedOrientation_.z * steerDirection.z,
                                            -1.0f, 1.0f)) * kRadiansToDegrees;
        const float maxAngle = milliseconds * 0.1f;
        const float applied = angle <= maxAngle ? angle : maxAngle;
        const float crossY = simulatedOrientation_.z * steerDirection.x - simulatedOrientation_.x * steerDirection.z;
        const float sign = crossY < 0.0f ? -1.0f : 1.0f;
        const float rotation = sign * applied * 0.017453292f;
        const float half = rotation * 0.5f;
        cS3DTransform rot;
        SetQuaternionRotation(rot, 0.0f, std::sin(half), 0.0f, std::cos(half));
        simulatedOrientation_ = Normalize(TransformVector(rot, simulatedOrientation_));
        turn_ = (angle * driveParameters_.maxTurn) / maxAngle;
        if (maxAngle == 0.0f) turn_ = 0.0f;
    } else {
        const float angle = std::acos(Clamp(simulatedOrientation_.x * steerDirection.x +
                                                simulatedOrientation_.y * steerDirection.y +
                                                simulatedOrientation_.z * steerDirection.z,
                                            -1.0f, 1.0f)) * kRadiansToDegrees;
        if (7.0f <= firstDistance) {
            const float step = milliseconds * 0.0025f;
            const float travel = firstDistance <= step ? firstDistance : step;
            simulatedPosition_ = simulatedPosition_ + Normalize(toFirst) * travel;
        } else if (!Helper_OrientationCloseEnough(toFirst)) {
            turningFlag_ = true;
        }
        turn_ = 0.0f;
        (void)angle;
    }
    speed_ = Clamp(speed_, driveParameters_.minSpeed, driveParameters_.maxSpeed);
    turn_ = Clamp(turn_, driveParameters_.minTurn, driveParameters_.maxTurn);
    (void)secondDistance;
}

// ---- crash sequences -------------------------------------------------------------------------------------------------
// @ 0x1004f9f4a. PARTIAL: the progress curve, the pitch/roll choice and the MoveTo/FinishMove tail are recovered;
// the two quaternion compositions that tilt the model into the terrain use the simplified Y/X rotation below because
// the decompiler's basis construction (0x1004fa1xx..0x1004fa4e3) is not unambiguously recoverable.
bool cSC4Watercraft::Crash_SequenceRunAground(float milliseconds) {
    if (driveParameters_.crashStateTimer <= crashTimer_) return false;
    if (driveParameters_.crashStateTimer * 0.5f <= crashTimer_ || !crashRollEnabled_) {
        // fall through to the progress integration
    } else {
        cS3DVector3 normal{0.0f, 1.0f, 0.0f};
        if (watercraftHost_) watercraftHost_->TerrainNormal(simulatedPosition_.x, simulatedPosition_.z, normal);
        normal = Normalize(normal);
        const cS3DVector3 orientation = Normalize(simulatedOrientation_);
        const float dot = Clamp(-(normal.x * orientation.x + normal.y * orientation.y + normal.z * orientation.z),
                                -1.0f, 1.0f);
        if (std::acos(dot) < driveParameters_.crashAngleDegrees) {
            crashRollEnabled_ = false;
        } else {
            speed_ = driveParameters_.agroundSpeedScale * speed_;
            if (watercraftHost_) {
                const float step = milliseconds * speed_ * driveParameters_.movementScale;
                const float newX = simulatedPosition_.x + simulatedOrientation_.x * step;
                const float newZ = simulatedPosition_.z + simulatedOrientation_.z * step;
                int cellX = 0, cellZ = 0;
                watercraftHost_->WorldToCell(newX, newZ, cellX, cellZ);
                const bool inCity = watercraftHost_->PointInCity(newX, newZ);
                const float cellHeight = watercraftHost_->TerrainHeightAtCell(cellX, cellZ);
                if (!inCity ||
                    !(std::fabs(cellHeight - simulatedPosition_.y) < driveParameters_.groundHeightTolerance)) {
                    crashRollEnabled_ = false;
                }
            }
        }
    }
    if (crashTimer_ < driveParameters_.crashStateTimer) {
        const float half = driveParameters_.crashStateTimer * 0.5f;
        const float rate = crashTimer_ < half && crashRollEnabled_
                               ? driveParameters_.agroundProgressRate
                               : driveParameters_.agroundProgressRate * 2.5f;
        const float target = (crashTimer_ / driveParameters_.crashStateTimer) * rate;
        const float delta = target - crashProgress_;
        crashProgress_ += delta;
        if (crashProgress_ < driveParameters_.agroundProgressRate * 2.5f) {
            cS3DTransform transform;
            GetTransform(transform);
            const float pitch = delta * -0.5f;
            const float halfPitch = pitch * 0.5f;
            cS3DTransform tilt;
            SetQuaternionRotation(tilt, std::sin(halfPitch), 0.0f, 0.0f, std::cos(halfPitch));
            cS3DVector3 forward = Normalize(TransformVector(tilt, simulatedOrientation_));
            cS3DVector3 up = Normalize(TransformVector(tilt, cS3DVector3{0.0f, 1.0f, 0.0f}));
            SetForwardUpRotation(transform, forward, up, true, simulatedPosition_);
            SetTransform(transform);
            simulatedOrientation_ = forward;
            position_ = simulatedPosition_;
            FinishMove(true, true);
            UpdateEffects();
            UpdateLighting();
            return true;
        }
    }
    return false;
}

// @ 0x1004fab22. PARTIAL: the progress curve (0x538 cap), the roll/pitch signs and the water-level lowering are
// recovered; the two quaternion basis compositions are simplified the same way as Crash_SequenceRunAground.
bool cSC4Watercraft::Crash_SequenceSink() {
    cS3DTransform transform;
    GetTransform(transform);
    const float maxProgress = driveParameters_.sinkMaxProgress;
    const float target = (crashTimer_ / driveParameters_.crashStateTimer) * maxProgress;
    const float delta = target - crashProgress_;
    crashProgress_ += delta;
    if (maxProgress <= crashProgress_) return false;
    cS3DVector3 up = Normalize(cS3DVector3{transform.rotation[0][1], transform.rotation[1][1], transform.rotation[2][1]});
    const float sign = crashRollSign_ ? 0.5f : -0.5f;
    const float roll = sign * crashProgress_;
    const float halfRoll = roll * 0.5f;
    cS3DTransform rollTransform;
    SetQuaternionRotation(rollTransform, 0.0f, 0.0f, std::sin(halfRoll), std::cos(halfRoll));
    cS3DVector3 forward = Normalize(TransformVector(rollTransform, simulatedOrientation_));
    const float pitchSign = crashPitchSign_ ? 0.5f : -0.5f;
    const float pitch = pitchSign * crashProgress_;
    const float halfPitch = pitch * 0.5f;
    cS3DTransform pitchTransform;
    SetQuaternionRotation(pitchTransform, std::sin(halfPitch), 0.0f, 0.0f, std::cos(halfPitch));
    forward = Normalize(TransformVector(pitchTransform, forward));
    up = Normalize(TransformVector(pitchTransform, up));
    SetForwardUpRotation(transform, forward, up, true, simulatedPosition_);
    if (maxProgress * 0.6f < crashProgress_ && watercraftHost_) {
        const float level = watercraftHost_->WaterLevel(simulatedPosition_.x, simulatedPosition_.z);
        const float terrain = watercraftHost_->TerrainHeight(simulatedPosition_.x, simulatedPosition_.z);
        const float drop = (crashProgress_ - maxProgress * 0.6f) / (maxProgress * 0.6f);
        simulatedPosition_.y = terrain - drop * std::fabs(level - terrain) * 0.5f;
        transform.translation[1] = simulatedPosition_.y;
    }
    SetTransform(transform);
    simulatedOrientation_ = forward;
    orientation_ = simulatedOrientation_;
    FinishMove(true, true);
    UpdateEffects();
    UpdateLighting();
    return true;
}

// ---- docking / following / undocking ---------------------------------------------------------------------------------
// @ 0x1004f67da. PARTIAL: the docking approach (the original's pair of station records, the acos alignment and the
// dt*speed scaling in 0x1004f67da..0x1004f6d98) is approximated: within dockDistance the boat steers at the station
// and takes a speed-scaled step; beyond it the AI motion runs. OnLand still forces the remove state.
bool cSC4Watercraft::DoDockingTick(float milliseconds) {
    FerryStationInformation other = Ferry_OtherStationInformation();
    const cS3DVector3 delta = other.position - simulatedPosition_;
    if (OnLand()) PushState(kWatercraftStateRemoveNow, nullptr, -1.0f, false, 0, 0, nullptr);
    simulatedOrientation_ = Normalize2D(simulatedOrientation_);
    if (Length(delta) <= driveParameters_.dockDistance) {
        const float step = milliseconds * driveParameters_.dockSpeedScale;
        const float turnStep = milliseconds * driveParameters_.dockTurnScale;
        cS3DVector3 direction = Normalize2D(delta);
        if (Length2D(direction) > 0.0f) {
            const float angle = std::acos(Clamp(simulatedOrientation_.x * direction.x +
                                                    simulatedOrientation_.z * direction.z, -1.0f, 1.0f)) *
                                kRadiansToDegrees;
            const float applied = angle <= turnStep ? angle : turnStep;
            const float crossY = simulatedOrientation_.z * direction.x - simulatedOrientation_.x * direction.z;
            const float rotation = (crossY < 0.0f ? -1.0f : 1.0f) * applied * 0.017453292f;
            const float half = rotation * 0.5f;
            cS3DTransform rot;
            SetQuaternionRotation(rot, 0.0f, std::sin(half), 0.0f, std::cos(half));
            simulatedOrientation_ = Normalize(TransformVector(rot, simulatedOrientation_));
            const float travel = Length2D(delta) <= step ? Length2D(delta) : step;
            simulatedPosition_ = simulatedPosition_ + direction * travel;
            if (watercraftHost_) {
                simulatedPosition_.y = watercraftHost_->TerrainHeight(simulatedPosition_.x, simulatedPosition_.z);
            }
        }
        previousPosition_ = simulatedPosition_;
        previousOrientation_ = simulatedOrientation_;
        position_ = simulatedPosition_;
        orientation_ = simulatedOrientation_;
        cS3DTransform transform;
        GetTransform(transform);
        transform.translation[0] = simulatedPosition_.x;
        transform.translation[1] = simulatedPosition_.y;
        transform.translation[2] = simulatedPosition_.z;
        cS3DTransform basis;
        SetForwardUpRotation(basis, simulatedOrientation_, cS3DVector3{0.0f, 1.0f, 0.0f}, false, cS3DVector3{});
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) transform.rotation[row][column] = basis.rotation[row][column];
        }
        transform.flags = static_cast<std::uint8_t>(transform.flags | nS3D::kTransformTranslation |
                                                    nS3D::kTransformRotation);
        ++transform.changeCounter;
        cSC4Automaton::MoveTo(&simulatedPosition_.x, &simulatedOrientation_.x);
        UpdateEffects();
        UpdateLighting();
        return true;
    }
    const float step = milliseconds <= driveParameters_.maxStep ? milliseconds : driveParameters_.maxStep;
    Engine_AIBoatMotion(step);
    Movement_PositionNew(step);
    Engine_DetectCollision();
    return true;
}

// @ 0x1004f6d9c. PARTIAL: follows the tow target's tail (leader position minus leader heading times its radius) with
// the same steering/step shape as the docking approach; the original's second bounding-sphere target is folded in.
bool cSC4Watercraft::DoFollowingTick(float milliseconds) {
    if (!tower_) return false;
    if (watercraftHost_) simulatedPosition_.y = watercraftHost_->TerrainHeight(simulatedPosition_.x, simulatedPosition_.z);
    const cSC4Watercraft* leader = tower_.get();
    const float leaderRadius = leader->GetRadius();
    const cS3DVector3 target = leader->simulatedPosition_ - leader->simulatedOrientation_ * leaderRadius;
    const cS3DVector3 delta = target - simulatedPosition_;
    const float distance = Length(delta);
    if (distance < 1.0f) return false;
    const cS3DVector3 direction = Normalize(delta);
    const float angle = std::acos(Clamp(simulatedOrientation_.x * direction.x + simulatedOrientation_.y * direction.y +
                                            simulatedOrientation_.z * direction.z,
                                        -1.0f, 1.0f)) *
                        kRadiansToDegrees;
    const float maxStep = milliseconds * driveParameters_.dockSpeedScale;
    if (angle < 1.0f && distance < 1.0f) return false;
    const float travel = maxStep <= (distance - (GetRadius() + 10.0f)) ? maxStep : distance - (GetRadius() + 10.0f);
    if (travel <= 0.0f) return false;
    const float applied = angle <= maxStep ? angle : maxStep;
    const float crossY = simulatedOrientation_.z * direction.x - simulatedOrientation_.x * direction.z;
    const float rotation = (crossY < 0.0f ? -1.0f : 1.0f) * applied * 0.017453292f;
    const float half = rotation * 0.5f;
    cS3DTransform rot;
    SetQuaternionRotation(rot, 0.0f, std::sin(half), 0.0f, std::cos(half));
    simulatedOrientation_ = Normalize(TransformVector(rot, simulatedOrientation_));
    simulatedPosition_ = simulatedPosition_ + direction * travel;
    if (watercraftHost_) simulatedPosition_.y = watercraftHost_->TerrainHeight(simulatedPosition_.x, simulatedPosition_.z);
    previousPosition_ = simulatedPosition_;
    previousOrientation_ = simulatedOrientation_;
    position_ = simulatedPosition_;
    orientation_ = simulatedOrientation_;
    cSC4Automaton::MoveTo(&simulatedPosition_.x, &simulatedOrientation_.x);
    UpdateEffects();
    UpdateLighting();
    return true;
}

// @ 0x1004f72aa: back away from the neighbour boat until the boat is in open water past twice its radius.
bool cSC4Watercraft::DoUndockingTick(float milliseconds) {
    if (!Engine_MovementPossible()) return false;
    if (!watercraftHost_) return false;
    cSC4Lot* lot = nullptr;
    if (neighbour_) {
        float neighbourPosition[3] = {0.0f, 0.0f, 0.0f};
        neighbour_->GetPosition(neighbourPosition);
        lot = watercraftHost_->LotAtPosition(neighbourPosition);
    }
    if (!watercraftHost_->IsWaterAtPosition(simulatedPosition_.x, simulatedPosition_.z, 2, lot)) return false;
    cS3DVector3 target = simulatedPosition_;
    if (neighbour_) {
        float neighbourPosition[3] = {0.0f, 0.0f, 0.0f};
        neighbour_->GetPosition(neighbourPosition);
        target = cS3DVector3{neighbourPosition[0], neighbourPosition[1], neighbourPosition[2]};
    }
    if (OnLand()) PushState(kWatercraftStateRemoveNow, nullptr, -1.0f, false, 0, 0, nullptr);
    const cS3DVector3 delta = simulatedPosition_ - target;
    const float distance = Length2D(delta);
    if (!(distance < GetRadius() * 2.0f)) return false;
    const cS3DVector3 direction = Normalize2D(delta);
    const float step = milliseconds * driveParameters_.dockTurnScale;
    const float travel = distance <= step ? distance : step;
    simulatedPosition_ = simulatedPosition_ + direction * travel;
    if (watercraftHost_) simulatedPosition_.y = watercraftHost_->TerrainHeight(simulatedPosition_.x, simulatedPosition_.z);
    previousPosition_ = simulatedPosition_;
    previousOrientation_ = simulatedOrientation_;
    position_ = simulatedPosition_;
    orientation_ = simulatedOrientation_;
    cSC4Automaton::MoveTo(&simulatedPosition_.x, &simulatedOrientation_.x);
    return true;
}

// ---- player drive ----------------------------------------------------------------------------------------------------
// @ 0x1004fb362. PARTIAL: the steering-mode selection, the speed ramp through AccelerationFunction, the waterski /
// wake effect hooks, the tow-search handshake and the heading/steering output are recovered; the original's analogue
// trigger integration (state +0xb0/+0xb4) and its effect-frame tables are only partly modelled.
bool cSC4Watercraft::TranslatePlayerDriveCommands(tAutomataStatePlayerDrive* state, float milliseconds) {
    if (!state) return false;
    IAutomatonGroupTemplate* group = GetAutomataGroup();
    if (!group) return false;
    bool hasProperty = false;
    for (IAutomatonGroupTemplate* node = group; node; node = node->Parent()) {
        if (dynamic_cast<IWatercraftGroupTemplate*>(node)) {
            hasProperty = true;
            break;
        }
    }
    if (!hasProperty) return false;
    float throttle = 0.0f;
    if (driveParameters_.rightTriggerThreshold <= state->history[0]) {
        driveMode_ = 2;
        throttle = state->history[0];
    } else if (driveParameters_.leftTriggerThreshold <= state->history[1]) {
        driveMode_ = 1;
        throttle = -state->history[1];
    } else {
        driveMode_ = 0;
        throttle = 0.0f;
    }
    state->value38 = throttle;
    if (throttle != 0.0f || std::isnan(throttle)) {
        const float ramp = driveParameters_.reverseAcceleration * driveParameters_.timeScale * milliseconds;
        const float reverseRamp = driveParameters_.reverseRate * driveParameters_.timeScale * milliseconds;
        const float target = AccelerationFunction(throttle);
        if (speed_ != 0.0f || std::isnan(speed_)) {
            const float targetSign = target < 0.0f ? -1.0f : 1.0f;
            const float speedSign = speed_ < 0.0f ? -1.0f : 1.0f;
            if (targetSign != speedSign || std::isnan(targetSign) || std::isnan(speedSign)) {
                if (speed_ <= 0.0f) {
                    speed_ = std::fabs(speed_) + std::fabs(target) < reverseRamp ? speed_ + reverseRamp : target;
                    state->history[0] = driveParameters_.rightTriggerThreshold;
                } else {
                    speed_ = std::fabs(target - speed_) < reverseRamp ? target : speed_ - reverseRamp;
                    state->history[1] = driveParameters_.leftTriggerThreshold;
                }
            } else {
                speed_ = std::fabs(target - speed_) < ramp ? target : speedSign * ramp + speed_;
            }
        } else {
            speed_ = target;
        }
    }
    if (0.0f <= state->effectTimerC4) {
        UpdateEffects();
        if (state->wakeStarted22b) {
            if (animations_[5]) animations_[5]->Play(false);
            state->effectDelay20c = 0;
            state->wakeStarted22b = 0;
            ++state->animStage1a4;
        }
    }
    if (watercraftHost_ && animations_[5]) {
        if (state->hint117) {
            if (!animations_[5]->IsPlaying()) {
                if (0.0f < speed_) {
                    cS3DTransform transform;
                    GetTransform(transform);
                    animations_[5]->SetTransform(transform);
                    animations_[5]->Play(false);
                }
            } else {
                animations_[5]->SetPaused(true);
            }
        }
        if (speed_ <= 0.0f) {
            if (animations_[5]->IsPlaying()) animations_[5]->SetPaused(true);
        } else if (animations_[5]->IsPlaying()) {
            cS3DTransform transform;
            GetTransform(transform);
            animations_[5]->SetTransform(transform);
        }
    }
    // Throttle/steer integration: the original ramps state +0x6c towards the trigger value (PARTIAL above).
    state->steering6c = Clamp(state->steering6c, driveParameters_.minTurn, driveParameters_.maxTurn);
    turn_ = state->steering6c;
    speed_ = Clamp(speed_, driveParameters_.minSpeed, driveParameters_.maxSpeed);
    state->steerVisual264 = state->steering6c * -0.125f;
    state->wakeAngle268 = 0.0f;
    state->wakeTurn26c = kTwoPi;
    const cS3DVector3 orientation = Normalize(simulatedOrientation_);
    state->heading270 = -orientation.z < 0.0f ? kTwoPi - std::acos(Clamp(-orientation.y, -1.0f, 1.0f)) : 0.0f;
    SpeedTween().SetDesired(std::fabs(speed_), 0.0f);
    if (watercraftHost_ && state->hint119 && speed_ == 0.0f && !towTarget_) {
        towSearchActive_ = true;
        if (animations_[3] && !animations_[3]->IsPlaying()) {
            cS3DTransform transform;
            GetTransform(transform);
            animations_[3]->SetTransform(transform);
            animations_[3]->Play(false);
        }
    }
    return cSC4Automaton::TranslatePlayerDriveCommands(state, milliseconds);
}

// ---- debug text ------------------------------------------------------------------------------------------------------
// @ 0x1004fcd1e. The original returns its static cRZString; the port returns its characters (base signature).
const char* cSC4Watercraft::GetDebugExtraText() const {
    static cRZString result;
    std::int32_t docks = 0, queues = 0;
    if (watercraftHost_) watercraftHost_->FleetCounts(docks, queues);
    int cellX = 0, cellZ = 0;
    if (watercraftHost_) watercraftHost_->WorldToCell(simulatedPosition_.x, simulatedPosition_.z, cellX, cellZ);
    result.Sprintf(
        "Tow: %d, (FT,MT): (%d,%d), (Q,D): (%d,%d), Turn: %3.2f, Speed: %3.2f, Cell(%3d,%3d), "
        "Pos(%3.2f,%3.2f,%3.2f), Ornt<%3.2f,%3.2f,%3.2f>",
        towTarget_ ? 1 : 0, static_cast<int>(docks), static_cast<int>(queues), queueingFlag_ ? 1 : 0,
        dockingFlag_ ? 1 : 0, static_cast<double>(turn_), static_cast<double>(speed_), cellX, cellZ,
        static_cast<double>(simulatedPosition_.x), static_cast<double>(simulatedPosition_.y),
        static_cast<double>(simulatedPosition_.z), static_cast<double>(simulatedOrientation_.x),
        static_cast<double>(simulatedOrientation_.y), static_cast<double>(simulatedOrientation_.z));
    return result.ToChar();
}

}  // namespace sc4
