// cSC4TrafficAutomataManager — the per-kind automata manager for road traffic: it decides which automata the visible
// layers spawn, keeps the traffic effects (accidents), turns dispatch requests into traffic automata and maintains the
// automaton leaders. It is the object the traffic automaton reaches through cSC4Automaton vslot 0x5a8 (the
// ITrafficAutomataManager seam) and the base the vehicle manager derives from (cSC4VehicleManager's constructor calls
// this constructor at 0x100111b96).
//
// 48 symbols at 0x100111b96..0x100116432 on top of cSC4AutomataManager (0xb08 bytes).
// Vtable: the class extends the base's four views and replaces slots 0x18 Init, 0x20 Shutdown, 0x40 RemoveAutomaton,
// 0x80 GetRandomCreationPoint, 0x88/0x90 Dispatch, 0x140 Write, 0x148 Read, 0x178 IsAutomatonOnscreen, 0x1d0
// HandleRemoveNetworkOccupant, 0x1d8 HandleNewBuilding, 0x1e8 HandleRemoveOccupant, 0x1f0 HandlePostCityInit, 0x228
// DoAnimationTick and 0x240 ScanExemplars, and adds the traffic slots 0x250..0x308 (in the original order below).
//
// Original layout after the base (the numbers are the original offsets; the port's std::list / std::map / std::vector are
// larger than the 2010-era libc++ ones, so the port's real offsets differ - each member carries the original offset):
//   +0xb08 traffic simulator (city vslot 0x228, counted; released by Shutdown)
//   +0xb10 default path type (cSC4VehicleManager's constructor sets 1)
//   +0xb14 default network type (cSC4VehicleManager's constructor sets 1; layer 1 of GetEdgeDensity)
//   +0xb18 std::list<LayerUpdate> (node: +0x10 layer, +0x14 elapsed ms); +0xb28 size, +0xb30 current node
//   +0xb38/+0xb3c current visible cell x/z (-1 = "start at the beginning of the visible rect")
//   +0xb40 crime-layer density multiplier (cSC4VehicleManager's constructor sets 0.2f)
//   +0xb48 std::vector<tResponseAutomataInfo> (0x28 bytes per entry), +0xb58 end
//   +0xb60 cSC4TrafficEffectManager (its own cRZTimer sits at +0xb78; the port keeps the timer next to it)
//
// Layers and groups (from the binary tables at 0x100a18b20 / 0x100a18b40): GetPathTypeForLayer maps 4->3, 5->3, 6->4,
// 9->6, 10->7 and GetAutomataGroupForLayer maps 2->0x4200, 3->0x4204, 4->0x4215, 5->0x4216, 6->0x4217, 8->0x4102,
// 9->0x422c, 10->0x422e; every other layer uses the default field (+0xb10 / the base's default group).
//
// Quirks kept (addresses in the .cpp):
//  * The constructor leaves +0xb10/+0xb14/+0xb40 and the layer list zero/empty; only cSC4VehicleManager fills them
//    (0x1000d9c40: +0xb10 = 0x100000001, +0xb40 = 0.2f, layers pushed in the order 1,2,3,5,4,7,6,8,9,10).
//  * Write stores the default path type again although Read discards that field (the port reads and drops it, 0x1001161f7).
//  * Read only accepts stream versions 2 and 3 (`(version & 0xfffe) == 2`) and only version 3 carries the effect list.
//  * GetEdgeDensity negates the whole four-float multiplier tail when it is negative (the original XORs the sign bit of
//    an xmmword at 0x100115273); the port negates the scalar, which the four rows then multiply identically.
//  * GetOccupantTripEdgeDensity transposes the 4x4 table for a negative multiplier (0x100115350: stride 8 columns,
//    +2 per row) and computes ushort * param * maxAutos in that order.
//  * IsOffscreenEntryEdge bumps the cell in the +x direction for entry edges 0 and 2 and +z for 1 and 3 (the four
//    five-byte jump-table bodies at 0x10011419e), then tests the visible rect.
//  * HandleRemoveNetworkOccupant removes *every* automaton whose network occupant is the removed one, and also
//    removes an onscreen automaton in the removed occupant's cell rect that carries flag 0x2000 but not 0x20000.
//  * The update-response list retires an entry after six failed lookups (0x10011247e) and gives it another 1000 ms
//    after every successful spawn (0x100112462 writes 0x447a0000).
//
// Seams: the traffic simulator (city vslot 0x228), the network manager (city vslot 0x140, only the density/iteration
// slots the base's IAutomataNetworkManager does not expose), the hour / time-of-day / clock-percent services (city
// vslots 0x220, 0x2b0, 0x178), the crime simulator, the lot manager and the traffic automaton's own traffic API go
// through ITrafficAutomataManagerHost / ITrafficNetworkManagerApi / ITrafficSimulatorApi / ITrafficAutomataAutomaton
// below, with the original slot in each comment. The base's IAutomataAutomaton objects are the same automata; the
// traffic-specific view is a documented cast the caller (the GZCOM automata adapter) guarantees.
#pragma once
#include <cstdint>
#include <list>
#include <vector>

