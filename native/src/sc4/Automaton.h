// cSC4Automaton — the base of every moving thing in the city (cars, pedestrians, trains, aircraft, boats): a cSC4Occupant
// that carries a position / heading / speed, an optional model instance, a stack of tAutomataState behaviour records and
// the controller handshake (a cISC4AutomataController picks the behaviour and is asked to tick it). 189 symbols at
// 0x1000e24f0..0x1006f3050; the primary interface vtable is 0x100a73b30 (136 slots) with the occupant view at +8, the
// serializable view at +0x10, the property holder at +0x18, the exemplar property holder at +0x40 and cIGZMessageTarget2
// at +0x68 (vtable 0x100a74230). Interfaces answered by QueryInterface: 1 and 0xa9b40f05 (cISC4Automaton, primary),
// 0x26bb767e (cISC4Occupant, +8), 0xe4fda3d4 (cIGZSerializable, +0x10), 0x452294aa (cIGZMessageTarget2, +0x68).
//
// Original layout (object size >= 0x308; sub-objects: +0 cISC4Automaton, +8 cSC4Occupant (0x60 bytes), +0x68 message
// target; the remaining fields are the automaton's own):
//   +0x70 u8 saved flag (Read sets it)      +0x74 position (cS3DVector3)      +0x80 destination (cS3DVector3)
//   +0x8c orientation (unit vector)         +0x98..+0xac six floats -10000 (history/limits; not serialized)
//   +0xb0 radius/length                     +0xb4 default speed (1/36)        +0xb8 automata flags (u32)
//   +0xbc dispatch info id (u32)            +0xc0 active sound instance id     +0xc4 priority (u8)
//   +0xc8 actual speed (m/s, written by DoAnimationTick)
//   +0xd0 attraction controller (cISC4AutomataController*, counted)  +0xd8/+0xdc attraction radius/distance (f32)
//   +0xe0 attraction vector (cS3DVector3)   +0xf0 leader automaton (counted)   +0xf8 std::vector<tAutomataState*>
//   +0x110 std::vector<cGZPersistResourceKey> model keys                 +0x128 auxiliary animated model (counted)
//   +0x130 cS3DModelProcessor (0x80)        +0x1b0 std::vector<cIS3DModelInstanceAnimated*> models (0x18)
//   +0x1c8 primary model key (12)           +0x1d4 model instance id (u32, duplicates the key's instance)
//   +0x1d8 cS3DModelProcessor (0x80)        +0x25c model bounding box (min xyz, max xyz: six floats to +0x274)
//   +0x278 std::list<cRZAutoRefCount<cIGZMessageTarget2>> callbacks (0x20)   +0x290 cSC4ModelRegEffects (0x28)
//   +0x2b8 TweenStruct speed (current, desired, duration, elapsed)           +0x2c8 TweenStruct opacity
//   +0x2d8 animation timer (f32, s)         +0x2dc automata group id (u32)   +0x2e0 std::vector<u8> active-sound data
//   +0x2f0 u32                             +0x2f8 active-sound loop handle (u32)  +0x300 u8 active sound enabled
//   +0x304 u32
//
// Behaviour state ids (the value the automaton stores in the state record and dispatches on):
//   1 idle-ish (remove), 2 removing, 4 wait in the controller's queue, 5 route destination reached, 6 dispatched,
//   7 idle tick, 0xa scripted tick, 0xb random walk, 0xc state entered, 0xd state exited, 0xe plummet, 0xf plop jump,
//   0x10 player drive, 0x11 bumped, 0x12 magnet/attraction, 0x13 attach animation, 0x14 collide, 0x15 pull over,
//   0x34 pursuit.
// Automata flags (+0xb8): 0x1 marked for remove, 0x2 self controlling, 0x4 destination reached, 0x8 collide candidate,
//   0x40 special route, 0x80 ignore the simulator speed modifier, 0x200000 force collision, 0x1000000 keep after
//   reaching the route destination, 0x2000000 player controlled, 0x8000000 disabled, 0x10000000 removed (fading);
//   0x2000/0x20000/0x40000/0x4000 and 0x400000/0x800000 are copied from the behaviour template (ignore paths/roads/
//   terrain/attractors, avoid lots/roads) by HandleEnterState.
//
// Seams: the model system (cS3DModelProcessor, cISC4ModelInstanceAnimated loading), the model-reg-effects object
// (cSC4ModelRegEffects), the renderer layers, the city / lot / network queries, the automata controller manager's
// attraction query, the game data registry, the sound manager and the simulator clock are unrecovered neighbours; they
// are reached through IAutomatonHost / IAutomatonModel / IAutomatonEffects below (the original call is in each comment).
// The controller itself is the recovered cSC4AutomataController; the automaton passes itself to it through the
// ControllerLink adapter (it answers IControllerAutomaton / IControllerLeader), which mirrors the original passing `this`.
//
// Quirks kept: SetState pops every completed state before pushing; MoveTo only checks the city bounds for NaN input;
// MoveTo and FinishMove are called even when nothing moved (FinishMove(falsy, falsy)); the anim timer snaps the speed
// tween only when it reaches its duration; PostInit calls GetGroupHierarchyDepth and discards the answer; the group id
// ends up being the largest occupant group; Remove(true) stops the effects without a fade while Remove(false) pushes
// state 2 and fades; StartSFX generates a new instance id on every call; the six -10000 floats are never read.
#pragma once
#include <cstdint>
#include <list>
#include <vector>

