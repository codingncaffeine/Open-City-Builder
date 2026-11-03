// cSC4Aircraft — the shared base of the flying automata (cSC4Airplane and cSC4Helicopter): a cSC4Automaton that
// additionally knows how high it is above the terrain/water/lot under it, keeps itself inside the city bounds, can be
// placed at a random point in the sky (PlaceSelf) and has a player-drive mode that steers its orientation from the
// player-drive state record.
//
// 36 symbols at 0x10050190a..0x1005024d8 (of which 17 are non-virtual thunks). Class id 0xc96e75dc. The class adds only
// two bytes of state to cSC4Automaton:
//   +0x308 float 1.0 (never read by this class; the helicopter/airplane keep their own field there)
//   +0x30c u8 player-drive mode (set by HandleEnterState when the state is 0x10, cleared by HandleExitState)
//
// Original vtables: primary 0x100abaa80 (+8 occupant view 0x100abaed8, +0x10 serializable 0x100abb020, +0x18 property
// holder 0x100abb060, +0x40 exemplar property holder 0x100abb150, +0x68 message target 0x100abb188). QueryInterface
// answers 0xc96e75dc in addition to the cSC4Automaton ids.
//
// Quirks kept:
//  * The ctor stamps occupant flag 0x10 (bit 4 of +0x5b) and the occupant type 0x480e6151 on every aircraft.
//  * Init always sets automata flag 0x2000 (ignore paths) before the automaton's own Init, even for a second Init.
//  * PlaceSelf returns the *self-controlling* flag, so an automaton driven by a controller is never placed.
//  * PlaceSelf places at (random x, terrain height + 500, random z) with city *cell counts* (city vslots 0x2e8/0x2f0),
//    not the world-size floats ClipToWorld uses (0x2c8/0x2d0).
//  * DoAnimationTick calls GetSelfControlling() after the base tick and discards the answer.
//  * DoPlayerDriveTick divides the distance moved by the *unclamped* delta even though it integrates with the delta
//    clamped to 0.2 s (the same quirk as the cSC4Automaton::DoPlayerDriveTick path).
//
// Seams: everything reached through SL::spTerrain, SL::spLotManager, SL::spCity, SL::spEffectsManager,
// cSC4AircraftManager::mpRandom, SL::AEMS and cSC4AircraftManager goes through IAircraftHost below (the original call or
// vtable slot is in each comment). The automaton-level services stay on IAutomatonHost (Automaton.h).
#pragma once
#include <cstdint>
#include <vector>

#include "gz/StreamOps.h"
#include "sc4/Automaton.h"

