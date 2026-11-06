// cSC4TrafficAutomaton — the car / truck automaton that drives on the road network: it walks the per-tile paths of
// cSC4PathInfo, picks the next tiles from the traffic network map, keeps a lookahead deque and a route deque, and drops
// and takes references in the map's per-tile automata lists. 63 functions at 0x1000e25d0..0x1006f3190 (plus the
// non-virtual thunks); base cSC4Automaton, map cSC4TrafficNetworkMap.
//
// Vtable: the class extends the cSC4Automaton primary vtable, 0x100a74990 (object pointer 0x100a749a0). The traffic
// slots (byte offset in the interface vtable, all virtual in the original) are:
//   0x440 DoPathTick             0x448 DoStraightLineTick      0x450 StartTrip            0x458 SetPath
//   0x460 InterpolateAlongPath   0x468 SnapToPath              0x470 HandleNetworkOccupantRemoved
//   0x478 GetPathKey             0x480 GetNetworkOccupant      0x488 GetPathOffset        0x490 SetPathOffset
//   0x498 GetPathPos             0x4a0 IsTrain                 0x4a8 BeginRoute            0x4b0 GetDispatchDestination
//   0x4b8 GetRouteInfo           0x4c0 SetRouteInfo            0x4c8 CloseEnoughToDispatchDestination
//   0x4d0 RemoveSurroundingAutomata  0x4d8 FindLeader          0x4e0 SetDetectCollisions   0x4e8 GetDetectCollisions
//   0x4f0 GetSafeDistance        0x4f8 GetStopPoint            0x500 GetCellLifespan      0x508 SetCellLifespan
//   0x510 UpdateOrientation      0x518 GetLookaheadDeque      0x520 OffsetPathPoint       0x528 RecalculatePathOffset
//   0x530 SlowDownForStopLight   0x538 ReturnToDefaultSpeed    0x540 UpdateCollision      0x548 CheckIntersectionDensity
//   0x550 DoStopSignTick         0x558 DoStopAtStationTick     0x560 ChooseNewDestination 0x568 ChooseLookaheadTiles
//   0x570 ChooseLookaheadTileByDensity  0x578 ChooseLookaheadTileByRoute  0x580 ChooseBestPath
//   0x588 ApplyAttractionWeight  0x590 GetEdgeFromVector      0x598 HandleExitNetwork     0x5a0 ChooseRandomPath
//   0x5a8 GetTrafficAutomataManager (pure in the original; cSC4Vehicle / cSC4Pedestrian override it)
//   0x5b0 GetDesiredLookaheadCount
// Overridden base slots: 0x18 Init, 0x20 Shutdown, 0x50 SetDestination, 0x278 DoAnimationTick, 0x3a0 DoStateTick,
//   0x3a8 HandleEnterState, 0x3b0 HandleExitState, 0x320 GetDebugExtraText (message-target group), 0x240 Write,
//   0x248 Read. QueryInterface also answers 0x8b99ee7c (the traffic automaton interface; not present in the symbols).
//
// Original layout after the cSC4Automaton fields:
//   +0x308 u32 populate layer (the cISC4AutomataManager::PopulateLayer; 7 in the constructor)
//   +0x30c u8 cell lifespan (decremented per lookahead step; <= 0 lets a stuck automaton be removed)
//   +0x310 cISC4NetworkOccupant* counted     +0x318 cISC4PathInfo* counted
//   +0x320 u32 path key (0xffff until SetPath)   +0x328 const std::vector<cS3DVector3>* borrowed path
//   +0x330 s32 path index into that vector       +0x334 f32 path offset (SetPathOffset / behavior +0x64)
//   +0x338 std::deque<tLookaheadNode> (0x10-byte nodes)   +0x368 std::deque<cISC4PathFinder::RouteData> (3 bytes)
// The class is 0x398 bytes before cSC4Vehicle's fields (+0x398 onward); cSC4Pedestrian also derives from it.
//
// Behaviour: Init runs the base Init, then (unless the automaton came from a save) UpdateAttraction and SnapToPath;
// DoAnimationTick runs the state machine and otherwise drives along the path (DoPathTick) or in a straight line
// (DoStraightLineTick). The driving state is state 0x10 (the player-drive state record, 0x2a0 bytes, whose runtime
// slots at +0x78..+0xa0 hold the two traffic effects, the dynamic particle system / particle, the tunnel attachment and
// the counted tunnel occupant), state 0xd is the stoplight wait, state 0x15 the pull-over and state 0x1e the station
// stop. SetPath enters the map's network (EnterNetwork) and leaves the old tile (ExitNetwork), toggling the tunnel
// opacity / audio transition.
//
// Quirks kept (addresses in the .cpp):
//  * HandleEnterState calls the base HandleEnterState twice for state 0x10 (once inside the branch, once at the end).
//  * GetSafeDistance subtracts the radius from the path-end distance and the radius + 3 from the leader distance, then
//    returns the smaller of the two (16 when neither applies). Only the path term with the 0x10 flag set is computed by
//    the original; without a path / flag / leader it reads an uninitialized stack slot into the min, so the port keeps
//    the 16 default for both terms (the intended answer).
//  * FindLeaderInTile starts the automata scan at the automaton itself when the tile belongs to its own occupant, so
//    only automata stored after it in the tile list are candidates; the distance ignores y (it compares x/z only).
//  * SetPath uses points[0] as the start and points[1] as the destination even for a one-point path (out of bounds in
//    the original; the port guards it).
//  * SetDestination ignores the y delta (it only turns on the x/z direction) and calls MoveTo(nullptr, direction).
//  * InterpolateAlongPath clamps a negative index to 0 and returns false (writing the remaining distance) when the index
//    already points past the last segment; at the last segment it writes the remaining distance before moving.
//  * GetPathOffset is a float at +0x334 but the original stores/reads it through a 32-bit word.
//  * SnapToPath gives up when the position still equals the base class' "uninitialized" sentinel
//    (kUninit3DVector: bits 0xffffffff / 0x01ffffff / 0x4f5f4090 in the binary); the port's base constructor starts
//    automata at (0,0,0), so the guard only fires when someone stores those bits explicitly.
//  * CheckIntersectionDensity rounds `density * weight * 10` with the float->int magic bias and compares the followed
//    automaton count against it; a leader chain entry that is not a traffic automaton is skipped.
//  * FindBestNeighborConnection is left PARTIAL: it needs the neighbours simulator's cISC4NeighborConnection list and
//    cISC4PathFinder::FindPath (slot 0xb8, not recovered).
//
// Seams: the network occupant, the path info, the traffic automata manager, the city / map, the neighbours simulator,
// the path finder, AEMS and the dynamic particle system are unrecovered neighbours (or recovered through a narrower
// interface than this class needs); they are reached through ITrafficAutomatonOccupant, ITrafficAutomataManager,
// ITrafficAutomatonHost, ITrafficAutomatonPeer and ITrafficParticleSystem below, with the original vtable slot in each
// comment. The traffic network map (cSC4TrafficNetworkMap) and the traffic effect manager (cSC4TrafficEffectManager)
// are recovered and used directly.
#pragma once
#include <cstdint>
#include <deque>
#include <vector>

