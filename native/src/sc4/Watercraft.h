// cSC4Watercraft — the boat/ship automaton (64 symbols at 0x1004ef26a..0x1004fe50e; primary vtable 0x100aba010).
// A cSC4Automaton that drives on the water network, ferries between two water stations, tows other boats, runs the
// crash (sink / run aground) sequences and can be player driven. It talks to cSC4WatercraftManager (collision, docking
// permission, counting), cSC4CompressedRouteReader (the compressed route of a neighbour boat) and the named-animation
// registry; those neighbours are unrecovered and are reached through IWatercraftHost below.
//
// Original layout (object size 0xf88; cSC4Automaton is 0x308 of it):
//   +0x308 FerryStationInformation station A (0x48)      +0x350 FerryStationInformation station B (0x48)
//   +0x398 FerryStationInformation current station (0x48)
//   +0x3e0 cRZAutoRefCount<cSC4Watercraft> tow target    +0x3e8 cRZAutoRefCount<cSC4Watercraft> tower (who tows me)
//   +0x3f0 u8 docking flag             +0x3f1 u8 queueing flag
//   +0x3f8 cSC4CompressedRouteReader current route (0x20)  +0x418 std::vector<TravelPoint> lookahead (0x18; growth 0xb)
//   +0x430 u8 docking active           +0x431 u8 turning   +0x432 u8 crash-marked (collision)
//   +0x434 float tow-wait threshold    +0x438 float tow-wait elapsed
//   +0x43c float idle timer (60000 = despawn)              +0x440 float crash timer
//   +0x444 float crash progress        +0x448 u8 crash roll sign   +0x449 u8 crash pitch sign
//   +0x44a u8 crash roll enabled       +0x44b u8 following flag    +0x44c u8 animation hold
//   +0x450 float still timer           +0x454 float reverse timer +0x458 u8 tow-search flag
//   +0x45c float speed (signed, drive units)               +0x460 float turn (signed)
//   +0x464 u32                         +0x468 previous position (cS3DVector3)
//   +0x474 current simulation position (cS3DVector3)       +0x480 previous orientation
//   +0x48c current simulation orientation                  +0x498 cRZAutoRefCount<cISC4Occupant> neighbour occupant
//   +0x4a0 std::vector<u8> neighbour route                 +0x4b8 cSC4CompressedRouteReader neighbour route reader
//   +0x4d8 u8 neighbour route set
//   +0x4e0 WatercraftDriveParameters (40 floats, to +0x580)  +0x580 cRZRandomSuper (0x9d0 bytes)
//   +0xf50 u32 player-drive steering mode                  +0xf58..+0xf80 six cRZAutoRefCount<named animation>
//
// Behaviour state ids used only by the watercraft: 0x46 run aground, 0x47 crash/roll, 0x48 remove immediately,
// 0x49 remove with fade, 0x4a docking, 0x4b queueing for a dock, 0x4c waiting, 0x4d following (towed), 0x4e undocking.
// Occupant groups: 0x4409 player drive, 0x440a/0x440f/0x4411 neighbour travel boats, 0x4410 towable, 0x4413/0x4414
// boat size medium/large, 0x4417 towable target.
//
// Quirks kept: SetStateInternal(4) sets the crash-marked byte before falling through to state 3; the ferry's
// "other station" picks station B when A and B are at the same place (it never compares against the current station);
// DoStateTick's idle-timeout path falls through into the crash-sequence handler and adds the frame twice to the
// crash timer; the collision response scales the boat's own speed by 0x554 (0.75) after regressing the position;
// LoadDriveParameters treats 0.0 in the exemplar array as "use the default"; Assist_DirLookup refuses the last two
// lookahead points; OutOfBounds reports "inside" only when all four bounding-box corners are city cells.
//
// Seams: the watercraft manager, the city/terrain/water queries, the route reader and the named animations are
// unrecovered neighbours. Each IWatercraftHost method carries the original vslot/global it replaces.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "rz/cRZAutoRefCount.h"
#include "rz/cRZRandomSuper.h"
#include "sc4/Automaton.h"
#include "sc4/Lot.h"