#include "gz/StreamOps.h"
#include "gz/cGZPersistResourceKey.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "rz/cRZAutoRefCount.h"
#include "rz/cRZString.h"
#include "s3d/AnimatedInstance.h"
#include "sc4/AutomataController.h"
#include "sc4/AutomataStates.h"
#include "sc4/Occupant.h"

namespace sc4 {

// ---- ids -------------------------------------------------------------------------------------------------------------
constexpr std::uint32_t kAutomatonOccupantType = 0x49d57875;         // occupant type id every automaton is created with
constexpr std::uint32_t kAutomatonMsgAutomaton = 0x09d92fc3;         // callback / "automaton removed" message
constexpr std::uint32_t kAutomatonPropertyActiveSound = 0x6c044bb5;  // occupant property: the active-sound data
constexpr std::uint32_t kAutomatonDefaultModelKey = 0;
constexpr float kAutomatonSpeedScale = 0.0002777778f;                // 1 / 3600 (the original's 0x3ce38e39)
constexpr float kAutomatonInitialSpeed = 1.0f / 36.0f;               // 0x3ce38e39
constexpr float kAutomatonUnsetCoordinate = -10000.0f;               // 0xc61c4000 (six fields at +0x98)
constexpr float kAutomatonCollisionSpeedScale = 1500.0f;             // AddToCollisionSystem

// Occupant flag bits the automaton uses (the occupant's own flags byte, +0x53 in cSC4Occupant / +0x5b here).
constexpr std::uint32_t kAutomatonOccupantInManager = 0x1;   // placed in the occupant manager
constexpr std::uint32_t kAutomatonOccupantInRenderer = 0x2;  // registered with the renderer

// Automata flags (+0xb8).
constexpr std::uint32_t kAutomataFlagMarkedForRemove = 0x1;
constexpr std::uint32_t kAutomataFlagSelfControlling = 0x2;
constexpr std::uint32_t kAutomataFlagReachedDestination = 0x4;
constexpr std::uint32_t kAutomataFlagCollideCandidate = 0x8;
constexpr std::uint32_t kAutomataFlagSpecialRoute = 0x40;
constexpr std::uint32_t kAutomataFlagIgnoreSpeedModifier = 0x80;
constexpr std::uint32_t kAutomataFlagIgnoreAttractors = 0x4000;    // behaviour template +0x5f
constexpr std::uint32_t kAutomataFlagIgnorePaths = 0x2000;         // behaviour template +0x5c
constexpr std::uint32_t kAutomataFlagIgnoreRoads = 0x20000;        // behaviour template +0x5d
constexpr std::uint32_t kAutomataFlagIgnoreTerrain = 0x40000;      // behaviour template +0x5e
constexpr std::uint32_t kAutomataFlagAvoidLots = 0x400000;         // behaviour template +0x60
constexpr std::uint32_t kAutomataFlagAvoidRoads = 0x800000;        // behaviour template +0x61
constexpr std::uint32_t kAutomataFlagNoCollision = 0x10000;
constexpr std::uint32_t kAutomataFlagForceCollision = 0x200000;
constexpr std::uint32_t kAutomataFlagKeepAfterRoute = 0x1000000;
constexpr std::uint32_t kAutomataFlagPlayerDrive = 0x2000000;
constexpr std::uint32_t kAutomataFlagDisabled = 0x8000000;
constexpr std::uint32_t kAutomataFlagRemoved = 0x10000000;

// State ids.
constexpr std::uint32_t kStateIdle = 1, kStateRemoving = 2, kStateQueue = 4, kStateRouteReached = 5, kStateDispatched = 6,
                        kStateIdleTick = 7, kStateScripted = 0xa, kStateRandomWalk = 0xb, kStateEntered = 0xc,
                        kStateExited = 0xd, kStatePlummet = 0xe, kStatePlopJump = 0xf, kStatePlayerDrive = 0x10,
                        kStateBumped = 0x11, kStateMagnet = 0x12, kStateAttachAnim = 0x13, kStateCollide = 0x14,
                        kStatePullOver = 0x15, kStatePursuit = 0x34;

constexpr std::uint32_t kAuxiliaryModelGhost = 0;   // cSC4Automaton::AuxiliaryModelSpecifier (LoadAuxiliaryModel)
// The exemplar properties (resource keys) LoadAuxiliaryModel reads for specifiers 1, 2 and 3.
constexpr std::uint32_t kAutomatonPropertyHeadlightModel = 0x09c80417, kAutomatonPropertyEmergencyLightModel = 0xc9e69af6,
                        kAutomatonPropertyTrailerModel = 0xec214180;

// The events SendCallbackMessage posts (cISC4Automaton::tAutomataEvent).
enum tAutomataEvent : std::uint32_t {
    kAutomataEventTick = 0,
    kAutomataEventAnimationDone = 1,
    kAutomataEventRemoving = 2,
    kAutomataEventDestinationReached = 4,
    kAutomataEventRouteReached = 5,
    kAutomataEventDispatched = 6,
    kAutomataEventMoveFinished = 9,
    kAutomataEventStateEntered = 0xc,
    kAutomataEventStateExited = 0xd,
    kAutomataEventPlayerDriveStart = 0xe,
    kAutomataEventPlayerDriveStop = 0xf,
};

// A network collision mask used by the movement step (the network manager's 0xc49 "walkable" mask).
constexpr std::uint32_t kAutomataMoveNetworkMask = 0xc49;

// TweenStruct (+0x2b8 speed, +0x2c8 opacity; 0x10 bytes): a value that eases from `current` to `desired` over
// `duration` seconds, `elapsed` seconds after the last SetDesired. 0x1000f1728 SetDesired, 0x1001065b2 Write.
struct AutomatonTween {
    float current = 0.0f;    // +0x00
    float desired = 0.0f;    // +0x04
    float duration = 0.0f;   // +0x08
    float elapsed = 0.0f;    // +0x0c