#include "sc4/AutomataStates.h"
#include "sc4/Automaton.h"
#include "sc4/MiscTypes.h"
#include "sc4/PathFinder.h"
#include "sc4/TrafficNetworkMap.h"

namespace sc4 {

class cISC4PathInfo;
class cSC4TrafficEffectManager;

constexpr std::uint32_t kGZIID_cISC4TrafficAutomaton = 0x8b99ee7c;   // the extra interface QueryInterface answers

// Traffic-specific automata flags (the base's are in Automaton.h; these are the bits the traffic code knows).
constexpr std::uint32_t kTrafficFlagWaiting = 0x10;      // held for a stoplight / train junction (UpdateCollision)
constexpr std::uint32_t kTrafficFlagStopped = 0x20;      // decelerating / stopped (DoPathTick, DoStopSignTick)
constexpr std::uint32_t kTrafficFlagNoJunctionWait = 0x800;   // no writer recovered; UpdateCollision clears 0x10

struct TrafficEffect;   // TrafficEffectManager.h

// The traffic states (state ids the traffic automaton dispatches on; they overlap the base ids on purpose).
constexpr std::uint32_t kTrafficStateDrive = 0x10;           // the player-drive record used as the driving state
constexpr std::uint32_t kTrafficStateStopSign = 0xd;         // wait at a stoplight / junction
constexpr std::uint32_t kTrafficStatePullOver = 0x15;
constexpr std::uint32_t kTrafficStateStopAtStation = 0x1e;

// cISC4NetworkOccupant (iid 0xa821ef94) as the traffic automaton uses it. This is the map's occupant interface plus the
// cISC4PathInfo query; the real cISC4NetworkOccupant object answers both, the port downcasts between the two views.
class ITrafficAutomatonOccupant : public ITrafficNetworkMapOccupant {
public:
    virtual cISC4PathInfo* GetPathInfoObject() = 0;   // 0x188 (the same object as the map's GetPathInfo view)
};

// The traffic automaton as another vehicle and as the traffic network map see it. The map only needs the
// ITrafficNetworkMapAutomaton part; RemoveSurroundingAutomata walks the map's tile lists and calls the rest on its peers.
class ITrafficAutomatonPeer : public ITrafficNetworkMapAutomaton {
public:
    virtual bool CheckAutomataFlag(std::uint32_t flag) const = 0;   // cSC4Automaton slot 0xf8
    virtual tAutomataState* FindState(std::uint32_t state) = 0;     // slot 0x1c8
    virtual std::uint32_t OccupantType() const = 0;                 // AsOccupant()->slot 0x38
    virtual void Remove(bool fade) = 0;                             // slot 0x68
    virtual void SetOpacity(float opacity, float seconds) = 0;      // slot 0x108
    virtual cSC4Automaton* AsAutomaton() = 0;                       // the base object (ShouldCollide takes it)
    virtual std::uint32_t GetPathKey() const = 0;                   // slot 0x478
};

// The object the original keeps in the driving state's +0x98 slot: vslot 0x20 detaches it (with false) before the
// record's destructor releases it. Nothing recovered in the traffic automaton sets it (cSC4Vehicle does).
class ITrafficAutomatonAttachment : public cIGZUnknown {
public:
    virtual void Detach(bool final) = 0;   // vslot 0x20
};

// One lookahead step (0x10 bytes): the counted occupant of the next tile, the path key chosen there and the dead-end
// flag that makes ChooseNewDestination remove the automaton. The first field is a cRZAutoRefCount in the original (its
// destructor releases), hence the released-then-zeroed pattern of ClearLookaheadDeque.
struct tLookaheadNode {
    cRZAutoRefCount<ITrafficAutomatonOccupant> occupant;   // +0x00
    std::uint32_t key = 0;                                 // +0x08
    std::uint8_t deadEnd = 0;                              // +0x0c
};

// The dynamic particle effect of the driving state (cSC4DynamicParticleEffect / cSC4DynamicParticleSystem, 61 symbols
// at 0x1006fe5xx, unrecovered). InitDynamicParticleSystem is a factory, AddParticle returns a particle to delete later
// and ShutdownDynamicParticleSystem tears the system down.
class ITrafficParticle {
public:
    virtual ~ITrafficParticle() = default;
};
class ITrafficParticleSystem {
public:
    virtual ~ITrafficParticleSystem() = default;
    virtual ITrafficParticle* AddParticle(const cS3DVector3& position) = 0;   // 0x1006fe600
    virtual void DeleteParticle(ITrafficParticle* particle) = 0;             // 0x1006fe894
    virtual void Shutdown() = 0;                                             // 0x1006fea4c
};

// cSC4TrafficAutomataManager (unrecovered; derives from cSC4AutomataManager) — the calls the automaton makes. The slot
// numbers are byte offsets in the manager's vtable; the traffic effect manager lives at manager + 0xb60.
class ITrafficAutomataManager {
public:
    virtual ~ITrafficAutomataManager() = default;
    virtual bool IsTrafficAutomaton(cSC4Automaton* automaton) const = 0;     // @ 0x100112d36
    virtual void SetCurrentLayer(std::uint32_t layer) = 0;                   // vslot 0x180
    virtual void ComputeEdgeWeights(ITrafficAutomatonOccupant* occupant, TrafficTileInfo* tile, std::uint32_t layer,
                                    float* weights) = 0;                     // vslot 0x270, 16 floats in 4 rows
    virtual std::uint32_t GetTravelType(cSC4Automaton* automaton) = 0;        // vslot 0x250
    virtual double GetLayerDensity(std::uint32_t layer) = 0;                 // vslot 0x288 (returns a double)
    virtual cSC4TrafficEffectManager* EffectManager() = 0;                   // +0xb60
    // vslot 0x3c CreateAutomaton(group, position, flags, no model key, no listener, priority, no velocity): a new automaton
    // of the group, held by the manager (the answer is borrowed); null when it makes none.
    virtual cSC4Automaton* CreateAutomaton(std::uint32_t group, const cS3DVector3& position, std::uint32_t flags, std::uint8_t priority) {
        (void)group; (void)position; (void)flags; (void)priority;
        return nullptr;
    }
};

// The tuning values the original reads from cSC4AutomataManager::sTuningParams (0x100b21880) and the loose globals.
// They are zero in the binary until cSC4AutomataManager::ReadTuningParameters runs; the host supplies them so the port
// does not need the unrecovered manager.
struct TrafficAutomatonTuning {
    std::uint32_t desiredLookaheadCount = 0;   // 0x100b21a64 (read as a byte by GetDesiredLookaheadCount)
    float lookaheadPenalty = 0.0f;             // 0x100b21a68 (per-automaton congestion penalty of ChooseBestPath)
    float junctionDuration = 0.0f;             // 0x100b219a4 (seconds; the stoplight state timer is this * 1000)
    float fadeDuration = 0.0f;                 // 0x100b219b4 (RemoveSurroundingAutomata fades over a quarter of it)
    std::uint32_t dispatchDistance = 0;        // 0x100b219f4 (CloseEnoughToDispatchDestination uses it * 16)
};

class ITrafficAutomatonHost {
public:
    virtual ~ITrafficAutomatonHost() = default;
    // SL::spTrafficNetworkMap (also SL::spCity slot 0x140); the city queries
    virtual cSC4TrafficNetworkMap* TrafficNetworkMap() = 0;
    virtual float CitySizeX() = 0;                                              // SL::spCity slot 0x2c8
    virtual float CitySizeZ() = 0;                                              // slot 0x2d0
    virtual void PositionToCell(float x, float z, int& cellX, int& cellZ) = 0;  // slot 0x2f8
    virtual int CityCellsX() = 0;                                               // slot 0x2e8
    virtual int CityCellsZ() = 0;                                               // slot 0x2f0
    // the manager (the automaton's vslot 0x5a8); null when the automaton is not in a traffic automata manager
    virtual ITrafficAutomataManager* TrafficAutomataManager() = 0;
    // SL::spTrafficSim slot 0x30 -> the path finder (FindBestNeighborConnection only)
    virtual cISC4PathFinder* PathFinder() = 0;
    // SL::spNeighborsSim slot 0x88, the five connection lists (kinds 3, 0xc, 8, 2, 10) in the order they are queried
    virtual void NeighborConnectionCells(std::vector<std::int32_t>& cells) = 0;
    // SL::AEMS slot 0x30 (start) / 0x28 (stop), event 0xe in SetPath
    virtual void StartAudioEvent(std::uint32_t event) = 0;
    virtual void StopAudioEvent(std::uint32_t event) = 0;
    // cSC4DynamicParticleEffect::InitDynamicParticleSystem (0x1006fe9e8)
    virtual ITrafficParticleSystem* CreateParticleSystem() = 0;
    // SC4ReadGZSerializable(stream, 0xa821ef94, &out, false): the GZCOM-created network occupant (null in a memory stream)
    virtual ITrafficAutomatonOccupant* ReadNetworkOccupant(cIGZIStream& stream) = 0;
    // the tuning table; null uses the zero-initialized defaults
    virtual const TrafficAutomatonTuning* Tuning() = 0;
};

class cSC4TrafficAutomaton : public cSC4Automaton, public ITrafficAutomatonPeer {
public:
    explicit cSC4TrafficAutomaton(ITrafficAutomatonHost* host, IAutomatonHost* automatonHost = nullptr,
                                  const cGZPersistResourceKey& modelKey = {});
    ~cSC4TrafficAutomaton() override;

