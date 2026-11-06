// cSC4AutomataController — the base of every automata controller (generator, attractor, prop occupant controller ...):
// a placed thing in the city (usually tied to an occupant, e.g. a building) that owns a list of automata, is switched on
// by the city clock / effect triggers / the camera and plays a looping sound while it is active. 53 functions at
// 0x1000f85b0..0x1000fbcc8; controller type ids 0x4a1628c5 (generator), 0x6a1628ba (attractor); class id 0x4a18672b,
// interfaces 0x6a1857f8 (and cIGZUnknown 1) answered by the primary pointer, 0xe4fda3d4 by the serializable view (+8).
// Original layout (size >= 0xa8; subclasses extend it):
//   +0x00 vptr  +0x08 serializable vptr  +0x10 initialised  +0x18 occupant*  +0x20 template*  +0x28 x +0x2c y +0x30 z
//   +0x34 refcount  +0x38 tract rect (4 bytes: x0 z0 x1 z1 in grid tracts)  +0x40 cRZString template name (used while no template)  +0x68 sound handle
//   +0x6c flags  +0x70 std::list<automaton*> (+0x80 size)  +0x88 automata count  +0x90 city context*  +0x98 age (s)
//   +0x9c lifetime (s, 0 = endless)  +0xa0 animation timer (s)
// Controller flags (CheckFlag/SetFlag, vslots 0x70/0x78; any-bit test): 0x1 active, 0x2 source data ready, 0x4 in view,
// 0x10 aim at the lot facing point, 0x20 fixed position (do not follow the occupant), 0x40 boxes use the position,
// 0x100 detach automata on shutdown, 0x200 always visible, 0x400 waiting for the start delay, 0x1000 terrain bound.
// The template's flags word (+0xbc) is copied into these flags by SetTemplate and OR-ed in by NotifySourceDataChanged.
//
// Original vslots (all calls on `this` below go through them so subclasses can override): QueryInterface 0, AddRef 8,
// Release 0x10, dtor 0x18/0x20, Init 0x28, Shutdown 0x30, GetTemplateName 0x48, GetTemplate 0x50, GetRadius 0x58, IsActive
// 0x60, GetPosition 0x68, CheckFlag 0x70, SetFlag 0x78, SetOccupant 0x80, SetTemplate 0x88, Activate 0xa0, SetLifetime 0xa8,
// GetOccupantBoundingBox 0xb0, GetBoundingBox 0xb8, GetLotFacingPoint 0xc0, IsPointWithinRadius 0xc8, CheckTimeOfDay 0xd0,
// CheckEffectTriggers 0xd8, NotifySourceDataChanged 0xe0, HandleViewChanged 0xe8, HandleStateTick 0xf0, HandleEnterState
// 0xf8, HandleExitState 0x100, HandleRemoveAutomaton 0x108, GetBehaviorTemplate 0x110, IsFull 0x118, TractRect 0x120,
// IsExpired 0x128, ReadyToExpire 0x130, IsTickable 0x138, DoAnimationTick 0x140, Write 0x148, Read 0x150, GetGZCLSID 0x158,
// FinalRelease 0x160.
//
// Quirks kept: HandleStateTick answers false for the queue state (4) only after positioning, true when the queue point is
// invalid; Release at refcount <= 1 runs FinalRelease (Shutdown + delete) without decrementing; GetLotFacingPoint answers
// (0,0,0) for an unknown facing; PlaySFX posts the controller's own pointer (low 32 bits) as the sound owner id and
// truncates the position to integers; Read does not clear the automata list first and never restores the template (only its
// name); Write sends the template name or, without a template, the stored name; the response curve of a new template is
// (0,1)-(24,1). A GetLotFacingPoint cRZRandom seeded from the clock is created and never used by the original (omitted).
#pragma once
#include <cstdint>
#include <list>
#include <vector>

