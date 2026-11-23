// cSC4Helicopter — the rotor aircraft automaton (police / fire / news / player helicopters). 53 symbols at
// 0x1006e848a..0x1006f214f, of which 14 are non-virtual thunks; class id 0x8ba98eda. It derives from cSC4Aircraft
// (Aircraft.h) and adds the whole rotor-craft model:
//  * HELI_DATA, the 0x218-byte physics block: ten HELI_ROTOR objects (linked list + two transforms each), two body
//    transforms, the six-degree-of-freedom state (position, velocity, yaw/pitch/roll angles and rates, engine power) and
//    the ten AI control outputs (collective/lift/rudder);
//  * HELI_ENGINE (0x40 bytes) built on the first PhysicsInit, holding the AI control inputs and the accumulated force;
//  * the flight state machine (0 idle, 1 hover, 4 engine starting, 5 descending, 6 landed) that NextFrame advances and
//    the AI behaviour dispatch (GetBehavior() 0x32..0x38: idle/circle, move, pursue, arrive, respond to crime, traffic
//    accident, land);
//  * two extra cSC4ModelRegEffects-style collections (+0x360 still blades, +0x388 spinning blades) and four created
//    effects (copter_spotlight +0x328, heli_closetoground +0x350, aircraftindicate +0x358, searchlight +0x320 which
//    comes from the model's registered effect list).
//
// Original layout, relative to cSC4Aircraft (which ends at +0x308):
//   +0x30d u8 position was loaded (Read)        +0x310 cS3DVector3 spawn position (also the saved position)
//   +0x31c float spawn altitude (hover reference; OnPlayerTakeOver refreshes it)
//   +0x320 IAirplaneEffect* searchlight ("fx_00_searchlight" registered effect, not created)
//   +0x328 IAirplaneEffect* copter_spotlight     +0x330 cS3DVector3 spotlight target
//   +0x33c cS3DVector3 spotlight aim (constructor (0,-1,1), Init normalises to (0,-1,1)/sqrt2)
//   +0x348 u8 spotlight on                       +0x350 IAirplaneEffect* heli_closetoground
//   +0x358 IAirplaneEffect* aircraftindicate     +0x360 cSC4ModelRegEffects (still rotor)
//   +0x388 cSC4ModelRegEffects (spinning rotor)  +0x3b0 HELI_DATA*
//   +0x3b8 HELI_ENGINE*                          +0x3c0 u8 "arrived at the spawn point" flag
//   +0x3c4 float player-drive yaw rate           +0x3c8 float player-drive pitch/collective rate
//   +0x3cc float player-drive rudder rate        +0x3d0 float player-drive slow-multiplier
//   +0x3d4 u16 call number                       +0x3d8 u32 dispatch state (cISC4DispatchManager::tDispatchState)
//
// HELI_DATA field offsets are the original ones (static_assert'ed in the .cpp); the two HELI_ROTOR transforms are the
// 0x38-byte cS3DTransform layout (u8 change counter, u8 flags, 3x3 rotation, translation, scale) that the original
// stamps with flags 4 on rotation changes and 1 on translation changes.
//
// Quirks kept:
//  * The constructor leaves +0x320/+0x328/+0x350/+0x358/+0x3b0/+0x3b8 null but writes the *unnormalised* spotlight aim
//    (0,-1,1); Init normalises it to (0, -1/sqrt2, 1/sqrt2).
//  * Init looks the skywriting-style registration point "fx_00_helipad" up through the first model's reg-point map and
//    subtracts it rotated by the model heading from the spawn position; the reg point is only used on the first Init
//    after a fresh construction (+0x30d false).
//  * Init's rotor registered-effect points are the literal, misspelled model points "helibladetsill"/"heliblade" (and
//    "mhelibladetsill"/"mheliblade" for occupant group 0x4307) — the binary really contains the "ts" transposition.
//  * PlaceSelf draws its random spawn from the *middle* of the city (0.5..0.75 of cellCount*cellSize) and then computes
//    a heading with GetNewDirection that it throws away (only the position is stored).
//  * PhysicsInit truncates the spawn x/z to int16 before storing the "last position" (+0x14/+0x18).
//  * PhysicsInit only initialises two of the ten rotors' rotations up front (the rest keep their constructor identity)
//    and leaves rotor +0xc8's position unset.
//  * The rotor links at +0x00 and +0x08 of every HELI_ROTOR are written with the same successor (the original builds
//    the list twice into two adjacent pointers).
//  * NextFrame's state 5 transition happens only through the int at +0xf8 < 0 and the landing check compares
//    position.y against GetAltitude of the *rotor position*; when already exactly at the ground it does not land.
//  * CalculateEnginePower ramps the engine 100/s up and down, never above 400, and adds a quadratic vertical-velocity
//    term whose sign flips above/below 0.
//  * YawPitchRollSlideReal clamps the rates by the type limits *before* integrating, wraps yaw at +/-3600 degrees
//    (not 360), and clamps roll/pitch to +/-890 and to the type limits.
//  * AI_ControlHover's error term adds the *already-accumulated* vertical speed back, so it over/under-shoots.
//  * AI_ControlMoveXZ moves each control at most delta x 100 per call; with the request behind and the helicopter at speed
//    it only brakes, and flying fast across the request it chases the velocity in the rotor frame (documented in the .cpp).
//  * UpdateSpotlight keeps the aim at (0,-1,1)/sqrt2 unless the "fx_00_spotlight" registered effect is on the stack and
//    the engine is enabled at zoom 1, and only then rotates it towards the tracked position.
//  * DetectCollisions' sweep uses the occupant's GetBoundingBox, pushes the helicopter out along the axis with the
//    smallest penetration, and adds random +/-3 deg kicks from cSC4Automaton::sRandom.
//  * TranslatePlayerDriveCommands writes the collective/yaw inputs with -100..100 limits and, when the player-drive
//    record's effect timer has expired, starts "helicoptermissle" / "ufolaser" before clearing +0x22b.
//
// PARTIAL (each with the concrete missing neighbour):
//  * UpdateSpotlight / TranslatePlayerDriveCommands: the renderer spotlight cone (SL::spRenderer slots 0x1e0/0x1e8) and
//    the player-drive state's tracked-effect fields are behind IHelicopterHost; the effect start/stop and the aim
//    update are reproduced.
//  * DetectCollisions: the occupant-manager rectangle query and the obstacle bounding box go through
//    IHelicopterHost (slots 0x120/0x128/0x180); the city fire-filter object (city slot 0x168) and the queue-state
//    (+0x3c) special case are not reproduced.
// The exact list is repeated under PARTIAL in notes/ledger_pending/cSC4Helicopter.txt.
//
// Seams: everything reached through the occupant manager rectangle queries (0x120/0x128/0xe0/0x180), the lot manager
// (0x30), the renderer spotlight cones (0x1e0/0x1e8), the city cell-centre helper (0x308) / dispatch manager (0x178),
// the crime simulator (0x40) and the effects manager's point query (0x48 -> 0x30) goes through IHelicopterHost below
// (the original call or vtable slot is in each comment). The aircraft-level services stay on IAircraftHost
// (Aircraft.h) and the automaton-level ones on IAutomatonHost (Automaton.h).
#pragma once
#include <cstdint>

