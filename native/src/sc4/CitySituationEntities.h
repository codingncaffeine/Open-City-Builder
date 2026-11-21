// Entity and service interfaces the city situation code (cSC4CitySituationAdvice / cSC4CitySituationManager) talks to.
// The original reaches all of them through the SL::sp* globals and raw vslots; the port replaces those with the small
// interfaces below (the original vslot offsets are in the comments, [inferred] where the meaning was derived from the
// callers rather than from a symbol). Everything here is a seam: the engine side implements it, the tests fake it.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "gz/cIGZMessage2.h"
#include "gz/cIGZString.h"
#include "gz/cIGZUnknown.h"
#include "rz/cRZString.h"

namespace sc4 {

class ISituationAutomaton;
class cISC4CitySituationAdvice;

// Occupant property that excludes an automaton occupant from the situation system (HandleInsertOccupant).
constexpr std::uint32_t kPropertyNoSituationAutomaton = 0x029244db;

class ISituationPropertyHolder {
public:
    virtual ~ISituationPropertyHolder() = default;
    virtual bool HasProperty(std::uint32_t propertyId) = 0;   // vslot 0x18
    // cSCPropertyHelp::GetPropertyValue(holder, id, u32&): false when the property is missing (the popup's dashboard id).
    virtual bool GetPropertyU32(std::uint32_t propertyId, std::uint32_t& value) { (void)propertyId; (void)value; return false; }
};

// cISC4Occupant
class ISituationOccupant : public cIGZUnknown {
public:
    virtual ISituationPropertyHolder* PropertyHolder() = 0;   // 0x30
    virtual std::uint32_t OccupantType() = 0;                 // 0x38 (e.g. 0x480e6151)
    virtual void Position(float xyz[3]) = 0;                  // 0x40
    virtual void SetPosition(const float xyz[3]) = 0;         // 0x48 [inferred] (coverage target occupant)
    virtual bool IsInGroup(std::uint32_t group) = 0;          // 0xb0
    // 0xb8: adds the occupant to an occupant group (TreatAutomatonAsGroup). Defaulted: the group filters of the
    // existing tests model the read side only.
    virtual void SetGroup(std::uint32_t group) { (void)group; }
    // QueryInterface(0xa9b40f05) without keeping the reference; null when the occupant is not an automaton.
    virtual ISituationAutomaton* AsAutomaton() = 0;
    // QueryInterface(0xa821ef94) = cISC4NetworkOccupant
    virtual bool IsNetworkOccupant() = 0;
    virtual bool NetworkFlagsMatch(std::uint32_t networkFlags) = 0;   // network occupant vslot 0x48
    virtual void NetworkCell(int& x, int& z) = 0;                      // network occupant vslot 0x1d8
    // QueryInterface(0x87dda3a9) then vslot 0x60: the occupant's localized name (GetCurrentTargetName). Defaulted:
    // only the target-name path needs it. Borrowed, may be null.
    virtual cIGZString* DisplayName() { return nullptr; }
};

// cISC4Automaton (the slots checked against the cSC4Automaton vtable; 64-bit offsets, the i386 slot is half)
class ISituationAutomaton : public cIGZUnknown {
public:
    virtual ISituationOccupant* Occupant() = 0;                    // 0x30 AsOccupant
    virtual void Position(float xyz[3]) = 0;                       // 0x38 GetPosition
    virtual void Remove(bool fade) = 0;                            // 0x68 Remove (HandleSituationCancelled drops a created target)
    virtual void GetOrientation(float xyz[3]) = 0;                 // 0x78 GetOrientation (CheckEscapeCitySuccess's heading)
    virtual void SetFlag(std::uint32_t flag, bool on) = 0;         // 0xf0 SetAutomataFlag
    virtual bool CheckFlag(std::uint32_t flag) = 0;                // 0xf8 CheckAutomataFlag
    virtual void AddListener(cIGZMessageTarget2* target) = 0;      // 0x138 AddCallback (the manager's message-target view)
    virtual void RemoveListener(cIGZMessageTarget2* target) = 0;   // 0x140 RemoveCallback
    virtual ISituationAutomaton* StateTarget() = 0;                // 0x1c0 GetState(): the current state's target automaton (+0x38)
    virtual std::uint32_t GetBehavior() = 0;                       // 0x1d8 ((behavior | 0x20) == 0x34 is not driveable)
    virtual void StartSFX(std::uint32_t sfx) = 0;                  // 0x1e8
    // 0x218 GetAutomataGroup: the group's situation indicator image ("csi_image", +0xf0), else the template chain's
    // (cSC4OccupantGroupTemplate::GetSituationIndicatorImageId); 0 = none.
    virtual std::uint32_t SituationIndicatorImageId() = 0;
    virtual void RegisterInGameDataRegistry() = 0;                 // 0x220
    // 0x218 GetAutomataGroup: the player-drive parameters of the group (its own block, else the template chain's): the
    // maximum speed (+0x8). False when the group has none.
    virtual bool PlayerDriveMaxSpeed(float& speed) { (void)speed; return false; }
};

// cISC4AutomataManager (vehicle, aircraft, pedestrian and watercraft managers, in that order)
class ISituationAutomataManager : public cIGZUnknown {
public:
    virtual void Pause() = 0;                                      // 0x48 (the countdown before a drive)
    virtual void Resume() = 0;                                     // 0x50 (the countdown finished)
    virtual bool HandlesGroup(std::uint32_t group) = 0;            // 0xf0 CanCreateGroup
    // Alert nearby automata of `group` to chase `target` within `radius` (0xb0 ChaseAutomaton(group, radius, target, all)).
    virtual void AlertGroup(float radius, std::uint32_t group, ISituationAutomaton* target, bool flag) = 0;
    // 0x100: adds the automata of `group` to `out` (the original appends to a shared unordered_set, so an automaton
    // that belongs to two managers is only kept once). `out` counts as owned by the managers until 0x108 releases it.
    virtual void AppendInstancesOfGroup(std::uint32_t group, std::unordered_set<ISituationAutomaton*>& out) = 0;
    virtual void ReleaseInstances(std::unordered_set<ISituationAutomaton*>& list) = 0;   // 0x108
    // 0x80 GetRandomCreationPoint: finds a free spawn position for a new automaton of `group` between `minDistance` and
    // `maxDistance` of `from`; fills `spawnPosition`.
    virtual bool FindSpawnPosition(float minDistance, float maxDistance, std::uint32_t group, const float from[3], float spawnPosition[3],
                                   std::uint32_t flags) = 0;
    // 0x78: creates an automaton of `group` at `spawnPosition` (no resource key, no listener, argument 0xff); returns a
    // counted reference (or null).
    virtual ISituationAutomaton* CreateAutomaton(std::uint32_t group, const float spawnPosition[3], std::uint32_t flags) = 0;
};

// cSC4DispatchVehicleView (the on-screen indicators)
class ISituationDispatchView {
public:
    virtual ~ISituationDispatchView() = default;
    virtual void ShowAllIndicators(int indicatorType, bool show) = 0;
    virtual void SetIndicatorMax(int indicatorType, int max) = 0;
    virtual std::uint32_t TrackOccupant(ISituationOccupant* occupant, int indicatorType, int indicatorValue, int flags) = 0;
    virtual void UntrackOccupant(std::uint32_t trackId, bool force) = 0;
    virtual void FlashIndicator(std::uint32_t trackId, bool on, float intensity) = 0;
};

// cISC4Lot as the situations need it
class ISituationLot {
public:
    virtual ~ISituationLot() = default;
    virtual void Bounds(int& x0, int& z0, int& x1, int& z1) = 0;   // vslot 0x70
};
class ISituationLotManager {
public:
    virtual ~ISituationLotManager() = default;
    virtual ISituationLot* LotOfOccupant(ISituationOccupant* occupant) = 0;   // vslot 0x70
};

// Transform handed to an effect (vslot 0x40/0x38): a 16-bit header (0x0101 for the plain one-shot effects, 0x0503 for the
// target arrow), a 3x3 rotation and the world position.
struct SituationEffectTransform {
    std::uint16_t header = 0x0101;
    float rotation[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float position[3] = {0, 0, 0};
};

// cISC4VisualEffect (effects manager products). Reference counted.
class ISituationEffect : public cIGZUnknown {
public:
    virtual void Start(bool restart) = 0;                          // 0x18
    virtual void Stop(bool immediately) = 0;                       // 0x20
    virtual void SetTransform(const SituationEffectTransform& transform) = 0;   // 0x38 / 0x40
    virtual void SetCellRect(const float rect[4]) = 0;             // 0x60(0xc, rect, 4): x0, z0, x1, z1 in cells
};
class ISituationEffectsManager {
public:
    virtual ~ISituationEffectsManager() = default;
    // 0x30: creates the named effect; returns a counted reference in `out` (an existing reference is released), false when unknown.
    virtual bool CreateEffect(const char* name, ISituationEffect*& out) = 0;
};

// The mission's moving "actor" kept at +0xb58 (set by message 0xcc0d905f): used for the position of service missions.
class ISituationMissionActor : public cIGZUnknown {
public:
    virtual bool IsValid() = 0;                    // 0x28
    virtual bool IsHidden() = 0;                   // 0x30
    virtual void Position(float xyz[3]) = 0;       // 0x78 (translation of its transform)
};

// The MySim the "mysim_mission" situations are bound to (kept at +0xbe8).
class ISituationMySim : public cIGZUnknown {
public:
    virtual ISituationOccupant* FindMissionOccupant() = 0;   // 0x58()->0x18(): the sim's current occupant (for group 0x193a)
    virtual void PrepareScript() = 0;                        // 0x220 (called before script calls of mysim missions)
};

// Pure geometry/city queries
class ISituationCity {
public:
    virtual ~ISituationCity() = default;
    virtual int CellsX() = 0;                                  // 0x2e8
    virtual int CellsZ() = 0;                                  // 0x2f0
    virtual float CellSizeXMeters() = 0;                       // 0x2d8
    virtual void WorldToCell(float x, float z, int& cellX, int& cellZ) = 0;   // 0x2f8
    virtual void CellToWorld(int cellX, int cellZ, float xyz[3]) = 0;         // 0x308 (y is the terrain height at the cell)
};
class ISituationZoneManager {
public:
    virtual ~ISituationZoneManager() = default;
    virtual int ZoneTypeAt(int x, int z) = 0;                  // zone grid (vslot 0x28 of the manager) vslot 0x38
};
class ISituationTerrain {
public:
    virtual ~ISituationTerrain() = default;
    virtual bool IsWaterAt(int x, int z) = 0;                  // vslot 0x110
    virtual float HeightAt(float x, float z) = 0;              // vslot 0x60
    virtual float SurfaceWaterDepthAt(int x, int z) = 0;       // spSurfaceWater vslot 0x30
    virtual bool IsCellWaterZoneCandidate(int x, int z) = 0;   // spSurfaceWater vslot 0x38 (x, z, 2, 0)
};
class ISituationTrafficNetwork {
public:
    virtual ~ISituationTrafficNetwork() = default;
    virtual bool HasNetworkAt(int x, int z) = 0;               // vslot 0x70 != 0
    // vslot 0x68: network occupants at the cell; true when one answers the flag test (vslot 0x48 of the occupant)
    virtual bool CellHasNetworkWithFlags(int x, int z, std::uint32_t networkFlags) = 0;
};
class ISituationOccupantManager {
public:
    virtual ~ISituationOccupantManager() = default;
    // vslot 0x128: occupants inside the box that pass the group filter (null group list = any), optionally within a distance
    // band of `center` ([minDistanceSquared, inf) when minDistanceSquared > 0). Returned in the manager's list order.
    virtual std::vector<ISituationOccupant*> OccupantsInBox(const float boxMin[3], const float boxMax[3], const std::vector<std::uint32_t>& groups,
                                                           const float center[3], float minDistanceSquared, std::uint32_t maxCount) = 0;
    virtual std::vector<ISituationOccupant*> NetworkOccupantsInBox(const float boxMin[3], const float boxMax[3], std::uint32_t networkFlags,
                                                                  const float center[3], float minDistanceSquared) = 0;
    virtual void ReleaseOccupants(std::vector<ISituationOccupant*>& list) = 0;   // vslot 0x180
    // Occupants of `group` visible in the camera frustum (falls back to the whole city); the group filter of FindOccupantByGroup.
    virtual std::vector<ISituationOccupant*> VisibleOccupantsOfGroup(std::uint32_t group, bool wholeCityFallback) = 0;
};
class ISituationNeighborConnection {
public:
    virtual ~ISituationNeighborConnection() = default;
    virtual void Cell(int& x, int& z) = 0;                     // vslot 0x30
};
class ISituationNeighborSim {
public:
    virtual ~ISituationNeighborSim() = default;
    virtual std::vector<ISituationNeighborConnection*> Connections() = 0;   // vslot 0x80
};
class ISituationAuraSim {
public:
    virtual ~ISituationAuraSim() = default;
    virtual void AddAura(float x, float z, float radius, float magnitude) = 0;   // vslot 0x30
};
class ISituationBudgetSim {
public:
    virtual ~ISituationBudgetSim() = default;
    virtual void AddMoney(long long amount) = 0;               // vslot 0x38
};
// The landfill lot object kept at +0xaf8 (its +0x10 points at the lot rectangle x0, z0, x1, z1).
class ISituationLandfillLot : public cIGZUnknown {
public:
    virtual const int* Rect() = 0;
};
class ISituationLandfillDeveloper {
public:
    virtual ~ISituationLandfillDeveloper() = default;
    virtual ISituationLandfillLot* LandfillLotAt(int cellX, int cellZ) = 0;   // vslot 0x60 (counted)
};

}  // namespace sc4