#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "gz/cIGZSerializable.h"
#include "gz/cIGZString.h"
#include "rz/cRZString.h"
#include "sc4/AutomataGenerator.h"
#include "sc4/Occupant.h"
#include "sc4/cSC4ResponseCurve.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4AutomataController = 0x4a18672b;
constexpr std::uint32_t kGZIID_cISC4AutomataController = 0x6a1857f8;
constexpr std::uint32_t kControllerTypeGenerator = 0x4a1628c5;
constexpr std::uint32_t kControllerTypeAttractor = 0x6a1628ba;

constexpr std::uint32_t kCtrlFlagActive = 0x1, kCtrlFlagReady = 0x2, kCtrlFlagInView = 0x4, kCtrlFlagLotFacing = 0x10,
                        kCtrlFlagFixedPosition = 0x20, kCtrlFlagBoxFromPosition = 0x40, kCtrlFlagDetachOnShutdown = 0x100,
                        kCtrlFlagAlwaysVisible = 0x200, kCtrlFlagDelayed = 0x400, kCtrlFlagTerrainBound = 0x1000;
constexpr std::uint32_t kBehaviorTypeQueue = 4;   // behaviour state / automaton state 4 = wait in the controller's queue

// Sound messages posted by PlaySFX (cSC4SoundSystem message ids).
constexpr std::uint32_t kSoundMsgStartLoop = 0xea451903, kSoundMsgSetPosition = 0xca45190f, kSoundMsgStopLoop = 0x6a451908;

// One entry of a template's behaviour list (tBehaviorTemplate, size 0x98: ctor 0x10ee00, dtor 0xfd1aa, GetTimeoutMilliseconds 0xf3ed0): what an
// automaton does in `state`, up to `radius` metres from the controller. The sort order of a template's behaviours is operator< below.
struct tBehaviorTemplate {
    std::uint32_t state = 0;                    // +0x00 (4 = wait in the controller's queue)
    float radius = 0.0f;                        // +0x04
    float percentage = 1.0f;                    // +0x08
    std::uint32_t priority = 1;                 // +0x0c
    std::vector<cRZString> anims;               // +0x10
    std::vector<std::uint32_t> sfx;             // +0x28 (sound ids)
    float timeout = 0.0f;                       // +0x40 (s)
    bool final = false;                         // +0x44
    float kphMultiply[2] = {1.0f, 1.0f};        // +0x48
    std::int32_t kph[2] = {-1, -1};             // +0x50
    float acceleration = 0.5f;                  // +0x58
    // +0x5c..+0x61: six option bytes. The automaton reads them in this order (HandleEnterState): the four ignore
    // flags first, then avoid lots / avoid roads (MoveTo checks the lot / network at the target cell for those two).
    bool ignorePaths = false;                   // +0x5c -> automata flag 0x2000
    bool ignoreRoads = false;                   // +0x5d -> 0x20000
    bool ignoreTerrain = false;                 // +0x5e -> 0x40000
    bool ignoreAttractors = false;              // +0x5f -> 0x4000
    bool avoidLots = false, avoidRoads = false; // +0x60, +0x61 -> 0x400000, 0x800000
    float pathOffset = 0.0f;                    // +0x64
    float delay[2] = {10.0f, 15.0f};            // +0x68 (random walk)
    float turn[2] = {0.1f, 0.2f};               // +0x70
    float strength[2] = {1.0f, 1.0f};           // +0x78
    float idleDelay[2] = {0.0f, 0.0f};          // +0x80
    float idleTime[2] = {5.0f, 10.0f};          // +0x88
    float deceleration = 0.5f;                  // +0x90

    float GetTimeoutMilliseconds() const;   // i386 0x000b4162: the timeout in ms as a float, -1.0f for a timeout of 0 or less
    // The comparison the script importer sorts with (the inlined std::__less): nearer first, then the higher priority.
    friend bool operator<(const tBehaviorTemplate& a, const tBehaviorTemplate& b) { return a.radius < b.radius || a.priority > b.priority; }
};