#include "rz/cRZAutoRefCount.h"
#include "rz/cRZTimer.h"
#include "sc4/AutomataManager.h"
#include "sc4/TrafficEffectManager.h"
#include "sc4/TrafficNetworkMap.h"

namespace sc4 {

class ITrafficAutomataAutomaton;
struct tTrafficNetworkOccupant;

// A response request (cSC4TrafficAutomataManager::tResponseAutomataInfo, 0x28 bytes): the occupant to send an
// automaton to, how many are still to be spawned and how long until the next attempt.
struct tResponseAutomataInfo {
    std::uint32_t group = 0;                    // +0x00
    cRZAutoRefCount<cISC4Occupant> occupant;     // +0x08 (counted)
    std::uint32_t count = 0;                    // +0x10 remaining automata
    bool forced = false;                        // +0x14 spawn even if a path already exists
    std::uint8_t pathType = 0;                  // +0x15
    std::uint32_t flags = 0;                    // +0x18 (DispatchResponseAutomata ORs 0x02000000 in)
    float timeLeft = 0.0f;                      // +0x1c milliseconds
    std::uint32_t retries = 0;                  // +0x20 failed lookups (retire at 6)
};

// One entry of the populate-layer list: the original's std::list node carries the layer at +0x10 and the elapsed
// milliseconds at +0x14.
struct tTrafficLayerUpdate {
    std::uint32_t layer = 0;      // original node +0x10
    float elapsed = 0.0f;         // original node +0x14
};

// -------------------------------------------------------------------------------------------------------------------------
// cSC4TrafficAutomaton seen by this manager (the traffic slots on top of the base's IAutomataAutomaton view).
// -------------------------------------------------------------------------------------------------------------------------
class ITrafficAutomataAutomaton : public IAutomataAutomaton {
public:
    virtual std::uint32_t PopulateLayer() const = 0;                              // +0x308
    virtual void SetPopulateLayer(std::uint32_t layer) = 0;                       // @ 0x100114798
    virtual void SetCellLifespan(std::uint8_t lifespan) = 0;                      // +0x30c
    virtual bool StartTrip(ITrafficNetworkMapOccupant* occupant, std::uint32_t key, bool param3) = 0;   // 0x450
    virtual void SetPathToOccupant(ITrafficNetworkMapOccupant* occupant, std::uint32_t key, const void* path,
                                   bool keepDestination) = 0;                     // 0x458
    virtual void HandleNetworkOccupantRemoved() = 0;                              // 0x470
    virtual std::uint32_t GetPathKey() const = 0;                                 // 0x478
    virtual ITrafficNetworkMapOccupant* GetNetworkOccupant() const = 0;           // 0x480
    virtual bool IsTrain() const = 0;                                             // 0x4a0
    virtual bool GetDetectCollisions() const = 0;                                 // 0x4e8
    virtual IAutomataAutomaton* FindLeader() = 0;                                 // 0x4d8
    virtual IAutomataAutomaton* Leader() const = 0;                               // +0xf0
    virtual void SetLeader(IAutomataAutomaton* leader) = 0;                       // +0xf0 (counted)
    virtual void SetTargetDirection(const float* xyz) = 0;                        // 0x2b8 (Dispatch)
};

// The occupant view the manager's network handlers use (cISC4NetworkOccupant slots 0x180 / 0x40): the base's
// IAutomataNetworkOccupant is deliberately empty, so this interface adds the two slots the traffic manager calls.
// The full cISC4NetworkOccupant view: the map's interface already declares slots 0x40 (HasNetworkFlag) and 0x180
// (AsOccupant); this type only joins it with the base manager's empty occupant view so one object can be both.
class ITrafficNetworkOccupant : public IAutomataNetworkOccupant, public ITrafficNetworkMapOccupant {};

// The automata stored in a tile list seen with the flags/opacity/removal slots RecallAutomatonFromNetwork uses
// (ITrafficNetworkMapAutomaton only declares the map's view of them).
class ITrafficTileAutomaton : public ITrafficNetworkMapAutomaton {
public:
    virtual std::uint32_t GetPathKey() const = 0;             // cSC4Automaton 0x478
    virtual bool CheckAutomataFlag(std::uint32_t flag) = 0;   // cSC4Automaton 0xf8
    virtual float GetOpacity() const = 0;                     // 0x100
    virtual void Remove(bool fade) = 0;                       // 0x68
};

// -------------------------------------------------------------------------------------------------------------------------
// The neighbours the base's seams do not cover.
// -------------------------------------------------------------------------------------------------------------------------
class ITrafficSimulatorApi {
public:
    virtual ~ITrafficSimulatorApi() = default;
    virtual std::uint32_t AddRef() = 0;
    virtual std::uint32_t Release() = 0;
    // cISC4TrafficSimulator vslot 0x60: the 4x4 ushort edge-count table of the occupant's cell (row = entry edge,
    // column = exit edge), built for `networkType`; `mirrored` swaps the direction (the manager passes it for a
    // negative density multiplier, 0x100115288).
    virtual const std::uint16_t* GetEdgeCountTable(ITrafficNetworkMapOccupant* occupant, unsigned cellX,
                                                   unsigned cellZ, unsigned networkType, bool mirrored) = 0;
    // cISC4TrafficSimulator GetCapacity (i386 vslot 0x5c): the simulator network's capacity at the cell (0 when none).
    virtual float GetCapacity(int simNetworkType, int cellX, int cellZ) { (void)simNetworkType; (void)cellX; (void)cellZ; return 0.0f; }
};

// cISC4NetworkManager (city vslot 0x140) beyond the base's IAutomataNetworkManager: the visible-rect, tile iteration
// and response-occupant slots the traffic manager calls.
class ITrafficNetworkManagerApi {
public:
    virtual ~ITrafficNetworkManagerApi() = default;
    virtual TrafficTileInfo* GetTileInfoAtCell(int cellX, int cellZ) = 0;          // 0x38
    virtual TrafficTileInfo* GetFirstTileInfoAtCell(int cellX, int cellZ) = 0;     // 0x68 (0x70 in the spawn paths)
    virtual void GetVisibleCellRect(int rect[4]) = 0;                              // 0x88 (x0, z0, x1, z1)
    // 0xc8: occupants that can be the target of a response automaton; out entries are AddRef'd by the callee.
    virtual bool GetResponseOccupants(cISC4Occupant* occupant, int networkTypeA, int networkTypeB,
                                      std::uint32_t mask, std::vector<ITrafficNetworkMapOccupant*>& out) = 0;
    // 0x110: "this occupant's request may be served" (the original's per-occupant gate in PopulateNetworkOccupant).
    virtual bool ResponseAllowed(ITrafficNetworkMapOccupant* occupant) = 0;
    // 0xa0 / 0xb0: the per-occupant rolling edge-density accumulators (float) and per-edge counters (ushort) that
    // PopulateNetworkOccupant(0x2a0) ages and reads.
    virtual float& OccupantPathDensity(ITrafficNetworkMapOccupant* occupant, std::uint32_t layer,
                                       std::uint32_t pathType) = 0;
    virtual std::uint16_t& OccupantPathEdgeCount(ITrafficNetworkMapOccupant* occupant, std::uint32_t layer,
                                                 std::uint32_t pathType, std::uint8_t entry, std::uint8_t exit) = 0;
    // MaybeSpawnAutomaton's neighbors: 0x4c the spawn accumulator (seconds) of one layer / path type / entry / exit of the
    // occupant, 0x5c the automata on one lane key; and the occupant's path info (cISC4PathInfo 0x30 lane count, 0x10 the path of
    // a key: its first point is where the automaton starts). Port seams with neutral defaults.
    virtual float& SpawnAccumulator(ITrafficNetworkMapOccupant*, std::uint32_t, std::uint32_t, std::uint8_t, std::uint8_t) {
        static float none = 0.0f;
        none = 0.0f;
        return none;
    }
    virtual std::uint32_t LaneAutomataCount(ITrafficNetworkMapOccupant*, std::uint32_t, std::uint32_t) { return 0; }
    virtual int LaneCount(ITrafficNetworkMapOccupant*, std::uint32_t, std::uint8_t, std::uint8_t) { return 0; }
    virtual bool PathStart(ITrafficNetworkMapOccupant*, std::uint32_t, cS3DVector3&) { return false; }
    // the map's automata of a path type on the occupant (0x60 in the car layers' pass)
    virtual std::uint32_t OccupantAutomataCount(ITrafficNetworkMapOccupant*, std::uint32_t) { return 0; }
};

class ITrafficAutomataManagerHost : public IAutomataManagerHost {
public:
    ~ITrafficAutomataManagerHost() override = default;
    virtual ITrafficSimulatorApi* TrafficSimulator() = 0;                         // SL::spCity vslot 0x228
    virtual ITrafficNetworkManagerApi* TrafficNetworkManager() = 0;               // the same object as NetworkManager()
    virtual int CityHour() = 0;                                                   // SL::spCity 0x220 -> 0x80
    virtual float CityTimeFraction() = 0;                                         // SL::spCity 0x2b0 -> 0xa0 (0..1)
    virtual float CityClockPercent() = 0;                                         // SL::spCity 0x178 -> 0xb8
    // The traffic automaton that matches a base automaton; the GZCOM automata adapter answers this.
    virtual ITrafficAutomataAutomaton* AsTrafficAutomaton(IAutomataAutomaton* automaton) = 0;
    // The two views of an occupant the response list and the dispatch locations swap between.
    virtual cISC4Occupant* AsCityOccupant(IAutomataOccupant* occupant) = 0;
    virtual IAutomataOccupant* AsAutomataOccupant(cISC4Occupant* occupant) = 0;
    // The traffic network occupant view of a base network occupant (the same object, wider interface).
    virtual ITrafficNetworkOccupant* AsTrafficNetworkOccupant(IAutomataNetworkOccupant* occupant) = 0;
};

// The two occupant types IsTrafficAutomaton accepts (0x100112d36).
constexpr std::uint32_t kTrafficOccupantType = 0xc7cc75f1;
constexpr std::uint32_t kTrainOccupantType = 0x496e5783;

// Occupant groups IsTrainGroup accepts besides 0x4005 (0x100112812).
constexpr std::uint32_t kTrainGroupFixed = 0x4005;

class cSC4TrafficAutomataManager : public cSC4AutomataManager, public ITrafficEffectHost {
public:
    explicit cSC4TrafficAutomataManager(ITrafficAutomataManagerHost* host = nullptr);   // @ 0x100111b96
    ~cSC4TrafficAutomataManager() override;                                            // @ 0x100111c96