    // ---- cISC4Automaton overrides (the base slots in the comment) ----
    bool Init() override;                                         // 0x18  @ 0x100116558
    bool Shutdown() override;                                     // 0x20  @ 0x1001165ee
    void SetDestination(const float* xyz) override;               // 0x50  @ 0x100117cc4
    bool DoAnimationTick(std::uint32_t unknown, float milliseconds) override;   // 0x278 @ 0x1001167f2
    bool DoStateTick(float milliseconds) override;                // 0x3a0 @ 0x1001172f8
    bool HandleEnterState(tAutomataState* state) override;        // 0x3a8 @ 0x100116f3e
    bool HandleExitState(const tAutomataState* state) override;   // 0x3b0 @ 0x10011711e

    // ---- COM ----
    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x100116706
    std::uint32_t AddRef() override { return cSC4Occupant::AddRef(); }
    std::uint32_t Release() override { return cSC4Automaton::Release(); }
    bool Write(cIGZOStream& stream) const override;                // @ 0x10011aa6a
    bool Read(cIGZIStream& stream) override;                       // @ 0x10011ac58
    const char* GetDebugExtraText() const override;                // @ 0x10011a85e

    // ---- traffic interface (the original vtable slots in the comments) ----
    virtual bool DoPathTick(float milliseconds);                            // 0x440 @ 0x10011696a
    bool DoStraightLineTick(float milliseconds);                            // 0x448 @ 0x1001176d8
    virtual bool StartTrip(ITrafficAutomatonOccupant* occupant, std::uint32_t key, bool param3);   // 0x450 @ 0x100119db2
    virtual void SetPath(ITrafficAutomatonOccupant* occupant, std::uint32_t pathKey,
                         const std::vector<cS3DVector3>* path, bool keepDestination);   // 0x458 @ 0x100117dbe
    bool InterpolateAlongPath(float distance, const cS3DVector3& from, cS3DVector3& out, std::int32_t* index,
                              const std::vector<cS3DVector3>* path, float* remaining);   // 0x460 @ 0x100117a96
    bool SnapToPath();                                                     // 0x468 @ 0x1001182e4
    virtual void HandleNetworkOccupantRemoved();                           // 0x470 @ 0x1001187cc
    std::uint32_t GetPathKey() const override;                             // 0x478 @ 0x1006f3190
    ITrafficAutomatonOccupant* GetNetworkOccupant() const;                 // 0x480 @ 0x100506880
    float GetPathOffset() const;                                           // 0x488 @ 0x1000e25d0
    void SetPathOffset(float offset);                                      // 0x490 @ 0x100507130
    virtual const cS3DVector3& GetPathPos() const;                         // 0x498 @ 0x1000f3cc0
    virtual bool IsTrain() const;                                          // 0x4a0 @ 0x1005075b0
    bool BeginRoute(bool onlyStart);                                       // 0x4a8 @ 0x100119aac
    const cS3DVector3& GetDispatchDestination() const;                     // 0x4b0 @ 0x1005075b8
    const std::deque<RouteData>& GetRouteInfo() const;                     // 0x4b8 @ 0x1000e25e0
    bool SetRouteInfo(const std::deque<RouteData>& route, bool begin, bool onlyStart);   // 0x4c0 @ 0x100119a46
    bool CloseEnoughToDispatchDestination() const;                         // 0x4c8 @ 0x10011aeb2
    void RemoveSurroundingAutomata(ITrafficAutomatonOccupant* occupant, int depth);   // 0x4d0 @ 0x10011a640
    cSC4Automaton* FindLeader();                                           // 0x4d8 @ 0x100115b4a
    void SetDetectCollisions(bool on);                                     // 0x4e0 @ 0x10011a24c
    bool GetDetectCollisions() const;                                      // 0x4e8 @ 0x100507140
    virtual float GetSafeDistance();                                       // 0x4f0 @ 0x10011a2e8
    bool GetStopPoint(cS3DVector3& out);                                   // 0x4f8 @ 0x10011a4ac
    std::uint8_t GetCellLifespan() const;                                  // 0x500 @ 0x100508e60
    void SetCellLifespan(std::uint8_t lifespan);                           // 0x508 @ 0x10050f560
    virtual void UpdateOrientation(const cS3DVector3& target, float* out) const;   // 0x510 @ 0x100119eec
    const std::deque<tLookaheadNode>& GetLookaheadDeque() const;           // 0x518 @ 0x1000e25f0
    void OffsetPathPoint(cS3DVector3& point);                              // 0x520 @ 0x100119f70
    virtual void RecalculatePathOffset();                                  // 0x528 @ 0x10011b73e
    virtual void SlowDownForStopLight();                                           // 0x530 @ 0x10011a580
    void ReturnToDefaultSpeed();                                           // 0x538 @ 0x10011a5d0
    void UpdateCollision();                                                // 0x540 @ 0x100116cb6
    bool CheckIntersectionDensity();                                       // 0x548 @ 0x1001174e2
    bool DoStopSignTick(float milliseconds);                               // 0x550 @ 0x1001173cc
    bool DoStopAtStationTick(float milliseconds);                          // 0x558 @ 0x10011733e
    virtual bool ChooseNewDestination();                                           // 0x560 @ 0x1001187dc
    bool ChooseLookaheadTiles();                                           // 0x568 @ 0x100118b1c
    bool ChooseLookaheadTileByDensity(const tLookaheadNode& from, tLookaheadNode& out);   // 0x570 @ 0x1001190b6
    bool ChooseLookaheadTileByRoute(const tLookaheadNode& from, tLookaheadNode& out);     // 0x578 @ 0x100119390
    bool ChooseBestPath(std::deque<tLookaheadNode>::const_iterator node, const tLookaheadNode& value,
                        const cS3DVector3& position, std::uint32_t& key, float& length);  // 0x580 @ 0x100118d98
    bool ApplyAttractionWeight(ITrafficAutomatonOccupant* occupant, float* weights);      // 0x588 @ 0x10011971c
    bool GetEdgeFromVector(ITrafficAutomatonOccupant* occupant, const cS3DVector3& direction, std::uint8_t& edge,
                           bool checkConnection);                                        // 0x590 @ 0x100119906
    bool HandleExitNetwork(ITrafficAutomatonOccupant* occupant, std::uint32_t key, void* lot, const SC4RectInt32& rect);   // 0x598
    bool ChooseRandomPath(ITrafficAutomatonOccupant* occupant, std::uint32_t key);         // 0x5a0 @ 0x10011968e
    virtual ITrafficAutomataManager* GetTrafficAutomataManager() const;                   // 0x5a8 (pure in the original)
    std::uint8_t GetDesiredLookaheadCount() const;                                        // 0x5b0 @ 0x100507160