    void SetDesired(float value, float seconds);   // @ 0x1000f1728
    // Elapsed/duration clamped to 0..1 (0 when the ratio is negative, the original's and-mask idiom).
    float Ratio() const;
    void Write(cIGZOStream& stream) const;         // @ 0x1001065b2
    void Read(cIGZIStream& stream);
};

class cSC4Automaton;

// AutomataOBB (+0x00..+0x20): an oriented bounding box used by the collision tests. 3 functions at
// 0x1000fd61c..0x1000fd898. +0 valid, +4/+8 centre (x/z), +0xc/+0x10 half size, +0x1c/+0x20 direction (x/z).
struct AutomataOBB {
    bool valid = false;       // +0x00
    float centerX = 0.0f;     // +0x04
    float centerZ = 0.0f;     // +0x08
    float halfX = 0.0f;       // +0x0c
    float halfZ = 0.0f;       // +0x10
    float pad20 = 0.0f;       // +0x14..+0x1b unused
    float dirX = 1.0f;        // +0x1c
    float dirZ = 0.0f;        // +0x20

    AutomataOBB() = default;
    // The box of `automaton`'s first model instance, scaled by `scale` (1 = the model bounds, 2 when the automaton
    // answers interface 0x4a416254). @ 0x1000fd61c
    AutomataOBB(const cSC4Automaton& automaton, const cS3DVector3& position, float scale);
    bool TestForCollision(const AutomataOBB& other) const;   // @ 0x1000fd898
};

// ---- seams -----------------------------------------------------------------------------------------------------------
class cSC4Automaton;

// One model instance of the automaton (the original's cIS3DModelInstanceAnimated; the cIS3DModelInstance base sits at
// +0xb0 of the animated object, hence the -0xb0 adjustor in the original).
class IAutomatonModel {
public:
    virtual ~IAutomatonModel() = default;
    virtual void AddRef() = 0;
    virtual void Release() = 0;
    virtual void GetBounds(float* minXyz, float* maxXyz) = 0;       // cIS3DModelInstance slot 0x38
    virtual void GetTransform(cS3DTransform& out) = 0;              // slot 0x48
    virtual void SetTransform(const cS3DTransform& transform) = 0;  // slot 0x50
    virtual void SetOpacity(float opacity, bool translucent) = 0;   // slot 0x128
    virtual void SetAutoFrameRate(float rate) {}                    // slot 0x108 (cS3DModelInstanceAnimated; pedestrian-only)
    virtual void UpdateAnimation(float seconds) = 0;                // slot 0x110 (cS3DModelProcessor::Update)
    virtual int GetFrame() const = 0;                               // slot 0xf0
    virtual int GetFrameCount() const = 0;                          // slot 0xf8
    virtual void SetZoomAndRotation(int zoom, int rotation) = 0;    // slot 0xd8
    virtual void SetPlayMode(s3d::PlayMode mode) = 0;
};

// The cSC4ModelRegEffects object embedded at +0x290 (unrecovered neighbour).
class IAutomatonEffects {
public:
    virtual ~IAutomatonEffects() = default;
    virtual void SetModel(IAutomatonModel* model) = 0;                              // @ 0x1001e3e26
    virtual bool Init() = 0;                                                        // @ 0x1001e3d42
    virtual void Update(const cS3DVector3& position, const cS3DVector3& direction, float dt) = 0;   // @ 0x1001e455a
    virtual void StartAll() = 0;                                                    // @ 0x1001e4924
    virtual void StopAll(bool fade) = 0;                                            // @ 0x1001e496c
    virtual void StartNamed(const cRZString& effect, int transition, bool loop) = 0;   // @ 0x1001e49f0
    virtual void StopNamed(const cRZString& effect, int transition) = 0;            // @ 0x1001e4af4
    virtual void ToggleNamed(const cRZString& effect, int transition, bool on) = 0;  // @ 0x1001e4bb2
    virtual void RecreateNamed(const cRZString& effect, int transition, bool on) = 0;   // @ 0x1001e4cd2
    virtual bool AddRegEffect(const cRZString& effect, const cRZString& point) = 0; // @ 0x1001e4e36
    virtual void* FindRegEffect(const cRZString& effect) = 0;                       // @ 0x1001e4f0e
    virtual void RemoveRegEffect(const cRZString& effect) = 0;                      // @ 0x1001e4f4c
    virtual void Shutdown() = 0;                                                    // @ 0x1001e3d58
    // cSC4ModelRegEffects::Update() @ 0x1001e3e84: re-place the registered effects at the model's transform. The
    // default does nothing (hosts without per-model effect placement).
    virtual void UpdatePlacement() {}
    // cSC4ModelRegEffects::StartNamedEffects' result (true when a registered effect of that name, any case, started).
    // The default starts through StartNamed and approximates the result with FindRegEffect.
    virtual bool StartNamedEffects(const cRZString& effect, int transition, bool loop) {
        StartNamed(effect, transition, loop);
        return FindRegEffect(effect) != nullptr;
    }
};

// The occupant group template of the automata script system (cSC4OccupantGroupTemplate); only the fields
// SetAutomataGroup / PostInit read.
class IAutomatonGroupTemplate {
public:
    virtual ~IAutomatonGroupTemplate() = default;
    virtual std::uint32_t GroupId() const = 0;            // +0x20
    virtual IAutomatonGroupTemplate* Parent() const = 0;   // +0x18
    virtual float SpeedMin() const = 0;                   // +0xd8
    virtual float SpeedMax() const = 0;                   // +0xdc
    virtual void GetGroupHierarchyDepth() = 0;            // cSC4OccupantGroupTemplate::GetGroupHierarchyDepth
    // +0x58 (i386): the groups of the cars a vehicle of this group pulls ("trailers"), in order; null without a list.
    virtual const std::list<std::uint32_t>* TrailerGroupIds() const { return nullptr; }
};

class cSC4AutomataController;

// Port seam: an object another layer attaches to an automaton (the automata managers' IAutomataAutomaton view,
// app/NativeAutomataViews.cpp); the automaton owns it and deletes it with itself.
class cSC4AutomatonExtension {
public:
    virtual ~cSC4AutomatonExtension() = default;
};

class IAutomatonHost {
public:
    virtual ~IAutomatonHost() = default;
    // ---- models and effects (cS3DModelProcessor, cSC4ModelRegEffects — unrecovered)
    virtual IAutomatonModel* CreateModel(const cGZPersistResourceKey& key) = 0;   // cS3DModelProcessor::AddModelInstance
    virtual void DestroyModel(IAutomatonModel* model) = 0;                        // the model's Release
    virtual IAutomatonEffects* CreateEffects() = 0;                               // cSC4ModelRegEffects ctor
    virtual void DestroyEffects(IAutomatonEffects* effects) = 0;                  // ~cSC4ModelRegEffects
    virtual bool AddModelToRenderer(IAutomatonModel* model) = 0;                  // SL::spRenderer slot 0xe0
    virtual bool RemoveModelFromRenderer(IAutomatonModel* model) = 0;             // slot 0xe8
    virtual void FadeModelIn(IAutomatonModel* model) = 0;                         // slot 0xf0
    virtual void FadeModelOut(IAutomatonModel* model, int length) = 0;            // slot 0xf8
    // ---- occupant manager (SL::spOccupantManager)
    virtual bool InsertOccupant(cISC4Occupant* occupant) = 0;                     // slot 0xa0
    virtual void RemoveOccupant(cISC4Occupant* occupant) = 0;                     // slot 0xa8
    virtual bool RefreshOccupant(cISC4Occupant* occupant) = 0;                    // slot 0xc0
    // ---- city (SL::spCity, the lot and network managers)
    virtual bool HasCity() = 0;
    virtual float CitySizeX() = 0;                                                // city slot 0x2c8
    virtual float CitySizeZ() = 0;                                                // city slot 0x2d0
    virtual float TerrainHeight(float x, float z) = 0;                            // network manager slot 0x90
    // The traffic network map (city slot 0xa0) as the random walk uses it: slot 0xb0 ConstrainVectorToNetworkTile keeps a
    // step from `from` to `point` on its network tile, slot 0x48 GetRoadAltitude is the road (or ground) height at a point.
    virtual bool ConstrainVectorToNetworkTile(const cS3DVector3& from, cS3DVector3& point, std::uint32_t flags) { (void)from; (void)point; (void)flags; return false; }
    virtual float RoadAltitude(float x, float z) { return TerrainHeight(x, z); }
    virtual bool NetworkAt(int cellX, int cellZ) = 0;                             // network manager slot 0x70
    virtual bool LotAt(int cellX, int cellZ) = 0;                                 // lot manager slot 0x28
    virtual void PlaceOnNetwork(const cS3DVector3& from, const cS3DVector3& to, std::uint32_t mask) = 0;   // slot 0x160
    // ---- automata controller manager (city slot 0x178)
    virtual cSC4AutomataController* FindAttraction(cISC4Occupant* occupant, cS3DVector3* attraction, float* radius,
                                                   float* distance) = 0;          // controller manager slot 0xb0
    virtual IAutomatonGroupTemplate* FindAutomataGroup(std::uint32_t id) = 0;      // script system slot 0x38
    // ---- simulator / messages / sound / random
    virtual bool AnimationUpdateAllowed() = 0;   // spSimulator 0x88 || !spSimulator 0x78
    virtual float SimSpeedModifier() = 0;        // cSC4AutomataManager::sfSimSpeedModifier
    virtual float MaxSpeedUnits() = 0;           // the int global 0x100b21888 (units per hour)
    virtual float FadeDurationSeconds() = 0;     // the float global 0x100b219b4
    virtual void PostMessage(cIGZMessage2* message) = 0;   // SL::spMS2 slot 0x20
    virtual std::uint32_t GenerateSoundInstanceId() = 0;   // cSC4AudioSoundManager::GenerateNewInstanceId
    virtual void StopSoundLoop(std::uint32_t handle) = 0;  // SL::SS slot 0x50, message 0x6a451908
    virtual void PostAudioEvent(std::uint32_t event) = 0;  // SL::AEMS slots 0x30/0x28
    virtual std::uint32_t RandomBelow(std::uint32_t range) = 0;               // cRZRandomSuper::RandomUint32Uniform(range)
    virtual std::int32_t RandomInt(std::int32_t lo, std::int32_t hi) = 0;     // RandomSint32RangeUniform
    virtual double RandomDouble(double lo, double hi) = 0;                    // RandomDoubleRangeUniform
    virtual double RandomUnit() = 0;                                          // RandomDoubleUniform
};

// The process-wide host (installed by the game / the test). Null means "no services": the automaton degrades to the
// no-city, no-model behaviour the original shows when its globals are null.
IAutomatonHost* AutomatonHost();
void SetAutomatonHost(IAutomatonHost* host);

class cSC4Automaton : public cSC4Occupant, public cIGZMessageTarget2, public IStateAutomaton {
public:
    explicit cSC4Automaton(IAutomatonHost* host = nullptr, const cGZPersistResourceKey& modelKey = {});
    ~cSC4Automaton() override;

