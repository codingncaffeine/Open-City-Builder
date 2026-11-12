// cSC4Pedestrian — the walking automaton (61 symbols at 0x1000f117c..0x1000f3d3a; primary vtable 0x100a71ea0). It is the
// cSC4TrafficAutomaton that walks on the road network: a crowd that follows an attraction, queues, strikes, riots and, on
// demand, ignites fires. It is the base of cSC4PedestrianOneShot.
//
// The class extends the cSC4TrafficAutomaton vtables (primary 0x100a71ea0, occupant 0x100a72470); its own slots are
//   0x5b8 HandleStopped()
// Overridden base slots: 0x18 Init, 0x20 Shutdown, 0x80 MoveTo, 0x88 GetRadius, 0x230/0x238 the destructor, 0x240 Write,
//   0x248 Read, 0x250 GetGZCLSID, 0x278 DoAnimationTick, 0x370 UpdateAnimation, 0x3a0 DoStateTick, 0x3a8 HandleEnterState,
//   0x3b0 HandleExitState, 0x470 HandleNetworkOccupantRemoved, 0x510 UpdateOrientation, 0x528 RecalculatePathOffset,
//   0x530 SlowDownForStopLight, 0x560 ChooseNewDestination, 0x5a8 GetTrafficAutomataManager.
// QueryInterface answers 0x896e75af (cISC4Pedestrian) and 0x8b99ee7c (the traffic automaton) in addition to the base ids.
//
// Original layout after the cSC4TrafficAutomaton fields (+0x398 onward):
//   +0x398 u32 crowd-reaction state (1 or 2, set by HandleCrowdReactionEvent)     +0x39c u8 crowd-reaction direction set
//   +0x3a0 cS3DVector3 crowd-reaction direction   +0x3ac f32 collision timer      +0x3b0 f32 animation speed (0.01)
// The object is 0x3b4 bytes before cSC4PedestrianOneShot's fields.
//
// Behaviour state ids used by the pedestrian (dispatched in DoStateTick): 2 bee-line, 3 crowd, 4 queue, 6 strike,
// 8 riot leader, 9 riot follower, 0xc ignite.
//
// Quirks kept (addresses in the .cpp):
//  * GetRadius always answers 0.5 (0x1000f3c00), ignoring the base radius field.
//  * MoveTo zeroes the y of the direction when it is non-null (0x1000f180a).
//  * UpdateOrientation always answers a ground-plane direction (the y delta is 0, 0x1000f34d8).
//  * SlowDownForStopLight is empty (0x1000f3562).
//  * HandleEnterState falls through 9 -> 4 -> 2/3 and 2/3 -> 4/9 (the switch fall-throughs).
//  * DoAnimationTick plays the "idle" animation when the stopped flag clears (HandleStopped, 0x1000f35a8).
//
// Seams: the crowd collision query (the occupant manager's box query) and the fire ignition (the disaster layer) are
// unrecovered neighbours reached through IPedestrianHost below; everything else goes through the recovered base classes
// (cSC4Automaton, cSC4TrafficAutomaton, cSC4TrafficNetworkMap).
#pragma once
#include <cstdint>
#include <vector>

#include "sc4/TrafficAutomaton.h"

