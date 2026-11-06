// cSC4Vehicle — see Vehicle.h for the class, the layout, the vtable slots and the quirks. Every function here is a hand
// port of the exported decompile; where the decompiler dropped SIMD lanes, arguments or the float/int rounding idiom the
// disassembly was used and the comment names the address (scripts/dec_addrs.py, /disassemble_function).
#include "sc4/Vehicle.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "rz/cRZRandom.h"
#include "s3d/S3DMath.h"
#include "sc4/GeometryUtilities.h"
#include "sc4/PathInfo.h"
#include "sc4/SimStreamOps.h"

namespace sc4 {

namespace {

constexpr float kUnsetX = -10000.0f;   // the first float of cSC4Automaton::kUninit3DVector (0x100b22550)
constexpr std::uint32_t kUnsetYBits = 0x01ffffffu;
constexpr std::uint32_t kUnsetZBits = 0x4f5f4090u;

float BitsToFloat(std::uint32_t bits) {
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
float Length3(const cS3DVector3& v) { return std::sqrt(v.z * v.z + v.y * v.y + v.x * v.x); }
float Distance3(const cS3DVector3& a, const cS3DVector3& b) { return Length3(Sub(a, b)); }
float Dot3(const cS3DVector3& a, const cS3DVector3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// The original's "is this vector still the uninitialised sentinel" test (kUninit3DVector, three hand-picked words).
bool IsUnsetVector(const cS3DVector3& v) {
    return std::fabs(v.x - kUnsetX) < 0.0001f && std::fabs(v.y - BitsToFloat(kUnsetYBits)) < 0.0001f &&
           std::fabs(v.z - BitsToFloat(kUnsetZBits)) < 0.0001f;
}

// world coordinate -> cell: the original's float->int idiom with the -0.5 bias and the 12582912 rounding constant is
// floor() for every finite input (the disassembly at 0x1000dfa15 and elsewhere).
int WorldToCell(float value) { return static_cast<int>(std::floor(value * kVehicleCellSize)); }

// The eased value of a tween: the desired value when the duration has not been set, otherwise the clamped elapsed ratio
// between current and desired (0x1000de24c and the four copies of the idiom in the drive code).
float EasedTween(const AutomatonTween& tween) {
    if (tween.duration <= 0.0f) return tween.desired;
    const float ratio = tween.elapsed / tween.duration;
    float clamped = 1.0f;
    if (ratio <= 1.0f) clamped = ratio;
    clamped = ratio < 0.0f ? 0.0f : clamped;
    return clamped * (tween.desired - tween.current) + tween.current;
}

cS3DVector3 SafeNormalize(const cS3DVector3& v, const cS3DVector3& fallback) {
    const float length = Length3(v);
    if (length <= 0.0f) return fallback;
    return Scale(v, 1.0f / length);
}

}  // namespace

// ---- host -------------------------------------------------------------------------------------------------------------
namespace {
IVehicleHost* gVehicleHost = nullptr;
}

IVehicleHost* VehicleHost() { return gVehicleHost; }
void SetVehicleHost(IVehicleHost* host) { gVehicleHost = host; }

const VehicleTuning& cSC4Vehicle::Tuning() const {
    static const VehicleTuning kDefaults{};
    if (vehicleHost_) {
        if (const VehicleTuning* tuning = vehicleHost_->Tuning()) return *tuning;
    }
    return kDefaults;
}

// ---- construction / destruction ----------------------------------------------------------------------------------------
// 0x1000dacf2: the base constructor, then the vehicle fields; the occupant type id becomes 0xc7cc75f1 (+0x50 of the
// object = the occupant sub-object's +0x48 type), the two path vectors and the four ghost slots start empty, the path
// history deque is constructed, and the default speed is a random value in the manager's range * 1/3600. The speed tween
// is then told to reach the maximum speed with a zero duration.
cSC4Vehicle::cSC4Vehicle(ITrafficAutomatonHost* host, IAutomatonHost* automatonHost, IVehicleHost* vehicleHost,
                         const cGZPersistResourceKey& modelKey)
    : cSC4TrafficAutomaton(host, automatonHost, modelKey), vehicleHost_(vehicleHost ? vehicleHost : VehicleHost()) {
    type_ = kVehicleOccupantType;
    headlightTimer_ = 0.0f;
    pathRemaining_ = 0.0f;
    pathPos_ = MakeVector(kUnsetX, BitsToFloat(kUnsetYBits), BitsToFloat(kUnsetZBits));
    prevOrient_ = pathPos_;
    const VehicleTuning& tuning = Tuning();
    float speed = 0.0f;
    if (IAutomatonHost* ih = this->Host()) {
        const std::int32_t rolled = ih->RandomInt(tuning.defaultSpeedMin, tuning.defaultSpeedMax);
        speed = static_cast<float>(rolled) * kAutomatonSpeedScale;
    }
    SetDefaultSpeed(speed);
    speedTween_.SetDesired(GetMaxSpeed(), 0.0f);
}

// 0x100510920: the path history deque, the two headlight models and the base. The virtual destructor generated at
// 0x1000e2558 frees the object (operator delete is the class allocator's).
cSC4Vehicle::~cSC4Vehicle() {
    pathHistory_.clear();
    for (IAutomatonModel*& model : headlights_) {
        if (model) {
            IAutomatonModel* old = model;
            model = nullptr;
            old->Release();
        }
    }
    // The original keeps these three in the player-drive state record; the port keeps them on the automaton, so the
    // record's destructor work happens here (one reference each).
    if (driveEffect_) {
        IVehicleEffect* old = driveEffect_;
        driveEffect_ = nullptr;
        old->Release();
    }
    if (driveOccupant_) {
        ITrafficAutomatonOccupant* old = driveOccupant_;
        driveOccupant_ = nullptr;
        old->Release();
    }
    if (accident_) {
        cIGZUnknown* old = accident_;
        accident_ = nullptr;
        old->Release();
    }
}

// ---- QueryInterface / GetGZCLSID ---------------------------------------------------------------------------------------
// 0x1000dd314: 0x896e714a and 0x8b99ee7c answer this object; everything else goes to the base (which answers 1 /
// 0xa9b40f05 -> this, 0x26bb767e -> +8, 0xe4fda3d4 -> +0x10, 0x452294aa -> +0x68). Every answer add-refs.
bool cSC4Vehicle::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    if (iid == kVehicleIID) {
        *out = this;
        AddRef();
        return true;
    }
    *out = nullptr;
    if (iid == kGZIID_cISC4TrafficAutomaton) {
        *out = this;
        AddRef();
        return true;
    }
    return cSC4Automaton::QueryInterface(iid, out);
}

// ---- Init / Shutdown ----------------------------------------------------------------------------------------------------
// 0x1000daeee: when not initialized, flag the automaton as a collision candidate with the manager default, roll the path
// offset, run the base Init, and turn the headlights on when the manager default or the scene lighting asks for it. The
// base result is answered (the headlights are decided even when the base failed).
bool cSC4Vehicle::Init() {
    if (IsInitialized()) return true;
    SetAutomataFlag(kAutomataFlagCollideCandidate, vehicleHost_ ? vehicleHost_->DefaultCollide() : false);
    RecalculatePathOffset();
    const bool result = cSC4TrafficAutomaton::Init();
    const bool lights = (vehicleHost_ && vehicleHost_->DefaultHeadlights()) || (vehicleHost_ && vehicleHost_->SceneLightsOn());
    if (lights) ShowHeadlights(true, false);
    return result;
}

// 0x1000daf80: the base Shutdown, release both headlight models, release every path-history node and clear it, then drop
// the motion-blur models.
bool cSC4Vehicle::Shutdown() {
    const bool result = cSC4TrafficAutomaton::Shutdown();
    for (IAutomatonModel*& model : headlights_) {
        if (model) {
            IAutomatonModel* old = model;
            model = nullptr;
            old->Release();
        }
    }
    pathHistory_.clear();
    ClearMotionBlur();
    return result;
}

// 0x1000db0ba: remove each ghost model from the renderer, release it and clear the slot. The original asks the model for
// its renderer node (slot 0x98 of the model) before RemoveModelFromRenderer; the port's seam takes the model.
void cSC4Vehicle::ClearMotionBlur() {
    for (IAutomatonModel*& model : motionBlur_) {
        if (model) {
            if (IAutomatonHost* h = Host()) h->RemoveModelFromRenderer(model);
            IAutomatonModel* old = model;
            model = nullptr;
            old->Release();
        }
    }
}

// ---- headlights / motion blur -------------------------------------------------------------------------------------------
// 0x1000dd3dc: only the first headlight is tested for "already in the wanted state". `instant` starts a random fade timer
// (0.2..15 s, negative when hiding) and returns without loading anything; otherwise the two models are loaded through
// LoadAuxiliaryModel(1, ...) or removed and released.
void cSC4Vehicle::ShowHeadlights(bool on, bool instant) {
    if ((headlights_[0] != nullptr) == on) return;
    if (instant) {
        const double rolled = Host() ? Host()->RandomDouble(0.2000000029802322, 15.0) : 0.0;
        headlightTimer_ = static_cast<float>(rolled);
        if (!on) headlightTimer_ = -static_cast<float>(rolled);
        return;
    }
    if (on) {
        for (IAutomatonModel*& model : headlights_) {
            if (model) {
                IAutomatonModel* old = model;
                model = nullptr;
                old->Release();
            }
            LoadAuxiliaryModel(kVehicleHeadlightSpecifier, &model);
        }
    } else {
        for (IAutomatonModel*& model : headlights_) {
            if (model) RemoveModelInstance(model);
            if (model) {
                IAutomatonModel* old = model;
                model = nullptr;
                old->Release();
            }
        }
    }
}

// 0x1000dd55c: flip the 0x80000 "motion blur on" flag; turning it off drops the ghosts. Always true.
void cSC4Vehicle::ShowMotionBlur(bool on) {
    if (static_cast<bool>(CheckAutomataFlag(kAutomataFlagMotionBlur)) != on) {
        SetAutomataFlag(kAutomataFlagMotionBlur, on);
        if (!on) ClearMotionBlur();
    }
}

// 0x1000dd5aa: the four ghost copies of the primary model. The current slot is created from the primary model key when
// empty (added to the renderer), otherwise it is faded in; the transform is the vehicle orientation. The opacity of the
// slot and its three predecessors is then set to the eased opacity minus 1/3 per step (clamped at 0), and the cursor
// advances. The float argument is unused in the original.
void cSC4Vehicle::UpdateMotionBlur(float milliseconds) {
    (void)milliseconds;
    IAutomatonModel* model = motionBlur_[motionBlurCursor_];
    bool added = false;
    if (!model) {
        if (IAutomatonHost* h = Host()) model = h->CreateModel(primaryKey_);
        if (model) {
            motionBlur_[motionBlurCursor_] = model;
            added = true;
        }
        if (!model) goto opacity;
    }
    {
        cS3DTransform transform;
        nS3D::Matrix3 rotation;
        nS3D::AlignZToDirWithYUp(orientation_, rotation);
        std::memcpy(transform.rotation, rotation.m, sizeof(rotation.m));
        transform.translation[0] = position_.x;
        transform.translation[1] = position_.y;
        transform.translation[2] = position_.z;
        transform.flags = static_cast<std::uint8_t>(transform.flags | nS3D::kTransformRotation | nS3D::kTransformTranslation);
        ++transform.changeCounter;
        model->SetTransform(transform);
        if (IAutomatonHost* h = Host()) {
            if (added) {
                h->AddModelToRenderer(model);
            } else {
                h->FadeModelIn(model);
            }
        }
    }
opacity:
    float level = EasedTween(opacityTween_);
    {
        float clamped = 1.0f;
        if (level <= 1.0f) clamped = level;
        level = clamped;
    }
    int remaining = kVehicleMotionBlurCount;
    do {
        IAutomatonModel* ghost = motionBlur_[motionBlurCursor_];
        if (ghost) ghost->SetOpacity(level, true);
        level -= 0.33333334f;
        motionBlurCursor_ = motionBlurCursor_ != 0 ? static_cast<std::uint8_t>(motionBlurCursor_ - 1) : 3;
        if (level <= 0.0f) level = 0.0f;
        --remaining;
    } while (remaining != 0);
    motionBlurCursor_ = static_cast<std::uint8_t>((motionBlurCursor_ + 1) & 3);
}

// ---- per-frame tick -----------------------------------------------------------------------------------------------------
// 0x1000db152: the motion blur ghosts are updated before the base tick (only while the 0x80000 flag is set); afterwards
// the headlight timer is counted down by the frame time and the lights are toggled when it crosses zero; the ghost model
// update runs last. The base result is answered.
bool cSC4Vehicle::DoAnimationTick(std::uint32_t unknown, float milliseconds) {
    if (CheckAutomataFlag(kAutomataFlagMotionBlur)) UpdateMotionBlur(milliseconds);
    const bool result = cSC4TrafficAutomaton::DoAnimationTick(unknown, milliseconds);
    if (std::fabs(headlightTimer_) >= 0.0001f) {
        if (headlightTimer_ >= 0.0f) {
            headlightTimer_ -= milliseconds / 1000.0f;
            if (!(headlightTimer_ > 0.0f)) {
                headlightTimer_ = 0.0f;
                ShowHeadlights(true, false);
            }
        } else {
            headlightTimer_ += milliseconds / 1000.0f;
            if (!(headlightTimer_ < 0.0f)) {
                headlightTimer_ = 0.0f;
                ShowHeadlights(false, false);
            }
        }
    }
    UpdateGhostModel();
    return result;
}

// 0x1000db260: when the base state machine does not handle the current state, the top of the state stack drains the two
// vehicle-specific states: 0x15 pull over, 0xe plummet.
bool cSC4Vehicle::DoStateTick(float milliseconds) {
    const bool result = cSC4TrafficAutomaton::DoStateTick(milliseconds);
    if (!result && !StateStack().empty()) {
        tAutomataState* state = StateStack().back();
        if (state) {
            if (state->stateType == kStatePullOver) return DoPullOverTick(milliseconds);
            if (state->stateType == kStatePlummet) return DoPlummetTick(milliseconds);
        }
    }
    return result;
}

// ---- the path step ------------------------------------------------------------------------------------------------------
// 0x1000db2c8: the vehicle's drive step. A special-route automaton whose route is used up and that is stopped reports
// the route destination. Otherwise: speed * modifier * frame time, collision update, safe distance (a leader within 16
// makes it adopt half the leader's speed), the step clamped by the safe distance. A non-positive step sets the stopped
// flag and removes the automaton when it is reversing by more than its radius (unless on a special route). A positive
// step interpolates along the path from the vehicle's own path position; the first frame copies the position into it.
// When the path ends the reached-destination flag and callback fire, otherwise the segment direction (averaged with the
// previous one) offsets the point by half the radius and the vehicle moves there. The actual speed is the distance
// travelled over the frame time.
bool cSC4Vehicle::DoPathTick(float milliseconds) {
    const cS3DVector3 startPosition = position_;
    float speed = GetCurrentSpeed();
    const float modifier = GetSpeedModifier();
    if (!GetSpecialRouteFlag() || Route().size() != 0 || !CheckAutomataFlag(kTrafficFlagStopped)) {
        float distance = speed * modifier * milliseconds;
        UpdateCollision();
        const float safe = GetSafeDistance();
        if (safe < 16.0f && leader_) {
            const float leaderSpeed = leader_->GetCurrentSpeed();
            SetSpeed(leaderSpeed, 0.5f);
        }
        const float step = safe <= distance ? safe : distance;
        bool result = true;
        if (step <= 0.0f) {
            distance = step;
            SetAutomataFlag(kTrafficFlagStopped, true);
            const float radius = GetRadius();
            if (distance < -radius && !CheckAutomataFlag(kAutomataFlagSpecialRoute)) {
                Remove(false);
                result = false;
            }
        } else {
            if (IsUnsetVector(pathPos_)) pathPos_ = position_;
            cS3DVector3 out = MakeVector(0.0f, 0.0f, 0.0f);
            if (!InterpolateAlongPath(step, pathPos_, out, reinterpret_cast<std::int32_t*>(&pathIndex_), path_,
                                      &pathRemaining_)) {
                SetAutomataFlag(kAutomataFlagReachedDestination, true);
                SendCallbackMessage(kAutomataEventDestinationReached, 0);
            } else {
                cS3DVector3 direction = MakeVector(0.0f, 0.0f, 0.0f);
                if (path_ && pathIndex_ + 1 < path_->size()) {
                    direction = Sub((*path_)[pathIndex_ + 1], (*path_)[pathIndex_]);
                }
                if (Length3(direction) <= kVehiclePathEpsilon) {
                    direction = IsUnsetVector(prevOrient_) ? MakeVector(0.0f, 0.0f, 1.0f) : prevOrient_;
                } else {
                    direction = Scale(direction, 1.0f / Length3(direction));
                    if (!IsUnsetVector(prevOrient_)) {
                        direction = MakeVector((direction.x + direction.x + prevOrient_.x) * 0.33333334f,
                                               (direction.y + direction.y + prevOrient_.y) * 0.33333334f,
                                               (direction.z + direction.z + prevOrient_.z) * 0.33333334f);
                    }
                }
                const float radius = GetRadius();
                const cS3DVector3 next = MakeVector(out.x - direction.x * radius * 0.5f,
                                                    out.y - direction.y * radius * 0.5f,
                                                    out.z - direction.z * radius * 0.5f);
                MoveTo(&next.x, &direction.x);
                prevOrient_ = direction;
                pathPos_ = out;
                SetAutomataFlag(kTrafficFlagStopped, false);
            }
            result = true;
        }
        SetActualSpeed(Distance3(position_, startPosition) / milliseconds);
        return result;
    }
    HandleReachedRouteDestination();
    return true;
}

// 0x1000dceb6: "<base> model:0x%16x 0x%16x 0x%16x" with the first model resource key. The original keeps a process-wide
// static cRZString; the port formats into a static buffer.
const char* cSC4Vehicle::GetDebugExtraText() const {
    static char buffer[512];
    std::uint32_t words[3] = {0, 0, 0};
    if (!modelKeys_.empty()) {
        const cGZPersistResourceKey& key = modelKeys_.front();
        const std::uint32_t* raw = reinterpret_cast<const std::uint32_t*>(&key);
        words[0] = raw[0];
        words[1] = raw[1];
        words[2] = raw[2];
    }
    const char* base = cSC4TrafficAutomaton::GetDebugExtraText();
    std::snprintf(buffer, sizeof(buffer), "%s model:0x%16x 0x%16x 0x%16x", base ? base : "",
                  static_cast<unsigned>(words[0]), static_cast<unsigned>(words[1]),
                  static_cast<unsigned>(words[2]));
    return buffer;
}

// ---- persistence --------------------------------------------------------------------------------------------------------
// 0x1000dd022: version 2, the base record, the path history size, then key + the occupant's two cell words per node, a
// zero terminator, the path position, the "uninitialised" vector the original writes from the kUninit3DVector global and
// the remaining distance.
bool cSC4Vehicle::Write(cIGZOStream& stream) const {
    stream.SetUint16(2);
    if (!cSC4TrafficAutomaton::Write(stream)) return false;
    stream.SetUint32(static_cast<std::uint32_t>(pathHistory_.size()));
    for (const tLookaheadNode& node : pathHistory_) {
        stream.SetUint32(node.key);
        std::uint32_t cellX = 0;
        std::uint32_t cellZ = 0;
        if (node.occupant) node.occupant->GetOccupiedCell(cellX, cellZ);
        stream.SetSint32(static_cast<std::int32_t>(cellX));
        stream.SetSint32(static_cast<std::int32_t>(cellZ));
    }
    stream.SetUint32(0);
    stream << pathPos_;
    stream << MakeVector(kUnsetX, BitsToFloat(kUnsetYBits), BitsToFloat(kUnsetZBits));
    stream.SetFloat32(pathRemaining_);
    return true;
}

// 0x1000dd1b6: version 2 only. The history entries are read into temporaries (the path history is not rebuilt - quirk),
// then the path position, a discarded vector and the remaining distance.
bool cSC4Vehicle::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 2) return false;
    if (!cSC4TrafficAutomaton::Read(stream)) return false;
    std::uint32_t count = 0;
    stream.GetUint32(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t key = 0;
        std::int32_t cellX = 0;
        std::int32_t cellZ = 0;
        stream.GetUint32(key);
        stream.GetSint32(cellX);
        stream.GetSint32(cellZ);
    }
    std::uint32_t terminator = 0;
    stream.GetUint32(terminator);
    stream >> pathPos_;
    cS3DVector3 discarded;
    stream >> discarded;
    stream.GetFloat32(pathRemaining_);
    return true;
}