// tControllerTemplate (size 0x158 for the generator type, 0x148 for the attractor): the base fields at their offsets.
struct tControllerTemplate {
    virtual ~tControllerTemplate() = default;
    cRZString name;                                  // +0x08 (string at +0x10)
    std::int32_t field28 = 0;                        // +0x28
    std::uint32_t controllerType = 0;                // +0x30
    std::uint32_t classKey = 0;                      // +0x34
    float radius = 0.0f;                             // +0x38
    cSC4ResponseCurve curve;                         // +0x40 (time of day -> weight)
    std::int32_t field60 = -1;                       // +0x60 (day mask for generators)
    std::vector<tBehaviorTemplate> behaviors;        // +0x68
    std::list<std::uint32_t> effectsOn;              // +0x80 (+0x90 size): effects that must all be on
    std::list<std::uint32_t> effectsOff;             // +0x98 (+0xa8 size): effects that must all be off
    float lifetime = 0.0f;                           // +0xb0 (s)
    float startDelay = 0.0f;                         // +0xb4 (s)
    std::uint32_t maxAutomata = 0;                   // +0xb8 (0 = unlimited)
    std::uint32_t flags = 0;                         // +0xbc
    float uniqueRadius = 0.0f;                       // +0xc0: no second controller of this template within this radius (< 1 = off)
    std::vector<std::uint32_t> sounds;               // +0xc8: sound ids, one picked at random on activation ("sfx")
    std::vector<std::uint32_t> ignoreStates;         // +0xe0 ("ignore_states")
    std::vector<std::uint32_t> groups;               // +0xf8: automata group ids, resolved from groupNames by LinkControllerTemplates
    std::list<cRZString> groupNames;                 // +0x110 ("automata")
};
// Generator flavour (type 0x4a1628c5): the extra fields from +0xf8 (see GeneratorTemplate in AutomataGenerator.h).
struct tGeneratorTemplate : tControllerTemplate {
    float countScale = 0.0f;                         // +0x128
    std::int32_t countMode = 0;                      // +0x12c
    std::int32_t groupSizeMin = 0, groupSizeMax = 0; // +0x130, +0x134
    float rate = 1.0f;                               // +0x138
    std::int32_t periodUnit = 0;                     // +0x13c
    std::uint32_t eventId = 0;                       // +0x140 ("event")
    float probability = 1.0f;                        // +0x144 ("random_chance")
    float radius2 = 0.0f;                            // +0x148 (second value of "radius")
    std::uint32_t lifetimeAutomataLimit = 0;         // +0x14c ("lifetime_count")
    std::uint8_t spawnArgument = 0;                  // +0x150
};

// ---- seams ------------------------------------------------------------------------------------------------------------
// cISC4Automaton as the controller uses it.
class IControllerLeader {   // what automaton vslot 0x180 answers (the automaton ahead in the queue)
public:
    virtual ~IControllerLeader() = default;
    virtual void GetPosition(float* xyz) = 0;   // vslot 0x58
    virtual float GetLength() = 0;              // vslot 0x88
};
class IControllerAutomaton {
public:
    virtual ~IControllerAutomaton() = default;
    virtual void AddRef() = 0;                         // vslot 0x08
    virtual void Release() = 0;                        // vslot 0x10
    virtual void SetController(void* controller) = 0;  // vslot 0x68 (the controller calls it with null)
    virtual void SetPosition(const float* xyz) = 0;    // vslot 0x50
    virtual IControllerLeader* GetLeader() = 0;        // vslot 0x180
    virtual void SetLeader(IControllerAutomaton* leader) { (void)leader; }   // vslot 0x188 (the automaton ahead in a queue; null = first)
};
// tControllerQInfo of cSC4AutomataControllerManager (queue point of an occupant).
class IControllerQueue {
public:
    virtual ~IControllerQueue() = default;
    virtual void InitQueuePoint(cISC4Occupant* occupant) = 0;    // tControllerQInfo::InitQueuePoint @ 0x1000f7efe
    virtual bool IsValid() = 0;                                  // byte at +0x34
    virtual const float* QueuePoint() = 0;                       // +0x1c (x, y, z)
    virtual const float* QueueDirection() = 0;                   // +0x28 (x, y, z)
    virtual void AddToQueue(IControllerAutomaton* automaton) = 0;       // @ 0x1000f9064
    virtual void RemoveFromQueue(IControllerAutomaton* automaton) = 0;  // @ 0x1000f9144
};