#include "gz/StreamOps.h"
#include "sc4/Aircraft.h"
#include "sc4/MiscTypes.h"

namespace sc4 {

class cISC4OccupantFilter;

constexpr std::uint32_t kGZCLSID_cSC4Helicopter = 0x8ba98eda;
constexpr std::uint32_t kHelicopterOccupantType = 0x278128a0;   // occupant-manager type of the collision queries
constexpr std::uint32_t kHelicopterGroupSpinning = 0x4307;      // "mheli..." rotor points (otherwise "heli...")
constexpr std::uint32_t kHelicopterGroupPlayer = 0x4308;        // player-drive AEMS event group

// HELI_DATA::state values (SetHelicopterState / NextFrame / AI_StartEngine).
enum class HelicopterState : std::int32_t {
    kIdle = 0,        // on the ground / engine off
    kHover = 1,       // engine running, hovering
    kStarting = 4,    // engine starting
    kDescending = 5,  // touching down
    kLanded = 6,      // down and stopped
};

// The behaviour ids AI_PursueGoal dispatches on (cISC4Automaton::GetBehavior): 0x32 idle, 0x33 move to the spawn
// point, 0x34 pursuit, 0x35 arrive/hold station, 0x36 respond to a crime, 0x37/0x38 traffic accident, and 0 (none).
constexpr std::uint32_t kHeliBehaviorIdle = 0x32;
constexpr std::uint32_t kHeliBehaviorSpawn = 0x33;
constexpr std::uint32_t kHeliBehaviorPursue = 0x34;
constexpr std::uint32_t kHeliBehaviorArrive = 0x35;
constexpr std::uint32_t kHeliBehaviorCrime = 0x36;
constexpr std::uint32_t kHeliBehaviorTraffic = 0x37;
constexpr std::uint32_t kHeliBehaviorTraffic2 = 0x38;

// The global the spotlight code rotates from (kDefaultSpotlightAim @ 0x100b8fd98: (0, -1, 0)).
constexpr cS3DVector3 kDefaultSpotlightAim = {0.0f, -1.0f, 0.0f};

// The kUninit3DVector sentinel the constructor stores in the destination (+0x80) and that the claimed-position code
// compares against (kInvalidVector at 0x100b8fdc4, DAT_100b8fdcc / DAT_100b8fdd0 next to it).
constexpr std::uint32_t kHeliInvalidVectorBits[3] = {0xffffffffu, 0x01ffffffu, 0x4f5f4090u};

// The 0x38-byte transform the rotor objects embed twice (cS3DTransform layout: +0 counter, +1 flags, +4 rotation,
// +0x28 translation, +0x34 scale).
struct HeliTransform {
    std::uint8_t counter = 0;                          // +0x00
    std::uint8_t flags = 0;                            // +0x01 (1 translation changed, 4 rotation changed)
    std::uint8_t pad[2] = {};
    float rotation[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};   // +0x04
    float translation[3] = {0, 0, 0};                          // +0x28
    float scale = 1.0f;                                        // +0x34

