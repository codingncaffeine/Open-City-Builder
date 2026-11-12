// cSC4Vehicle — the road vehicle automaton (54 symbols at 0x1000dac10..0x100510920; every symbol is named cSC4Vehicle,
// the two operator-new/delete are in ClassAllocators). It is the cSC4TrafficAutomaton the player drives: two headlight
// model instances, a four-deep motion-blur "ghost" trail, a saved path history (the last tiles it visited) and the whole
// player-drive physics (speed / steering integration, off-road drift, lot and building collisions, damage and the crash /
// injury effects). It is also used for mission vehicles (the dispatch code and every AI car are cSC4Vehicle instances).
//
// The class extends the cSC4TrafficAutomaton vtables (primary 0x100a70b68, object pointer 0x100a70b78); its own slots are
//   0x5b8 ShowHeadlights(bool,bool)      0x5c0 ShowMotionBlur(bool)       0x5c8 SetPathPos(cS3DVector3 const&)
//   0x5d0 GetPrevOrient() const         0x5d8 SetPrevOrient(...)         0x5e0 SavePathHistory(deque&, u8)
//   0x5e8 FindBackPosition(...)         0x5f0 GetPathHistory() const     0x5f8 SkipTown()
//   0x600 DoPlummetTick(float)          0x608 DoPullOverTick(float)
//   0x610 DoPlayerDriveOffRoadTick(float, tAutomataStatePlayerDrive*, float, tAutomataPlayerDriveParams*, cISC4NetworkOccupant*)
//   0x618 RestrictPlayerDrivePosition(cS3DVector3 const&, cS3DVector3&)
//   0x620 GetPlayerDriveLotIntersection(cS3DVector3 const&, cS3DVector3&, cS3DVector2*, cS3DVector2*)
//   0x628 DoCollisionCheck(cS3DVector3&, float&)                         0x630 UpdateMotionBlur(float)
// Overridden base slots: 0x18 Init, 0x20 Shutdown, 0x50 (none), 0x1e0 HandlePlayerDriveCommand, 0x208 HandleCollision,
//   0x240 Write, 0x248 Read, 0x250 GetGZCLSID, 0x278 DoAnimationTick, 0x320 GetDebugExtraText, 0x3a0 DoStateTick,
//   0x3a8 HandleEnterState, 0x3d0 DoPlayerDriveTick, 0x3d8 DoBumpedTick, 0x3f8 DoCollideTick, 0x400
//   TranslatePlayerDriveCommands, 0x410 GetCollidingAutomaton, 0x418 RecalculateLeaders, 0x420 GetSpeedModifier,
//   0x440 DoPathTick, 0x450 StartTrip, 0x458 SetPath, 0x470 HandleNetworkOccupantRemoved, 0x498 GetPathPos,
//   0x4f0 GetSafeDistance, 0x528 RecalculatePathOffset, 0x5a8 GetTrafficAutomataManager.
// QueryInterface answers 0x896e714a (cISC4Vehicle) in addition to the base ids.
//
// Original layout after the cSC4TrafficAutomaton fields (+0x398 onward; the base is 0x390..0x398 bytes in the binary):
//   +0x398 float    headlight timer (seconds; negative = the lights are fading out)
//   +0x3a0 cIS3DModelInstanceAnimated* left headlight  (counted)
//   +0x3a8 cIS3DModelInstanceAnimated* right headlight (counted)
//   +0x3b0 float    remaining path distance (InterpolateAlongPath's `remaining` output, re-read by StartTrip)
//   +0x3b4 cS3DVector3 path position (the point the path interpolation starts from; the base class' +0x74 is the
//                      rendered / simulated position, this one only advances along the path)
//   +0x3c0 cS3DVector3 previous orientation
//   +0x3d0 std::deque<tPathNode> path history (0x10-byte nodes: counted occupant + key + dead-end byte)
//   +0x3f8 u32      path-history size (the deque's __size_ member)
//   +0x400 cIS3DModelInstanceAnimated* motion-blur models[4] (each one a copy of the primary model, faded)
//   +0x420 u8       motion-blur cursor (0..3), advanced every UpdateMotionBlur
// The vehicle object is freed through operator delete (ClassAllocators); the virtual destructor deletes only the two
// headlight models, the path history and the base.
//
// Behaviour: Init flags the automaton as a collision candidate (cSC4VehicleManager::sbDefaultCollide), rolls the random
// path offset and turns the headlights on when the manager default or the scene lighting asks for it. DoAnimationTick
// runs the motion-blur trail behind the base tick and counts the headlight timer down; DoPathTick drives along the path
// with the vehicle's own path position / previous orientation; the player-drive state (0x10) is handled by
// DoPlayerDriveTick, whose off-road half integrates the steering, pulls the car back towards the network and resolves
// lot / occupant collisions through DoCollisionCheck and the crash effects.
//
// Quirks kept (addresses in the .cpp):
//  * ShowHeadlights(!on, true) starts a random fade timer and returns without touching the models; ShowHeadlights(on,
//    false) loads / releases them (LoadAuxiliaryModel specifier 1 for both, so both use the same model key).
//  * UpdateMotionBlur rebuilds the model instance from the *primary* model key when the current ghost slot is empty, adds
//    it to the renderer and then walks the four ghosts backwards by one third of the opacity; the loop reads the model
//    of the *previous* slot each iteration.
//  * The headlight timer in DoAnimationTick is decremented only when |timer| >= 0.0001, and the lights are shown with
//    `true` when it crossed zero from positive and `false` otherwise; it also calls UpdateGhostModel even though the
//    base returns true.
//  * GetDebugExtraText builds a process-wide static cRZString (the port returns a static buffer) and appends the first
//    model key as three words; the base text ends up in the same static.
//  * Write saves the path history as a count, then key + two occupant cell words per node, then a 0 key terminator, the
//    path position, the base's "uninitialised" vector (kUninit3DVector, written, never read) and the remaining distance.
//  * Read accepts version 2 only, reads the history entries into temporaries (the path history is *not* rebuilt), then
//    reads the path position and the remaining distance.
//  * Init calls RecalculatePathOffset *before* the base Init, so the random offset is in place before the first path;
//    the base Init failing still leaves the flags set.
//  * HandleEnterState for state 0x10 zeroes value40 and copies the first automata group's player-drive parameter word
//    +8 into groupSpeed, then calls the base (which calls the base HandleEnterState a second time).
//  * SkipTown sets the flags 0x201180a and stores a random 0..49 cell lifespan, and always answers false.
//  * GetSafeDistance measures the path term from the *path position* (+0x3b4), not the simulated position.
//
// Seams: the vehicle manager, the automata-manager tuning table, the occupant manager's rectangle query, the lot
// manager, the visual-effect manager, the disaster layer, the automata group chain and CanDriveOnCell (a free function
// in the same translation unit, 0x1000e11df, not part of the class) are unrecovered neighbours. They are reached through
// IVehicleHost below, each method naming the original global / vtable slot it replaces. Everything else goes through
// the recovered base classes (cSC4Automaton, cSC4TrafficAutomaton, cSC4TrafficNetworkMap, cISC4PathInfo).
#pragma once
#include <cstdint>
#include <deque>
#include <vector>