namespace sc4 {

class cSC4Watercraft;

// ---- ids -------------------------------------------------------------------------------------------------------------
constexpr std::uint32_t kWatercraftIid = 0x096e75f5;            // cISC4Watercraft (QueryInterface)
constexpr std::uint32_t kWatercraftGZCLSID = 0x096e75f5;        // GetGZCLSID
constexpr std::uint32_t kWatercraftOccupantType = 0x897f7ec2;   // +0x50 / GetType
constexpr std::uint32_t kWatercraftMessageTow = 0xac400a31;     // registered with the message server
constexpr std::uint32_t kWatercraftMessageReloadDriveParams = 0x26d31ec1;

// Occupant groups (cISC4Occupant::IsOccupantGroup, vslot 0xb0).
constexpr std::uint32_t kWatercraftGroupPlayerDrive = 0x4409;
constexpr std::uint32_t kWatercraftGroupTravelBoat1 = 0x440a;
constexpr std::uint32_t kWatercraftGroupTravelBoat2 = 0x440f;
constexpr std::uint32_t kWatercraftGroupTowable = 0x4410;
constexpr std::uint32_t kWatercraftGroupTravelBoat3 = 0x4411;
constexpr std::uint32_t kWatercraftGroupMediumBoat = 0x4413;
constexpr std::uint32_t kWatercraftGroupLargeBoat = 0x4414;
constexpr std::uint32_t kWatercraftGroupTowTarget = 0x4417;

// Automata flags with no base-class name (the base header documents the rest).
constexpr std::uint32_t kAutomataFlagWatercraft0x8000 = 0x8000;   // Init sets it (purpose not recovered)

// Watercraft behaviour states (the base header names the shared ones).
constexpr std::uint32_t kWatercraftStateRunAground = 0x46;
constexpr std::uint32_t kWatercraftStateSink = 0x47;
constexpr std::uint32_t kWatercraftStateRemoveNow = 0x48;
constexpr std::uint32_t kWatercraftStateRemoveFade = 0x49;
constexpr std::uint32_t kWatercraftStateDocking = 0x4a;
constexpr std::uint32_t kWatercraftStateQueueing = 0x4b;
constexpr std::uint32_t kWatercraftStateWaiting = 0x4c;
constexpr std::uint32_t kWatercraftStateFollowing = 0x4d;
constexpr std::uint32_t kWatercraftStateUndocking = 0x4e;

// The named animations loaded by Init (original offsets in the members below).
constexpr int kWatercraftAnimationCount = 6;
extern const char* const kWatercraftAnimationNames[kWatercraftAnimationCount];

// A point of the lookahead of the route the boat is following (+0x00 x, +0x04 z, +0x08 flags, +0x0c direction).
struct WatercraftTravelPoint {
    std::int32_t x = 0;
    std::int32_t z = 0;
    std::uint32_t flags = 0;   // bit n: direction n has been tried/invalidated (InvalidateLookaheadCell)
    std::int32_t direction = 0;

    bool operator==(const WatercraftTravelPoint& other) const {
        return x == other.x && z == other.z && flags == other.flags && direction == other.direction;
    }
};

// One ferry station (the original's FerryStationInformation, 0x48 bytes). The position is the station's dock point;
// the second vector is carried along untouched by the recovered code. `flags` is the u16 at +0x40: bit 8 marks the
// station as queued/docked (station A/B) and, on the current station, that a route is available.
struct FerryStationInformation {
    cRZAutoRefCount<cSC4Lot> lot;             // +0x00
    std::vector<std::uint8_t> route;          // +0x08 (compressed route points)
    cS3DVector3 position;                     // +0x20
    cS3DVector3 dockDirection;                // +0x2c (copied, not read by the recovered code)
    std::uint8_t dockRequested = 0;           // +0x38 (set to 1 by Ferry_InitializeCurrentRoute)
    std::uint8_t pad39[3] = {0, 0, 0};
    std::int32_t routeIndex = 0;              // +0x3c (lookahead points to skip first)
    std::uint16_t flags = 0;                  // +0x40