    // ---- non-virtual helpers / lifecycle (no vtable slot) ----
    void SetPopulateLayer(std::uint32_t layer);                            // @ 0x100114798
    void ClearLookaheadDeque();                                            // @ 0x100116632
    cSC4Automaton* FindLeaderInTile(ITrafficAutomatonOccupant* occupant, std::uint32_t key, std::uint32_t maskA,
                                    std::uint32_t maskB);                  // @ 0x100115ce8
    std::uint8_t ChooseExitEdge(ITrafficAutomatonOccupant* occupant, TrafficTileInfo* tile, std::uint8_t entry,
                                std::uint8_t* edges);                      // @ 0x100119256
    bool ShouldKeepAlive() const;                                         // @ 0x10011932e
    ITrafficAutomatonOccupant* FindBestNeighborConnection();              // @ 0x10011af24 (PARTIAL)

    // ---- accessors for the tests and the neighbours ----
    std::uint32_t PopulateLayer() const { return populateLayer_; }
    ITrafficAutomataManager* Manager() const;
    ITrafficAutomatonHost* TrafficHost() const { return trafficHost_; }
    const TrafficAutomatonTuning& Tuning() const;
    const std::vector<cS3DVector3>* CurrentPath() const { return path_; }
    std::uint32_t PathIndex() const { return pathIndex_; }
    void SetPathIndex(std::uint32_t index) { pathIndex_ = index; }
    const cISC4PathInfo* CurrentPathInfo() const { return pathInfo_; }
    std::deque<RouteData>& Route() { return route_; }