#include "sc4/AutomataDriveParams.h"
#include "sc4/AutomataStates.h"
#include "sc4/Automaton.h"
#include "sc4/MiscTypes.h"
#include "sc4/Occupant.h"
#include "sc4/PathInfo.h"
#include "sc4/TrafficAutomaton.h"
#include "sc4/TrafficNetworkMap.h"

namespace sc4 {

class cSC4Vehicle;

// ---- ids -------------------------------------------------------------------------------------------------------------
constexpr std::uint32_t kVehicleIID = 0x896e714a;              // cISC4Vehicle (QueryInterface / GetGZCLSID)
constexpr std::uint32_t kVehicleOccupantType = 0xc7cc75f1;     // the occupant type id written to +0x50 by the ctor
constexpr std::uint32_t kVehicleGroupPlayerDrive = 0x4202;     // occupant group tested by PlayerDriveUpdateSpeed
constexpr std::uint32_t kVehicleMessageCollision = 0x49e64802; // SC4PostMessage2 in HandleCollision
constexpr std::uint32_t kVehicleDamageMessage = 0x0b;          // SendCallbackMessage(0xb, 1) after a crash
constexpr std::uint32_t kVehicleEventOnRoad = 0x0a;            // SendCallbackMessage(10, offRoad)
constexpr std::uint32_t kVehicleAudioEventDrive = 0x10;        // AEMS start / stop while player driving
constexpr std::uint32_t kVehicleAudioEventOffRoad = 0x05;      // AEMS start / stop off the network
constexpr std::uint32_t kVehicleAudioEventTunnel = 0x0e;       // AEMS start / stop when the path enters a tunnel
constexpr std::uint32_t kVehicleAudioEventCrash = 0x12;        // AEMS start / stop when the speed turns negative
constexpr std::uint32_t kVehicleAudioEventCollision = 0x07;    // AEMS start / stop pair after DoCollisionCheck
constexpr std::uint32_t kAutomataFlagMotionBlur = 0x80000;     // the ghost trail is active (ShowMotionBlur / DoAnimationTick)
const char* const kVehicleHazeEffect = "haze";                 // 0x1000dee93: the named effect of the drive state
const char* const kVehicleSplashEffect = "car_splash";         // DoPlummetTick
const char* const kVehicleCrashEffect = "udriveit_crash";      // UpdateAfterDamage
const char* const kVehicleInjuredEffect = "udriveit_injured";  // UpdateAfterDamage

// The tunnel interface a network occupant answers in SetPath (iid 0x8a4bd52b).
constexpr std::uint32_t kVehicleNetworkFlagTunnelInterface = 0x80000;
// The network flag the player-drive code uses to recognise a road cell (HasAnyNetworkFlag 0x40341004 in the original;
// 0x1f4f is the "drivable network" mask).
constexpr std::uint32_t kVehicleNetworkFlagDriveable = 0x1f4f;
constexpr std::uint32_t kVehicleNetworkFlagOnRoad = 0x40341004;

constexpr float kVehicleStoppedSpeed = 0.0002777778f;   // 1/3600, the plummet's resting speed (0x3991a2b5)
constexpr float kVehicleGravityStep = 0.00083333347f;   // 1/1200, the plummet's per-millisecond gravity
constexpr float kVehicleCollideTimer = 5000.0f;         // 0x100a181a4: the collide state's timer
constexpr float kVehicleFrameMilliseconds = 33.0f;      // 0x100a18180: one player-drive physics sub-step
constexpr float kVehicleOffRoadRadius = 0.5f;           // the radius * 0.5 offset of the path position
constexpr float kVehiclePathEpsilon = 1e-05f;
constexpr float kVehicleCellSize = 0.0625f;             // 1 / 16: world -> cell
constexpr float kVehicleHeightTolerance = 5.0f;         // DoPlummetTick / HandleNetworkOccupantRemoved

// The motion-blur ghost models and the headlight models use LoadAuxiliaryModel / the primary key.
constexpr int kVehicleMotionBlurCount = 4;
constexpr int kVehicleHeadlightSpecifier = 1;   // cSC4Automaton::LoadAuxiliaryModel(1, &model) for both lights

// ---- tuning -----------------------------------------------------------------------------------------------------------
// cSC4VehicleManager / cSC4AutomataManager tuning the vehicle reads (0x100b21880 and the loose globals). The binary
// zero-fills them and cSC4AutomataManager::ReadTuningParameters fills them from the simulator exemplar, so the host
// supplies them; the defaults are the zero-filled values.
struct VehicleTuning {
    std::int32_t defaultSpeedMin = 0;      // 0x100b21884 (units per hour, RandomSint32RangeUniform lower bound)
    std::int32_t defaultSpeedMax = 0;      // 0x100b21888
    float pathOffsetMin = 0.0f;            // 0x100b21a2c (RecalculatePathOffset's RandomDoubleRangeUniform range)
    float pathOffsetMax = 0.0f;            // 0x100b21a30
    // sTuningParams + 0x158 is a pointer to a float[13] indexed by network type (2 = road). The binary's table is
    // zero-filled until ReadTuningParameters runs; the 1.0 entries are placeholders so a host-less run does not freeze.
    float networkSpeed[13] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    float pathCurveFactor = 1.0f;          // sTuningParams + 0x138 (a straight path multiplies it by 0.6)
    float turnMax = 1.5707964f;            // sTuningParams + 0x1b8 (curve clamp)
    float turnDivisor = 1.0f;              // sTuningParams + 0x1b4 (curve clamp divisor)
    // The trains' part (i386 sTuningParams +0x13e..+0x148): how many cars a freight train (car group 0x4203) and any other
    // train pulls, and the trains' speed range in units per hour.
    std::uint8_t freightCarsMin = 0, freightCarsMax = 0;       // +0x13e / +0x13f (property 0xea441c97)
    std::uint8_t passengerCarsMin = 0, passengerCarsMax = 0;   // +0x140 / +0x141 (property 0x8a441c9c)
    std::uint32_t trainSpeedMin = 0, trainSpeedMax = 0;        // +0x144 / +0x148 (property 0x6a440d40)
};

// One "named / managed visual effect" object as the vehicle uses it: the effect the effects manager hands out (slot 0x30)
// and the object a driving state keeps at +0x98 / the plummet's splash at +0x50. vslot numbers of cISC4VisualEffect.
class IVehicleEffect : public cIGZUnknown {
public:
    virtual void SetTransform(const float* matrix) = 0;   // vslot 0x40 (the call drops the transform argument)
    virtual void SetFlag(bool on) = 0;                    // vslot 0x18 (always called with false)
    virtual void Detach(bool final) = 0;                  // vslot 0x20
};

// ---- seat ---------------------------------------------------------------------------------------------------------------
class IVehicleHost {
public:
    virtual ~IVehicleHost() = default;
    // cSC4AutomataManager::sTuningParams (0x100b21880) / the loose globals; null uses the zero-filled defaults
    virtual const VehicleTuning* Tuning() = 0;
    // cSC4VehicleManager::sbDefaultCollide / sbDefaultHeadlights (Init)
    virtual bool DefaultCollide() = 0;
    virtual bool DefaultHeadlights() = 0;
    // SL::spLightingManager slot 0x48 (Init: the scene lights are on)
    virtual bool SceneLightsOn() = 0;
    // the automata manager's vslot 0x258 (RecalculateLeaders is a tail call into it)
    virtual void ManagerRecalculateLeaders(cSC4Automaton* automaton) = 0;
    // AsOccupant()->vslot 0xb0 (PlayerDriveUpdateSpeed / TranslatePlayerDriveCommands ask for group 0x4202)
    virtual bool IsOccupantGroup(cISC4Occupant* occupant, std::uint32_t group) = 0;
    // The automata group chain: GetAutomataGroup(), then +0x18 (parent) until a group's +0xe8 (player-drive params) is
    // non-null. Answers whether the automaton has a group at all; `*out` is null when no group carries parameters.
    virtual bool FindPlayerDriveParams(cSC4Automaton* automaton, const tAutomataPlayerDriveParams** out) = 0;
    // SL::spTrafficSim slot 0xc8: how many automata of `travelType` are in the cell (DoPullOverTick's density test)
    virtual int CellAutomatonCount(int travelType, int cellX, int cellZ) = 0;
    // SL::spTrafficNetworkMap slot 0x70 (the occupant list of a cell) walked through the +0x88 next link
    virtual void OccupantsInCell(int cellX, int cellZ, std::vector<ITrafficAutomatonOccupant*>& out) = 0;
    // CanDriveOnCell @ 0x1000e11df (a free function of the same translation unit: terrain passability, the occupant
    // manager's lot list and the lot's orientation)
    virtual bool CanDriveOnCell(int cellX, int cellZ, float x, float z) = 0;
    // SL::spCity slot 0x310: is the world point inside the city bounds (GetSpeedModifier / HandleNetworkOccupantRemoved)
    virtual bool IsInsideCity(float x, float z) = 0;
    // SL::spTerrain slot 0xf8: the surface height (network / water) at a world point, the second half of the
    // max(terrain 0x60, surface 0xf8) ground test (HandleNetworkOccupantRemoved, DoPlummetTick)
    virtual float SurfaceHeight(float x, float z) = 0;
    // SL::spCity slot 0x300: a cell's world rectangle corner (RestrictPlayerDrivePosition)
    virtual void CellToWorld(int cellX, int cellZ, float& x, float& z) = 0;
    // the lot manager (city slot 0x108) -> lot slot 0x70: the bounding cells of the lot in a cell
    virtual bool LotBoundingRectAtCell(int cellX, int cellZ, std::int32_t rect[4]) = 0;
    // SL::spEffectsManager slot 0x30: create a named effect (returns one counted reference)
    virtual IVehicleEffect* CreateNamedEffect(const char* name) = 0;
    // cSC4TrafficAutomataManager::CreateAccident(occupant, type, duration)
    virtual cIGZUnknown* CreateAccident(cISC4Occupant* occupant, std::int32_t type, std::uint32_t duration) = 0;
    // SL::spOccupantManager slot 0x120: the occupants of the box (min, max) of a type, at most `count`
    virtual void OccupantsInBox(const float* minXyz, const float* maxXyz, std::uint32_t type, int count,
                                std::vector<cISC4Occupant*>& out) = 0;
    // SL::spDisasterLayer slot 0x50 / 0x38 (QI 0xa9b5ab9e) / slot 0x28: the damage at a position (0 when no disaster)
    virtual float PlayerDriveDisasterDamage(float x, float z) = 0;
    // cSC4AutomataManager::GetCollidingAutomaton(automaton, distance) @ 0x100102666 (GetCollidingAutomaton's override)
    virtual cSC4Automaton* CollidingAutomaton(cSC4Automaton* automaton, float distance) = 0;
    // SC4PostMessage2(0x49e64802, 0, 0, 0, 0, null) in HandleCollision
    virtual void PostCollisionMessage() = 0;
    // ---- what the trains and the bus ask (sc4/Train.h, sc4/SpecialVehicles.h); the defaults are a city without them
    // SL::spDisasterLayer slot 0x28 IsADisasterOngoing (cSC4TrainCar::DoCollideTick)
    virtual bool DisasterOngoing() { return false; }
    // SL::spTrafficSim slot 0x44 CheckRailAccident(cellX, cellZ) (cSC4Train::StartTrip)
    virtual bool CheckRailAccident(int cellX, int cellZ) { (void)cellX; (void)cellZ; return false; }
    // message 0x0c05e4cd with the cell in its first two data words, sent through SL::spMS2 (cSC4Train::StartTrip)
    virtual void PostRailAccidentMessage(int cellX, int cellZ) { (void)cellX; (void)cellZ; }
    // the occupant asked for 0x4a10afc2 (an underground piece), then its slot 0x1d8 GetZipperPieceType; -1 when the
    // occupant is not underground (cSC4TrainCar::SetPath)
    virtual int ZipperPieceType(ITrafficAutomatonOccupant* occupant) { (void)occupant; return -1; }
    // SL::spLotManager slot 0x14 GetLot(cellX, cellZ, false) != null (cSC4MassTransitTrain::ChooseNewDestination)
    virtual bool IsLotAtCell(int cellX, int cellZ) { (void)cellX; (void)cellZ; return false; }
    // the lot's building (lot slot 0xc0), as an occupant, is in `group` (the HandleExitNetwork overrides)
    virtual bool IsLotBuildingInGroup(void* lot, std::uint32_t group) { (void)lot; (void)group; return false; }
};

// The process-wide vehicle host (installed by the game / the test). Null degrades to the no-manager behaviour.
IVehicleHost* VehicleHost();
void SetVehicleHost(IVehicleHost* host);

class cSC4Vehicle : public cSC4TrafficAutomaton {
public:
    explicit cSC4Vehicle(ITrafficAutomatonHost* host = nullptr, IAutomatonHost* automatonHost = nullptr,
                         IVehicleHost* vehicleHost = nullptr, const cGZPersistResourceKey& modelKey = {});
    ~cSC4Vehicle() override;