    // The original's "if dirty, reset to identity" block: clears flags, bumps the change counter (no-op otherwise).
    void ResetIfDirty();
    void SetRotation(const float (&matrix)[3][3]);   // stamps flags |= 4 and bumps the counter
    void SetTranslation(const cS3DVector3& value);   // stamps flags |= 1 and bumps the counter
};

// A rotor: two identical forward links, a position and two transforms. 0xb0 bytes.
struct HELI_ROTOR {
    HELI_ROTOR* link0 = nullptr;    // +0x00
    HELI_ROTOR* link1 = nullptr;    // +0x08 (same successor as link0 in the original)
    std::uint32_t pad10 = 0;        // +0x10
    std::uint32_t pad14 = 0;        // +0x14
    std::uint16_t axis = 0;         // +0x18 (5 for the main rotor, 0x21 for the others)
    std::uint16_t field1a = 0;      // +0x1a
    std::uint32_t pad1c = 0;        // +0x1c
    std::uint64_t pad20 = 0;        // +0x20
    std::uint32_t pad28 = 0;        // +0x28
    std::uint32_t pad2c = 0;        // +0x2c
    cS3DVector3 position;           // +0x30 (world position / 0.3, i.e. in model units)
    HeliTransform body;             // +0x3c
    HeliTransform blade;            // +0x74
    std::uint32_t padAc = 0;        // +0xac
};

// The engine/AI block built by PhysicsInit (0x40 bytes).
struct HELI_ENGINE {
    float collective = 0.0f;    // +0x00 driven to 0 by AI_ControlReset
    float field04 = 0.0f;       // +0x04 driven to 0 by AI_ControlReset
    float liftControl = 0.0f;   // +0x08 vertical control (AI_ControlHover), -150..100
    float rudder = 0.0f;        // +0x0c driven to 0 by AI_ControlReset
    float axisX = 0.0f;         // +0x10 temporary up-axis / -collective
    float axisY = 0.0f;         // +0x14 (1000 when built from the controls)
    float axisZ = 0.0f;         // +0x18 temporary up-axis / -field04
    float upX = 0.0f;           // +0x1c rotor up axis (normalised)
    float upY = 0.0f;           // +0x20
    float upZ = 0.0f;           // +0x24
    float power = 0.0f;         // +0x28 engine power written by CalculateEnginePower
    float throttle = 0.0f;      // +0x2c engine ramp 0..1
    float tilt = 0.0f;          // +0x30 rotor tilt factor
    float forceX = 0.0f;        // +0x34 accumulated force
    float forceY = 0.0f;        // +0x38
    float forceZ = 0.0f;        // +0x3c
};

// The 0x218-byte physics block (allocated by InitInstance, freed by PhysicsShutdown).
struct HELI_DATA {
    HELI_TYPE_DATA* type = nullptr;   // +0x00
    std::int32_t state = 0;           // +0x08 (HelicopterState)
    std::uint8_t flags = 0;           // +0x0c (bit 0 = position follows the rotor, bit 1 = ?)
    std::uint8_t pad0d = 0;           // +0x0d
    std::uint16_t pad0e = 0;          // +0x0e
    std::int32_t direction = 0;       // +0x10 (-1/0/1, the sign of the vertical velocity)
    float startX = 0.0f;              // +0x14 (last position, truncated to int16 by PhysicsInit)
    float startZ = 0.0f;              // +0x18
    float currentX = 0.0f;            // +0x1c
    float currentZ = 0.0f;            // +0x20
    HeliTransform body;               // +0x24
    HeliTransform blade;              // +0x5c
    HELI_ROTOR* rotors[10] = {};      // +0x98..+0xe0 (0x98,0xa0,0xa8,0xb0,0xb8,0xc0,0xc8,0xd0,0xd8,0xe0)
    std::uint32_t padE8 = 0;          // +0xe8
    std::uint32_t padEc = 0;          // +0xec
    std::uint32_t padF0 = 0;          // +0xf0
    std::uint32_t padF4 = 0;          // +0xf4
    std::int32_t altitudeState = 0;   // +0xf8 (<0 = too low, drives the state machine)
    float fieldFc = 0.0f;             // +0xfc
    // The `mv` record (mass + velocity) that Apply_Force writes.
    float mass = 0.0f;                // +0x100
    float speed = 0.0f;               // +0x104 horizontal speed
    float velX = 0.0f;                // +0x108
    float velY = 0.0f;                // +0x10c
    float velZ = 0.0f;                // +0x110
    float inertia = 0.0f;             // +0x114 (mass copy used by the angular integration)
    float speedOut = 0.0f;            // +0x118 (Apply_Force's speed output)
    float pitchRate = 0.0f;           // +0x11c
    float yawRate = 0.0f;             // +0x120
    float rollRate = 0.0f;            // +0x124
    cS3DVector3 heading{0.0f, 0.0f, 1.0f};   // +0x128 normalised velocity direction
    float yawDeg = 0.0f;              // +0x134
    std::int32_t diveTimer = 0;       // +0x138 (>0 counts down and overrides the horizontal speed)
    float pitchDeg = 0.0f;            // +0x13c
    float field140 = 0.0f;            // +0x140
    float rollDeg = 0.0f;             // +0x144
    float rollClamped = 0.0f;         // +0x148
    std::uint32_t pad14c = 0;         // +0x14c
    std::uint32_t pad150 = 0;         // +0x150
    float pitchClamped = 0.0f;        // +0x154
    float verticalSpeed = 0.0f;       // +0x158
    float climbRate = 0.0f;           // +0x15c
    float speedXZ = 0.0f;             // +0x160
    float speedDelta = 0.0f;          // +0x164
    float moveX = 0.0f;               // +0x168
    float moveZ = 0.0f;               // +0x16c
    float heightAboveGround = 0.0f;   // +0x170
    std::int32_t field174 = 0;        // +0x174
    std::uint32_t pad178 = 0;         // +0x178
    std::int32_t field17c = 0;        // +0x17c
    std::int32_t counter180 = 0;      // +0x180
    float enginePower = 0.0f;         // +0x184 (0..400)
    float field188 = 0.0f;            // +0x188
    float groundAltitude = 0.0f;      // +0x18c (GetAltitude / 0.3)
    float obstacleAltitude = 0.0f;    // +0x190 (bottom of the occupant under the point / 0.3)
    std::uint32_t pad194 = 0;         // +0x194
    std::uint32_t pad198 = 0;         // +0x198
    std::uint32_t pad19c = 0;         // +0x19c
    std::uint32_t pad1a0 = 0;         // +0x1a0
    float landingTimer = 0.0f;        // +0x1a4 (0.8 s on touchdown)
    std::uint32_t field1a8 = 0;       // +0x1a8
    std::int32_t grounded = 0;        // +0x1ac
    std::uint32_t pad1b0[16] = {};    // +0x1b0..+0x1ef
    // +0x1f0..+0x217: engine bookkeeping written by PhysicsInit/NextFrame (u64 halves).
    std::uint32_t field1f0 = 0;       // +0x1f0 (0x11)
    std::uint32_t field1f4 = 0;       // +0x1f4 (1)
    std::uint32_t field1f8 = 0;       // +0x1f8 (1)
    std::uint32_t field1fc = 0;       // +0x1fc
    std::uint32_t field200 = 0;       // +0x200
    std::uint32_t field204 = 0;       // +0x204
    float field208 = 0.0f;            // +0x208 (type+0x40 at init)
    float field20c = 0.0f;            // +0x20c
    float flightRatio = 0.0f;         // +0x210
    std::int32_t engineEnabled = 0;   // +0x214 (1 when the engine runs; the "hover" flag)
};

// ---------------------------------------------------------------------------------------------------------------------
// Seams
// ---------------------------------------------------------------------------------------------------------------------

// One occupant returned by the collision sweep (occupant manager 0x120/0x128/0xe0). The original uses the occupant's
// +0x08 AddRef, +0x10 Release and +0x50 GetBoundingBox (via the cISC4Occupant sub-object).
class IHelicopterObstacle {
public:
    virtual ~IHelicopterObstacle() = default;
    virtual void AddRef() = 0;                                          // occupant vslot +0x08
    virtual void Release() = 0;                                         // vslot +0x10
    virtual void GetBounds(cS3DVector3& minCorner, cS3DVector3& maxCorner) = 0;   // vslot +0x50
};

// The two extra cSC4ModelRegEffects collections (+0x360/+0x388). The original calls StartAllEffects /
// StopAllEffects with a cISC4VisualEffect::tTransitionType (0 = instant, 1 = fade).
class IHelicopterEffects {
public:
    virtual ~IHelicopterEffects() = default;
    virtual void SetModel(IAutomatonModel* model) = 0;                              // cSC4ModelRegEffects::SetModel
    virtual bool Init() = 0;
    virtual void Update() = 0;                                                     // cSC4ModelRegEffects::Update
    virtual void StartAll(int transition) = 0;                                     // StartAllEffects(transition)
    virtual void StopAll(int transition) = 0;                                      // StopAllEffects(transition)
    virtual bool StartNamed(const cRZString& effect, int transition, bool loop) = 0;   // 0x1001e49f0
    virtual void StopNamed(const cRZString& effect, int transition) = 0;           // 0x1001e4af4
    virtual bool AddRegEffect(const cRZString& effect, const cRZString& point) = 0;   // 0x1001e4e36
    virtual void Shutdown() = 0;
};

class IHelicopterHost {
public:
    virtual ~IHelicopterHost() = default;
    // ---- cSC4ModelRegEffects ctor / dtor for the two rotor collections ----------------------------------------------
    virtual IHelicopterEffects* CreateEffects() = 0;
    virtual void DestroyEffects(IHelicopterEffects* effects) = 0;
    // The registered-effect instance's GetTransform (vslot 0x78 of the object IAirplaneEffect models).
    virtual void GetEffectTransform(IAirplaneEffect* effect, cS3DTransform& transform) = 0;
    // ---- occupant manager -------------------------------------------------------------------------------------------
    // slot 0xe0: first occupant of `type` at the point (MoveYReal's "building under the helicopter" query).
    virtual IHelicopterObstacle* FindObstacleAt(float x, float z, std::uint32_t type) = 0;
    // slot 0x120 (filter == nullptr) / 0x128 (filter != nullptr): first occupant in the box. DetectCollisions uses the
    // first one only and releases the list through slot 0x180 afterwards.
    virtual IHelicopterObstacle* FindObstacleInBox(const cS3DVector3& minCorner, const cS3DVector3& maxCorner,
                                                   std::uint32_t type, cISC4OccupantFilter* filter) = 0;
    virtual void EndObstacleQuery() = 0;                                            // slot 0x180
    // ---- lot manager slot 0x30 (pointer identity is all DetectCollisions reads) -------------------------------------
    virtual const void* LotAtPoint(const cS3DVector3& point) = 0;
    // ---- the renderer's terrain picks (cSC43DRender::PickTerrain by ray, slot 0x1e0, and by viewport point, slot 0x1e8):
    // where the spotlight meets the ground, written to `target`; false when it misses -----------------------------------
    virtual bool RendererSpotlightCone(const cS3DVector3& origin, const cS3DVector3& direction, cS3DVector3& target) = 0;
    virtual bool RendererSpotlightHit(float x, float z, cS3DVector3& target) = 0;
    // ---- city -------------------------------------------------------------------------------------------------------
    virtual float CityCellSizeX() = 0;                                             // city slot 0x2d8
    virtual float CityCellSizeZ() = 0;                                             // city slot 0x2e0
    virtual void CityCellCentre(float x, float z, float& outX, float& outZ) = 0;    // city slot 0x308
    // city slot 0x178 (the automata controller manager): state 5 -> vslot 0x28 AddControllerFromTemplateName(occupant,
    // "riot_police_repulsor"), any other state -> vslot 0x70 RemoveControllers(occupant).
    virtual void DispatchManagerReport(cISC4Occupant* occupant, int state) = 0;
    // slot 0x158 -> cSC4TrafficEffectManager::TrafficEffectManagerGetMostRecentAccidentLocation (0x10011b6b6).
    virtual bool TrafficAccidentLocation(cS3DVector3& position) = 0;
    // ---- crime simulator slot 0x40 (0x100213f58 is Reset; the query used here returns true with a position) ---------
    virtual bool CrimeAt(const cS3DVector3& position, float radius, cS3DVector3& crime) = 0;
    // RespondToCrimeScene's crime simulator calls (SL::spCrimeSim i386 0x24 GetNearestCrimeSim / 0x28 GetNearestCrimeProp): the
    // criminal and the idle crime prop nearest the point (borrowed, or null); the criminal held (its vslot 0x84 SetOpacity(0, 0.2),
    // then vslot 0xd0 PushState(7, none, 5000, true)); a prop's state set (vslot 0x2c SetState).
    virtual void* NearestCrimeSim(const cS3DVector3& position, float radius) { (void)position; (void)radius; return nullptr; }
    virtual void* NearestCrimeProp(const cS3DVector3& position, float radius) { (void)position; (void)radius; return nullptr; }
    virtual void HoldCriminal(void* automaton) { (void)automaton; }
    virtual void SetCrimePropState(void* prop, int state) { (void)prop; (void)state; }
    // sTuningParams i386 +0xbc (the port's cSC4AutomataTuningParams::f134, 1 by default): the pursuit's fade time in seconds.
    virtual float FadeSeconds() { return 1.0f; }
    // SL::spTrafficSim vslot 0x28 GetCongestionMap: its tract widths (vslots 0x38 / 0x3c GetTractWidthX / Z), a tract's value
    // (vslot 0x58 GetTractValue; 0 outside the map) and a tract's center (vslot 0x54 TractCenterToPosition). False without a map.
    virtual bool CongestionMapWidths(float& widthX, float& widthZ) { widthX = widthZ = 0.0f; return false; }
    virtual int CongestionAt(int tractX, int tractZ) { (void)tractX; (void)tractZ; return 0; }
    virtual void CongestionTractCenter(int tractX, int tractZ, float& x, float& z) { (void)tractX; (void)tractZ; x = z = 0.0f; }
};

IHelicopterHost* HelicopterHost();
void SetHelicopterHost(IHelicopterHost* host);

class cSC4Helicopter final : public cSC4Aircraft {
public:
    explicit cSC4Helicopter(IAutomatonHost* host = nullptr, IAircraftHost* aircraftHost = nullptr,
                            IHelicopterHost* helicopterHost = nullptr);
    ~cSC4Helicopter() override;