    // ---- ITrafficNetworkMapAutomaton ----
    bool IsRemoved() override { return GetMarkedForRemove(); }
    std::uint32_t GetLane() override { return pathKey_; }
    std::uint32_t PathTypeWord() const override { return populateLayer_; }

    // ---- ITrafficAutomatonPeer ----
    bool CheckAutomataFlag(std::uint32_t flag) const override { return cSC4Automaton::CheckAutomataFlag(flag); }
    tAutomataState* FindState(std::uint32_t state) override { return cSC4Automaton::FindState(state); }
    std::uint32_t OccupantType() const override { return GetType(); }
    void SetOpacity(float opacity, float seconds) override { cSC4Automaton::SetOpacity(opacity, seconds); }
    cSC4Automaton* AsAutomaton() override { return this; }
    void Remove(bool fade) override { cSC4Automaton::Remove(fade); }

protected:
    void StartDrivingState();
    void StopDrivingState();

    ITrafficAutomatonHost* trafficHost_ = nullptr;                  // port seam (not in the original)
    std::uint32_t populateLayer_ = 7;                              // +0x308
    std::uint8_t cellLifespan_ = 0;                                // +0x30c
    ITrafficAutomatonOccupant* networkOccupant_ = nullptr;         // +0x310 counted
    cISC4PathInfo* pathInfo_ = nullptr;                            // +0x318 counted
    std::uint32_t pathKey_ = 0xffff;                               // +0x320
    const std::vector<cS3DVector3>* path_ = nullptr;               // +0x328 borrowed
    std::uint32_t pathIndex_ = 0;                                  // +0x330
    float pathOffset_ = 0.0f;                                      // +0x334
    std::deque<tLookaheadNode> lookahead_;                         // +0x338
    std::deque<RouteData> route_;                                  // +0x368
    // Run-time driving state (the original's player-drive record +0x78..+0xa0 / +0x254; see the class comment).
    TrafficEffect* drivingEffect78_ = nullptr;                     // +0x78 of the driving state
    TrafficEffect* drivingEffect80_ = nullptr;                     // +0x80
    ITrafficParticleSystem* particleSystem_ = nullptr;             // +0x88
    ITrafficParticle* particle_ = nullptr;                         // +0x90
    ITrafficAutomatonAttachment* tunnelAttachment_ = nullptr;      // +0x98 (vslot 0x20 detach)
    ITrafficAutomatonOccupant* tunnelOccupant_ = nullptr;          // +0xa0 counted
    std::uint8_t drivingFlag254_ = 0;                              // +0x254 (write-only in the recovered code)
};

// The "never moved" position sentinel the original compares against in SnapToPath (cSC4Automaton::kUninit3DVector at
// 0x100b22550: the raw words 0xffffffff, 0x01ffffff, 0x4f5f4090 - uninitialized-looking values kept verbatim).
constexpr std::uint32_t kTrafficUninitPositionBits[3] = {0xffffffffu, 0x01ffffffu, 0x4f5f4090u};

}  // namespace sc4