    // ---- cIGZUnknown via cSC4Occupant
    bool QueryInterface(std::uint32_t iid, void** out) override;      // @ 0x10010ded6
    std::uint32_t AddRef() override { return cSC4Occupant::AddRef(); }   // @ 0x10010df6e
    std::uint32_t Release() override;                                 // @ 0x10010dfc2

    // ---- cISC4Automaton, in the original vtable order (slot offsets in the comments)
    bool Init() override;                                             // 0x18  @ 0x100105ff8
    bool Shutdown() override;                                         // 0x20  @ 0x1001060ce
    cISC4Occupant* AsOccupant() const { return const_cast<cSC4Automaton*>(this); }   // 0x28  @ 0x10010ec20
    cISC4Occupant* AsOccupant() { return this; }                      // 0x30  @ 0x100502370
    void GetPosition(float* xyz) override;                            // 0x38  @ 0x100106e62
    void SetPosition(const float* xyz) override;                      // 0x40  @ 0x100106e92
    void SetVisible(bool visible);                                    // 0x48  @ 0x100106bda
    virtual void SetDestination(const float* xyz);                            // 0x50  @ 0x100107128
    void GetDestination(float* xyz) const;                            // 0x58  @ 0x100107108
    bool GetTransform(cS3DTransform& transform) const;                // 0x60  @ 0x10010aa34
    void Remove(bool fade);                                           // 0x68  @0x10010a5a2
    void SetOrientation(const float* xyz);                            // 0x70  @ 0x100106f24
    bool GetOrientation(float* xyz) const;                            // 0x78  @ 0x1001070e6
    virtual void MoveTo(const float* position, const float* direction);        // 0x80  @ 0x100107572
    virtual float GetRadius() const { return radius_; }                       // 0x88  @ 0x1005070c0
    bool GetDisabled() const;                                         // 0x90  @ 0x1002f8090
    bool PlayAnim(const cGZPersistResourceKey& key);                  // 0x98  @ 0x10010ab32
    bool PlayAnim(std::uint32_t animId);                              // 0xa0  @ 0x10010ae6a
    bool PlayAnimByName(const cRZString& name);                       // 0xa8  @ 0x10010aeba
    bool AttachAnim(const cGZPersistResourceKey& key, s3d::PlayMode mode, bool flag, float rate);   // 0xb0
    virtual std::uint16_t GetCallNumber() const { return 0; }                 // 0xb8  @ 0x1000e24f0
    virtual void SetCallNumber(std::uint16_t) {}                              // 0xc0  @ 0x10010ec30
    virtual void SetDispatchState(std::uint32_t) {}                           // 0xc8  @ 0x1006d6b20
    virtual std::uint32_t GetDispatchState() const { return 3; }              // 0xd0  @ 0x1004fdee0
    std::uint32_t GetDispatchInfoID() const { return dispatchInfoId_; }   // 0xd8  @ 0x1000e2500
    void SetDispatchInfoID(std::uint32_t id) { dispatchInfoId_ = id; }    // 0xe0  @ 0x1000f3c10
    virtual void ReturnToSourceOccupant() {}                                  // 0xe8  @ 0x10010ec40
    void SetAutomataFlag(std::uint32_t flag, bool on);                // 0xf0  @ 0x1006d6b30
    bool CheckAutomataFlag(std::uint32_t flag) const;                 // 0xf8  @ 0x1006d6b50
    float GetOpacity() const;                                         // 0x100 @ 0x1006d6b60
    void SetOpacity(float opacity, float seconds);                    // 0x108 @ 0x10010a650
    float GetCurrentSpeed() const;                                    // 0x110 @ 0x100502388
    float GetDefaultSpeed() const { return defaultSpeed_; }           // 0x118 @ 0x1006f1ee0
    void SetSpeed(float speed, float seconds);                        // 0x120 @ 0x1004fdef0
    float GetMaxSpeed() const;                                        // 0x128 @ 0x10010bb88
    float GetActualSpeed() const { return actualSpeed_; }             // 0x130 @ 0x1005023f0
    void AddCallback(cIGZMessageTarget2* target);                     // 0x138 @ 0x10010be1c
    void RemoveCallback(cIGZMessageTarget2* target);                  // 0x140 @ 0x10010beb2
    void AddRegEffect(const cRZString& effect, const cRZString& point);   // 0x148 @ 0x10010b97e
    void FindRegEffect(const cRZString& effect);                      // 0x150 @ 0x10010b9a4
    void RemoveRegEffect(const cRZString& effect);                    // 0x158 @ 0x10010b9b6
    void StartNamedEffects(const cRZString& effect, int transition, bool loop);   // 0x160
    void StopNamedEffects(const cRZString& effect, int transition);   // 0x168 @ 0x10010ec50
    void ToggleNamedEffects(const cRZString& effect, int transition, bool on);    // 0x170
    void RecreateNamedEffects(const cRZString& effect, int transition, bool on);  // 0x178
    cSC4Automaton* GetLeader() const;                                 // 0x180 @ 0x10011b710
    void SetLeader(cSC4Automaton* leader);                            // 0x188 @ 0x100106b86
    cSC4Automaton* FindDrivingAutomaton() { return this; }            // 0x190 @ 0x100107cd8
    bool SetState(std::uint32_t state, cSC4AutomataController* controller, float timer, bool flag, std::uint32_t animOffset,
                  std::uint32_t sfx, const tBehaviorTemplate* behavior);   // 0x198 @ 0x10010cda2
    bool PushState(std::uint32_t state, cSC4AutomataController* controller, float timer, bool flag, std::uint32_t animOffset,
                   std::uint32_t sfx, const tBehaviorTemplate* behavior);  // 0x1a0 @ 0x10010ce42
    bool PopState();                                                  // 0x1a8 @ 0x10010cfb2
    void SetBehavior(const tBehaviorTemplate* behavior, cSC4AutomataController* controller);   // 0x1b0
    bool PushBehavior(const tBehaviorTemplate* behavior, cSC4AutomataController* controller);  // 0x1b8
    tAutomataState* GetState();                                       // 0x1c0 @ 0x10010d088
    tAutomataState* FindState(std::uint32_t state) const;             // 0x1c8 @ 0x10010d0a6
    tAutomataState* PrevState(const tAutomataState* state) const;     // 0x1d0 @ 0x10010d0d4
    std::uint32_t GetBehavior() const;                                // 0x1d8 @ 0x10010d0f8
    virtual bool HandlePlayerDriveCommand(std::uint32_t command, bool on);    // 0x1e0 @ 0x1001096e0
    bool StartSFX(std::uint32_t sfx);                                 // 0x1e8 @ 0x10010d250
    bool StopSFX(std::uint32_t sfx);                                  // 0x1f0 @ 0x10010d336
    std::uint8_t GetPriority() const { return priority_; }            // 0x1f8 @ 0x1006d6bc0
    void SetPriority(std::uint8_t priority) { priority_ = priority; }  // 0x200 @ 0x1000e2540
    virtual bool HandleCollision(cSC4Automaton* other, const cS3DVector3& point);   // 0x208 @ 0x10010d400
    void GetCollidePosition(cS3DVector3& out) const;                  // 0x210 @ 0x10010ba66
    IAutomatonGroupTemplate* GetAutomataGroup() const;                 // 0x218 @ 0x10010de86
    void RegisterInGameDataRegistry();                                // 0x220 @ 0x10010c00a
    void UnregisterInGameDataRegistry();                              // 0x228 @ 0x10010cc76
    void GetBoundingBox(float* minXyz, float* maxXyz) override;       // 0x258 @ 0x100107148
    void StopSound(std::uint32_t handle) override { StopSFX(handle); }   // IStateAutomaton (tAutomataState::Shutdown)
    bool DoMessage(cIGZMessage2* message) override;                   // 0x260 @ 0x100106ae4
    bool PlaceInOccupantManager(bool add);                            // 0x268 @ 0x100107216
    bool PlaceInRenderer(bool add);                                   // 0x270 @ 0x100107370
    virtual bool DoAnimationTick(std::uint32_t unknown, float milliseconds);   // 0x278 @ 0x100107ce2
    virtual bool ShouldCollide(cSC4Automaton* other) const;                   // 0x280 @ 0x10010b9c8
    void SetZoomAndRotation(int zoom, int rotation);                  // 0x288 @ 0x10010bba2
    void SetTransform(const cS3DTransform& transform);                // 0x290 @ 0x10010aa68
    std::uint32_t GetAnimOffsetByName(const cRZString& name) const;   // 0x298 @ 0x10010aee2
    void EnableActiveSound(bool on);                                  // 0x2a0 @ 0x10010ea18
    void UpdateActiveSound(float milliseconds);                       // 0x2a8 @ 0x10010ea78
    virtual void HandleReachedRouteDestination();                             // 0x2b0 @ 0x10010a436
    virtual void HandleDispatched(const cS3DVector3& position, cISC4Occupant* occupant);   // 0x2b8 @ 0x10010a4ac
    bool HandleRemoveController(cSC4AutomataController* controller);  // 0x2c0 @ 0x10010a4c4
    IAutomatonModel* GetModelInstance() const;                        // 0x2c8 @ 0x100506820
    void UpdateOpacity(float milliseconds);                           // 0x2d0 @ 0x10010a6e6
    void SetSpeedFromBehaviorTemplate(const tBehaviorTemplate* behavior);   // 0x2d8 @ 0x10010a0c0
    bool GetMarkedForRemove() const;                                  // 0x2e0 @ 0x1000e25b0
    bool GetSelfControlling() const;                                  // 0x2e8 @ 0x100502450
    bool GetReachedDestination() const;                               // 0x2f0 @ 0x1000f3c80
    bool GetSpecialRouteFlag() const;                                 // 0x2f8 @ 0x100506840
    void SetMarkedForRemove(bool on);                                 // 0x300 @ 0x1002f80e0
    void SetSpecialRouteFlag(bool on);                                // 0x308 @ 0x100506860
    void SetSelfControlling(bool on);                                 // 0x310 @ 0x1000f3ca0
    const char* GetDebugFlagsText() const;                            // 0x318 @ 0x10010d408
    virtual const char* GetDebugExtraText() const;                            // 0x320 @ 0x10010d8bc
    tAutomataState* CreateAutomataState(std::uint32_t state);         // 0x328 @ 0x10010d96a
    virtual void SetAutomataGroup(IAutomatonGroupTemplate* group);     // 0x330 @ 0x10010dcac
    cGZPersistResourceKey GetModelResourceKey(int index) const;       // 0x338 @ 0x1001071bc
    bool LoadModels();                                                // 0x340 @ 0x10010b4e6
    bool UnloadModels();                                              // 0x348 @ 0x10010b81a
    bool LoadAuxiliaryModel(std::uint32_t specifier, IAutomatonModel** out);   // 0x350 @ 0x10010b8b6
    void FinishMove(bool fadeIn, bool fadeOut);                       // 0x358 @ 0x100107b2a
    bool PostInit();                                                  // 0x360 @ 0x100108016
    void SendCallbackMessage(std::uint32_t event, std::uint32_t data);   // 0x368 @ 0x10010bf22
    virtual void UpdateAnimation(float milliseconds);                         // 0x370 @ 0x100108136
    void UpdateEffects();                                             // 0x378 @ 0x10010b956
    virtual void UpdateAttraction();                                          // 0x380 @ 0x10010bbfa
    void InitLighting();                                              // 0x388 @ 0x10010a9b4
    void UpdateLighting();                                            // 0x390 @ 0x10010a858
    void ClearStateStack();                                           // 0x398 @ 0x10010cd3e
    virtual bool DoStateTick(float milliseconds);                             // 0x3a0 @ 0x100108230
    virtual bool HandleEnterState(tAutomataState* state);                     // 0x3a8 @ 0x1001098ae
    virtual bool HandleExitState(const tAutomataState* state);                // 0x3b0 @ 0x10010a172
    bool DoIdleTick(float milliseconds);                              // 0x3b8 @ 0x1001083dc
    bool DoRandomWalkTick(float milliseconds);                        // 0x3c0 @ 0x10010840c
    bool DoPlopJumpTick(float milliseconds);                          // 0x3c8 @ 0x1001089f2
    virtual bool DoPlayerDriveTick(float milliseconds);               // 0x3d0 @ 0x100108a8e
    virtual bool DoBumpedTick(float milliseconds);                    // 0x3d8 @ 0x1001092f8
    bool DoMagnetTick(float milliseconds);                            // 0x3e0 @ 0x1001093ce
    bool DoScriptedTick(float milliseconds);                          // 0x3e8 @ 0x1001095a4
    bool DoAttachAnimTick(float milliseconds);                        // 0x3f0 @ 0x1001095ba
    virtual bool DoCollideTick(float milliseconds);                   // 0x3f8 @ 0x10010930e
    virtual bool TranslatePlayerDriveCommands(tAutomataStatePlayerDrive* state, float milliseconds);   // 0x400
    bool GetChaseVector(cS3DVector3& out) const;                      // 0x408 @ 0x10010bada
    virtual cSC4Automaton* GetCollidingAutomaton(float distance) { (void)distance; return nullptr; }   // 0x410
    virtual void RecalculateLeaders() {}                              // 0x418 @ 0x10010bad4
    virtual float GetSpeedModifier() const;                           // 0x420 @ 0x10010bb54
    bool InitGhostModel();                                            // 0x428 @ 0x100108b64
    bool ShutdownGhostModel();                                        // 0x430 @ 0x100108ee0
    bool UpdateGhostModel();                                          // 0x438 @ 0x100108f8c