    // ---- cISC4Automaton / cIGZSerializable overrides ----
    bool Init() override;                                                    // 0x18  @ 0x1000daeee
    bool Shutdown() override;                                                // 0x20  @ 0x1000daf80
    bool QueryInterface(std::uint32_t iid, void** out) override;             // 0x00  @ 0x1000dd314
    std::uint32_t AddRef() override { return cSC4Occupant::AddRef(); }
    std::uint32_t Release() override { return cSC4Automaton::Release(); }
    bool Write(cIGZOStream& stream) const override;                          // 0x240 @ 0x1000dd022
    bool Read(cIGZIStream& stream) override;                                 // 0x248 @ 0x1000dd1b6
    std::uint32_t GetGZCLSID() const override { return kVehicleIID; }        // 0x250 @ 0x1000e2596
    bool DoAnimationTick(std::uint32_t unknown, float milliseconds) override;   // 0x278 @ 0x1000db152
    bool DoStateTick(float milliseconds) override;                           // 0x3a0 @ 0x1000db260
    bool HandleEnterState(tAutomataState* state) override;                   // 0x3a8 @ 0x1000e1b8c
    bool HandlePlayerDriveCommand(std::uint32_t command, bool on) override;  // 0x1e0 @ 0x1000e18a8
    bool HandleCollision(cSC4Automaton* other, const cS3DVector3& point) override;   // 0x208 @ 0x1000e229e
    const char* GetDebugExtraText() const override;                          // 0x320 @ 0x1000dceb6
    bool DoPlayerDriveTick(float milliseconds) override;                     // 0x3d0 @ 0x1000de6c0
    bool DoBumpedTick(float milliseconds) override;                          // 0x3d8 @ 0x1000e23ee
    bool DoCollideTick(float milliseconds) override;                         // 0x3f8 @ 0x1000e23f6
    bool TranslatePlayerDriveCommands(tAutomataStatePlayerDrive* state, float milliseconds) override;   // 0x400
    cSC4Automaton* GetCollidingAutomaton(float distance) override;           // 0x410 @ 0x1000dd7e6
    void RecalculateLeaders() override;                                      // 0x418 @ 0x1000dd81e
    float GetSpeedModifier() const override;                                 // 0x420 @ 0x1000dd852