// ---- accessors ----------------------------------------------------------------------------------------------------------
void cSC4Vehicle::SetPathPos(const cS3DVector3& position) { pathPos_ = position; }
void cSC4Vehicle::SetPrevOrient(const cS3DVector3& orientation) { prevOrient_ = orientation; }

void cSC4Vehicle::SetDriveOccupant(ITrafficAutomatonOccupant* occupant) {
    if (occupant) occupant->AddRef();
    if (driveOccupant_) driveOccupant_->Release();
    driveOccupant_ = occupant;
}

void cSC4Vehicle::SetDriveEffect(IVehicleEffect* effect) {
    if (effect) effect->AddRef();
    if (driveEffect_) driveEffect_->Release();
    driveEffect_ = effect;
}

void cSC4Vehicle::SetAccident(cIGZUnknown* accident) {
    if (accident) accident->AddRef();
    if (accident_) accident_->Release();
    accident_ = accident;
}

// 0x1000dd7a6: a random path offset in the manager's range.
void cSC4Vehicle::RecalculatePathOffset() {
    const VehicleTuning& tuning = Tuning();
    const double rolled = Host() ? Host()->RandomDouble(tuning.pathOffsetMin, tuning.pathOffsetMax) : 0.0;
    pathOffset_ = static_cast<float>(rolled);
}

// 0x1000dd7e6: the automata manager's collision query at the position.
cSC4Automaton* cSC4Vehicle::GetCollidingAutomaton(float distance) {
    return vehicleHost_ ? vehicleHost_->CollidingAutomaton(this, distance) : nullptr;
}

// 0x1000dd81e: a tail call into the automata manager (vslot 0x258).
void cSC4Vehicle::RecalculateLeaders() {
    if (vehicleHost_) vehicleHost_->ManagerRecalculateLeaders(this);
}