    bool Queued() const { return (flags & 0x100) != 0; }
    void SetQueued(bool on) { flags = static_cast<std::uint16_t>(on ? (flags | 0x100) : (flags & ~0x100)); }
    bool HasRoute() const { return (flags & 0x100) != 0; }
};

// The drive parameters (+0x4e0..+0x57c, 40 floats) read from the automata group's property array (+0xe0); every
// zero entry falls back to the default. Names describe what the recovered code does with the value; the original
// offsets are in the member order.
struct WatercraftDriveParameters {
    float reverseAcceleration = 0.1f;        // +0x4e0
    float acceleration = 0.001f;             // +0x4e4
    float reverseRate = 1.0f;                // +0x4e8
    float brakeRate = 0.05f;                 // +0x4ec
    float movementScale = 0.2f;              // +0x4f0
    float reverseBoost = 5.0f;               // +0x4f4
    float minSpeed = -0.1f;                  // +0x4f8
    float maxSpeed = 0.3f;                   // +0x4fc
    float minTurn = -2.0f;                   // +0x500
    float maxTurn = 2.0f;                    // +0x504
    float timeScale = 0.001f;                // +0x508
    float turnStride = 50.0f;                // +0x50c
    float reverseTurnStride = 5.0f;          // +0x510
    float aiTimeScale = 0.001f;              // +0x514
    float turnRampScale = 0.001f;            // +0x518
    float boatCollisionThreshold = 0.1f;     // +0x51c
    float staticCollisionThreshold = 0.5f;   // +0x520
    float rollRate = 1.0471976f;             // +0x524 (pi/3)
    float maxRoll = 0.31415927f;             // +0x528 (pi/10)
    float pitchRate = 0.13089970f;           // +0x52c (pi/24)
    float maxPitch = 0.26179940f;            // +0x530 (pi/12)
    float crashStateTimer = 8000.0f;         // +0x534 (ms)
    float sinkMaxProgress = 0.78539753f;     // +0x538
    float agroundSpeedScale = 0.95f;         // +0x53c
    float agroundProgressRate = 2.1816616f;  // +0x540
    float crashAngleDegrees = 1.0471976f;    // +0x544 (radians, despite the name; acos threshold)
    float groundHeightTolerance = 10.0f;     // +0x548
    float playerBoatCollisionThreshold = 0.5f;    // +0x54c
    float playerStaticCollisionThreshold = 0.25f; // +0x550
    float collisionSpeedScale = 0.75f;       // +0x554 (multiplies the speed after a boat collision)
    float speedMin = -2000.0f;               // +0x558 (AccelerationFunction range)
    float speedMax = 3000.0f;                // +0x55c
    float leftTriggerThreshold = 150.0f;     // +0x560
    float rightTriggerThreshold = 0.0f;      // +0x564
    float value568 = 2.0f;                   // +0x568
    float maxStep = 100.0f;                  // +0x56c (clamp of one tick's movement)
    float dockRequestRadius = 300.0f;        // +0x570 (Ferry_DockRequests)
    float dockDistance = 90.0f;              // +0x574 (DoDockingTick)
    float dockSpeedScale = 1.0f;             // +0x578
    float dockTurnScale = 1.0f;              // +0x57c
};
static_assert(sizeof(WatercraftDriveParameters) == 40 * sizeof(float), "40 drive parameters");

// ---- seams -----------------------------------------------------------------------------------------------------------
// cSC4CompressedRouteReader (0x1000d4ef6 SetData, 0x1000d4f52 GetNext; two embedded 0x20-byte readers at +0x3f8 and
// +0x4b8). The compressed point format is part of that unrecovered class; the host hands out a reader.
class IWatercraftRouteReader {
public:
    virtual ~IWatercraftRouteReader() = default;
    virtual void SetData(const std::vector<std::uint8_t>& data) = 0;
    virtual bool GetNext(std::int16_t& x, std::int16_t& z) = 0;   // UncompressedData: x at +0, z at +2
};

// The named animated instance (slot 0x18 play, 0x20 set-paused, 0x28 is-playing, 0x40 set-transform; the original
// also has the cIGZUnknown AddRef/Release at 0x08/0x10).
class IWatercraftAnimation {
public:
    virtual ~IWatercraftAnimation() = default;
    virtual void AddRef() = 0;
    virtual void Release() = 0;
    virtual void Play(bool loop) = 0;                            // vslot 0x18
    virtual void SetPaused(bool paused) = 0;                     // vslot 0x20
    virtual bool IsPlaying() const = 0;                          // vslot 0x28
    virtual void SetTransform(const cS3DTransform& transform) = 0;   // vslot 0x40
};

// The automata group template as LoadDriveParameters / HandleEnterState read it: +0xe0 the 40-float drive array,
// +0xe8 a node whose +8 is the group speed. Additive over the recovered IAutomatonGroupTemplate (dynamic_cast, the
// original reads the raw fields).
class IWatercraftGroupTemplate : public IAutomatonGroupTemplate {
public:
    virtual const float* DriveParameters() const = 0;   // +0xe0 (40 floats, may be null)
    virtual float DriveSpeed() const = 0;               // +0xe8 -> +0x8
};

// The unrecovered watercraft manager (cSC4WatercraftManager, city slot 0x170) plus the city / terrain / surface water
// / lot-manager queries and the named-animation registry. Every method names the original call it replaces.
class IWatercraftHost : public IAutomatonHost {
public:
    // ---- cSC4WatercraftManager
    virtual cSC4Watercraft* LookForBoatsAtPosition(const float* xyz, cSC4Watercraft* exclude) = 0;   // @ 0x1004ef14e
    virtual cSC4Watercraft* CollisionDetection(float radius, cSC4Watercraft* self,
                                               float* outPosition) = 0;                              // @ 0x1004f4908
    virtual bool RequestPermissionToDock(cSC4Lot* lot, cSC4Watercraft* boat) = 0;   // @ 0x1004f4690
    virtual bool RequestPermissionToWait(cSC4Lot* lot, cSC4Watercraft* boat) = 0;   // @ 0x1004f472e
    virtual void DoneDocking(cSC4Lot* lot, cSC4Watercraft* boat) = 0;               // @ 0x1004f47cc
    virtual void DoneQueueing(cSC4Lot* lot, cSC4Watercraft* boat) = 0;              // @ 0x1004f4864
    virtual bool CanDockNow(cSC4Watercraft* boat) = 0;                              // manager vslot 0x178
    virtual void FleetCounts(std::int32_t& docks, std::int32_t& queues) = 0;        // manager +0xb28 / +0xb78
    // ---- message server notifications (SL::spMS2 vslots 0x28 / 0x30)
    virtual bool AddNotification(cIGZMessageTarget2* target, std::uint32_t messageType) = 0;
    virtual bool RemoveNotification(cIGZMessageTarget2* target, std::uint32_t messageType) = 0;
    // ---- city (SL::spCity)
    virtual void WorldToCell(float x, float z, int& cellX, int& cellZ) = 0;          // vslot 0x2f8
    virtual void CellToWorld(int cellX, int cellZ, float& x, float& z) = 0;          // vslot 0x308
    virtual bool CellInCity(int cellX, int cellZ) = 0;                               // vslot 0x318
    virtual bool PointInCity(float x, float z) = 0;                                  // vslot 0x310
    virtual void CityCellCount(int& sizeX, int& sizeZ) = 0;                          // vslots 0x2e8 / 0x2f0
    virtual float CityExtent() = 0;                                                  // vslot 0x2d8
    // ---- terrain (SL::spTerrain)
    virtual float TerrainHeightAtCell(int cellX, int cellZ) = 0;                     // vslot 0x90
    virtual void TerrainNormal(float x, float z, cS3DVector3& out) = 0;              // vslot 0xd8
    virtual float WaterLevel(float x, float z) = 0;                                  // vslot 0x60
    // ---- surface water (SL::spSurfaceWater)
    virtual bool IsWaterCell(int cellX, int cellZ) = 0;                              // vslot 0x68
    virtual bool IsWaterAtCell(int cellX, int cellZ, int boatSize) = 0;              // vslot 0x38
    virtual bool IsWaterAtPosition(float x, float z, int mask, cSC4Lot* lot) = 0;    // vslot 0x40
    // ---- lot manager (SL::spCity + 0x108)
    virtual cSC4Lot* LotAtCell(int cellX, int cellZ) = 0;                            // vslot 0x28
    virtual cSC4Lot* LotAtPosition(const float* xyz) = 0;                            // vslot 0x30
    // ---- cSC4CompressedRouteReader / the named-animation registry (SL::spCity + 0x2a8, vslot 0x30)
    virtual IWatercraftRouteReader* CreateRouteReader() = 0;
    virtual IWatercraftAnimation* CreateNamedAnimation(const char* name) = 0;
};

class cSC4Watercraft : public cSC4Automaton {
public:
    enum eW_ChangeState : std::uint32_t {
        kChangeIdle = 0,
        kChangeRemove = 1,
        kChangeRunAground = 2,
        kChangeSink = 3,
        kChangeSinkRoll = 4,
    };

