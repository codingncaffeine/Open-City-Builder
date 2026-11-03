// cSC4AutomataGenerator — a controller that spawns automata (pedestrians, cars, ...) around an occupant or along the
// networks. Recovered from Sim City 4 Deluxe Editionsub 1.2.1 (code 0x1000fa3e0..0x1000fbbe0; class id / controller
// type 0x4a1628c5; object size 0xe8; vtables 0x100a73590 (primary) and 0x100a73718 (secondary, +8: serializable)).
// It derives from cSC4AutomataController (ported elsewhere). Layout of the generator's own members:
//   +0x10 initialized (base)   +0x18 occupant (base)   +0x20 template (base, tControllerTemplate*)   +0x34 refcount
//   +0x48 string (base, cleared by SetTemplate)  +0x6c base value copied from template +0xbc
//   +0x70 std::list<cISC4Automaton*> spawned automata (+0x80 size)   +0x88 live automaton count
//   +0x98 lifetime elapsed (base)  +0x9c lifetime (copied from template +0xb0)   +0xa8 cRZRandom (seed 0xffffffff = clock)
//   +0xc0 tControllerTime elapsed since the last spawn tick   +0xc8 tControllerTime period to the next tick
//   +0xd0 automata spawned so far   +0xd4 lifetime automata limit   +0xd8 current group   +0xdc spawned from the group
//   +0xe0 group size limit
// Everything the base class and the city provide (flags, positions, bounding boxes, networks, clock, automata managers)
// is reached through IAutomataGeneratorHost; the original uses vslots of itself and the SL::sp* globals.
//
// Quirks kept: SetAutomataGroups overwrites the (shared) template's group list; the spawn loop breaks when the live
// count EXCEEDS the limit while the tick only spawns when it is below it; the list-spawn branch keeps the y of the
// previous attempt until a network position is accepted; ChooseAutomataGroup on an empty group list reads garbage in the
// original (group 0 here); a null template crashes the original in DoAnimationTick/TriggerGenerator (here it is skipped).
#pragma once
#include <cstdint>
#include <list>
#include <string>
#include <vector>

#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "rz/cRZRandom.h"
#include "sc4/cSC4ResponseCurve.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4AutomataGenerator = 0x4a1628c5;
constexpr std::uint32_t kAutomataGeneratorTemplateType = 0x4a1628c5;
// controller flags (CheckFlag, vslot 0x70)
constexpr std::uint32_t kAutoFlagTickable = 0x1, kAutoFlagVisible = 0x4, kAutoFlagLotFacing = 0x10, kAutoFlagNoNetworks = 0x80,
                        kAutoFlagReleaseOnDeactivate = 0x100, kAutoFlagSuspended = 0x400, kAutoFlagLocalRoads = 0x800;
constexpr std::uint32_t kAutomataNetworkMaskAll = 0xc49;      // GetNetworksInRadius mask when flag 0x80 is set
constexpr std::uint32_t kAutomataNetworkMaskLocal = 0x2;      // mask when flag 0x800 is set
constexpr std::uint32_t kAutomataSpawnFlags = 0x2000002;
#ifndef SC4_BUILDING_OCCUPANT_TYPE_DEFINED   // defined by both automata halves (AutomataGenerator.h, AutomataManager.h)
#define SC4_BUILDING_OCCUPANT_TYPE_DEFINED
constexpr std::uint32_t kBuildingOccupantType = 0x278128a0;
#endif

// tControllerTime: hours in a float plus a signed day count; operator+= folds the delta into the 0..24 range by day.
struct ControllerTime {
    float time = 0.0f;       // +0
    std::int16_t days = 0;   // +4
    ControllerTime& operator+=(float delta);   // @ 0x1000f5aae
};

// tControllerTemplate of type 0x4a1628c5 (size 0x158): the fields the generator reads, at their original offsets.
struct GeneratorTemplate {
    std::uint32_t type = kAutomataGeneratorTemplateType;   // +0x30
    float minRadius = 0.0f;                                // +0x38
    cSC4ResponseCurve spawnCurve;                          // +0x40: time of day -> spawn weight (signed)
    std::uint32_t dayMask = 0xffffffff;                    // +0x60: bit per day (day & 31)
    float lifetime = 0.0f;                                 // +0xb0
    std::uint32_t maxAutomata = 0;                         // +0xb8 (0 = unlimited)
    std::uint32_t field_bc = 0;                            // +0xbc
    std::vector<std::uint32_t> groups;                     // +0xf8
    float countScale = 0.0f;                               // +0x128
    std::int32_t countMode = 0;                            // +0x12c: 0 per tick, 1 per building, 2 per city
    std::int32_t groupSizeMin = 0, groupSizeMax = 0;       // +0x130, +0x134
    float rate = 1.0f;                                     // +0x138
    std::int32_t periodUnit = 0;                           // +0x13c index into {1/60, 1, 24, 168}
    float probability = 0.0f;                              // +0x144
    float radius = 0.0f;                                   // +0x148
    std::uint32_t lifetimeAutomataLimit = 0;               // +0x14c
    std::uint8_t spawnArgument = 0;                        // +0x150
};