// 0x1000de4b8: the class overrides the traffic answer with the city's automata manager (city slot 0x158).
ITrafficAutomataManager* cSC4Vehicle::GetTrafficAutomataManager() const { return Manager(); }

// 0x1000e2400: leave town: set the "self controlling + 0x20000000 + 0x100000" flag word and roll a 0..49 cell lifespan.
// Always false.
void cSC4Vehicle::SkipTown() {
    SetAutomataFlag(0x201180a, true);
    const std::uint32_t rolled = Host() ? Host()->RandomBelow(0x32) : 0;
    SetCellLifespan(static_cast<std::uint8_t>(rolled));
}

// 0x1000e23ee: the bumped state ends immediately.
bool cSC4Vehicle::DoBumpedTick(float milliseconds) {
    (void)milliseconds;
    return false;
}

// 0x1000e23f6: the base collide tick.
bool cSC4Vehicle::DoCollideTick(float milliseconds) { return cSC4Automaton::DoCollideTick(milliseconds); }

// 0x1000e18a8: the vehicle adds nothing to the base command translation.
bool cSC4Vehicle::HandlePlayerDriveCommand(std::uint32_t command, bool on) {
    return cSC4Automaton::HandlePlayerDriveCommand(command, on);
}


// ---- path assignment ----------------------------------------------------------------------------------------------------
// 0x1000dc7a2: the base path bookkeeping plus the vehicle's own start point: the path position is the first path point
// (offset by OffsetPathPoint), the direction is the vector to the second point, and the vehicle is placed half a radius
// behind the start with that heading. Moving to a different network occupant toggles the tunnel audio / opacity pair (the
// start / stop mapping is the vehicle's own, the reverse of the base class'; the opacity arguments were dropped by the
// decompiler and are taken from the base class' pair). Finally the map's network lists and the leader are updated.
void cSC4Vehicle::SetPath(ITrafficAutomatonOccupant* occupant, std::uint32_t pathKey, const std::vector<cS3DVector3>* path,
                          bool keepDestination) {
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
        } else if (!points->empty()) {
            cS3DVector3 start = points->front();
            OffsetPathPoint(start);
            cS3DVector3 end = points->size() > 1 ? points->at(1) : points->front();
            OffsetPathPoint(end);
            pathPos_ = start;
            const cS3DVector3 direction = SafeNormalize(Sub(end, start), orientation_);
            const float radius = GetRadius();
            const cS3DVector3 next = MakeVector(start.x - direction.x * radius * 0.5f,
                                                start.y - direction.y * radius * 0.5f,
                                                start.z - direction.z * radius * 0.5f);
            MoveTo(&next.x, &direction.x);
        }
    }
    // The tunnel transition. Quirk: the vehicle starts event 0xe when the *old* occupant was a tunnel and stops it when
    // the new one is (the base class maps the same test the other way round).
    if (networkOccupant_ != old) {
        void* newTunnel = nullptr;
        void* oldTunnel = nullptr;
        if (networkOccupant_ && networkOccupant_->HasNetworkFlag(kVehicleNetworkFlagTunnelInterface))
            networkOccupant_->QueryInterface(0x8a4bd52b, &newTunnel);
        if (old && old->HasNetworkFlag(kVehicleNetworkFlagTunnelInterface)) old->QueryInterface(0x8a4bd52b, &oldTunnel);
        const bool startTunnel = oldTunnel != nullptr;
        const bool stopTunnel = !startTunnel && newTunnel != nullptr;
        if (startTunnel || stopTunnel) {
            SetOpacity(startTunnel ? 1.0f : 0.0f, startTunnel ? 0.1f : 0.8f);
            if (FindState(kTrafficStateDrive) && trafficHost_) {
                if (startTunnel) {
                    trafficHost_->StartAudioEvent(kVehicleAudioEventTunnel);
                } else {
                    trafficHost_->StopAudioEvent(kVehicleAudioEventTunnel);
                }
            }
        }
        if (newTunnel) static_cast<cIGZUnknown*>(newTunnel)->Release();
        if (oldTunnel) static_cast<cIGZUnknown*>(oldTunnel)->Release();
    }
    if (cSC4TrafficNetworkMap* map = trafficHost_ ? trafficHost_->TrafficNetworkMap() : nullptr) {
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

// 0x1000dc3d2: the base StartTrip, then (unless the ignore-paths flag is set) either a snap onto the current path when
// no remaining distance is stored, or a first interpolation by min(safe distance, remaining) along the path from its
// first point; the segment direction (averaged with the previous one) places the vehicle half a radius off the start.
bool cSC4Vehicle::StartTrip(ITrafficAutomatonOccupant* occupant, std::uint32_t key, bool param3) {
    const bool result = cSC4TrafficAutomaton::StartTrip(occupant, key, param3);
    if (!result || !path_ || CheckAutomataFlag(kAutomataFlagIgnorePaths)) return result;
    if (pathRemaining_ <= 0.0f) {
        if (pathInfo_) pathInfo_->SnapPointToPath(pathPos_, path_);
        return result;
    }
    const float safe = GetSafeDistance();
    float distance = safe <= pathRemaining_ ? safe : pathRemaining_;
    pathRemaining_ = distance;
    const cS3DVector3 start = path_->front();
    const bool ok = InterpolateAlongPath(distance, start, pathPos_, reinterpret_cast<std::int32_t*>(&pathIndex_), path_,
                                         &pathRemaining_);
    cS3DVector3 direction = MakeVector(0.0f, 0.0f, 0.0f);
    if (!ok) {
        if (path_->size() >= 2) {
            direction = Sub(path_->at(path_->size() - 1), path_->at(path_->size() - 2));
        }
    } else if (pathIndex_ + 1 < path_->size()) {
        direction = Sub(path_->at(pathIndex_ + 1), path_->at(pathIndex_));
    } else {
        direction = Sub(path_->back(), path_->front());
    }
    if (Length3(direction) <= kVehiclePathEpsilon) {
        direction = IsUnsetVector(prevOrient_) ? MakeVector(0.0f, 0.0f, 1.0f) : prevOrient_;
    } else {
        direction = Scale(direction, 1.0f / Length3(direction));
        if (!IsUnsetVector(prevOrient_)) {
            direction = MakeVector((direction.x + direction.x + prevOrient_.x) * 0.33333334f,
                                   (direction.y + direction.y + prevOrient_.y) * 0.33333334f,
                                   (direction.z + direction.z + prevOrient_.z) * 0.33333334f);
        }
    }
    const float radius = GetRadius();
    const cS3DVector3 next = MakeVector(pathPos_.x - direction.x * radius * 0.5f,
                                        pathPos_.y - direction.y * radius * 0.5f,
                                        pathPos_.z - direction.z * radius * 0.5f);
    MoveTo(&next.x, &direction.x);
    prevOrient_ = direction;
    SetAutomataFlag(kTrafficFlagStopped, false);
    return result;
}

// 0x1000dccae: the traffic safe distance, but measured from the vehicle's *path* position (+0x3b4) instead of the
// simulated one. The path term only counts with the 0x10 flag set; the leader term needs a leader and the collision
// flag. Both start at 16 (the smaller one wins), like the base class.
float cSC4Vehicle::GetSafeDistance() {
    float pathSafe = 16.0f;
    if (path_ && CheckAutomataFlag(0x10)   /* the traffic wait bit */) {
        const float distance = Distance3(path_->back(), pathPos_) - GetRadius();
        pathSafe = distance < 1.0f ? 0.0f : distance;
    }
    float leaderSafe = 16.0f;
    if (leader_ && GetDetectCollisions()) {
        cS3DVector3 collide{};
        leader_->GetCollidePosition(collide);
        leaderSafe = Distance3(collide, pathPos_) - GetRadius() - 3.0f;
    }
    return pathSafe < leaderSafe ? pathSafe : leaderSafe;
}

// 0x1000ddb08: when the removed occupant leaves the automaton hanging in the air (self controlling, the position is
// inside the city and more than 5 units above the terrain), push the plummet state with a -1 timer and stop. Otherwise
// the base handler removes the automaton.
void cSC4Vehicle::HandleNetworkOccupantRemoved() {
    if (!CheckAutomataFlag(kAutomataFlagMarkedForRemove) && CheckAutomataFlag(kAutomataFlagSelfControlling)) {
        if (vehicleHost_ && vehicleHost_->IsInsideCity(position_.x, position_.z)) {
            const float terrain = Host() ? Host()->TerrainHeight(position_.x, position_.z) : 0.0f;
            const float surface = vehicleHost_->SurfaceHeight(position_.x, position_.z);
            const float ground = surface <= terrain ? terrain : surface;
            if (std::fabs(position_.y - ground) > kVehicleHeightTolerance) {
                PushState(kStatePlummet, nullptr, -1.0f, false, 0, 0, nullptr);
                return;
            }
        }
    }
    cSC4TrafficAutomaton::HandleNetworkOccupantRemoved();
}

// 0x1000e1b8c: entering the player-drive state clears value40 and copies the player-drive parameter word +8 of the first
// automation group that has parameters into the state's group speed (1.0 when the group chain has none). The base enter
// handler runs afterwards (and calls the base's own enter handler a second time - traffic quirk).
bool cSC4Vehicle::HandleEnterState(tAutomataState* state) {
    if (state && state->stateType == kTrafficStateDrive) {
        auto* drive = static_cast<tAutomataStatePlayerDrive*>(state);
        drive->value40 = 0;
        const tAutomataPlayerDriveParams* params = nullptr;
        if (vehicleHost_ && vehicleHost_->FindPlayerDriveParams(this, &params)) {
            drive->groupSpeed = params ? params->F(0x8) : 1.0f;
        }
    }
    return cSC4TrafficAutomaton::HandleEnterState(state);
}

// 0x1000e229e: a collision turns the heading half-way towards a fresh random direction, marks the automaton removed and
// pushes the collide state with a 5 s timer. A null collision point, or already being in the collide state (0x14),
// answers true without touching anything. The result is the PushState answer.
bool cSC4Vehicle::HandleCollision(cSC4Automaton* other, const cS3DVector3& point) {
    (void)other;
    (void)point;
    if (vehicleHost_) vehicleHost_->PostCollisionMessage();
    if (GetBehavior() == kStateCollide) return true;
    cRZRandom random(0xffffffffu);
    const cS3DVector3 direction = nS3D::RandomDirection3(random);
    const cS3DVector3 mixed = Add(orientation_, Scale(direction, 0.5f));
    orientation_ = SafeNormalize(mixed, orientation_);
    SetAutomataFlag(kAutomataFlagRemoved, true);
    return PushState(kStateCollide, nullptr, kVehicleCollideTimer, true, 0, 0, nullptr);
}

// 0x1000e18b6: the vehicle's command translation. The forward / reverse command picks the parameter speed at +0x10 or
// +0x14 depending on the current (eased) speed sign and the two history flags; the acceleration accumulates in value38.
// The steering rate is a constant (+0x2c) on a path-less automaton or ramps between +0x24 and +0x28 with the actual /
// maximum speed ratio. The steering angle moves at that rate while history flag 3 (right) is set, decays towards 0 at the
// +0x30 rate otherwise; history flag 2 (left) adds the rate. The result is finally clamped to +-params+0x20 with paths
// ignored, +-pi/2 without, and the base command translation runs last (its answer is returned).
bool cSC4Vehicle::TranslatePlayerDriveCommands(tAutomataStatePlayerDrive* state, float milliseconds) {
    const tAutomataPlayerDriveParams* params = nullptr;
    if (!vehicleHost_ || !vehicleHost_->FindPlayerDriveParams(this, &params) || !params) return false;
    float speed = params->F(0x14);
    const float eased = EasedTween(speedTween_);
    if ((eased > 0.0f && state->history[0] >= 0.0f) || (eased < 0.0f && state->history[1] >= 0.0f)) speed = params->F(0x10);
    const float dt = milliseconds / 1000.0f;
    if (state->history[0] >= 0.0f) {
        state->value38 += speed * dt;
    } else if (state->history[1] >= 0.0f) {
        state->value38 -= speed * dt;
    } else {
        state->value38 = 0.0f;
    }
    float steerRate = params->F(0x2c);
    if (CheckAutomataFlag(kAutomataFlagIgnorePaths)) {
        const float ratio = params->F(0x8) != 0.0f ? GetActualSpeed() / params->F(0x8) : 0.0f;
        float clamped = 1.0f;
        if (ratio <= 1.0f) clamped = ratio;
        clamped = ratio < 0.0f ? 0.0f : clamped;
        steerRate = clamped * (params->F(0x28) - params->F(0x24)) + params->F(0x24);
    }
    if (state->history[3] >= 0.0f) {
        state->steering6c -= dt * steerRate;
    } else if (state->steering6c < 0.0f) {
        state->steering6c += params->F(0x30) * dt;
        if (state->steering6c > 0.0f) state->steering6c = 0.0f;
    }
    if (state->history[2] >= 0.0f) {
        state->steering6c += dt * steerRate;
    } else if (state->steering6c > 0.0f) {
        state->steering6c -= params->F(0x30) * dt;
        if (state->steering6c < 0.0f) state->steering6c = 0.0f;
    }
    float limitMax = 1.5707964f;
    float limitMin = -1.5707964f;
    if (CheckAutomataFlag(kAutomataFlagIgnorePaths)) {
        limitMax = params->F(0x20);
        limitMin = -limitMax;
    }
    if (state->steering6c > limitMax) state->steering6c = limitMax;
    if (state->steering6c < limitMin) state->steering6c = limitMin;
    return cSC4Automaton::TranslatePlayerDriveCommands(state, milliseconds);
}

// 0x1000de4d0: the player-drive speed integration. The acceleration in value38 is added to the current speed and the
// result clamped between the parameter minimum / maximum (scaled by +0x18 when the automaton is not on the network);
// reverse only engages from a standstill (a non-zero speed forces the minimum to 0, as does `a` being false). The
// +0x1c deceleration drags an idle two-flag reverse history back to 0. The lateral term (orientation.y) slows the
// vehicle while it banks or turns. An occupant-group member that is not steering in the direction of travel has its
// speed zeroed. The desired speed of the tween is set and the speed answered; a negative speed survives only off the
// network (onRoad254), positive speeds always do.
float cSC4Vehicle::PlayerDriveUpdateSpeed(tAutomataStatePlayerDrive* state, const tAutomataPlayerDriveParams* params,
                                          bool a, bool b, float milliseconds) {
    float speed = state->value3c;
    float maxSpeed = params->F(0x8);
    float minSpeed = 0.0f;
    if (b) {
        minSpeed = params->F(0xc);
    } else {
        maxSpeed = params->F(0x8) * params->F(0x18);
        minSpeed = params->F(0x18) * params->F(0xc);
    }
    if (speed >= 0.0f || state->onRoad254 == 0) {
        if (!a) minSpeed = 0.0f;
        minSpeed = speed != 0.0f ? 0.0f : minSpeed;
    }
    speed = speed + state->value38 * milliseconds;
    if (speed <= maxSpeed) maxSpeed = speed;
    float clamped = speed < minSpeed ? minSpeed : maxSpeed;
    if (state->history[0] == -1.0f && state->history[1] == -1.0f) {
        const float drag = (milliseconds / 1000.0f) * params->F(0x1c);
        if (clamped <= 0.0f) {
            clamped += drag;
            if (clamped > 0.0f) clamped = 0.0f;
        } else {
            clamped -= drag;
            if (clamped < 0.0f) clamped = 0.0f;
        }
    }
    state->value3c = clamped;
    const float lateral = orientation_.y;
    const float magnitude = std::fabs(lateral);
    float factor = clamped;
    if (magnitude > 0.2f) {
        const bool sameSign = (lateral < 0.0f) == (clamped < 0.0f);
        if (b) {
            if (sameSign) factor = clamped * ((4.0f - magnitude) * 0.25f);
        } else if (sameSign && magnitude <= 0.8f) {
            factor = clamped * ((2.0f - magnitude) * 0.5f);
        }
        clamped = lateral * magnitude * -0.0003f * milliseconds + factor;
    }
    const bool inGroup = vehicleHost_ &&
                        vehicleHost_->IsOccupantGroup(static_cast<cISC4Occupant*>(this), kVehicleGroupPlayerDrive);
    float signedSpeed = clamped;
    if (inGroup) {
        signedSpeed = 0.0f;
        const bool steeringWithTravel = (state->value3c <= 0.0f && state->value3c != 0.0f) == (clamped < 0.0f);
        if (steeringWithTravel) signedSpeed = clamped;
    }
    float result = clamped >= 0.0f ? clamped : (state->onRoad254 != 0 ? signedSpeed : 0.0f);
    speedTween_.SetDesired(result, 0.0f);
    return result;
}

// ---- pure geometry ------------------------------------------------------------------------------------------------------
// 0x1000db946: the two intersections of the segment a..b with the sphere (centre, radius). The larger root is written to
// `first` and the smaller to `second`; when only the smaller lies inside [0, 1] the same point is copied into `first`.
// Answers the number of points written (0, 1 or 2). A degenerate segment divides by zero exactly like the original.
int cSC4Vehicle::LineIntersectsSphere(const cS3DVector3& a, const cS3DVector3& b, const cS3DVector3& centre, float radius,
                                      cS3DVector3& first, cS3DVector3& second) const {
    const cS3DVector3 d = Sub(a, centre);
    const cS3DVector3 e = Sub(b, a);
    const float aa = Dot3(e, e);
    const float bb = 2.0f * Dot3(e, d);
    const float cc = Dot3(d, d) - radius * radius;
    const float discriminant = bb * bb - 4.0f * (aa * cc);
    if (!(discriminant >= 0.0f)) return 0;
    const float root = std::sqrt(discriminant);
    const float tFirst = (root - bb) / (aa + aa);
    const float tSecond = (-bb - root) / (aa + aa);
    int count = 0;
    if (tFirst >= 0.0f && tFirst <= 1.0f) {
        first = MakeVector(a.x + e.x * tFirst, e.y * tFirst + a.y, a.z + e.z * tFirst);
        count = 1;
    }
    if (tSecond >= 0.0f && tSecond <= 1.0f) {
        second = MakeVector(e.x * tSecond + a.x, e.y * tSecond + a.y, e.z * tSecond + a.z);
        if (count == 0) first = second;
        ++count;
    }
    return count;
}

// 0x1000dbad6: the intersection of the segment a..b with the sphere whose point faces the segment direction. With two
// intersections the second one wins when the vector centre->point is at least `minCosine` away from the segment
// direction; otherwise the first one is tested the same way. Degenerate (equal end points) answers false.
bool cSC4Vehicle::FindPathIntersection(const cS3DVector3& a, const cS3DVector3& b, const cS3DVector3& centre, float radius,
                                       cS3DVector3& out, float minCosine) const {
    if (std::fabs(a.x - b.x) < 0.0001f && std::fabs(a.y - b.y) < 0.0001f && std::fabs(a.z - b.z) < 0.0001f) return false;
    cS3DVector3 temp = MakeVector(0.0f, 0.0f, 0.0f);
    const int count = LineIntersectsSphere(a, b, centre, radius, out, temp);
    if (count == 0) return false;
    const cS3DVector3 direction = SafeNormalize(Sub(b, a), MakeVector(0.0f, 0.0f, 1.0f));
    const auto faces = [&](const cS3DVector3& point) {
        const cS3DVector3 delta = Sub(centre, point);
        const float length = Length3(delta);
        if (!(length > kVehiclePathEpsilon)) return false;
        return minCosine <= Dot3(Scale(delta, 1.0f / length), direction);
    };
    if (count == 2 && faces(temp)) {
        out = temp;
        return true;
    }
    return faces(out);
}

// 0x1000e1592: one edge of the lot rectangle against the sphere. The edge is lifted to the sphere centre's y. A second
// intersection closer than the current best replaces it; the first intersection is then tested the same way.
void cSC4Vehicle::Check2DIntersection(float x0, float z0, float x1, float z1, const cS3DVector3& centre, float radius,
                                      cS3DVector3& out, float& best) const {
    const cS3DVector3 a = MakeVector(x0, centre.y, z0);
    const cS3DVector3 b = MakeVector(x1, centre.y, z1);
    cS3DVector3 first = MakeVector(0.0f, 0.0f, 0.0f);
    cS3DVector3 second = MakeVector(0.0f, 0.0f, 0.0f);
    const int count = LineIntersectsSphere(a, b, centre, radius, first, second);
    if (count == 0) return;
    if (count == 2) {
        const float distance = Distance3(second, centre);
        if (distance < best) {
            best = distance;
            out = second;
        }
    }
    const float distance = Distance3(first, centre);
    if (distance < best) {
        best = distance;
        out = first;
    }
}

// 0x1000dbd64: walk backwards along the current path (and then along the path of each history tile) for the first segment
// that crosses the sphere of radius `distance` around `position`; answer that intersection. When nothing crosses, the
// answer is `position` pulled back by `distance` along the current segment direction (or the orientation) and false is
// returned.
bool cSC4Vehicle::FindBackPosition(const cS3DVector3& position, cS3DVector3& out, float distance,
                                   const std::deque<tLookaheadNode>& history) {
    const std::vector<cS3DVector3>* path = path_;
    std::vector<cS3DVector3> owned;
    std::size_t index = path && path->size() >= 2 ? path->size() - 1 : 0;
    std::deque<tLookaheadNode>::const_reverse_iterator node = history.rbegin();
    while (path && index > 0) {
        const cS3DVector3 current = path->at(index - 1);
        const cS3DVector3 previous = path->at(index);
        cS3DVector3 hit;
        if (FindPathIntersection(current, previous, position, distance, hit, 0.1f)) {
            out = hit;
            return true;
        }
        --index;
        if (index != 0) continue;
        // Walk the history deque backwards for the path of the previous tile.
        while (node != history.rend()) {
            const tLookaheadNode& entry = *node;
            ++node;
            if (!entry.occupant) continue;
            cISC4PathInfo* pathInfo = entry.occupant->GetPathInfoObject();
            if (!pathInfo) continue;
            const tPath* tilePath = pathInfo->GetPath(entry.key, nullptr);
            if (!tilePath || tilePath->points.size() < 2) continue;
            owned = tilePath->points;
            path = &owned;
            index = owned.size() - 1;
            break;
        }
    }
    cS3DVector3 direction = orientation_;
    if (path_ && pathIndex_ + 1 < path_->size()) {
        const cS3DVector3 segment = Sub(path_->at(pathIndex_ + 1), path_->at(pathIndex_));
        if (Length3(segment) > kVehiclePathEpsilon) direction = Scale(segment, 1.0f / Length3(segment));
    }
    out = MakeVector(position.x - direction.x * distance, position.y - direction.y * distance,
                     position.z - direction.z * distance);
    return false;
}

// 0x1000dc0e4: remember the current network occupant and path key in the history deque (the last entry is replaced when
// it is the same occupant) and drop entries from the front until `keep` remain. Nothing happens while the ignore-paths
// flag is set, when `keep` is zero or when there is no occupant.
void cSC4Vehicle::SavePathHistory(std::deque<tLookaheadNode>& history, std::uint8_t keep) {
    if (CheckAutomataFlag(kAutomataFlagIgnorePaths) || keep == 0 || !networkOccupant_) return;
    tLookaheadNode node;
    node.occupant = networkOccupant_;
    node.key = pathKey_;
    if (!history.empty() && history.back().occupant.get() == networkOccupant_) history.pop_back();
    history.push_back(node);
    while (history.size() > keep) history.pop_front();
}

// ---- pull over / plummet ------------------------------------------------------------------------------------------------
// 0x1000ddf7c: the pull-over state. value38 is the state timer. On entry (0) the heading becomes the state's steering
// direction and the perpendicular direction is built from it, flipped when it points away from the controller's
// occupant. Between 0 and 1000 ms the vehicle drives sideways towards that direction at the current speed (the direction
// is smoothed by the parameter at +0x38) and its actual speed is zeroed. From 1000 ms on the heading is re-applied and
// the state pops when the cell is crowded (more than three automata at the travel type 1) or the controller's point is
// out of radius. value38 grows by the frame time at the end.
bool cSC4Vehicle::DoPullOverTick(float milliseconds) {
    if (CheckAutomataFlag(kAutomataFlagMarkedForRemove)) return true;
    auto* state = static_cast<tAutomataStatePullOver*>(GetState());
    if (!state) return true;
    const float timer = state->value38;
    static const bool trace = std::getenv("SC4_ATTRACTDEBUG") != nullptr;
    if (trace && ((timer == 0.0f) || (timer < 1000.0f && timer + milliseconds >= 1000.0f)))
        std::fprintf(stderr, "pull over: vehicle %p at (%.1f, %.1f) %s\n", static_cast<void*>(this), position_.x, position_.z, timer == 0.0f ? "starts" : "stands at the side");
    if (timer == 0.0f && !std::isnan(timer)) {
        SetSpeed(GetDefaultSpeed(), 0.0f);
        state->orientation3c = SafeNormalize(orientation_, MakeVector(0.0f, 0.0f, 1.0f));
        state->direction48 = MakeVector(-state->orientation3c.z, state->orientation3c.y, state->orientation3c.x);
        if (state->controller) {
            if (cISC4Occupant* target = state->controller->GetOccupant()) {
                float targetPosition[3] = {0.0f, 0.0f, 0.0f};
                target->GetPosition(targetPosition);
                const cS3DVector3 delta = Sub(position_, MakeVector(targetPosition[0], targetPosition[1], targetPosition[2]));
                if (Dot3(delta, state->direction48) < 0.0f) state->direction48 = Scale(state->direction48, -1.0f);
            }
        }
    } else if (timer >= 1000.0f) {
        orientation_ = state->orientation3c;
        MoveTo(nullptr, &orientation_.x);
        int cellX = 0;
        int cellZ = 0;
        if (trafficHost_) trafficHost_->PositionToCell(position_.x, position_.z, cellX, cellZ);
        if (vehicleHost_ && vehicleHost_->CellAutomatonCount(1, cellX, cellZ) > 3) PopState();
        if (state->controller && !state->controller->IsPointWithinRadius(&position_.x, nullptr)) PopState();
    } else {
        const float speed = GetCurrentSpeed();
        if (speed * milliseconds > 0.0f) {
            cS3DVector3 steer = state->direction48;
            const float steerLength = Length3(steer);
            if (steerLength > 0.0f) {
                steer = Scale(steer, (GetRadius() * 1.5f * milliseconds / 1000.0f) / steerLength);
            }
            cS3DVector3 heading = state->orientation3c;
            const float headingLength = Length3(heading);
            if (headingLength > 0.0f) heading = Scale(heading, (speed * milliseconds) / headingLength);
            const cS3DVector3 next = MakeVector(steer.x + heading.x + position_.x, steer.y + heading.y + position_.y,
                                                steer.z + heading.z + position_.z);
            float altitude = position_.y;
            if (cSC4TrafficNetworkMap* map = trafficHost_ ? trafficHost_->TrafficNetworkMap() : nullptr) {
                altitude = map->GetRoadAltitude(next.x, next.z);
            }
            const cS3DVector3 delta = Sub(MakeVector(next.x, altitude, next.z), position_);
            const cS3DVector3 direction = SafeNormalize(delta, orientation_);
            if (std::isfinite(delta.y)) orientation_ = direction;
            position_ = MakeVector(next.x, altitude, next.z);
            MoveTo(&position_.x, &direction.x);
            SetActualSpeed(0.0f);
        }
    }
    state->value38 = timer + milliseconds;
    return true;
}

// 0x1000ddc04: the plummet state. While the state timer (+0x44) runs the vehicle spins: the y of its heading follows the
// spin rate (+0x3c) around the target tilt (+0x40) and bounces at +-0.05, and only the heading is set. Afterwards gravity
// (+0x38) accelerates the fall, an active splash timer (+0x48) counts down and removes the automaton when it expires; the
// city bounds end the fall, and at the ground the vehicle either vanishes (at or below the terrain) or makes a
// "car_splash" effect, sets its opacity and starts a random splash timer (0..1000 ms). The position is moved every frame.
bool cSC4Vehicle::DoPlummetTick(float milliseconds) {
    if (CheckAutomataFlag(kAutomataFlagMarkedForRemove)) return true;
    auto* state = static_cast<tAutomataStatePlummet*>(GetState());
    if (!state) return true;
    if (state->value44 > 0.0f) {
        state->value44 -= milliseconds;
        float target = milliseconds * 0.01f * state->value3c + orientation_.y;
        if (target < state->value40 - 0.05f) {
            state->value3c = -state->value3c;
            target = state->value40 - 0.05f;
        } else if (state->value40 + 0.05f < target) {
            state->value3c = -state->value3c;
            target = state->value40 + 0.05f;
        }
        const cS3DVector3 direction = SafeNormalize(MakeVector(orientation_.x, target, orientation_.z), orientation_);
        MoveTo(nullptr, &direction.x);
        return true;
    }
    const cS3DVector3 position = position_;
    cS3DVector3 next = MakeVector(position.x, state->value38 * milliseconds + position.y, position.z);
    if (state->value48 > 0.0f) {
        state->value48 -= milliseconds;
        if (state->value48 <= 0.0f) {
            if (auto* effect = static_cast<IVehicleEffect*>(reinterpret_cast<IVehicleEffect*>(state->runtime50))) {
                effect->Detach(false);
                effect->Release();
                state->runtime50 = 0;
            }
            Remove(false);
        }
    }
    if (!CheckAutomataFlag(kAutomataFlagMarkedForRemove)) {
        state->value38 -= milliseconds * kVehicleGravityStep;
        if (!vehicleHost_ || !vehicleHost_->IsInsideCity(position.x, position.z)) {
            Remove(false);
        } else {
            const float terrain = Host() ? Host()->TerrainHeight(position.x, position.z) : 0.0f;
            const float surface = vehicleHost_->SurfaceHeight(position.x, position.z);
            const float ground = surface <= terrain ? terrain : surface;
            if (next.y <= ground && state->value48 <= 0.0f) {
                state->value38 = kVehicleStoppedSpeed;
                if (next.y <= terrain) {
                    state->value48 = 0.0f;
                    Remove(false);
                } else {
                    if (IVehicleEffect* effect = vehicleHost_->CreateNamedEffect(kVehicleSplashEffect)) {
                        state->runtime50 = reinterpret_cast<std::uint64_t>(effect);
                        float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, position.x, next.y, position.z, 1};
                        effect->SetTransform(matrix);
                        effect->SetFlag(false);
                    }
                    const double rolled = Host() ? Host()->RandomUnit() : 0.0;
                    milliseconds = static_cast<float>(rolled);
                    SetOpacity(1.0f, 0.0f);
                    state->value48 = milliseconds * 1000.0f;
                }
            }
        }
    }
    MoveTo(&next.x, nullptr);
    return true;
}