    // ---- cIGZSerializable
    bool Write(cIGZOStream& stream) const override;                   // @ 0x100106266
    bool Read(cIGZIStream& stream) override;                          // @ 0x10010662c
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4Occupant; }

    // ---- accessors for the tests and the neighbours
    bool AddModelResourceKey(const cGZPersistResourceKey& key);       // @ 0x100100a94
    void InitEffectsList();                                           // @ 0x10010604c
    void StartAllEffects();                                           // @ 0x100106c5e
    void StopKeyEffects();                                            // @ 0x10010a3b2
    void InitKeyEffects();                                            // @ 0x10010dd56
    IAutomatonModel* AddModelInstance(const cGZPersistResourceKey& key);   // @ 0x10010b052
    bool RemoveModelInstance(IAutomatonModel* model);                 // @ 0x10010a25c
    bool AddToCollisionSystem(tAutomataStateCollide* state);          // @ 0x100109d28
    void PositionModelInstance(IAutomatonModel* model);               // @ 0x100107006
    IAutomatonHost* Host() const { return host_; }
    IAutomatonEffects* Effects() const { return effects_; }
    const std::vector<IAutomatonModel*>& Models() const { return models_; }
    bool IsSavedFlagSet() const { return saved_; }
    bool IsActiveSoundEnabled() const { return activeSoundEnabled_; }
    void SetSavedFlag(bool on) { saved_ = on; }
    std::uint32_t AutomataFlags() const { return automataFlags_; }
    std::uint32_t AutomataGroupId() const { return automataGroupId_; }
    float AnimationTimer() const { return animTimer_; }
    float Radius() const { return radius_; }
    const cS3DVector3& Position() const { return position_; }
    const cS3DVector3& Orientation() const { return orientation_; }
    const cS3DVector3& Destination() const { return destination_; }
    const cGZPersistResourceKey& PrimaryModelKey() const { return primaryKey_; }
    std::uint32_t PrimaryModelInstanceId() const { return primaryInstanceId_; }
    const std::vector<cGZPersistResourceKey>& ModelKeys() const { return modelKeys_; }
    std::list<cRZAutoRefCount<cIGZMessageTarget2>>& Callbacks() { return callbacks_; }
    const std::list<cRZAutoRefCount<cIGZMessageTarget2>>& Callbacks() const { return callbacks_; }
    std::vector<tAutomataState*>& StateStack() { return states_; }
    AutomatonTween& SpeedTween() { return speedTween_; }
    AutomatonTween& OpacityTween() { return opacityTween_; }
    void SetRadius(float radius) { radius_ = radius; }
    void SetAnimationTimer(float t) { animTimer_ = t; }
    void SetActualSpeed(float speed) { actualSpeed_ = speed; }
    void SetDefaultSpeed(float speed) { defaultSpeed_ = speed; }
    // Port seam: the object another layer attached (the automata managers' view); the automaton deletes it with itself.
    cSC4AutomatonExtension* Extension() const { return extension_; }
    void SetExtension(cSC4AutomatonExtension* extension) { extension_ = extension; }   // takes ownership

protected:
    // The adapter the automaton hands to cSC4AutomataController (the original passes `this`).
    class ControllerLink;
    friend class ControllerLink;