struct CellPoint { std::int32_t x = 0, z = 0; };   // SC4Point<int>

class cSC4AutomataGenerator;

// cISC4Automaton as the generator uses it
class IAutomaton {
public:
    virtual ~IAutomaton() = default;
    virtual void AddRef() = 0;                                       // vslot 0x08
    virtual void Release() = 0;                                      // vslot 0x10
    virtual void DetachController(void* controller) = 0;             // vslot 0x68 (called with 0)
    virtual void SetStartTime(const void* clock) = 0;                // vslot 0x138 (city clock object + 0x10)
    virtual void StartBehavior(const void* behavior, cSC4AutomataGenerator* controller) = 0;   // vslot 0x1b8
    virtual void StartDefault(float speed, cSC4AutomataGenerator* controller) = 0;             // vslot 0x1a0 (-1.0f)
};
class IAutomatonManager {
public:
    virtual ~IAutomatonManager() = default;
    // vslot 0x78: (group, position[3], flags, zero vector[3], 0, byte argument, 0) -> new automaton or null
    virtual IAutomaton* CreateAutomaton(std::uint32_t group, const float position[3], std::uint32_t flags,
                                        const float zeroVector[3], std::uint32_t zero, std::uint8_t argument) = 0;
};

class IAutomataGeneratorHost {
public:
    virtual ~IAutomataGeneratorHost() = default;
    // ---- cSC4AutomataController (base class) services
    virtual bool BaseInit() = 0;                                              // cSC4AutomataController::Init
    virtual bool BaseShutdown() = 0;
    virtual void BaseActivate(bool active) = 0;
    virtual void BaseDoAnimationTick(float dt, std::uint32_t arg, const ControllerTime& time) = 0;
    virtual bool BaseWrite(cIGZOStream& stream) = 0;
    virtual bool BaseRead(cIGZIStream& stream) = 0;
    virtual bool CheckFlag(std::uint32_t flags) = 0;                          // vslot 0x70
    virtual void GetPosition(float out[3]) = 0;                               // vslot 0x68
    virtual void GetLotFacingPoint(float out[3]) = 0;                         // vslot 0xc0
    virtual void GetOccupantBoundingBox(float outMin[3], float outMax[3]) = 0;   // vslot 0xb0
    // 0x1000f97e6: ray from `from` (x,z) towards `to` (x,z) against the occupant box; out = hit point (x,z)
    virtual bool GetBoundingBoxIntersection(const float from[2], const float to[2], float out[2]) = 0;
    virtual const void* GetBehaviorTemplate(float distance, const float position[3]) = 0;   // vslot 0x110
    // ---- occupant (+0x18)
    virtual bool HasOccupant() = 0;
    virtual std::uint32_t OccupantType() = 0;                                 // occupant vslot 0x38
    virtual bool OccupantHasFlag(std::uint32_t flag) = 0;                     // occupant vslot 0xb0
    virtual void OccupantLotCell(int& x, int& z) = 0;                         // occupant vslot 0x58
    virtual int OccupantFacing() = 0;                                         // city vslot 0x108 -> object(occupant) vslot 0xb8; -1 when none
    // ---- city
    virtual const void* Clock() = 0;                                          // city vslot 0x178 (null = unavailable)
    virtual void ClockNow(float& time, std::int32_t& day) = 0;                // clock vslot 0xc0
    virtual bool IsAutomataLayerVisible() = 0;                                // cSC4TrafficAutomataManager::IsLayerVisible(7)
    virtual int CityCount() = 0;                                              // city vslot 0x220 -> vslot 0x80 (mode 2)
    virtual float OccupantMeasureA() = 0;                                     // vslot 0x220 -> 0xd0(occupant) (flags 0x1503/0x1504)
    virtual float OccupantMeasureB() = 0;                                     // vslot 0x220 -> 0xd8(occupant) (flag 0x1507)
    virtual bool LotMeasure(int cellX, int cellZ, float& out) = 0;            // lot manager vslot 0x28 -> lot vslot 0x1e8; false when no lot
    virtual bool NetworkCellHasType(int x, int z, std::uint32_t mask) = 0;    // city vslot 0x140 -> 0x70(x,z) -> 0x48(mask)
    virtual bool NetworkPositionAccepts(const float position[3], std::uint32_t mask) = 0;   // network manager vslot 0x158
    virtual float NetworkHeight(float x, float z) = 0;                        // network manager vslot 0x90
    virtual bool TerrainHeightEnabled() = 0;                                  // city vslot 0x310
    virtual float TerrainHeight(float x, float z) = 0;                        // spTerrain vslot 0x60
    virtual IAutomatonManager* ManagerForGroup(std::uint32_t group) = 0;      // cSC4AutomataManager::GetAutomataManagerForGroup
};