// 0x1000e16ce: clamp the player-drive target position against the lot it is over: the four edges of the lot's bounding
// rectangle are intersected with the movement sphere and the closest intersection replaces the position.
void cSC4Vehicle::RestrictPlayerDrivePosition(const cS3DVector3& from, cS3DVector3& position) {
    const float length = Distance3(position, from);
    if (!(length > kVehiclePathEpsilon)) return;
    if (!trafficHost_ || !vehicleHost_) return;
    int cellX = 0;
    int cellZ = 0;
    trafficHost_->PositionToCell(position.x, position.z, cellX, cellZ);
    std::int32_t rect[4] = {0, 0, 0, 0};
    if (!vehicleHost_->LotBoundingRectAtCell(cellX, cellZ, rect)) return;
    float minX = 0.0f;
    float minZ = 0.0f;
    float maxX = 0.0f;
    float maxZ = 0.0f;
    vehicleHost_->CellToWorld(rect[0], rect[1], minX, minZ);
    vehicleHost_->CellToWorld(rect[2] + 1, rect[3] + 1, maxX, maxZ);
    float best = FLT_MAX;
    cS3DVector3 out = position;
    Check2DIntersection(minX, minZ, maxX, minZ, from, length, out, best);
    Check2DIntersection(maxX, minZ, maxX, maxZ, from, length, out, best);
    Check2DIntersection(maxX, maxZ, minX, maxZ, from, length, out, best);
    Check2DIntersection(minX, maxZ, minX, minZ, from, length, out, best);
    if (best < FLT_MAX) position = out;
}