namespace sc4 {

class cSC4Pedestrian;

constexpr std::uint32_t kGZCLSID_cSC4Pedestrian = 0x896e75af;
constexpr std::uint32_t kGZIID_cISC4Pedestrian = 0x896e75af;       // QueryInterface / GetGZCLSID
constexpr std::uint32_t kPedestrianOccupantType = 0x496e5783;      // the occupant type id written to +0x50 by the ctor
constexpr float kPedestrianRadius = 0.5f;                          // GetRadius (0x3f000000)
constexpr float kPedestrianAnimationSpeed = 0.01f;                 // +0x3b0 (0x3c23d70a)
constexpr float kPedestrianCrowdSpeed = 0.0013888889f;             // 5/3600, HandleEnterState states 8/9
constexpr float kPedestrianRiotSpeed = 0.0027777778f;              // 10/3600, DoRiotFollowerTick
const char* const kPedestrianIdleAnimation = "idle";               // HandleStopped

// Behaviour state ids (tAutomataState::stateType) the pedestrian dispatches on.
constexpr std::uint32_t kPedestrianStateBeeLine = 2;
constexpr std::uint32_t kPedestrianStateCrowd = 3;
constexpr std::uint32_t kPedestrianStateQueue = 4;
constexpr std::uint32_t kPedestrianStateStrike = 6;
constexpr std::uint32_t kPedestrianStateRiotLeader = 8;
constexpr std::uint32_t kPedestrianStateRiotFollower = 9;
constexpr std::uint32_t kPedestrianStateIgnite = 0xc;

// ---- tuning -----------------------------------------------------------------------------------------------------------
// The tuning values the pedestrian reads from cSC4AutomataManager's table (0x100b218xx / 0x100b21axx). They are zero
// until ReadTuningParameters runs, so the host supplies them; the defaults are the zero-filled / ±1 values.
struct PedestrianTuning {
    std::int32_t speedMin = 0;      // 0x100b21898 (RandomSint32RangeUniform lower bound)
    std::int32_t speedMax = 0;      // 0x100b2189c
    float pathOffsetMin = -1.0f;    // 0x100b21a24 (RecalculatePathOffset's RandomDoubleRangeUniform range)
    float pathOffsetMax = 1.0f;     // 0x100b21a28
    std::uint8_t oneShotAnimMin = 0;   // 0x100b21a40 (tuning property 0x6a09f630: how many times a one-shot figure plays, low)
    std::uint8_t oneShotAnimMax = 0;   // 0x100b21a41 (high)
};

// ---- seams -----------------------------------------------------------------------------------------------------------
// The unrecovered neighbours the pedestrian reaches directly. Everything else is the base's ITrafficAutomatonHost.
class IPedestrianHost : public ITrafficAutomatonHost {
public:
    ~IPedestrianHost() override = default;
    // SL::spOccupantManager slot 0x120: the occupants of the box (min, max) of a type, at most `count`. The callee
    // AddRefs each entry; the caller releases them.
    virtual void OccupantsInBox(const float* minXyz, const float* maxXyz, std::uint32_t type, int count,
                                std::vector<cISC4Occupant*>& out) = 0;
    // SL::spCity slot 0x240 (the disaster layer) -> GetDisasterManager(0x621cda33) -> cISC4DisasterManagerFire (iid 0x49b43f57):
    // IgniteOccupant(occupant) (interface slot 0x20), then SetCause(2) (slot 0x48).
    virtual void IgniteOccupant(cISC4Occupant* occupant) = 0;
    // SL::spLotManager slot 0x28: the lot at a city cell (flag 0), or null; DoStrikeTick reads its bounding rect.
    virtual ITrafficNetworkMapLot* LotAt(int cellX, int cellZ) = 0;
    // The pedestrian tuning table (0x100b218xx / 0x100b21axx); null uses the zero-filled defaults.
    virtual const PedestrianTuning* PedestrianTuningTable() = 0;
};

class cSC4Pedestrian : public cSC4TrafficAutomaton {
public:
    explicit cSC4Pedestrian(IPedestrianHost* host = nullptr, const cGZPersistResourceKey& modelKey = {});
    ~cSC4Pedestrian() override;

    // ---- cISC4Automaton / cIGZSerializable overrides ----
    bool QueryInterface(std::uint32_t iid, void** out) override;          // @ 0x1000f3796
    bool Init() override;                                                 // @ 0x1000f1792
    bool Shutdown() override;                                             // @ 0x1000f17f2
    void MoveTo(const float* position, const float* direction) override;  // @ 0x1000f180a
    float GetRadius() const override { return kPedestrianRadius; }        // @ 0x1000f3c00
    bool DoAnimationTick(std::uint32_t unknown, float milliseconds) override;   // @ 0x1000f187a
    bool DoStateTick(float milliseconds) override;                        // @ 0x1000f201a
    bool HandleEnterState(tAutomataState* state) override;                // @ 0x1000f1a98
    bool HandleExitState(const tAutomataState* state) override;           // @ 0x1000f1fe0
    void HandleNetworkOccupantRemoved() override;                         // @ 0x1000f3334
    void UpdateOrientation(const cS3DVector3& target, float* out) const override;   // @ 0x1000f34d8
    void RecalculatePathOffset() override;                                // @ 0x1000f3568
    void SlowDownForStopLight() override {}                               // @ 0x1000f3562
    bool ChooseNewDestination() override;                                 // @ 0x1000f190a
    ITrafficAutomataManager* GetTrafficAutomataManager() const override;  // @ 0x1000f385e
    bool Write(cIGZOStream& stream) const override;                       // @ 0x1000f3626
    bool Read(cIGZIStream& stream) override;                              // @ 0x1000f36ce
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4Pedestrian; }   // @ 0x1000f3c6a