    // What the recovered cSC4AutomataController calls on the automaton (its IControllerAutomaton seam):
    // 0x68 Remove(false), 0x50 SetDestination, 0x180 GetLeader, 0x188 SetLeader.
public:
    IControllerAutomaton* ControllerLinkFor();
    static cSC4Automaton* OfControllerLink(IControllerAutomaton* link);   // the automaton behind a link ControllerLinkFor made
protected:
    IAutomatonEffects* EnsureEffects();
    bool StateIsFinished(const tAutomataState* state) const;

    IAutomatonHost* host_ = nullptr;   // port seam (not in the original)
    IAutomatonEffects* effects_ = nullptr;

    bool saved_ = false;                                         // +0x70
    cS3DVector3 position_;                                       // +0x74
    cS3DVector3 destination_;                                    // +0x80
    cS3DVector3 orientation_{1.0f, 0.0f, 0.0f};                  // +0x8c
    float unset_[6] = {kAutomatonUnsetCoordinate, kAutomatonUnsetCoordinate, kAutomatonUnsetCoordinate,
                       kAutomatonUnsetCoordinate, kAutomatonUnsetCoordinate, kAutomatonUnsetCoordinate};   // +0x98
    float radius_ = 0.0f;                                        // +0xb0
    float defaultSpeed_ = kAutomatonInitialSpeed;                // +0xb4
    std::uint32_t automataFlags_ = 0;                            // +0xb8
    std::uint32_t dispatchInfoId_ = 0;                           // +0xbc
    std::uint32_t soundInstanceId_ = 0;                          // +0xc0
    std::uint8_t priority_ = 0;                                  // +0xc4
    float actualSpeed_ = 0.0f;                                   // +0xc8
    cSC4AutomataController* attractionController_ = nullptr;     // +0xd0
    float attractionDistance_ = 0.0f;                              // +0xd8 (how far the strongest attractor is)
    float attractionStrength_ = 0.0f;                            // +0xdc (its pull; negative repels)
    cS3DVector3 attraction_{0.0f, 0.0f, 0.0f};                   // +0xe0
    cSC4Automaton* leader_ = nullptr;                            // +0xf0
    std::vector<tAutomataState*> states_;                        // +0xf8
    std::vector<cGZPersistResourceKey> modelKeys_;               // +0x110
    IAutomatonModel* auxiliaryModel_ = nullptr;                  // +0x128
    std::vector<IAutomatonModel*> models_;                       // +0x1b0
    cGZPersistResourceKey primaryKey_;                           // +0x1c8
    std::uint32_t primaryInstanceId_ = 0;                        // +0x1d4
    float boundingBox_[6] = {1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f};   // +0x25c
    std::list<cRZAutoRefCount<cIGZMessageTarget2>> callbacks_;   // +0x278
    AutomatonTween speedTween_;                                  // +0x2b8
    AutomatonTween opacityTween_;                                // +0x2c8
    float animTimer_ = 0.0f;                                     // +0x2d8
    std::uint32_t automataGroupId_ = 0;                          // +0x2dc
    std::vector<std::uint8_t> activeSoundData_;                  // +0x2e0
    std::uint32_t field2f0_ = 0;                                 // +0x2f0
    std::uint32_t activeSoundHandle_ = 0;                        // +0x2f8
    bool activeSoundEnabled_ = false;                            // +0x300
    std::uint32_t field304_ = 0;                                 // +0x304
    ControllerLink* controllerLink_ = nullptr;                    // port seam: the IControllerAutomaton adapter
    cSC4AutomatonExtension* extension_ = nullptr;                 // port seam: the automata managers' view (owned)
};

}  // namespace sc4