// 0x1000e0de0: the player-drive lot border crossing. Starting and ending cell are computed; when they differ and only
// the starting cell is drivable, the cells between them are walked and the first non-drivable one gives the crossing
// point and the outward direction (the crossing point is pulled back by 0.1 when walking towards positive coordinates).
bool cSC4Vehicle::GetPlayerDriveLotIntersection(const cS3DVector3& from, cS3DVector3& position, cS3DVector2* outPoint,
                                                cS3DVector2* outDirection) {
    if (!vehicleHost_) return false;
    int startX = WorldToCell(from.x);
    int startZ = WorldToCell(from.z);
    int endX = WorldToCell(position.x);
    int endZ = WorldToCell(position.z);
    if (startX == endX && startZ == endZ) return false;
    if (!vehicleHost_->CanDriveOnCell(startX, startZ, from.x, from.z)) return false;
    if (vehicleHost_->CanDriveOnCell(endX, endZ, position.x, position.z)) return false;
    const float dx = position.x - from.x;
    const float dz = position.z - from.z;
    const int stepX = endX < startX ? -1 : 1;
    const int stepZ = endZ < startZ ? -1 : 1;
    bool movesX = startX != endX;
    bool movesZ = startZ == endZ;
    int x = startX;
    int z = startZ;
    do {
        float tX = FLT_MAX;
        float tZ = FLT_MAX;
        float pointX = 0.0f;
        float pointZ = 0.0f;
        if (movesX) {
            const float boundary = static_cast<float>((endX < startX ? 1 : 0) + stepX + x) * 16.0f;
            tX = (boundary - from.x) / dx;
            pointZ = dz * tX + from.z;
            pointX = boundary;
        }
        if (!movesZ) {
            const float boundary = static_cast<float>((endZ < startZ ? 1 : 0) + stepZ + z) * 16.0f;
            tZ = (boundary - from.z) / dz;
            pointX = dx * tZ + from.x;
            pointZ = boundary;
        }
        if (tZ <= tX) {
            z += stepZ;
            if (!vehicleHost_->CanDriveOnCell(x, z, pointX, pointZ)) {
                if (outPoint) *outPoint = cS3DVector2{dx * tZ + from.x, pointZ};
                if (outDirection) *outDirection = cS3DVector2{0.0f, static_cast<float>(-stepZ)};
                if (endZ < startZ) return true;
                if (outPoint) outPoint->y -= 0.1f;
                return true;
            }
        } else {
            x += stepX;
            if (!vehicleHost_->CanDriveOnCell(x, z, pointX, pointZ)) {
                if (outPoint) *outPoint = cS3DVector2{pointX, dz * tX + from.z};
                if (outDirection) *outDirection = cS3DVector2{static_cast<float>(-stepX), 0.0f};
                if (endX < startX) return true;
                if (outPoint) outPoint->x -= 0.1f;
                return true;
            }
        }
        movesX = x != endX;
        movesZ = z == endZ;
    } while (!movesZ || x != endX);
    return false;
}