    // ---- pedestrian slots ----
    virtual bool HandleStopped();                                        // 0x5b8 @ 0x1000f35a8

    // ---- state tick helpers ----
    bool HandleCrowdReactionEvent(const cS3DVector3& position, std::uint32_t event);   // @ 0x1000f117c
    bool DoBeeLineTick(float milliseconds);                              // @ 0x1000f2124
    bool DoCrowdTick(float milliseconds);                                // @ 0x1000f21c2
    bool DoQueueTick(float milliseconds);                                // @ 0x1000f2470
    bool DoStrikeTick(float milliseconds);                               // @ 0x1000f2602
    bool DoRiotLeaderTick(float milliseconds, bool initialize);          // @ 0x1000f1ca8
    bool DoRiotFollowerTick(float milliseconds);                         // @ 0x1000f293a
    bool DoIgniteTick(float milliseconds);                               // @ 0x1000f2cb2
    bool MoveForwardWithCollision(const cS3DVector3& direction, float milliseconds);   // @ 0x1000f2de8
    bool TestCollisionExpensive(const cS3DVector3& position, float milliseconds);      // @ 0x1000f2ffa
    bool VerifyNetworkOccupant();                                        // @ 0x1000f328e
    void UpdateAnimation(float milliseconds) override;                   // @ 0x1000f338c

    // ---- accessors for the tests and the neighbours ----
    IPedestrianHost* PedestrianHost() const { return pedestrianHost_; }
    const PedestrianTuning& Tuning() const;
    std::uint32_t CrowdReactionState() const { return crowdReactionState_; }
    void SetCrowdReactionState(std::uint32_t value) { crowdReactionState_ = value; }
    bool CrowdReactionDirectionSet() const { return crowdReactionDirectionSet_; }
    void SetCrowdReactionDirectionSet(bool on) { crowdReactionDirectionSet_ = on; }
    const cS3DVector3& CrowdReactionDirection() const { return crowdReactionDirection_; }
    void SetCrowdReactionDirection(const cS3DVector3& direction) { crowdReactionDirection_ = direction; }
    float CollisionTimer() const { return collisionTimer_; }
    void SetCollisionTimer(float value) { collisionTimer_ = value; }
    float AnimationSpeed() const { return animationSpeed_; }
    void SetAnimationSpeed(float value) { animationSpeed_ = value; }

protected:
    std::uint32_t RandomBelow(std::uint32_t range);       // cSC4Automaton::sRandom.RandomUint32Uniform(range)
    float RandomRange(double low, double high);           // sRandom.RandomDoubleRangeUniform(low, high) as a float

    IPedestrianHost* pedestrianHost_ = nullptr;    // port seam (not in the original)
    std::uint32_t crowdReactionState_ = 0;         // +0x398 (1 or 2)
    bool crowdReactionDirectionSet_ = false;       // +0x39c
    cS3DVector3 crowdReactionDirection_{0.0f, 0.0f, 0.0f};   // +0x3a0
    float collisionTimer_ = 0.0f;                  // +0x3ac
    float animationSpeed_ = kPedestrianAnimationSpeed;   // +0x3b0
};

// cSC4PedestrianOneShot (Mac i386 0x000b1c0a..0x000b28cc, class id 0xaa09dc28): a figure that stands in place and plays its
// animation a few times, then leaves: the crime layer's fights, muggings and the like. The count is rolled at Init from the
// tuning's range, or the exemplar's own (property 0xea958845, two bytes).
constexpr std::uint32_t kGZCLSID_cSC4PedestrianOneShot = 0xaa09dc28;
constexpr std::uint32_t kPedestrianOneShotAnimCountProperty = 0xea958845;

class cSC4PedestrianOneShot : public cSC4Pedestrian {
public:
    using cSC4Pedestrian::cSC4Pedestrian;                                 // i386 0x000b28cc
    bool QueryInterface(std::uint32_t iid, void** out) override;          // i386 0x000b1c0a
    bool Init() override;                                                 // i386 0x000b1f4c
    bool DoAnimationTick(std::uint32_t unknown, float milliseconds) override;   // i386 0x000b1e66
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4PedestrianOneShot; }
    std::uint8_t GetAnimCount() const { return animCount_; }              // i386 0x0063373c
    void SetAnimCount(std::uint8_t count) { animCount_ = count; }         // i386 0x0063374c

private:
    std::uint8_t animCount_ = 5;   // +0x280 (i386)
};

}  // namespace sc4