    // ---- cSC4Aircraft / cSC4Automaton overrides --------------------------------------------------------------------
    bool Init() override;                                            // @ 0x1006e86d6
    bool Shutdown() override;                                        // @ 0x1006ea340
    bool Write(cIGZOStream& stream) const override;                  // @ 0x1006ea8b2
    bool Read(cIGZIStream& stream) override;                         // @ 0x1006ea92c
    void SetOrientation(const float* direction);                     // @ 0x1006f1b44 (vslot 0x70)
    bool DoAnimationTick(std::uint32_t data, float milliseconds) override;   // @ 0x1006eacc0 (vslot 0x278)
    bool DoPlayerDriveTick(float milliseconds) override;             // @ 0x1006f0222 (vslot 0x3d0)
    bool HandleEnterState(tAutomataState* state) override;           // @ 0x1006f1a44 (vslot 0x3a8)
    bool HandleExitState(const tAutomataState* state) override;      // @ 0x1006f1ac6 (vslot 0x3b0)
    void UpdateEffects();                                            // @ 0x1006f1756 (vslot 0x378)
    void SetDispatchState(std::uint32_t state);                      // @ 0x1006f1d70 (vslot 0xc8)
    std::uint32_t GetDispatchState() const;                          // @ 0x1006f1ece (vslot 0xd0)
    std::uint16_t GetCallNumber() const;                             // @ 0x1006f1eb2 (vslot 0xb8)
    void SetCallNumber(std::uint16_t callNumber);                    // @ 0x1006f1ec0 (vslot 0xc0)
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4Helicopter; }   // @ 0x1006f1f30