// ---- player-drive helpers -----------------------------------------------------------------------------------------------
// 0x1000dfbe4: find the network occupant under the driving position. The occupants of the cell are scanned: one with the
// drivable network mask and a path that starts near the position wins when its altitude is below the current one (and at
// most 4 units below it). The best occupant is either returned directly (when it is a tunnel interface) or stored in the
// state's occupant slot when it carries the on-road flag; `onRoad` is set in both cases, and `out` (when given) receives
// the position with the corrected y. Without a candidate, the stored occupant is reused only while the car is above the
// terrain by more than two units and it is not already marked off-road.
ITrafficAutomatonOccupant* cSC4Vehicle::PlayerDriveGetNetwork(tAutomataStatePlayerDrive* state, bool& onRoad,
                                                             const cS3DVector3& position, cS3DVector3* out) {
    onRoad = false;
    if (!vehicleHost_ || !vehicleHost_->IsInsideCity(position.x, position.z)) return nullptr;
    const int cellX = WorldToCell(position.x);
    const int cellZ = WorldToCell(position.z);
    const float terrain = Host() ? Host()->TerrainHeight(position.x, position.z) : 0.0f;
    float altitude = terrain;
    std::vector<ITrafficAutomatonOccupant*> occupants;
    vehicleHost_->OccupantsInCell(cellX, cellZ, occupants);
    ITrafficAutomatonOccupant* best = nullptr;
    float bestDistance = FLT_MAX;
    for (ITrafficAutomatonOccupant* occupant : occupants) {
        if (!occupant) continue;
        if (!occupant->HasAnyNetworkFlag(kVehicleNetworkFlagDriveable)) continue;
        cISC4PathInfo* pathInfo = occupant->GetPathInfoObject();
        if (!pathInfo) continue;
        const tPath* path = pathInfo->GetBestPath(1, kPathEdgeWildcard, kPathEdgeWildcard, position, nullptr, nullptr);
        if (!path) continue;
        const float occupantAltitude = occupant->GetAltitude(position.x, position.z);
        cS3DVector3 snapped = position;
        const int index = pathInfo->SnapPointToPath(snapped, &path->points);
        float candidate = occupantAltitude;
        if (index != -1 && !occupant->HasAnyNetworkFlag(kVehicleNetworkFlagOnRoad) &&
            std::fabs(occupantAltitude - position.y) < std::fabs(snapped.y - position.y)) {
            candidate = occupantAltitude;
        }
        if (candidate < bestDistance && candidate - 4.0f < position.y) {
            best = occupant;
            bestDistance = candidate;
        }
    }
    if (best) {
        if (!best->HasNetworkFlag(kVehicleNetworkFlagTunnelInterface)) {
            if (best->HasAnyNetworkFlag(kVehicleNetworkFlagOnRoad)) {
                onRoad = true;
                altitude = bestDistance;
            }
        } else {
            SetDriveOccupant(best);
            onRoad = true;
            altitude = bestDistance;
        }
    } else if (state->onRoad254 == 0) {
        if (altitude - 2.0f > position.y && driveOccupant_) {
            altitude = driveOccupant_->GetAltitude(position.x, position.z);
            onRoad = true;
            best = driveOccupant_;
        }
    }
    if (out) {
        *out = position;
        out->y = altitude;
    }
    return best;
}

// 0x1000dffb6: turn a collision into damage and a speed change. The collision amount is the impact speed over the
// parameter at +0x48; when it is above 2 (or the normal is non-zero) the damage grows by the normal length times 2000
// (scaled by the impact above 2, un-scaled below 5), the crash / injury effects are updated, and the speed becomes
// min(desired, max(0, amount * dot(normal, heading) + current)), updated in the tween and the state.
void cSC4Vehicle::ResolveCollision(const cS3DVector3& normal, float speed, tAutomataStatePlayerDrive* state,
                                   const tAutomataPlayerDriveParams* params) {
    const float amount = speed / params->F(0x48);
    const float normalLength = Length3(normal);
    if (amount > 2.0f || normalLength > 0.01f) {
        const float scaled = (normalLength - 0.01f) * 2000.0f;
        const float added = amount < 5.0f ? amount * scaled : scaled;
        state->damage70 = static_cast<std::int32_t>(static_cast<float>(static_cast<std::uint32_t>(state->damage70)) + added);
    }
    UpdateAfterDamage(state, params);
    const float tween = EasedTween(speedTween_);
    const float dot = normal.z * orientation_.z + normal.y * orientation_.y + normal.x * orientation_.x;
    float value = amount * dot + tween;
    if (!(value >= 0.0f)) value = 0.0f;
    if (value > tween) value = tween;
    speedTween_.SetDesired(value, 0.0f);
    state->value3c = value;
    if (value == 0.0f) state->value38 = 0.0f;
}

// 0x1000e0138: try every key of the array until one of its paths snaps the position within 4 units vertically; the key
// and the snapped index are answered through the references. The position is restored between attempts. Null when no
// key fits.
const tPath* cSC4Vehicle::ChoosePathFromKeyArray(cISC4PathInfo* pathInfo, const std::vector<std::uint32_t>& keys,
                                                 cS3DVector3& position, std::uint32_t& key, int& index) {
    if (!pathInfo || keys.empty()) return nullptr;
    const cS3DVector3 saved = position;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        key = keys[i];
        const tPath* path = pathInfo->GetPath(key, nullptr);
        if (!path) continue;
        index = pathInfo->SnapPointToPath(position, &path->points);
        if (std::fabs(position.y - saved.y) < 4.0f) return path;
        position = saved;
    }
    return nullptr;
}

// 0x1000e0228: pick a path next to the tile the vehicle is about to enter. The occupant of the neighbour tile (edge =
// the low byte of the path key, type 1) is asked for paths near the vehicle's path position entering through the edge
// opposite the current one and leaving through the direction the vehicle is facing; the first key wins, with a second
// try leaving through any edge. The chosen path is installed (keeping the destination) and true answered.
bool cSC4Vehicle::ChooseNewPlayerDriveDestination(cS3DVector3& position) {
    cSC4TrafficNetworkMap* map = trafficHost_ ? trafficHost_->TrafficNetworkMap() : nullptr;
    if (!map || !networkOccupant_) return false;
    TrafficTileInfo* tile = map->GetTileInfoAtNeighborEdge(networkOccupant_, static_cast<std::uint8_t>(pathKey_ & 0xff),
                                                          false, 1);
    if (!tile || !tile->occupant) return false;
    auto* occupant = static_cast<ITrafficAutomatonOccupant*>(tile->occupant);
    cISC4PathInfo* pathInfo = occupant->GetPathInfoObject();
    if (!pathInfo) return false;
    std::uint8_t exit = kPathEdgeWildcard;
    GetEdgeFromVector(occupant, position, exit, true);
    const std::uint8_t entry = static_cast<std::uint8_t>(((pathKey_ & 0xff) + 2) & 3);
    std::uint32_t key = 0;
    const tPath* path = nullptr;
    std::vector<std::uint32_t> keys = pathInfo->GetPathsNearStartPoint(1, pathPos_, entry, exit, 4.0f);
    if (!keys.empty()) {
        key = keys.front();
        path = pathInfo->GetPath(key, nullptr);
    }
    if (!path) {
        keys = pathInfo->GetPathsNearStartPoint(1, pathPos_, entry, kPathEdgeWildcard, 4.0f);
        if (!keys.empty()) {
            key = keys.front();
            path = pathInfo->GetPath(key, nullptr);
        }
    }
    if (!path) return false;
    SetPath(occupant, key, &path->points, true);
    return true;
}

// 0x1000e03f4: the damage thresholds. At half the parameter at +0x5c the injury effect is created; at the full value the
// accident object is created (type 2, 30 s), the automaton is disabled, the damage callback fires and the crash effect is
// created. The effect (if any) replaces the driving state's effect, follows the vehicle with an identity transform and is
// started.
void cSC4Vehicle::UpdateAfterDamage(tAutomataStatePlayerDrive* state, const tAutomataPlayerDriveParams* params) {
    IVehicleEffect* created = nullptr;
    const float damage = static_cast<float>(static_cast<std::uint32_t>(state->damage70));
    if (params->F(0x5c) * 0.5f <= damage) {
        if (params->F(0x5c) <= damage) {
            if (vehicleHost_) SetAccident(vehicleHost_->CreateAccident(static_cast<cISC4Occupant*>(this), 2, 30000));
            SetAutomataFlag(kAutomataFlagDisabled, true);
            SendCallbackMessage(kVehicleDamageMessage, 1);
            if (vehicleHost_) created = vehicleHost_->CreateNamedEffect(kVehicleCrashEffect);
        } else if (vehicleHost_) {
            created = vehicleHost_->CreateNamedEffect(kVehicleInjuredEffect);
        }
    }
    if (!created) return;
    SetDriveEffect(created);
    float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, position_.x, position_.y, position_.z, 1};
    created->SetTransform(matrix);
    created->SetFlag(false);
    created->Release();
}