    // ---- base overrides (the original vtable slot in the comment) ----
    bool Init() override;                                                              // 0x18  @ 0x100111db4
    bool Shutdown() override;                                                          // 0x20  @ 0x100111e24
    bool RemoveAutomaton(IAutomataAutomaton* automaton, bool force) override;          // 0x40  @ 0x1001159c4
    bool GetRandomCreationPoint(std::uint32_t group, const cS3DVector3& origin, float minDistance, float maxDistance,
                                const cS3DVector3& center, cS3DVector3& direction, std::uint32_t flags) override;   // 0x80
    IAutomataAutomaton* Dispatch(std::uint32_t group, const tAutomataLocation& from, const tAutomataLocation& to,
                                 std::uint32_t flags, std::uint8_t type, const cGZPersistResourceKey& key,
                                 std::uint8_t argument) override;                      // 0x88 @ 0x100112842
    bool Dispatch(IAutomataAutomaton* automaton, const tAutomataLocation& from, const tAutomataLocation& to,
                  std::uint32_t flags, std::uint8_t type) override;                    // 0x90 @ 0x10011298e
    bool Write(cIGZOStream& stream) const override;                                    // 0x140 @ 0x100116078
    bool Read(cIGZIStream& stream) override;                                           // 0x148 @ 0x1001161b4
    bool IsAutomatonOnscreen(IAutomataAutomaton* automaton) override;                  // 0x178 @ 0x1001159fc
    bool HandleRemoveNetworkOccupant(IAutomataNetworkOccupant* occupant) override;     // 0x1d0 @ 0x100111ee2
    bool HandleNewBuilding(IAutomataBuildingOccupant* building) override;              // 0x1d8 @ 0x10011206a
    bool HandleRemoveOccupant(IAutomataOccupant* occupant) override;                   // 0x1e8 @ 0x100112074
    void HandlePostCityInit() override;                                                // 0x1f0 @ 0x100114e88
    void DoAnimationTick(std::uint32_t data, float deltaMilliseconds) override;        // 0x228 @ 0x100112d88
    void ScanExemplars() override;                                                     // 0x240 @ 0x10011606e