    // ---- class-specific vtable slots -------------------------------------------------------------------------------
    bool PlaceSelf();                                                // @ 0x1006ea9c2 (vslot 0x440)
    std::uint32_t GetModelResourceInstance() const { return 0x10c10000; }   // @ 0x1006eacb4 (vslot 0x448)
    void GetNewDirection(const cS3DVector3& position, cS3DVector3& direction);   // @ 0x1006eab34 (vslot 0x450)
    void LoadModels();                                               // @ 0x1006eacaa (vslot 0x340 = base)

    // ---- physics / AI ----------------------------------------------------------------------------------------------
    void InitInstance(int typeIndex);                                // @ 0x1006e9054
    void PhysicsInit(HELI_DATA* data, const cS3DVector3& position);  // @ 0x1006e9978
    void PhysicsShutdown();                                          // @ 0x1006ea72e
    void LoadPlayerDriveParameters(HELI_TYPE_DATA* type);            // @ 0x1006ecb6a
    void LoadDriveParameters(HELI_TYPE_DATA* type);                  // @ 0x1006ecc6a
    void SetHelicopterState(std::uint32_t state);                    // @ 0x1006ece66
    void NextFrame(HELI_DATA* data, float delta);                    // @ 0x1006ed0ac
    void CalculateEnginePower(HELI_DATA* data, float delta);         // @ 0x1006ed36a
    void YawPitchRollSlideReal(HELI_DATA* data, float delta);        // @ 0x1006ed524
    void MoveXZReal(HELI_DATA* data, float delta);                   // @ 0x1006ee136
    void MoveYReal(HELI_DATA* data, float delta);                    // @ 0x1006ee796
    void WeathervaneTransverseForce(HELI_DATA* data, cS3DVector3* force);   // @ 0x1006ee9ee
    void Get3dDrag(float throttle, HELI_DATA* data, cS3DVector3* drag, float altitude);   // @ 0x1006eeb90
    float LiftCoeff(HELI_DATA* data);                                // @ 0x1006eed5a
    void ApplyForce(const cS3DVector3& force, float* massVelocity, float delta, const cS3DVector3& drag,
                    float* speedOut);                                // @ 0x1006eee70
    void UpdatePhysics(float delta);                                 // @ 0x1006ebe98
    void UpdateSpotlight();                                          // @ 0x1006ec416
    void DetectCollisions();                                         // @ 0x1006ef0b8
    void AI_PursueGoal(float delta);                                 // @ 0x1006eadf8
    void AI_ControlReset(float delta);                               // @ 0x1006eef38
    void AI_ControlHover(float targetAltitude, float delta);         // @ 0x1006f0174
    void AI_StartEngine();                                           // @ 0x1006f0354
    void AI_AimSpotlight(const cS3DVector3& target, float delta);    // @ 0x1006f03a4
    void AI_ControlMoveXZ(const cS3DVector3& direction, float maxControl, float delta);   // @ 0x1006f0af4
    void AI_ControlLiftOverBuildings(float delta);                   // @ 0x1006f130c
    void AI_ControlMaintainPositionXZ(float delta);                  // @ 0x1006f1582
    void OnPlayerTakeOver();                                         // @ 0x1006f19cc
    bool TranslatePlayerDriveCommands(tAutomataStatePlayerDrive* state, float milliseconds) override;   // @ 0x1006ef960
    void RespondToCrimeScene();                                      // @ 0x1006f149c  PARTIAL