    // ---- traffic automaton overrides ----
    bool DoPathTick(float milliseconds) override;                            // 0x440 @ 0x1000db2c8
    bool StartTrip(ITrafficAutomatonOccupant* occupant, std::uint32_t key, bool param3) override;   // 0x450
    void SetPath(ITrafficAutomatonOccupant* occupant, std::uint32_t pathKey,
                 const std::vector<cS3DVector3>* path, bool keepDestination) override;   // 0x458
    void HandleNetworkOccupantRemoved() override;                            // 0x470 @ 0x1000ddb08
    const cS3DVector3& GetPathPos() const override { return pathPos_; }      // 0x498 @ 0x100508e48
    float GetSafeDistance() override;                                        // 0x4f0 @ 0x1000dccae
    void RecalculatePathOffset() override;                                   // 0x528 @ 0x1000dd7a6
    ITrafficAutomataManager* GetTrafficAutomataManager() const override;     // 0x5a8 @ 0x1000de4b8

    // ---- vehicle slots ----
    virtual void ShowHeadlights(bool on, bool instant);                      // 0x5b8 @ 0x1000dd3dc
    virtual void ShowMotionBlur(bool on);                                    // 0x5c0 @ 0x1000dd55c
    virtual void SetPathPos(const cS3DVector3& position);                    // 0x5c8 @ 0x10050f570
    virtual const cS3DVector3& GetPrevOrient() const { return prevOrient_; }  // 0x5d0 @ 0x10050f590
    virtual void SetPrevOrient(const cS3DVector3& orientation);              // 0x5d8 @ 0x1000e2600
    virtual void SavePathHistory(std::deque<tLookaheadNode>& history, std::uint8_t keep);   // 0x5e0 @ 0x1000dc0e4
    virtual bool FindBackPosition(const cS3DVector3& position, cS3DVector3& out, float distance,
                                  const std::deque<tLookaheadNode>& history);   // 0x5e8 @ 0x1000dbd64
    virtual const std::deque<tLookaheadNode>& GetPathHistory() const { return pathHistory_; }   // 0x5f0
    virtual void SkipTown();                                                 // 0x5f8 @ 0x1000e2400
    virtual bool DoPlummetTick(float milliseconds);                          // 0x600 @ 0x1000ddc04
    virtual bool DoPullOverTick(float milliseconds);                         // 0x608 @ 0x1000ddf7c
    virtual bool DoPlayerDriveOffRoadTick(float milliseconds, tAutomataStatePlayerDrive* state, float distance,
                                          const tAutomataPlayerDriveParams* params,
                                          ITrafficAutomatonOccupant* occupant);   // 0x610 @ 0x1000e061a (PARTIAL)
    virtual void RestrictPlayerDrivePosition(const cS3DVector3& from, cS3DVector3& position);   // 0x618
    virtual bool GetPlayerDriveLotIntersection(const cS3DVector3& from, cS3DVector3& position, cS3DVector2* outPoint,
                                               cS3DVector2* outDirection);   // 0x620 @ 0x1000e0de0
    virtual bool DoCollisionCheck(cS3DVector3& position, float& amount);     // 0x628 @ 0x1000e1bfa
    virtual void UpdateMotionBlur(float milliseconds);                       // 0x630 @ 0x1000dd5aa