    explicit cSC4Watercraft(IWatercraftHost* host = nullptr, const cGZPersistResourceKey& modelKey = {});
    ~cSC4Watercraft() override;

    // ---- cIGZUnknown / cISC4Occupant / cIGZSerializable
    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x1004f604e
    bool Init() override;                                          // @ 0x1004f4e06
    bool Shutdown() override;                                      // @ 0x1004f5b7a
    bool Write(cIGZOStream& stream) const override;                // @ 0x1004f5e7e
    bool Read(cIGZIStream& stream) override;                       // @ 0x1004f5f32
    std::uint32_t GetGZCLSID() const override { return kWatercraftGZCLSID; }   // @ 0x1004fdf42

    // ---- cIGZMessageTarget2 (the original override at 0x1004f6116, thunk 0x1004f63a2)
    bool DoMessage(cIGZMessage2* message) override;

    // ---- cISC4Automaton overrides specific to the watercraft
    void MoveTo(const float* position, const float* direction);     // @ 0x1004f63b0
    bool DoStateTick(float milliseconds) override;                 // @ 0x1004f6484
    bool DoAnimationTick(std::uint32_t unknown, float milliseconds) override;   // @ 0x1004f9194
    bool HandleEnterState(tAutomataState* state) override;         // @ 0x1004f77d2
    bool HandleExitState(const tAutomataState* state) override;    // @ 0x1004f75c0
    bool DoPlayerDriveTick(float milliseconds);                    // @ 0x1004f9ce0
    bool TranslatePlayerDriveCommands(tAutomataStatePlayerDrive* state, float milliseconds);   // @ 0x1004fb362