class IAutomataControllerHost {
public:
    virtual ~IAutomataControllerHost() = default;
    // ---- city
    virtual cIGZUnknown* CityContext() = 0;                      // SL::spCity vslot 0x180 (not counted; the controller AddRefs it)
    // City vslot 0x108 (lot manager) -> vslot 0x70(occupant) -> lot; lot vslot 0x70 (cell rect x0,z0,x1,z1) and 0xb8 (facing 0..3).
    virtual bool GetOccupantLot(cISC4Occupant* occupant, SC4CellRect& rect, std::uint32_t& facing) = 0;
    virtual bool PointInCity(float x, float z) = 0;              // SL::spCity vslot 0x310
    virtual float TerrainHeight(float x, float z) = 0;           // SL::spTerrain vslot 0x60
    virtual float CityTerrainHeight(float x, float z) = 0;       // SL::spCity vslot 0x2a0 -> vslot 0x60
    virtual float GroundHeight(float x, float z) = 0;            // SL::spCity vslot 0x140 -> vslot 0x90
    virtual float CityWidth() = 0;                               // SL::spCity vslot 0x2c8
    virtual float CityDepth() = 0;                               // SL::spCity vslot 0x2d0
    // City vslot 0x178 (controller manager): GetControllerQInfo(occupant, create) @ 0x1000f7b66; null when none.
    virtual IControllerQueue* GetQueue(cISC4Occupant* occupant, bool create) = 0;
    virtual void RegisterActiveController(void* controller) = 0; // controller manager vslot 0x80 = MoveController (on activation)
    // ---- effects (SL::spView3DWin QueryInterface(0xfa47b3f9))
    virtual bool BeginEffectQuery() = 0;                         // false: no view / interface
    virtual bool IsEffectActive(std::uint32_t effectId) = 0;     // interface vslot 0x170
    virtual void EndEffectQuery() = 0;                           // releases the view (vslot 0x10)
    // ---- sound (SL::spSS)
    virtual bool SoundAvailable() = 0;
    virtual void PostSound(std::uint32_t message, std::uint32_t a, std::int64_t b, std::int64_t c, std::int64_t d) = 0;   // vslot 0x50
    virtual std::uint32_t RandomBelow(std::uint32_t count) = 0;  // cRZRandom(seed 0xffffffff).RandomUint32UniformBelow
    // ---- camera (SL::spViewUtilities + SL::spTerrain)
    virtual bool ViewAvailable() = 0;
    virtual bool IsPointInViewFrustum(const float* xyz) = 0;     // SC4ViewUtilities::IsPointInViewFrustum
    // ---- controller grid / queue points (cSC4ControllerGrid, tControllerQInfo)
    virtual int CityCellsX() { return 0; }                       // SL::spCity vslot 0x2e8
    virtual int CityCellsZ() { return 0; }                       // SL::spCity vslot 0x2f0
    virtual bool HasNetworkAt(int cellX, int cellZ) { (void)cellX; (void)cellZ; return false; }   // SL::spCity vslot 0x140 -> vslot 0x70(x, z) != 0
    // ---- automaton identity (port seam). The original keeps the automaton object itself; the port's controllers keep the
    // automaton's IControllerAutomaton link. An automaton read from a save (interface 0xa9b40f05, the reference carries over)
    // or carried by a message becomes its link, and a link becomes the serializable object again for writing.
    virtual IControllerAutomaton* AutomatonLink(void* object) { return static_cast<IControllerAutomaton*>(object); }
    virtual const cIGZSerializable* AutomatonObject(IControllerAutomaton* link) { return reinterpret_cast<const cIGZSerializable*>(link); }
};
IAutomataControllerHost* AutomataControllerHost();
void SetAutomataControllerHost(IAutomataControllerHost* host);