namespace sc4 {

class cISC4OccupantFilter;

constexpr std::uint32_t kGZCLSID_cSC4Aircraft = 0xc96e75dc;
constexpr std::uint32_t kAircraftOccupantType = 0x480e6151;
constexpr std::uint8_t kAircraftOccupantFlag = 0x10;   // occupant flags (+0x53 of cSC4Occupant)

// The model basis the aircraft code uses (cSC4Airplane.cpp's __GLOBAL__sub_I, 0x1006d5c2f): kModelFacing is +Z and
// the three other vectors are the side/up axes of the model.
constexpr cS3DVector3 kAircraftModelFacing = {0.0f, 0.0f, 1.0f};   // @ 0x100b8fa10
constexpr cS3DVector3 kAircraftModelSide = {0.0f, 1.0f, 0.0f};     // @ 0x100b8fa00
constexpr cS3DVector3 kAircraftModelUp = {1.0f, 0.0f, 0.0f};       // @ 0x100b8fa1c
constexpr cS3DVector3 kAircraftWorldUp = {0.0f, 1.0f, 0.0f};       // @ 0x100b8fa28

// One registered effect instance (the object cSC4ModelRegEffects::FindRegEffect returns and the effects manager creates;
// original slots +0x08 AddRef, +0x10 Release, +0x18 Start(transition), +0x20 Stop(fade), +0x28 IsPlaying,
// +0x38 SetTransform(cS3DTransform), +0x40 SetPosition(cS3DVector3)).
enum class AircraftColliderQuery { kBuildings /* type 0x278128a0 */, kAircraft /* manager filter */, kType_c772bf98 };
struct AircraftCollider {
    cS3DVector3 position;
    float boxMin[3] = {0.0f, 0.0f, 0.0f}, boxMax[3] = {0.0f, 0.0f, 0.0f};
};

class IAirplaneEffect {
public:
    virtual ~IAirplaneEffect() = default;
    virtual void AddRef() = 0;                                    // +0x08
    virtual void Release() = 0;                                   // +0x10
    virtual void Start(int transition) = 0;                       // +0x18
    virtual void Stop(bool fade) = 0;                             // +0x20
    virtual bool IsPlaying() = 0;                                 // +0x28
    virtual void SetTransform(const cS3DTransform& transform) = 0;   // +0x38
    virtual void SetPosition(const cS3DVector3& position) = 0;     // +0x40
};

class IAircraftHost {
public:
    virtual ~IAircraftHost() = default;
    // ---- SL::spTerrain ---------------------------------------------------------------------------------------------
    virtual bool TerrainPointInBounds(float x, float z) = 0;      // slot 0x58
    virtual float TerrainHeight(float x, float z) = 0;            // slot 0x60
    virtual float WaterHeight(float x, float z) = 0;              // slot 0xf8
    virtual float DefaultAltitude() = 0;                          // slot 0xe8 (query failed)
    virtual float AltitudeClamp() = 0;                            // slot 0xb0 (ClipToWorld's upper margin base)
    virtual float TallestBuildingAltitude() = 0;                  // SL::spEffectsManager slot 0x50
    // cSC4AircraftManager (SL::spCity slot 0x168): the runway end points near `position`, two per runway
    virtual void GetRunwayList(const cS3DVector3& position, std::vector<cS3DVector3>& out) = 0;
    // cSC4AircraftManager::mpRandom->RandomUint32WeightedChoice(count, weights)
    virtual std::uint32_t RandomWeightedChoice(std::uint32_t count, const double* weights) = 0;
    // SL::spLotManager slot 0x30: the lot at a point, compared only by identity (null when none)
    virtual const void* LotAt(const cS3DVector3& position) = 0;
    virtual float OpacityFadeSeconds() = 0;                        // automata tuning params +0x134
    virtual bool CellHasWater(std::uint32_t cellX, std::uint32_t cellZ) = 0;   // SL::spTerrain slot 0x110
    // DetectCollisions' occupant-manager queries over a box (slot 0x120 by occupant type, slot 0x128 with the aircraft
    // manager's filter at +0xb38): whether anything is there, and the first hit's position (its slot 0x40) and bounds
    // (slot 0x50).
    virtual bool FindCollider(AircraftColliderQuery query, const float* boxMin, const float* boxMax,
                              AircraftCollider& hit) = 0;
    // ---- SL::spLotManager -------------------------------------------------------------------------------------------
    // lot manager slot 0x30 finds the lot at the point; if the lot's slot 0x60 has bit 0 set its slot 0x80 is the
    // altitude GetAltitude returns instead of the terrain/water maximum.
    virtual bool LotAltitudeAt(const cS3DVector3& position, float* altitude) = 0;
    // ---- SL::spCity -------------------------------------------------------------------------------------------------
    virtual float CitySizeX() = 0;                                // slot 0x2c8
    virtual float CitySizeZ() = 0;                                // slot 0x2d0
    virtual int CityCellCountX() = 0;                             // slot 0x2e8 (PlaceSelf's random range)
    virtual int CityCellCountZ() = 0;                             // slot 0x2f0
    // occupant manager slot 0x88: world (x, z) -> cell coordinates (DoPlayerDriveTick)
    virtual void WorldToCell(float x, float z, std::int32_t& cellX, std::int32_t& cellZ) = 0;
    // occupant manager slot 0x128: first occupant of `filter` in the rectangle, then slot 0x30 = its position.
    virtual bool FindFireOccupant(const cS3DVector3& minCorner, const cS3DVector3& maxCorner,
                                  cISC4OccupantFilter* filter, cS3DVector3& position) = 0;
    // ---- cSC4AircraftManager::mpRandom (cRZRandomSuper::RandomDoubleRangeUniform) ------------------------------------
    virtual double RandomDoubleRange(double lo, double hi) = 0;
    // ---- automata script group template parameter blocks ------------------------------------------------------------
    // The group template's +0xe0 pointer: 18 floats (LoadDriveParameters). nullptr = keep the defaults.
    virtual const float* FindDriveParameters(IAutomatonGroupTemplate* group) = 0;
    // The group template's +0xe8 pointer (the player-drive block; LoadPlayerDriveParameters). nullptr = keep current.
    virtual const float* FindPlayerDriveParameters(IAutomatonGroupTemplate* group) = 0;
    // ---- effects (SL::spEffectsManager slot 0x30 = create a named effect; returns one reference) ---------------------
    virtual IAirplaneEffect* CreateEffect(const cRZString& name) = 0;
    // cS3DRegPoints look-up used to initialise the skywriting position (Init). False when the point is absent.
    virtual bool FindRegPointTransform(const cRZString& name, cS3DVector3& out) = 0;
    // SL::spEffectsManager slot 0x48 -> the returned object's slot 0x30: water/effect height at a point
    // (EvaluateClearance samples this when the point is inside the terrain).
    virtual float EffectsWaterHeight(float x, float z) = 0;
    // ---- process-wide services --------------------------------------------------------------------------------------
    virtual bool DisasterActive() = 0;                            // SL::spDisasterLayer slot 0x50
    virtual float DisasterSpeedMultiplier() = 0;                  // automata tuning params +0x1dc
    virtual bool SimulatorPaused() = 0;                           // SL::spSimulator slot 0x78
    virtual bool SimulatorAdvancing() = 0;                        // SL::spSimulator slot 0x88
    virtual int ViewZoom() = 0;                                   // SL::spView3D slot 0xb0
    virtual void PlayAudioEvent(std::uint32_t event) = 0;         // SL::AEMS slot 0x28
    virtual void StopAudioEvent(std::uint32_t event) = 0;         // SL::AEMS slot 0x30
};

IAircraftHost* AircraftHost();
void SetAircraftHost(IAircraftHost* host);

class cSC4Aircraft : public cSC4Automaton {
public:
    explicit cSC4Aircraft(IAutomatonHost* host = nullptr, IAircraftHost* aircraftHost = nullptr);
    ~cSC4Aircraft() override;