    // ---- accessors for the neighbours and the test -----------------------------------------------------------------
    HELI_DATA* Physics() const { return physics_; }
    HELI_ENGINE* Engine() const { return engine_; }
    IHelicopterHost* HelicopterHostPtr() const { return helicopterHost_; }
    bool PositionLoaded() const { return positionLoaded_; }          // +0x30d
    bool SpotOn() const { return spotOn_; }                          // +0x348
    bool ArrivedFlag() const { return arrived_; }                    // +0x3c0
    float SpawnAltitude() const { return spawnAltitude_; }           // +0x31c
    const cS3DVector3& SpawnPosition() const { return spawnPosition_; }   // +0x310
    const cS3DVector3& SpotTarget() const { return spotTarget_; }    // +0x330
    const cS3DVector3& SpotAim() const { return spotAim_; }          // +0x33c
    IAirplaneEffect* EffectSearchlight() const { return effectSearchlight_; }   // +0x320
    IAirplaneEffect* EffectSpotlight() const { return effectSpotlight_; }       // +0x328
    IAirplaneEffect* EffectCloseToGround() const { return effectCloseToGround_; }   // +0x350
    IAirplaneEffect* EffectIndicate() const { return effectIndicate_; }         // +0x358
    IHelicopterEffects* RotorStill() const { return rotorStill_; }    // +0x360
    IHelicopterEffects* RotorSpin() const { return rotorSpin_; }      // +0x388
    float PlayerYawRate() const { return playerYawRate_; }           // +0x3c4
    float PlayerPitchRate() const { return playerPitchRate_; }       // +0x3c8
    float PlayerRudderRate() const { return playerRudderRate_; }     // +0x3cc
    float PlayerSlowScale() const { return playerSlowScale_; }       // +0x3d0
    float Field31c() const { return spawnAltitude_; }

private:
    IHelicopterHost* helicopterHost_ = nullptr;   // port seam (not in the original)
    bool positionLoaded_ = false;                 // +0x30d
    cS3DVector3 spawnPosition_;                   // +0x310
    float spawnAltitude_ = 0.0f;                  // +0x31c
    IAirplaneEffect* effectSearchlight_ = nullptr;   // +0x320
    IAirplaneEffect* effectSpotlight_ = nullptr;     // +0x328
    cS3DVector3 spotTarget_;                      // +0x330
    cS3DVector3 spotAim_{0.0f, -1.0f, 1.0f};      // +0x33c
    bool spotOn_ = false;                         // +0x348
    IAirplaneEffect* effectCloseToGround_ = nullptr;   // +0x350
    IAirplaneEffect* effectIndicate_ = nullptr;        // +0x358
    IHelicopterEffects* rotorStill_ = nullptr;    // +0x360
    IHelicopterEffects* rotorSpin_ = nullptr;     // +0x388
    HELI_DATA* physics_ = nullptr;                // +0x3b0
    HELI_ENGINE* engine_ = nullptr;               // +0x3b8
    bool arrived_ = false;                        // +0x3c0
    float playerYawRate_ = 0.0f;                  // +0x3c4
    float playerPitchRate_ = 0.0f;                // +0x3c8
    float playerRudderRate_ = 0.0f;               // +0x3cc
    float playerSlowScale_ = 0.0f;                // +0x3d0
    std::uint16_t callNumber_ = 0;                // +0x3d4
    std::uint16_t pad3d6_ = 0;                    // +0x3d6
    std::uint32_t dispatchState_ = 0;             // +0x3d8
};

// HELI_TYPE_DATA is copied wholesale by the original; these helpers read its float fields by byte offset.
inline float HeliTypeFloat(const HELI_TYPE_DATA& type, int byteOffset) {
    float value = 0.0f;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&type);
    for (int i = 0; i < 4; ++i) reinterpret_cast<std::uint8_t*>(&value)[i] = bytes[byteOffset + i];
    return value;
}

}  // namespace sc4