class cSC4AutomataController : public cIGZSerializable {
public:
    cSC4AutomataController() = default;
    // 0x1000f85b0 is the base-object (D2) destructor body, the only one the original ever runs: the complete-object
    // (D1, 0x1000f8658) and deleting (D0, 0x1000f865e) slots are clang `push rbp; mov rbp, rsp; ud2` traps because a
    // controller is only ever destroyed through a concrete subclass's own destructor (which is why FinalRelease's
    // `delete this` goes through vslot 0x20 of the *dynamic* vtable). The port has one destructor = the D2 body, so
    // deletion works where the bare-base case would trap; subclasses never hit that path either way.
    virtual ~cSC4AutomataController();

    // ---- cIGZUnknown
    bool QueryInterface(std::uint32_t iid, void** out) override;              // @ 0x1000f9dd4 (thunk 0x1000f9e08)
    std::uint32_t AddRef() override { return ++refCount_; }                   // @ 0x1000fbc32 (thunk 0x1000fbc96)
    std::uint32_t Release() override;                                         // @ 0x1000fbc40 (thunk 0x1000fbca4)

    virtual std::uint32_t GetControllerType() const { return 0; }              // vslot 0x38 (pure in the base; generator / attractor answer their type id)
    virtual bool Init();                                                      // @ 0x1000f8664
    virtual bool Shutdown();                                                  // @ 0x1000f895c
    virtual const cIGZString& GetTemplateName() const;                        // @ 0x1000fb8e8
    virtual tControllerTemplate* GetTemplate() const { return template_; }    // @ 0x1000fb902
    virtual float GetRadius() const { return template_ ? template_->radius : 0.0f; }   // @ 0x1000fb90c
    virtual bool IsActive() const;                                            // @ 0x1000fb926
    virtual void GetPosition(float* xyz) const;                               // @ 0x1000f946c
    virtual bool CheckFlag(std::uint32_t flag) const { return (flags_ & flag) != 0; }   // @ 0x1000fb97e
    virtual void SetFlag(std::uint32_t flag, bool on);                        // @ 0x1000fb98a
    virtual cISC4Occupant* GetOccupant() const { return occupant_; }          // @ 0x1000fb8de (vslot 0x40)
    virtual void SetOccupant(cISC4Occupant* occupant);                        // @ 0x1000f8af8
    virtual void SetTemplate(tControllerTemplate* tmpl);                      // @ 0x1000f8b46
    virtual void SetAutomataGroups(const std::vector<std::uint32_t>&) {}      // vslot 0x90 (pure in the base; the attractor overrides it)
    virtual void SetPosition(const float* xyz) { pos_[0] = xyz[0]; pos_[1] = xyz[1]; pos_[2] = xyz[2]; }   // @ 0x1000fb9a4 (vslot 0x98)
    virtual void Activate(bool on);                                           // @ 0x1000f8a1e
    virtual void SetLifetime(float seconds) { lifetime_ = seconds; }          // @ 0x1000fb9bc
    virtual void GetOccupantBoundingBox(float* minXyz, float* maxXyz) const;  // @ 0x1000f98ea
    virtual void GetBoundingBox(float* minXyz, float* maxXyz) const;          // @ 0x1000f99fe
    virtual void GetLotFacingPoint(float* xyz) const;                         // @ 0x1000f94c0
    virtual bool IsPointWithinRadius(const float* point, float* distance);    // @ 0x1000f9670
    virtual void CheckTimeOfDay(const ControllerTime&, const ControllerTime&) {}   // @ 0x1000f8d6e (empty)
    virtual void CheckEffectTriggers();                                       // @ 0x1000f8c60
    virtual void NotifySourceDataChanged();                                   // @ 0x1000f8d74
    virtual bool HandleViewChanged();                                         // @ 0x1000f9288
    virtual bool HandleStateTick(IControllerAutomaton* automaton, std::uint32_t state, float dt);   // @ 0x1000f8ec2
    virtual bool HandleEnterState(IControllerAutomaton* automaton, std::uint32_t state);            // @ 0x1000f9016
    virtual bool HandleExitState(IControllerAutomaton* automaton, std::uint32_t state);             // @ 0x1000f90fa
    virtual bool HandleRemoveAutomaton(IControllerAutomaton* automaton);      // @ 0x1000f91e2
    virtual const tBehaviorTemplate* GetBehaviorTemplate(float distance, const float* position) const;   // @ 0x1000f8dce
    virtual bool IsFull() const;                                              // @ 0x1000fb9ca
    virtual std::uint8_t* TractRect() { return tractRect_; }                  // @ 0x1000fb9f0
    virtual bool IsExpired() const { return lifetime_ > 0.0f && lifetime_ <= age_; }   // @ 0x1000fb9fa
    virtual bool ReadyToExpire() const { return true; }                       // @ 0x1000fba22
    virtual bool IsTickable() const { return CheckFlag(kCtrlFlagActive); }    // @ 0x1000fba2a
    virtual void DoAnimationTick(float dtMilliseconds, std::uint32_t arg, const ControllerTime& time);   // @ 0x1000f8b8a
    virtual void FinalRelease();                                              // @ 0x1000fbc6c