    // ---- state helpers
    void SetStateInternal(eW_ChangeState change);                  // @ 0x1004f1050
    void Crash_SetStateInternal(eW_ChangeState change, bool force);   // @ 0x1004f6414

    // ---- ferry / route
    void Ferry_SetStationInformation(const FerryStationInformation& stationA, const FerryStationInformation& stationB,
                                     bool initializeRoute);        // @ 0x1004f1cc8
    bool IsNeighborTravelBoat();                                   // @ 0x1004f27aa
    void SetNbrRouteData(const std::vector<std::uint8_t>& route, cISC4Occupant* occupant);   // @ 0x1004f2818
    bool InvalidateLookaheadCell(int index);                       // @ 0x1004f3894
    FerryStationInformation Ferry_OtherStationInformation() const; // @ 0x1004f7c50
    void Ferry_SwitchActiveStation();                              // @ 0x1004f7720
    void Ferry_InitializeCurrentRoute(const FerryStationInformation& station);   // @ 0x1004fdbf0
    void Ferry_DockRequests();                                     // @ 0x1004f7a9c
    void Ferry_ClearDockRequests();                                // @ 0x1004f9bf6
    void EndDocking();                                             // @ 0x1004f7a24
    void EndTowing();                                              // @ 0x1004f76ba

    // ---- ticks / movement
    void DoCrashSequenceTick(float milliseconds);                  // @ 0x1004f65e0
    bool DoDockingTick(float milliseconds);                        // @ 0x1004f67da
    bool DoFollowingTick(float milliseconds);                      // @ 0x1004f6d9c
    bool DoUndockingTick(float milliseconds);                      // @ 0x1004f72aa
    void Engine_AIBoatMotion(float milliseconds);                  // @ 0x1004f7ea0
    void Movement_PositionNew(const float& milliseconds);          // @ 0x1004f89f0
    bool Engine_DetectCollision();                                 // @ 0x1004f8d06
    bool Engine_MovementPossible();                                // @ 0x1004f910e
    bool OutOfBounds();                                            // @ 0x1004f9e90
    bool OnLand();                                                 // @ 0x1004f7e06
    bool Crash_SequenceRunAground(float milliseconds);             // @ 0x1004f9f4a
    bool Crash_SequenceSink();                                     // @ 0x1004fab22
    float AccelerationFunction(const float& value);                // @ 0x1004fbbfa
    float AccelerationFunctionInverse(const float& value);         // @ 0x1004fbd80
    int BoatSize();                                                // @ 0x1004fbefe
    void Crash_RegressToPreviousLocation();                        // @ 0x1004fbf50
    bool Crash_InanimateCollisionDetection();                      // @ 0x1004fc036
    void Crash_DetermineCase();                                    // @ 0x1004fc1ec
    bool Helper_OrientationCloseEnough(cS3DVector3 direction);     // @ 0x1004fc3e2
    bool Engine_AIPickLocation();                                  // @ 0x1004fc4f0
    int Assist_PickInitialDir();                                   // @ 0x1004fca96
    void Assist_PickNewDir(int option, int direction, int* out);   // @ 0x1004fcc3c
    void Assist_DirIntoCell(int direction, int& cellX, int& cellZ);   // @ 0x1004fccb4
    const char* GetDebugExtraText() const override;                // @ 0x1004fcd1e
    void Movement_PositionTurning(float milliseconds, cS3DTransform& transform, cS3DVector3& position,
                                  cS3DTransform& extra);           // @ 0x1004fcea0
    void Movement_PositionStraight(float milliseconds, cS3DTransform& transform, cS3DVector3& position);   // @ 0x1004fd4c4
    void Movement_Rotation(cS3DTransform& transform);              // @ 0x1004fd56c
    void Movement_Pitch(cS3DTransform& transform);                 // @ 0x1004fda16
    int Assist_DirLookup(int index) const;                         // @ 0x1004fdcc8
    void Crash_PitchDetermination(bool first, bool second);        // @ 0x1004fdd82