    // ---- traffic slots (original vtable order) ----
    virtual int GetDefaultPathType(ITrafficAutomataAutomaton* automaton) const;        // 0x250 @ 0x100115fa0
    virtual void RecalculateLeaders(ITrafficAutomataAutomaton* automaton);             // 0x258 @ 0x100115ac0
    virtual bool DispatchResponseAutomata(std::uint32_t group, cISC4Occupant* occupant, std::uint32_t count, bool forced,
                                          std::uint8_t pathType, std::uint32_t flags);   // 0x260 @ 0x100112114
    virtual ITrafficAutomataAutomaton* SpawnOccupantAutomaton(ITrafficNetworkMapOccupant* occupant, int param2,
                                                              std::uint32_t group, std::uint32_t flags, bool param5,
                                                              int pathType);            // 0x268 @ 0x100114fea (PARTIAL)
    virtual void GetEdgeDensity(ITrafficNetworkMapOccupant* occupant, TrafficTileInfo* tile, PopulateLayer layer,
                                float clockMultiplier, float (*weights)[4]);            // 0x270 @ 0x100115196
    virtual int GetPathTypeForLayer(PopulateLayer layer) const;                        // 0x278 @ 0x100114f92
    virtual std::uint32_t GetAutomataGroupForLayer(PopulateLayer layer) const;         // 0x280 @ 0x100114fbe
    virtual double GetEdgeDensityMultiplier(PopulateLayer layer);                      // 0x288 @ 0x1001153c6
    virtual void PopulateVisibleArea(float deltaMilliseconds);                         // 0x290 @ 0x100112f76
    virtual void PopulateNetworkOccupant(ITrafficNetworkMapOccupant* occupant, TrafficTileInfo* tile, PopulateLayer layer,
                                         int cellX, int cellZ, float clockMultiplier, double densityMultiplier);   // 0x298
    virtual void PopulateNetworkOccupant(ITrafficNetworkMapOccupant* occupant, TrafficTileInfo* tile, int cellX,
                                         int cellZ, float elapsedMilliseconds);        // 0x2a0 @ 0x10011347e (PARTIAL)
    // MaybeSpawnAutomaton's and SpawnAutomaton's common part: the edge's lane key, the automaton made there on its trip.
    bool SpawnOnEdge(ITrafficNetworkMapOccupant* occupant, PopulateLayer layer, std::uint8_t entry, std::uint8_t exit,
                     bool gated, float elapsedSeconds, double density);
    virtual bool MaybeSpawnAutomaton(ITrafficNetworkMapOccupant* occupant, TrafficTileInfo* tile, PopulateLayer layer,
                                     float elapsedMilliseconds, double density, std::uint8_t entry,
                                     std::uint8_t exit);                              // 0x2a8 @ 0x100114214 (PARTIAL)
    virtual void SpawnAutomaton(ITrafficNetworkMapOccupant* occupant, TrafficTileInfo* tile, PopulateLayer layer,
                                std::uint8_t entry, std::uint8_t exit, float elapsedMilliseconds);   // 0x2b0 (PARTIAL)
    virtual void RecallAutomatonFromNetwork(TrafficTileInfo* tile, std::uint32_t pathType);   // 0x2b8 @ 0x100114c6e
    virtual bool IsOffscreenEntryEdge(TrafficTileInfo* tile, int cellX, int cellZ, std::uint8_t entry);   // 0x2c0
    virtual bool PopulateCrimeLayer(float clockMultiplier);                            // 0x2c8 @ 0x1001154ba (PARTIAL)
    virtual bool UpdateResponseList(float deltaMilliseconds);                          // 0x2d0 @ 0x10011220c
    virtual cSC4ResponseCurve& GetEdgeDensityCurve(PopulateLayer layer);               // 0x2d8 @ 0x100115462
    virtual float GetMaxAutosPerSecond(PopulateLayer layer);                           // 0x2e0 @ 0x10011548c
    virtual float GetCommotionMultiplier(ITrafficNetworkMapOccupant* occupant, PopulateLayer layer);   // 0x2e8
    virtual void GetOccupantTripEdgeDensity(ITrafficNetworkMapOccupant* occupant, TrafficTileInfo* tile,
                                            float multiplier, float (*weights)[4]);    // 0x2f0 @ 0x100115310
    virtual bool PopulateNetworkOccupantForCrimeLayer(ITrafficNetworkMapOccupant* occupant, TrafficTileInfo* tile,
                                                      float density, std::uint32_t cellX, std::uint32_t cellZ);   // 0x2f8
    virtual bool GetDispatchDestinationPoint(ITrafficAutomataAutomaton* automaton, cS3DVector3& out);   // 0x300 (PARTIAL)
    virtual int ChooseNetworkTypeForPathType(ITrafficNetworkMapOccupant* occupant, std::uint32_t pathType);   // 0x308