    bool Write(cIGZOStream& stream) const override;                           // @ 0x1000f9b44 (thunk 0x1000f9c74)
    bool Read(cIGZIStream& stream) override;                                  // @ 0x1000f9c82 (thunk 0x1000f9dc6)
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4AutomataController; }   // @ 0x1000fbc60 (thunk 0x1000fbcc8)

    void PlaySFX(bool start);                                                 // @ 0x1000f8846
    std::uint32_t ControllerId() const { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(this)); }
    // Creates a template for controller `type` named `name`, or null for an unknown type. @ 0x1000f9e16
    // 0x4a1628c5 -> `new tGeneratorTemplate` (0x158 bytes, ctor inlined in the original); 0x6a1628ba ->
    // `new cSC4AutomataAttractor::tTemplate` (0x148 bytes, ctor @ 0x1000fbe8e, the unit's tAttractorTemplate).
    // Both then get classKey/controllerType = type, radius 0, field60 -1 and the (0,1)-(24,1) response curve.
    static tControllerTemplate* CreateTemplateInstance(const cRZString& name, std::uint32_t type);

    // @ 0x1000f97e6: the first crossing of the segment a..b (2D points x,z) with the occupant box edges; out = the crossing
    bool GetBoundingBoxIntersection(const float* a, const float* b, float* out) const;
    std::list<IControllerAutomaton*>& Automata() { return automata_; }
    std::uint32_t AutomataCount() const { return automataCount_; }
    std::uint32_t RefCountValue() const { return refCount_; }
    float Age() const { return age_; }
    std::uint32_t Flags() const { return flags_; }
    std::uint32_t SoundHandle() const { return soundHandle_; }
    void SetAutomataCount(std::uint32_t n) { automataCount_ = n; }

protected:
    IControllerQueue* QueueOf(bool create) const;

    bool initialized_ = false;                      // +0x10
    cISC4Occupant* occupant_ = nullptr;             // +0x18
    tControllerTemplate* template_ = nullptr;       // +0x20
    float pos_[3] = {0, 0, 0};                      // +0x28
    std::uint32_t refCount_ = 0;                    // +0x34
    std::uint8_t tractRect_[8] = {0, 0, 0, 0, 0, 0, 0, 0};   // +0x38 (SC4Rect<uchar>: x0, z0, x1, z1 in controller-grid tracts)
    cRZString name_;                                // +0x40
    std::uint32_t soundHandle_ = 0;                 // +0x68
    std::uint32_t flags_ = 0;                       // +0x6c
    std::list<IControllerAutomaton*> automata_;     // +0x70
    std::uint32_t automataCount_ = 0;               // +0x88
    cIGZUnknown* city_ = nullptr;                   // +0x90
    float age_ = 0.0f;                              // +0x98
    float lifetime_ = 0.0f;                         // +0x9c
    float animTimer_ = 0.0f;                        // +0xa0
};

}  // namespace sc4