// 0x1000e061a: the off-road sub-step. The steering angle (state +0x6c) rotates the heading about the up axis; the path
// position is seeded half a radius in front of the vehicle when it is still unset. The driving position is advanced along
// the correction from the point the last step expected (state +0x54/+0x60) to the path target, then snapped back onto the
// network. Without a collision the path position is placed a radius in front of the driving position, the driving
// position is pulled a radius behind the path position and the drive direction is smoothed by the parameter at +0x38.
// PARTIAL: the collision branch (path position reset to the network and the speed reflected) and the final midpoint move
// cannot be reconstructed from the decompile - the dropped SIMD lanes hide the reset position and the reflection blend.
bool cSC4Vehicle::DoPlayerDriveOffRoadTick(float /*unused*/, tAutomataStatePlayerDrive* state, float distance,
                                           const tAutomataPlayerDriveParams* params, ITrafficAutomatonOccupant* occupant) {
    (void)occupant;
    if (!(distance != 0.0f || std::isnan(distance))) return true;
    const float angle = state->steering6c;
    const float sine = std::sin(angle);
    const float cosine = std::cos(angle);
    const cS3DVector3 rotated = MakeVector(orientation_.x * cosine + orientation_.z * sine, orientation_.y,
                                           orientation_.z * cosine - orientation_.x * sine);
    if (IsUnsetVector(pathPos_)) {
        const float radius = GetRadius();
        pathPos_ = MakeVector(position_.x + radius * orientation_.x * 0.5f,
                              position_.y + radius * orientation_.y * 0.5f,
                              position_.z + radius * orientation_.z * 0.5f);
    }
    cS3DVector3 delta = MakeVector(
        pathPos_.x + rotated.x * distance - (state->driveDir60.x * distance + state->drivePos54.x),
        pathPos_.y + rotated.y * distance - (state->driveDir60.y * distance + state->drivePos54.y),
        pathPos_.z + rotated.z * distance - (state->driveDir60.z * distance + state->drivePos54.z));
    cS3DVector3 direction = SafeNormalize(delta, orientation_);
    state->drivePos54 = MakeVector(state->drivePos54.x + direction.x * distance,
                                   state->drivePos54.y + direction.y * distance,
                                   state->drivePos54.z + direction.z * distance);
    bool onRoad = false;
    PlayerDriveGetNetwork(state, onRoad, state->drivePos54, &state->drivePos54);
    const float radius = GetRadius();
    cS3DVector3 collisionPoint = pathPos_;
    float collisionAmount = 1.0f;
    if (!DoCollisionCheck(collisionPoint, collisionAmount)) {
        pathPos_ = MakeVector(direction.x * radius + state->drivePos54.x, state->drivePos54.y,
                              direction.z * radius + state->drivePos54.z);
        PlayerDriveGetNetwork(state, onRoad, pathPos_, &pathPos_);
        const cS3DVector3 toPath = Sub(pathPos_, state->drivePos54);
        const cS3DVector3 forward = SafeNormalize(toPath, orientation_);
        state->drivePos54 = MakeVector(pathPos_.x - forward.x * radius, pathPos_.y - forward.y * radius,
                                       pathPos_.z - forward.z * radius);
        const float smoothing = params->F(0x38);
        state->driveDir60 = MakeVector((forward.x - state->driveDir60.x) * smoothing + state->driveDir60.x,
                                       (forward.y - state->driveDir60.y) * smoothing + state->driveDir60.y,
                                       (forward.z - state->driveDir60.z) * smoothing + state->driveDir60.z);
        direction = forward;
    } else {
        // PARTIAL: the reflected step is kept as "stay put and stop" (see the function comment).
        state->drivePos54 = pathPos_;
        state->driveDir60 = direction;
        speedTween_.SetDesired(0.0f, 0.0f);
        state->value38 = 0.0f;
    }
    const cS3DVector3 midpoint = MakeVector(pathPos_.x - (pathPos_.x - state->drivePos54.x) * 0.5f,
                                            pathPos_.y - (pathPos_.y - state->drivePos54.y) * 0.5f,
                                            pathPos_.z - (pathPos_.z - state->drivePos54.z) * 0.5f);
    MoveTo(&midpoint.x, &direction.x);
    return true;
}