    // ---- non-virtual helpers (the original declares them as plain members) ----
    bool IsTrafficAutomaton(IAutomataAutomaton* automaton) const;                     // @ 0x100112d36
    void CreateAccident(cISC4Occupant* occupant, int type, std::uint32_t duration);   // @ 0x100113b98
    void ChooseRandomModel(std::uint32_t group, ITrafficNetworkMapOccupant* occupant, PopulateLayer layer,
                           cGZPersistResourceKey& out);                               // @ 0x10011471a
    bool IsTrainGroup(std::uint32_t group) const;                                     // @ 0x100112812
    bool IsLayerVisible(std::uint32_t layer);                                         // @ 0x100113438
    std::uint32_t DispatchResponseCount() const { return static_cast<std::uint32_t>(responses_.size()); }

    // ---- cSC4VehicleManager's constructor writes these five (0x1000d9c40); the plain traffic manager leaves them 0 ----
    void SetDefaultTypes(std::uint32_t pathType, std::uint32_t networkType);   // +0xb10 / +0xb14
    void SetCrimeLayerMultiplier(float multiplier);                            // +0xb40
    void AddLayer(std::uint32_t layer);                                        // +0xb18 push_back
    static const std::uint32_t* VehicleLayerOrder();                           // 1,2,3,5,4,7,6,8,9,10
    static constexpr int kVehicleLayerCount = 10;