    // ---- model / effects wiring
    void StartRegularEffects();                                    // @ 0x1004f99aa
    void StopRegularEffects();                                     // @ 0x1004f976a

    // ---- tests / neighbours
    IWatercraftHost* WatercraftHost() const { return watercraftHost_; }
    const WatercraftDriveParameters& DriveParameters() const { return driveParameters_; }
    void LoadDriveParameters();                                    // @ 0x1004f54c6
    FerryStationInformation& StationA() { return stationA_; }
    FerryStationInformation& StationB() { return stationB_; }
    FerryStationInformation& CurrentStation() { return currentStation_; }
    cSC4Watercraft* TowTarget() const { return towTarget_.get(); }
    cSC4Watercraft* Tower() const { return tower_.get(); }
    void SetTower(cSC4Watercraft* tower) { tower_ = tower; }
    bool DockingFlag() const { return dockingFlag_; }
    bool QueueingFlag() const { return queueingFlag_; }
    bool DockingActive() const { return dockingActive_; }
    bool TurningFlag() const { return turningFlag_; }
    bool CrashMarked() const { return crashMarked_; }
    bool CrashRollEnabled() const { return crashRollEnabled_; }
    bool Following() const { return following_; }
    bool AnimationHold() const { return animationHold_; }
    bool TowSearchActive() const { return towSearchActive_; }
    bool NeighbourRouteSet() const { return neighbourRouteSet_; }
    float Speed() const { return speed_; }
    float Turn() const { return turn_; }
    float CrashTimer() const { return crashTimer_; }
    float CrashProgress() const { return crashProgress_; }
    float IdleTimer() const { return idleTimer_; }
    float StillTimer() const { return stillTimer_; }
    float ReverseTimer() const { return reverseTimer_; }
    std::uint32_t DriveMode() const { return driveMode_; }
    std::uint32_t Field464() const { return field464_; }
    float TowWaitThreshold() const { return towWaitThreshold_; }
    float TowWaitElapsed() const { return towWaitElapsed_; }
    std::uint32_t CrashRollSign() const { return crashRollSign_; }
    std::uint32_t CrashPitchSign() const { return crashPitchSign_; }
    void SetSpeed(float speed) { speed_ = speed; }
    void SetTurn(float turn) { turn_ = turn; }
    void SetCrashTimer(float timer) { crashTimer_ = timer; }
    void SetCrashRollEnabled(bool on) { crashRollEnabled_ = on; }
    void SetIdleTimer(float timer) { idleTimer_ = timer; }
    void SetAnimationHold(bool on) { animationHold_ = on; }
    void SetDockingActive(bool on) { dockingActive_ = on; }
    void SetTowWaitThreshold(float value) { towWaitThreshold_ = value; }
    void SetTowWaitElapsed(float value) { towWaitElapsed_ = value; }
    void SetDriveMode(std::uint32_t mode) { driveMode_ = mode; }
    void SetNeighbourRouteSet(bool on) { neighbourRouteSet_ = on; }
    void SetTowSearchActive(bool on) { towSearchActive_ = on; }
    const cS3DVector3& SimulatedPosition() const { return simulatedPosition_; }
    const cS3DVector3& PreviousPosition() const { return previousPosition_; }
    const cS3DVector3& SimulatedOrientation() const { return simulatedOrientation_; }
    const cS3DVector3& PreviousOrientation() const { return previousOrientation_; }
    void SetSimulatedPosition(const cS3DVector3& value) { simulatedPosition_ = value; }
    void SetSimulatedOrientation(const cS3DVector3& value) { simulatedOrientation_ = value; }
    void SetPreviousPosition(const cS3DVector3& value) { previousPosition_ = value; }
    void SetPreviousOrientation(const cS3DVector3& value) { previousOrientation_ = value; }
    std::vector<WatercraftTravelPoint>& Lookahead() { return lookahead_; }
    const std::vector<WatercraftTravelPoint>& Lookahead() const { return lookahead_; }
    IWatercraftAnimation* Animation(int index) const {
        return index >= 0 && index < kWatercraftAnimationCount ? animations_[index].get() : nullptr;
    }
    int AnimationLoadCount() const { return animationLoadCount_; }
    IWatercraftRouteReader* CurrentRouteReader() const { return currentRoute_.get(); }
    IWatercraftRouteReader* NeighbourRouteReader() const { return neighbourRouteReader_.get(); }
    std::vector<std::uint8_t>& NeighbourRoute() { return neighbourRoute_; }
    cISC4Occupant* Neighbour() const { return neighbour_.get(); }
    void SetNeighbour(cISC4Occupant* occupant) { neighbour_ = occupant; }
    cRZRandomSuper& Random() { return random_; }

protected:
    IWatercraftGroupTemplate* WatercraftGroup() const;