    // ---- non-virtual helpers (no vtable slot in the original) ----
    void ClearMotionBlur();                                                  // @ 0x1000db0ba
    float PlayerDriveUpdateSpeed(tAutomataStatePlayerDrive* state, const tAutomataPlayerDriveParams* params, bool a,
                                 bool b, float milliseconds);            // @ 0x1000de4d0
    ITrafficAutomatonOccupant* PlayerDriveGetNetwork(tAutomataStatePlayerDrive* state, bool& offRoad,
                                                     const cS3DVector3& position, cS3DVector3* out);   // @ 0x1000dfbe4
    void ResolveCollision(const cS3DVector3& normal, float speed, tAutomataStatePlayerDrive* state,
                          const tAutomataPlayerDriveParams* params);         // @ 0x1000dffb6
    const tPath* ChoosePathFromKeyArray(cISC4PathInfo* pathInfo, const std::vector<std::uint32_t>& keys,
                                        cS3DVector3& position, std::uint32_t& key, int& index);   // @ 0x1000e0138
    bool ChooseNewPlayerDriveDestination(cS3DVector3& position);             // @ 0x1000e0228
    void UpdateAfterDamage(tAutomataStatePlayerDrive* state, const tAutomataPlayerDriveParams* params);   // @ 0x1000e03f4
    // Pure geometry (the same three helpers the collision paths use).
    int LineIntersectsSphere(const cS3DVector3& a, const cS3DVector3& b, const cS3DVector3& centre, float radius,
                             cS3DVector3& first, cS3DVector3& second) const;   // @ 0x1000db946
    bool FindPathIntersection(const cS3DVector3& a, const cS3DVector3& b, const cS3DVector3& centre, float radius,
                              cS3DVector3& out, float minCosine) const;        // @ 0x1000dbad6
    void Check2DIntersection(float x0, float z0, float x1, float z1, const cS3DVector3& centre, float radius,
                             cS3DVector3& out, float& best) const;             // @ 0x1000e1592