// Rounds like (int)(v + 12582912.0f) - 0x4b400000 (round to nearest even).
int AutomataRoundMagic(float v);
// The floor idiom of GetNetworksInRadius: n = round(x - 0.5); result n + 1 when x == (float)(n + 1), else n.
int AutomataFloorIdiom(float x);

class cSC4AutomataGenerator {
public:
    explicit cSC4AutomataGenerator(IAutomataGeneratorHost* host);
    ~cSC4AutomataGenerator();
    cSC4AutomataGenerator(const cSC4AutomataGenerator&) = delete;
    cSC4AutomataGenerator& operator=(const cSC4AutomataGenerator&) = delete;

    // identity / refcounting (vtable 0x100a73590; AddRef 0x1000fbab2, Release 0x1000fbac0, thunk versions at +8)
    std::uint32_t GetGZCLSID() const { return kGZCLSID_cSC4AutomataGenerator; }
    std::uint32_t GetControllerType() const { return kGZCLSID_cSC4AutomataGenerator; }
    std::uint32_t AddRef() { return ++refCount_; }
    std::uint32_t Release();          // deletes the object when the count is <= 1
    void FinalRelease();              // Shutdown(), then delete

    bool Init();
    bool Shutdown();
    void Activate(bool active);
    void SetTemplate(GeneratorTemplate* tmpl);
    void SetAutomataGroups(const std::vector<std::uint32_t>& groups);
    void CheckTimeOfDay(const ControllerTime& unused, const ControllerTime& delta);
    bool ReadyToExpire() const { return true; }
    bool IsExpired() const;
    bool IsTickable() { return host_->CheckFlag(kAutoFlagTickable); }
    float GetRadius() const { return tmpl_ ? tmpl_->radius : 0.0f; }
    void SetLifetimeAutomataCount(std::uint32_t n) { lifetimeLimit_ = n; }

    void DoAnimationTick(float dt, std::uint32_t arg, const ControllerTime& time);
    void TriggerGenerator(float countArg);   // the original's second parameter is the curve value; the first (this) is the controller
    void GetNetworksInRadius(std::vector<CellPoint>& out, std::uint32_t mask);
    void ChooseAutomataGroup();

    bool Write(cIGZOStream& stream) const;
    bool Read(cIGZIStream& stream);

    // state access (base-class fields the base services update)
    cRZRandom& Random() { return random_; }
    GeneratorTemplate* Template() { return tmpl_; }
    void SetLifetimeElapsed(float seconds) { lifetimeElapsed_ = seconds; }   // +0x98, advanced by the base tick
    float Lifetime() const { return lifetime_; }
    std::uint32_t BaseValue6c() const { return baseValue6c_; }
    std::string& Name() { return name_; }
    const std::list<IAutomaton*>& Automata() const { return automata_; }
    std::uint32_t LiveCount() const { return liveCount_; }
    std::uint32_t SpawnedCount() const { return spawned_; }
    std::uint32_t LifetimeLimit() const { return lifetimeLimit_; }
    std::uint32_t Group() const { return group_; }
    std::uint32_t GroupSpawned() const { return groupSpawned_; }
    std::uint32_t GroupLimit() const { return groupLimit_; }
    const ControllerTime& Elapsed() const { return elapsed_; }
    const ControllerTime& Period() const { return period_; }
    ControllerTime& MutableElapsed() { return elapsed_; }
    ControllerTime& MutablePeriod() { return period_; }
    bool initialized = false;   // +0x10, maintained by the base Init/Shutdown

private:
    IAutomataGeneratorHost* host_;
    GeneratorTemplate* tmpl_ = nullptr;   // +0x20
    std::uint32_t refCount_ = 0;          // +0x34
    std::string name_;                    // +0x48
    std::uint32_t baseValue6c_ = 2;       // +0x6c (ctor stores 2)
    std::list<IAutomaton*> automata_;     // +0x70
    std::uint32_t liveCount_ = 0;         // +0x88
    float lifetimeElapsed_ = 0, lifetime_ = 0;   // +0x98, +0x9c
    cRZRandom random_;                    // +0xa8
    ControllerTime elapsed_, period_;     // +0xc0, +0xc8
    std::uint32_t spawned_ = 0, lifetimeLimit_ = 0, group_ = 0, groupSpawned_ = 0, groupLimit_ = 0;   // +0xd0..+0xe0
};

}  // namespace sc4