    IWatercraftHost* watercraftHost_ = nullptr;   // port seam (the original's SL::sp* globals)

    FerryStationInformation stationA_;             // +0x308
    FerryStationInformation stationB_;             // +0x350
    FerryStationInformation currentStation_;       // +0x398
    cRZAutoRefCount<cSC4Watercraft> towTarget_;    // +0x3e0
    cRZAutoRefCount<cSC4Watercraft> tower_;        // +0x3e8
    bool dockingFlag_ = false;                     // +0x3f0
    bool queueingFlag_ = false;                    // +0x3f1
    std::unique_ptr<IWatercraftRouteReader> currentRoute_;   // +0x3f8 (cSC4CompressedRouteReader)
    std::vector<WatercraftTravelPoint> lookahead_;  // +0x418
    bool dockingActive_ = false;                   // +0x430
    bool turningFlag_ = false;                     // +0x431
    bool crashMarked_ = false;                     // +0x432
    float towWaitThreshold_ = 0.0f;                // +0x434
    float towWaitElapsed_ = 0.0f;                  // +0x438
    float idleTimer_ = 0.0f;                       // +0x43c
    float crashTimer_ = 0.0f;                      // +0x440
    float crashProgress_ = 0.0f;                   // +0x444
    std::uint8_t crashRollSign_ = 0;               // +0x448
    std::uint8_t crashPitchSign_ = 0;              // +0x449
    bool crashRollEnabled_ = true;                 // +0x44a
    bool following_ = false;                       // +0x44b
    bool animationHold_ = false;                   // +0x44c
    float stillTimer_ = 0.0f;                      // +0x450
    float reverseTimer_ = 0.0f;                    // +0x454
    bool towSearchActive_ = false;                 // +0x458
    float speed_ = 0.0f;                           // +0x45c
    float turn_ = 0.0f;                            // +0x460
    std::uint32_t field464_ = 0;                   // +0x464
    cS3DVector3 previousPosition_;                 // +0x468
    cS3DVector3 simulatedPosition_;                // +0x474
    cS3DVector3 previousOrientation_{1.0f, 0.0f, 0.0f};   // +0x480
    cS3DVector3 simulatedOrientation_{1.0f, 0.0f, 0.0f};  // +0x48c
    cRZAutoRefCount<cISC4Occupant> neighbour_;     // +0x498
    std::vector<std::uint8_t> neighbourRoute_;     // +0x4a0
    std::unique_ptr<IWatercraftRouteReader> neighbourRouteReader_;   // +0x4b8
    bool neighbourRouteSet_ = false;               // +0x4d8
    WatercraftDriveParameters driveParameters_;    // +0x4e0
    cRZRandomSuper random_;                        // +0x580 (0x9d0 bytes)
    std::uint32_t driveMode_ = 0;                  // +0xf50
    int animationLoadCount_ = 0;                   // port bookkeeping: successful CreateNamedAnimation calls
    cRZAutoRefCount<IWatercraftAnimation> animations_[kWatercraftAnimationCount];   // +0xf58..+0xf80
};

}  // namespace sc4