    // ---- accessors for the tests and the neighbours ----
    IVehicleHost* VehicleHostPtr() const { return vehicleHost_; }
    void SetDriveOccupant(ITrafficAutomatonOccupant* occupant);
    ITrafficAutomatonOccupant* DriveOccupant() const { return driveOccupant_; }
    IVehicleEffect* DriveEffect() const { return driveEffect_; }
    void SetDriveEffect(IVehicleEffect* effect);
    cIGZUnknown* Accident() const { return accident_; }
    void SetAccident(cIGZUnknown* accident);
    IAutomatonModel* HeadlightModel() const { return headlights_[0]; }
    IAutomatonModel* SecondHeadlightModel() const { return headlights_[1]; }
    IAutomatonModel* MotionBlurModel(int index) const {
        return (index >= 0 && index < kVehicleMotionBlurCount) ? motionBlur_[index] : nullptr;
    }
    float HeadlightTimer() const { return headlightTimer_; }
    void SetHeadlightTimer(float t) { headlightTimer_ = t; }
    float PathRemaining() const { return pathRemaining_; }
    void SetPathRemaining(float r) { pathRemaining_ = r; }
    std::deque<tLookaheadNode>& PathHistory() { return pathHistory_; }
    const VehicleTuning& Tuning() const;
    cGZPersistResourceKey LastEffectKey() const { return lastEffectKey_; }

protected:
    void NormalizeLoadedSpeed();                                             // port choice (not in the original), from DoPathTick
    bool loadedSpeedChecked_ = false;                                        // port choice: a loaded vehicle's speed was looked at
    IVehicleHost* vehicleHost_ = nullptr;                                    // port seam (not in the original)
    float headlightTimer_ = 0.0f;                                            // +0x398
    IAutomatonModel* headlights_[2] = {nullptr, nullptr};                    // +0x3a0 / +0x3a8
    float pathRemaining_ = 0.0f;                                             // +0x3b0
    cS3DVector3 pathPos_{kAutomatonUnsetCoordinate, kAutomatonUnsetCoordinate, kAutomatonUnsetCoordinate};   // +0x3b4
    cS3DVector3 prevOrient_{kAutomatonUnsetCoordinate, kAutomatonUnsetCoordinate, kAutomatonUnsetCoordinate};   // +0x3c0
    std::deque<tLookaheadNode> pathHistory_;                                 // +0x3d0
    IAutomatonModel* motionBlur_[kVehicleMotionBlurCount] = {nullptr, nullptr, nullptr, nullptr};   // +0x400
    std::uint8_t motionBlurCursor_ = 0;                                      // +0x420
    // The driving state's run-time pointers the port keeps on the automaton (see the traffic header note): the occupant
    // at the original state's +0xa0, the effect at +0x98 and the accident at +0x80.
    ITrafficAutomatonOccupant* driveOccupant_ = nullptr;
    IVehicleEffect* driveEffect_ = nullptr;
    cIGZUnknown* accident_ = nullptr;
    cGZPersistResourceKey lastEffectKey_{};
};

}  // namespace sc4