// 0x1000e1bfa: the collision sweep of the player-drive step. The vehicle speed vector (heading * eased speed) is built,
// the bounding box is expanded by 24 units and the occupant manager is asked for up to five other vehicles of the same
// occupant type in it. Each candidate's box is intersected with the expanded box (a one-unit overlap in x or z is
// ignored), the candidate is queried for the automaton interface, skipped when marked for removal, and its oriented box
// is tested against the vehicle's. The relative velocity (candidate heading * its actual speed - ours) and the ratio of
// the two box areas are answered: a ratio >= 2 or a farther candidate replaces the previous answer, and a ratio < 2 makes
// the candidate handle the collision at the centre of the overlap box. With the driving state active the collision audio
// event restarts. Answers the number of collisions found (non-zero).
bool cSC4Vehicle::DoCollisionCheck(cS3DVector3& position, float& amount) {
    if (!vehicleHost_) return false;
    float velocityX = orientation_.x;
    float velocityY = orientation_.y;
    float velocityZ = orientation_.z;
    const float speed = EasedTween(speedTween_);
    const float lengthSquared = velocityX * velocityX + velocityY * velocityY + velocityZ * velocityZ;
    if (lengthSquared > 0.0f) {
        const float scale = speed / std::sqrt(lengthSquared);
        velocityX *= scale;
        velocityY *= scale;
        velocityZ *= scale;
    }
    float bounds[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    GetBoundingBox(bounds, bounds + 3);
    float minXyz[3] = {bounds[0] - 24.0f, bounds[1] - 24.0f, bounds[2] - 24.0f};
    float maxXyz[3] = {bounds[3] + 24.0f, bounds[4] + 24.0f, bounds[5] + 24.0f};
    std::vector<cISC4Occupant*> occupants;
    vehicleHost_->OccupantsInBox(minXyz, maxXyz, kVehicleOccupantType, 5, occupants);
    if (occupants.empty()) return false;
    AutomataOBB self(*this, pathPos_, 1.0f);
    const float selfArea = self.halfX * 4.0f * self.halfZ;
    float bestDistance = -FLT_MAX;
    unsigned count = 0;
    for (cISC4Occupant* occupant : occupants) {
        if (!occupant || occupant == static_cast<cISC4Occupant*>(this)) continue;
        float otherMin[3] = {0.0f, 0.0f, 0.0f};
        float otherMax[3] = {0.0f, 0.0f, 0.0f};
        occupant->GetBoundingBox(otherMin, otherMax);
        float overlapMin[3] = {0.0f, 0.0f, 0.0f};
        float overlapMax[3] = {0.0f, 0.0f, 0.0f};
        if (!SC4GeometryUtilities::BoundingBoxesIntersect(minXyz, maxXyz, otherMin, otherMax, overlapMin, overlapMax)) continue;
        if (!(std::fabs(overlapMax[0] - overlapMin[0]) > 1.0f) && !(std::fabs(overlapMax[2] - overlapMin[2]) > 1.0f)) continue;
        void* raw = nullptr;
        if (!occupant->QueryInterface(kAutomatonIid, &raw) || !raw) continue;
        auto* other = static_cast<cSC4Automaton*>(raw);
        if (other->CheckAutomataFlag(kAutomataFlagMarkedForRemove)) {
            other->Release();
            continue;
        }
        cS3DVector3 otherPosition{};
        other->GetPosition(&otherPosition.x);
        AutomataOBB otherBox(*other, otherPosition, 1.0f);
        if (!ShouldCollide(other) || !self.TestForCollision(otherBox)) {
            other->Release();
            continue;
        }
        cS3DVector3 otherDirection{};
        other->GetOrientation(&otherDirection.x);
        const float otherSpeed = other->GetActualSpeed();
        const cS3DVector3 relative = Sub(Scale(SafeNormalize(otherDirection, otherDirection), otherSpeed),
                                         MakeVector(velocityX, velocityY, velocityZ));
        const float distance = Length3(relative);
        const float otherArea = otherBox.halfX * 4.0f * otherBox.halfZ;
        const float ratio = selfArea != 0.0f ? otherArea / selfArea : (otherArea != 0.0f ? FLT_MAX : 0.0f);
        if (ratio >= 2.0f || bestDistance < distance) {
            position = relative;
            amount = ratio;
            bestDistance = distance;
        }
        if (ratio < 2.0f) {
            const cS3DVector3 centre = MakeVector((overlapMax[0] - overlapMin[0]) * 0.5f + overlapMin[0],
                                                  (overlapMax[1] - overlapMin[1]) * 0.5f + overlapMin[1],
                                                  (overlapMax[2] - overlapMin[2]) * 0.5f + overlapMin[2]);
            other->HandleCollision(this, centre);
        }
        if (FindState(kTrafficStateDrive) && trafficHost_) {
            trafficHost_->StopAudioEvent(kVehicleAudioEventCollision);
            trafficHost_->StartAudioEvent(kVehicleAudioEventCollision);
        }
        ++count;
        other->Release();
    }
    return count != 0;
}

// 0x1000dd852: the traffic speed modifier for a vehicle. Above the terrain (or on a rail-type occupant) a road vehicle
// with the local driver at the wheel multiplies the base modifier by the network type's factor; otherwise the factor of
// the occupant's network type is used. When neither the player-drive behavior nor the "above terrain" case applies, a
// straight path (entry == exit edge) uses the curve factor times 0.6 and a path of at least four points with the next
// edge differs uses the plain curve factor; a special route returns before the turn factor. The turn factor comes from
// asin(orientation.y) scaled by the tuning maximum / divisor: 1 - min(o, max) for a positive pitch, 1 + o for a
// negative one and the curve value itself at zero pitch.
float cSC4Vehicle::GetSpeedModifier() const {
    const float base = cSC4Automaton::GetSpeedModifier();
    if (!networkOccupant_) return base;
    const VehicleTuning& tuning = Tuning();
    const std::uint32_t behavior = GetBehavior();
    bool aboveTerrain = true;
    if (!networkOccupant_->IsOfType(0xc)) {
        aboveTerrain = false;
        if (networkOccupant_->IsOfType(2) && vehicleHost_ && vehicleHost_->IsInsideCity(position_.x, position_.z)) {
            const float terrain = Host() ? Host()->TerrainHeight(position_.x, position_.z) : 0.0f;
            aboveTerrain = position_.y - terrain > 5.0f;
        }
    }
    float value = base;
    if (behavior != kStatePlayerDrive && !CheckAutomataFlag(0x1000)) {
        if (aboveTerrain) {
            return base * tuning.networkSpeed[2];
        }
        for (std::uint32_t type = 0; type < 13; ++type) {
            if (networkOccupant_->IsOfType(type)) {
                value = base * tuning.networkSpeed[type];
                break;
            }
        }
    }
    if (behavior == kStatePlayerDrive || aboveTerrain) return value;
    const std::uint8_t exitEdge = static_cast<std::uint8_t>(pathKey_ & 0xff);
    const std::uint8_t entryEdge = static_cast<std::uint8_t>((pathKey_ >> 8) & 0xff);
    if (exitEdge == entryEdge) {
        value = value * (tuning.pathCurveFactor * 0.6f);
    } else if (static_cast<std::uint8_t>((exitEdge + 1) & 3) != entryEdge && path_ && path_->size() >= 4) {
        value = value * tuning.pathCurveFactor;
    }
    if (CheckAutomataFlag(kAutomataFlagSpecialRoute)) return value;
    const float pitch = orientation_.y;
    float turn = 1.0f;
    if (pitch > 0.0f) {
        const float scaled = std::asin(pitch) * tuning.turnMax / tuning.turnDivisor;
        turn = 1.0f - (scaled <= tuning.turnMax ? scaled : tuning.turnMax);
    } else if (pitch < 0.0f) {
        const float scaled = std::asin(pitch) * tuning.turnMax / tuning.turnDivisor;
        turn = 1.0f + (scaled <= tuning.turnMax ? scaled : tuning.turnMax);
    } else {
        return value;
    }
    return value * turn;
}

// 0x1000de6c0: the player-drive frame. PARTIAL (see below). The parameter block of the first automata group with
// player-drive parameters gates everything (a damaged vehicle above the +0x5c threshold does nothing and answers true).
// The crash audio event follows the sign of the eased speed, the base player-drive tick runs, the network occupant is
// looked up and the on/off-road callback fires when its state changed. The speed is then updated
// (PlayerDriveUpdateSpeed), and the frame is split into ceil(milliseconds / 33) sub-steps of `subStep` distance.
//   * Off the network: ignore-paths is set, the path is dropped, the off-road audio / "haze" effect pair follows whether
//     there is an occupant, and each sub-step runs DoPlayerDriveOffRoadTick plus the collision check / response.
//   * On the network: the audio / haze pair is the other way round, a missing path is re-chosen (the neighbours' paths
//     near the path position with the steered edge), and each sub-step interpolates along the path (a finished path
//     chooses a new destination or removes the automaton), moves the vehicle half a radius behind the interpolated point,
//     stores the driving state's position / heading pair and runs the collision check.
//   * On the network a strong steering command also re-targets the path through the path info's GetBestPath with the
//     current entry edge.
//   * The tail moves the damage effect with the vehicle and adds the disaster layer's damage (three times its value)
//     before the crash effect update; the actual speed is the travelled distance over the frame time.
// PARTIAL: the driving state's dynamic-particle writes (+0x90: position, angle and the heading * 1000 * tween fields) are
// not reproduced (cSC4DynamicParticleEffect is unrecovered), and the skid path query through the path info's vslot 0x138
// (used when the state bytes +0x112/+0x113 are set) has no counterpart in the recovered cISC4PathInfo.
bool cSC4Vehicle::DoPlayerDriveTick(float milliseconds) {
    const cS3DVector3 startPosition = position_;
    auto* state = static_cast<tAutomataStatePlayerDrive*>(GetState());
    if (!state) return false;
    const tAutomataPlayerDriveParams* params = nullptr;
    if (!vehicleHost_ || !vehicleHost_->FindPlayerDriveParams(this, &params) || !params) return false;
    const float damage = static_cast<float>(static_cast<std::uint32_t>(state->damage70));
    if (damage > params->F(0x5c)) return true;
    const float reverseBefore = state->history[1];
    const float easedSpeed = EasedTween(speedTween_);
    if (trafficHost_) {
        if (easedSpeed >= 0.0f) {
            trafficHost_->StartAudioEvent(kVehicleAudioEventCrash);
        } else {
            trafficHost_->StopAudioEvent(kVehicleAudioEventCrash);
        }
    }
    if (!cSC4Automaton::DoPlayerDriveTick(milliseconds)) return false;
    const float marker = state->history[4];
    bool onRoad = false;
    ITrafficAutomatonOccupant* occupant = PlayerDriveGetNetwork(state, onRoad, pathPos_, nullptr);
    const bool offRoad = marker == -1.0f ? occupant == nullptr : !onRoad;
    if ((state->onRoad254 != 0) != offRoad) {
        SendCallbackMessage(kVehicleEventOnRoad, offRoad ? 1 : 0);
        state->onRoad254 = offRoad ? 1 : 0;
    }
    bool boost = offRoad;
    if (reverseBefore > 0.0f || state->history[1] <= 0.0f) boost = false;
    PlayerDriveUpdateSpeed(state, params, boost, occupant != nullptr, milliseconds);
    int steps = static_cast<int>(std::ceil(milliseconds / kVehicleFrameMilliseconds));
    const float tweenSpeed = EasedTween(speedTween_);
    const float subStep = steps > 0 ? (milliseconds / static_cast<float>(steps)) * tweenSpeed : 0.0f;
    if (steps < 0) steps = 0;
    if (!occupant || offRoad) {
        SetAutomataFlag(kAutomataFlagIgnorePaths, true);
        if (occupant != networkOccupant_) {
            SetPath(occupant, 0, nullptr, true);
            occupant = networkOccupant_;
        }
        if (occupant && state->onRoad254 && trafficHost_) {
            trafficHost_->StopAudioEvent(kVehicleAudioEventOffRoad);
            occupant = networkOccupant_;
        }
        if (!occupant) {
            if (tweenSpeed == 0.0f) {
                if (trafficHost_) trafficHost_->StartAudioEvent(kVehicleAudioEventDrive);
                StopNamedEffects(kVehicleHazeEffect, 0);
            } else {
                if (trafficHost_) {
                    trafficHost_->StopAudioEvent(kVehicleAudioEventDrive);
                    trafficHost_->StopAudioEvent(kVehicleAudioEventOffRoad);
                }
                StartNamedEffects(kVehicleHazeEffect, 0, true);
            }
        } else {
            if (trafficHost_) trafficHost_->StartAudioEvent(kVehicleAudioEventDrive);
            StopNamedEffects(kVehicleHazeEffect, 0);
        }
        for (int i = 0; i < steps; ++i) {
            DoPlayerDriveOffRoadTick(0.0f, state, subStep, params, occupant);
            cS3DVector3 point = MakeVector(0.0f, 0.0f, 0.0f);
            float amount = 1.0f;
            if (DoCollisionCheck(point, amount)) {
                ResolveCollision(point, amount, state, params);
                break;
            }
        }
    } else {
        if (trafficHost_) {
            trafficHost_->StartAudioEvent(kVehicleAudioEventOffRoad);
            trafficHost_->StartAudioEvent(kVehicleAudioEventDrive);
        }
        StopNamedEffects(kVehicleHazeEffect, 0);
        if (CheckAutomataFlag(kAutomataFlagIgnorePaths) || IsUnsetVector(pathPos_)) {
            if (cISC4PathInfo* pathInfo = occupant->GetPathInfoObject()) {
                const float angle = state->steering6c;
                const float sine = std::sin(angle);
                const float cosine = std::cos(angle);
                const cS3DVector3 steered = MakeVector(orientation_.x * cosine + orientation_.z * sine, orientation_.y,
                                                       orientation_.z * cosine - orientation_.x * sine);
                std::uint8_t edge = kPathEdgeWildcard;
                GetEdgeFromVector(occupant, steered, edge, true);
                const std::uint32_t savedIndex = pathIndex_;
                std::uint32_t key = 0;
                int index = 0;
                cS3DVector3 near = pathPos_;
                std::vector<std::uint32_t> keys =
                    pathInfo->GetPathsNearStartPoint(1, near, edge, kPathEdgeWildcard, 16.0f);
                const tPath* path = ChoosePathFromKeyArray(pathInfo, keys, near, key, index);
                if (!path) {
                    keys = pathInfo->GetPathsNearStartPoint(1, near, edge, kPathEdgeWildcard, 16.0f);
                    path = ChoosePathFromKeyArray(pathInfo, keys, near, key, index);
                }
                SetPath(occupant, path ? key : pathKey_, path ? &path->points : nullptr, true);
                if (path) pathIndex_ = savedIndex;
            }
        }
        SetAutomataFlag(kAutomataFlagIgnorePaths, false);
        for (int i = 0; i < steps; ++i) {
            cS3DVector3 out = MakeVector(0.0f, 0.0f, 0.0f);
            if (!InterpolateAlongPath(subStep, pathPos_, out, reinterpret_cast<std::int32_t*>(&pathIndex_), path_,
                                      &pathRemaining_)) {
                if (!ChooseNewPlayerDriveDestination(out)) {
                    Remove(false);
                    break;
                }
            }
            cS3DVector3 direction = MakeVector(0.0f, 0.0f, 0.0f);
            if (path_ && pathIndex_ + 1 < path_->size()) {
                direction = Sub(path_->at(pathIndex_ + 1), path_->at(pathIndex_));
            }
            if (Length3(direction) <= kVehiclePathEpsilon) {
                direction = IsUnsetVector(prevOrient_) ? MakeVector(0.0f, 0.0f, 1.0f) : prevOrient_;
            } else {
                direction = Scale(direction, 1.0f / Length3(direction));
                if (!IsUnsetVector(prevOrient_)) {
                    direction = MakeVector((direction.x + direction.x + prevOrient_.x) * 0.33333334f,
                                           (direction.y + direction.y + prevOrient_.y) * 0.33333334f,
                                           (direction.z + direction.z + prevOrient_.z) * 0.33333334f);
                }
            }
            destination_ = out;
            const float radius = GetRadius();
            const cS3DVector3 next = MakeVector(out.x - direction.x * radius * 0.5f, out.y - direction.y * radius * 0.5f,
                                                out.z - direction.z * radius * 0.5f);
            MoveTo(&next.x, &direction.x);
            prevOrient_ = direction;
            pathPos_ = out;
            SetAutomataFlag(kTrafficFlagStopped, false);
            state->drivePos54 = MakeVector(out.x - direction.x * radius, out.y - direction.y * radius,
                                           out.z - direction.z * radius);
            state->driveDir60 = direction;
            state->steer48 = direction;
            cS3DVector3 point = MakeVector(0.0f, 0.0f, 0.0f);
            float amount = 1.0f;
            if (DoCollisionCheck(point, amount)) {
                ResolveCollision(point, amount, state, params);
                break;
            }
        }
        // A strong steering command re-targets the path through the neighbour occupant's best path.
        const float reverseLimit = params->F(0x34);
        if (networkOccupant_ && (state->steering6c < -reverseLimit || reverseLimit < state->steering6c)) {
            if (cISC4PathInfo* pathInfo = networkOccupant_->GetPathInfoObject()) {
                std::uint8_t exit = kPathEdgeWildcard;
                GetEdgeFromVector(networkOccupant_, MakeVector(orientation_.x, orientation_.y, orientation_.z), exit, true);
                std::uint32_t key = 0;
                const tPath* path = pathInfo->GetBestPath(1, exit, static_cast<std::uint8_t>((pathKey_ >> 8) & 0xff),
                                                          pathPos_, &key, nullptr);
                if (path && &path->points != path_) {
                    cS3DVector3 snapped = pathPos_;
                    const int index = pathInfo->SnapPointToPath(snapped, &path->points);
                    if (std::fabs(snapped.y - pathPos_.y) < 4.0f) {
                        SetPath(networkOccupant_, key, &path->points, true);
                        pathIndex_ = static_cast<std::uint32_t>(index);
                        destination_ = snapped;
                    }
                }
            }
        }
    }
    if (driveEffect_) {
        float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, position_.x, position_.y, position_.z, 1};
        driveEffect_->SetTransform(matrix);
    }
    if (vehicleHost_) {
        const float disaster = vehicleHost_->PlayerDriveDisasterDamage(position_.x, position_.z);
        if (disaster > 0.0f) {
            state->damage70 += static_cast<std::int32_t>(std::ceil(disaster * 3.0f));
            UpdateAfterDamage(state, params);
        }
    }
    if (milliseconds != 0.0f) SetActualSpeed(Distance3(position_, startPosition) / milliseconds);
    return true;
}

}  // namespace sc4