    // ---- effect host (cSC4TrafficEffectManager's ITrafficEffectHost) ----
    std::uint32_t ElapsedMilliseconds() override;                              // the timer at +0xb78
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override;     // SL::spCity slot 0x2f8
    bool AttachAccidentController(cISC4Occupant* occupant, const float* position) override;   // SL::spCity slot 0x178
    void DetachController(cISC4Occupant* occupant) override;                    // SL::spCity slot 0x178

    // ---- test / engine access ----
    ITrafficAutomataManagerHost* TrafficHost() const { return trafficHost_; }
    cSC4TrafficEffectManager& EffectManager() { return effectManager_; }        // +0xb60
    const cSC4TrafficEffectManager& EffectManager() const { return effectManager_; }
    const std::list<tTrafficLayerUpdate>& Layers() const { return layerUpdates_; }
    const std::vector<tResponseAutomataInfo>& Responses() const { return responses_; }
    std::uint32_t DefaultPathType() const { return defaultPathType_; }          // +0xb10
    std::uint32_t DefaultNetworkType() const { return defaultNetworkType_; }    // +0xb14
    float CrimeLayerMultiplier() const { return crimeLayerMultiplier_; }        // +0xb40
    ITrafficSimulatorApi* TrafficSimulator() const { return trafficSimulator_; }   // +0xb08
    static int FloorIdiom(float value);                                        // the float->int rounding at 0x100111fc5
    static const std::uint32_t* PathTypeTable();                               // 0x100a18b20
    static const std::uint32_t* AutomataGroupTable();                          // 0x100a18b40

protected:
    ITrafficAutomataManagerHost* TrafficHostOrGlobal() const;
    void AdvanceLayer();

    ITrafficAutomataManagerHost* trafficHost_ = nullptr;   // port seam (not in the original)
    ITrafficSimulatorApi* trafficSimulator_ = nullptr;     // +0xb08 counted
    std::uint32_t defaultPathType_ = 0;                    // +0xb10
    std::uint32_t defaultNetworkType_ = 0;                 // +0xb14
    std::list<tTrafficLayerUpdate> layerUpdates_;           // +0xb18 (original std::list, node +0x10/+0x14)
    std::list<tTrafficLayerUpdate>::iterator currentLayer_ = layerUpdates_.end();   // +0xb30
    bool currentLayerValid_ = false;                       // the original's +0xb30 == null test
    int currentCellX_ = -1;                                // +0xb38
    int currentCellZ_ = -1;                                // +0xb3c
    float crimeLayerMultiplier_ = 0.0f;                    // +0xb40
    std::vector<tResponseAutomataInfo> responses_;         // +0xb48 (0x28 bytes per entry in the original)
    cSC4TrafficEffectManager effectManager_;               // +0xb60
    cRZTimer timer_{cRZTimer::kMilliseconds};              // the effect manager's timer, +0xb78 in the original
};

}  // namespace sc4