    // ---- cIGZUnknown via cSC4Occupant ----
    bool QueryInterface(std::uint32_t iid, void** out) override;      // @ 0x100501a34
    bool Init() override;                                            // @ 0x1005019b4
    bool Shutdown() override;                                        // @ 0x100501a1c
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4Aircraft; }   // @ 0x100502440

    // ---- cSC4Aircraft ----
    bool PlaceSelf();                                                // @ 0x100501afc
    bool Write(cIGZOStream& stream) const override;                  // @ 0x100501bf2
    bool Read(cIGZIStream& stream) override;                         // @ 0x100501ca6
    bool HandleEnterState(tAutomataState* state) override;           // @ 0x100501dc2
    bool HandleExitState(const tAutomataState* state) override;      // @ 0x100501e36
    bool DoAnimationTick(std::uint32_t data, float milliseconds) override;   // @ 0x100501e52
    bool DoPlayerDriveTick(float milliseconds);                      // @ 0x100501e78
    float GetAltitude(const cS3DVector3& position) const;            // @ 0x100502098
    void ClipToWorld(cS3DVector3& position) const;                   // @ 0x100502160

    IAircraftHost* AircraftHostPtr() const { return aircraftHost_; }
    bool IsPlayerDriving() const { return playerDriving_; }          // +0x30c
    float Field308() const { return field308_; }                     // +0x308

protected:
    IAircraftHost* aircraftHost_ = nullptr;   // port seam (not in the original)
    float field308_ = 1.0f;                   // +0x308
    bool playerDriving_ = false;              // +0x30c
};

}  // namespace sc4
