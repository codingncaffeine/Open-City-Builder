// cSC4WatercraftManager — the per-kind automata manager that owns the city's boats and ferries (42 symbols,
// 0x1004edca0..0x1004fe4aa; primary vtable 0x100ab9d00). It derives from cSC4AutomataManager in the original (the
// city's per-kind managers are the four objects the automata manager asks for a group: vehicle / aircraft / pedestrian
// / watercraft) and adds the watercraft-specific bookkeeping:
//   * four registries: ferry-terminal lots -> terminal information, terminal lots -> terminal occupant,
//     marina occupants -> their boat, and the ambient boats spawned along the view edges;
//   * docking permission: a terminal can hand out one docking slot and one queueing slot at a time
//     (RequestPermissionToDock/Wait, DoneDocking/DoneQueueing);
//   * the position queries the boats use: LookForBoatsAtPosition, CollisionDetection, ClearToLaunchBoat,
//     GetPropPositionAndOrientation, GetLotBodyOfWater, GetRandomCreationPoint;
//   * ferry/marina creation (CreateFerry, CreateFerryStruct, CreateMarinaBoat) and the ambient spawner
//     (PopulateVisibleArea, GetEdgeCells);
//   * persistence (Write/Read, stream version 6).
//
// Class id 0xa98f9d28; the primary vtable carries the four cSC4AutomataManager views (0x98f9bf4 primary,
// 1 cIGZMessageTarget, 0xe4fda3d4 cIGZSerializable, 0x452294aa cIGZMessageTarget2). Init registers message
// 0x2c06ba82 (the "ferry routes changed" notification DoAnimationTick consumes).
//
// Original layout (0xba0 bytes; cSC4AutomataManager is 0xb08 of it):
//   +0x44 the inherited "initialised" flag            +0x48 the inherited refcount
//   +0xa84 the inherited min zoom (HandleTuningParametersChanged writes it from the automata tuning exemplar)
//   +0xb08 the flag message 0x2c06ba82 sets / DoAnimationTick clears (a ferry-route rebuild is pending)
//   +0xb10 std::unordered_map<cRZAutoRefCount<cISC4Lot>, FerryTerminalInformation>   (0x28; size at +0xb28)
//   +0xb38 std::unordered_map<cRZAutoRefCount<cISC4Lot>, cRZAutoRefCount<cISC4Occupant>>  (size at +0xb50)
//   +0xb60 std::unordered_map<cRZAutoRefCount<cISC4Occupant>, cRZAutoRefCount<cSC4Watercraft>>  (size at +0xb78)
//   +0xb88 std::vector<cRZAutoRefCount<cSC4Watercraft>>  (the ambient boats, group 0x4418)
// The two counts the boats read as "docks"/"queues" are the sizes at +0xb28 (terminal lots) and +0xb78 (marina
// occupants); see FleetCounts.
//
// Occupant properties (checked on cISC4Occupant::AsPropertyHolder()):
//   0xec39dd14 ferry terminal (a lot whose prop occupant marks a ferry dock),
//   0x8c392d39 marina terminal lot (lot -> occupant registry),
//   the ten kMarinaBuildingProperties marina/seaport buildings (occupant -> boat registry),
//   0x029244db "no automata" (the automaton DoMessage filter).
//
// The parent: the recovered cSC4AutomataManager cannot be inherited here because its header and the recovered
// cSC4Watercraft / cSC4Automaton headers are mutually incompatible in the port (both define sc4::IAutomatonGroupTemplate
// and several iids). The inherited calls therefore go through IWatercraftManagerBase, one method per original call;
// the object that owns the manager supplies it (hook-up item). The port also keeps its own boat list (see Boats())
// because the recovered cSC4Watercraft does not implement IAutomataAutomaton, which the base registry stores.
//
// Quirks kept (each asserted by the test, with the original address):
//  * RequestPermissionToDock/Wait return true with a null boat without recording anything (they only fail when the
//    terminal is unknown or the slot is taken).
//  * DoneDocking/DoneQueueing clear the slot only when the stored boat is the one passed in; a foreign boat leaves
//    the slot (and its reference) in place.
//  * LookForBoatsAtPosition accepts a boat at *twice* its radius (the comparison is `distance < radius + radius`).
//  * HandleOccupantInserted registers a terminal only when GetPropPositionAndOrientation succeeds, but the marina
//    lot registry (0x8c392d39) is filled with the result ignored; a terminal lot already in the map is left alone.
//  * HandleOccupantRemoved only removes a ferry-terminal entry when the removed occupant is *that* entry's terminal
//    occupant; it pushes state 0x49 (remove with fade, timer -1) on every ferry boat of the entry first.
//  * GetEdgeCells advances the edge index modulo 4, and its four edges are (x0..x1-1, z0), (x1, z0..z1-1),
//    (x0..x1-1, z1), (x0, z0..z1-1); it computes the city centre world position and never uses it.
//  * GetRandomCreationPoint's spiral is bounded by the *city size*, not by the last cell (max = size, not size-1), and
//    its final "inside the city" test passes the cell indices as floats to PointInCity.
//  * GetLotBodyOfWater queries the water body with (z, x) corner pairs (the argument order the original passes).
//  * CollisionDetection picks the collision distance from the *larger* of the two boats' bounding-box extents, and
//    the second bounding box is built with scale 1.0 and a zero position (the original's constant).
//  * GetPropPositionAndOrientation's `offsetVariant` branch only moves x; its y and z additions are +-0.0.
//  * HandleTuningParametersChanged passes the raw bits of the inherited tuning field f218a8 (a float slot) to
//    SetMaxCount, then reads the min zoom from the exemplar (property 0x4998c91b) and stores it minus one.
//  * Write clamps the ferry route state to {0, 1} and writes -1 for anything else; the runtime-only fields (the
//    ferry boat of a route and the "at station" flag) are not written.
//
// Seams: the watercraft manager reaches the city/terrain/surface-water/traffic-network-map/RM/stream globals of the
// original through IWatercraftManagerHost below (each method names the call it replaces).
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gz/StreamOps.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "rz/cRZAutoRefCount.h"
#include "rz/cRZRandomSuper.h"
#include "sc4/Exemplar.h"
#include "sc4/Lot.h"
#include "sc4/Occupant.h"
#include "sc4/Watercraft.h"

namespace sc4 {

class cSC4WatercraftManager;
class IWatercraftManagerHost;

constexpr std::uint32_t kGZCLSID_cSC4WatercraftManager = 0xa98f9d28;
constexpr std::uint32_t kGZIID_cISC4WatercraftManager = 0xa98f9d28;
constexpr std::uint32_t kMessageWatercraftRoutesChanged = 0x2c06ba82;   // registered in Init / consumed by DoAnimationTick

// Occupant properties (cISCPropertyHolder::HasProperty).
constexpr std::uint32_t kPropertyWatercraftFerryTerminal = 0xec39dd14;
constexpr std::uint32_t kPropertyWatercraftMarinaLot = 0x8c392d39;
constexpr std::uint32_t kPropertyNoAutomata = 0x029244db;               // the automaton manager's filter
// The marina/seaport buildings whose occupant gets a boat (the ten properties HandleOccupantInserted probes in order).
constexpr std::uint32_t kMarinaBuildingProperties[10] = {
    0x4c18336d, 0xac183320, 0x6c183ffb, 0x8c184040, 0xac18330c,
    0x4c1832f5, 0xec1832e7, 0x6c1832d1, 0xcc3a1723, 0x6c36283c,
};

// The groups the manager creates and tests (the names shared with cSC4Watercraft live in Watercraft.h).
constexpr std::uint32_t kWatercraftManagerGroupFerryA = 0x4400;   // the two ferry kinds have the group-template ids
constexpr std::uint32_t kWatercraftManagerGroupFerryB = 0x4402;   // 0x4400 / 0x4402 (cSC4Watercraft::GetAutomataGroup)
constexpr std::uint32_t kWatercraftManagerGroupMarinaBoat = 0x4416;
constexpr std::uint32_t kWatercraftManagerGroupAmbientBoat = 0x4418;

// The constants the spawner stamps on the boats it creates.
constexpr float kWatercraftTowWaitThreshold = 45000.0f;       // PopulateVisibleArea
constexpr float kWatercraftMarinaWaitThreshold = 60000.0f;    // CreateMarinaBoat
constexpr std::uint32_t kWatercraftCreateFlags = 0x10002;     // the creation flags of every manager-made boat
constexpr int kWatercraftMaxCount = 500;                       // Init's SetMaxCount

// ---- the ferry route records --------------------------------------------------------------------------------------
// cISC4TrafficSimulator::FerryRouteInfo (0x58 bytes). The class is unrecovered (its routes come from
// cSC4TrafficSimulator::GetFerryRoutesInUse); the watercraft manager only copies it and reads/writes its fields, so
// the port keeps the same layout and names the fields after what the manager does with them.
struct FerryRouteInfo {
    cRZAutoRefCount<cSC4Lot> fromLot;    // +0x00 the terminal the route starts at
    cRZAutoRefCount<cSC4Lot> toLot;      // +0x08 the terminal it ends at
    std::int32_t state = 0;              // +0x10 only 0 and 1 survive Write (everything else is written as -1)
    std::uint16_t f14 = 0;               // +0x14
    std::uint16_t f16 = 0;               // +0x16
    std::uint16_t f18 = 0;               // +0x18
    std::uint16_t f1a = 0;               // +0x1a
    std::uint8_t f1c = 0;                // +0x1c
    std::int16_t f1e = 0;                // +0x1e
    std::int16_t f20 = 0;                // +0x20
    std::vector<std::uint8_t> route;     // +0x28 (compressed route points)
    std::vector<std::uint8_t> route2;    // +0x40 (carried along)
};
static_assert(sizeof(FerryRouteInfo) == 0x58, "FerryRouteInfo layout");

// cSC4WatercraftManager::FerryRouteInformation (0x68 bytes): one ferry-route slot of a terminal.
struct FerryRouteInformation {
    cRZAutoRefCount<cSC4Watercraft> boat;   // +0x00 the boat running this route (runtime only, not persisted)
    FerryRouteInfo route;                   // +0x08
    bool atStation = false;                 // +0x60 toggled by CreateFerry (which station the boat starts from)
};
static_assert(sizeof(FerryRouteInformation) == 0x68, "FerryRouteInformation layout");

// cSC4WatercraftManager::FerryTerminalInformation (0x30 bytes): the docking state of one ferry-terminal lot.
struct FerryTerminalInformation {
    cRZAutoRefCount<cSC4Watercraft> dockBoat;    // +0x00 the boat holding the docking slot
    cRZAutoRefCount<cSC4Watercraft> waitBoat;    // +0x08 the boat holding the queueing slot
    std::vector<FerryRouteInformation> routes;   // +0x10
    cRZAutoRefCount<cISC4Occupant> occupant;     // +0x28 the terminal occupant (0xec39dd14)
};
static_assert(sizeof(FerryTerminalInformation) == 0x30, "FerryTerminalInformation layout");

// The hash/equality the original uses for all four registries (tVoidPtrHash): the pointer value, not the pointee.
template <class T>
struct WatercraftRefHash {
    std::size_t operator()(const cRZAutoRefCount<T>& value) const noexcept {
        return std::hash<const void*>()(static_cast<const void*>(value.get()));
    }
};
template <class T>
struct WatercraftRefEqual {
    bool operator()(const cRZAutoRefCount<T>& a, const cRZAutoRefCount<T>& b) const noexcept {
        return a.get() == b.get();
    }
};

// ---- seams ---------------------------------------------------------------------------------------------------------
// The surface-water "body of water" object (SL::spSurfaceWater vslot 0x48 returns it; its vslot 0x38 answers the body
// id of a cell, 0 when the cell is not water).
class IWatercraftBodyOfWater {
public:
    virtual ~IWatercraftBodyOfWater() = default;
    virtual std::int16_t BodyOfWaterAt(int cellX, int cellZ) = 0;   // water object vslot 0x38
};

// cSC4AutomataManager as this class uses it (the inherited half). One method per original entry point; the original
// object is the base sub-object of the manager (hook-up item: make the two recovered headers coexist and inherit).
class IWatercraftManagerBase {
public:
    virtual ~IWatercraftManagerBase() = default;
    virtual bool Initialized() const = 0;                                  // +0x44
    virtual bool Init() = 0;                                               // @ 0x1000fee58
    virtual bool Shutdown() = 0;                                           // @ 0x1000ff06a
    virtual bool DoMessage(cIGZMessage2* message) = 0;                     // @ 0x1000ff7d4
    virtual bool Write(cIGZOStream& stream) const = 0;                     // @ 0x1001047a0
    virtual bool Read(cIGZIStream& stream) = 0;                            // @ 0x100104870
    virtual bool ReadInternal(cIGZIStream& stream, std::uint16_t version) = 0;   // @ 0x1001048aa
    virtual void DoAnimationTick(std::uint32_t data, float milliseconds) = 0;   // @ 0x100101424
    virtual bool CalculateSimSpeedModifier() = 0;                          // @ 0x1001015f2
    virtual bool SimSpeedPaused() const = 0;                               // sbSimSpeedPaused
    virtual bool IsDrivingMode() const = 0;                                // @ 0x1000ff79e (vslot 0x68)
    virtual bool CanCreateGroup(std::uint32_t group) const = 0;            // @ 0x100104e1c (vslot 0xf0)
    virtual void SetMaxCount(int count) = 0;                               // @ 0x100102580 (vslot 0xb8)
    virtual void SetMinZoom(int zoom) = 0;                                 // base +0xa84
    virtual void DestroyAutomata() = 0;                                    // @ 0x1001017a0 (vslot 0x198)
    virtual bool PostCityInitPending() const = 0;                          // base +0x20
    virtual bool QueryInterface(std::uint32_t iid, void** out) = 0;        // the four inherited views
    virtual std::uint32_t AddRef() = 0;
    virtual std::uint32_t Release() = 0;
    virtual cRZRandomSuper& Random() = 0;                                  // base +0x78 (SL::mpRandom)
    virtual float TuningMaxCountField() const = 0;                         // sTuningParams+0x28 (read as raw 32 bits)
    virtual float RetireOpacityDuration() const = 0;                       // sTuningParams.f219b4
};

// Everything the watercraft manager reached through an engine global. Each method names the original call.
class IWatercraftManagerHost {
public:
    virtual ~IWatercraftManagerHost() = default;
    // ---- message server (SL::spMS2 vslots 0x28 / 0x30)
    virtual bool AddMessageNotification(cIGZMessageTarget2* target, std::uint32_t type) = 0;
    virtual bool RemoveMessageNotification(cIGZMessageTarget2* target, std::uint32_t type) = 0;
    // ---- city (SL::spCity)
    virtual void WorldToCell(float x, float z, int& cellX, int& cellZ) = 0;      // vslot 0x2f8
    virtual void CellToWorld(int cellX, int cellZ, float& x, float& z) = 0;      // vslot 0x308
    virtual bool CellInCity(int cellX, int cellZ) = 0;                           // vslot 0x318
    virtual bool PointInCity(float x, float z) = 0;                              // vslot 0x310
    virtual void CityCellCount(int& sizeX, int& sizeZ) = 0;                      // vslots 0x2e8 / 0x2f0
    virtual float CellSize() = 0;                                                // vslot 0x2d8
    virtual cSC4Lot* LotAtPosition(const float* xyz) = 0;                        // 0x108 -> vslot 0x30 (counted)
    virtual cSC4Lot* LotAtCell(int cellX, int cellZ) = 0;                        // 0x108 -> vslot 0x28 (counted)
    // ---- terrain (SL::spTerrain vslot 0xe8: the height at a world position)
    virtual float TerrainHeight(float x, float z) = 0;
    // ---- surface water (SL::spSurfaceWater)
    virtual IWatercraftBodyOfWater* BodyOfWater() = 0;                           // vslot 0x48 (borrowed)
    virtual bool IsWaterCell(int cellX, int cellZ) = 0;                          // vslot 0x38 (ClearToLaunchBoat)
    // ---- the prop view of an occupant (QueryInterface 0xe9793a65, then vslot 0x48 = the facing index).
    //      Returns false when the occupant is not a prop occupant (the original's QI failure).
    virtual bool PropOccupantFacing(cISC4Occupant* occupant, int& facing) = 0;
    // ---- the traffic network map's visible cell rect (SL::spTrafficNetworkMap vslot 0x88: x0, z0, x1, z1)
    virtual void VisibleCellRect(int rect[4]) = 0;
    // ---- resource manager (SL::spRM vslot 0x18, key 0xa998d30b6534284a/1, iid 0xa52160f5, counted)
    virtual cISCResExemplar* GetAutomataTuningExemplar() = 0;
    // ---- streams: SC4WriteGZSerializable @ 0x1000435e3 / SC4ReadGZSerializable @ 0x100043267 with the DB segment
    //      stream the original is handed. cSC4Lot has no cIGZSerializable view in the port, so lots get their own
    //      pair (still one original call each: SC4WriteGZSerializable(stream, lot) / SC4ReadGZSerializable(stream,
    //      0xe818fced, &lot, false)).
    virtual bool WriteSerializable(cIGZOStream& stream, cIGZUnknown* object) = 0;
    virtual cIGZUnknown* ReadSerializable(cIGZIStream& stream, std::uint32_t iid) = 0;
    virtual bool WriteLot(cIGZOStream& stream, cSC4Lot* lot) = 0;
    virtual cSC4Lot* ReadLot(cIGZIStream& stream) = 0;
    // ---- the per-kind creation entry of the base manager (cSC4AutomataManager::CreateAutomaton @ 0x100100698).
    //      The port has to seam it: the recovered base returns IAutomataAutomaton*, and the recovered
    //      cSC4Watercraft does not implement that interface (hook-up item).
    virtual cSC4Watercraft* CreateWatercraft(std::uint32_t group, const cS3DVector3& position, std::uint32_t flags,
                                             const cGZPersistResourceKey& key, cIGZMessageTarget2* listener,
                                             std::uint8_t argument, const cS3DVector3* velocity) = 0;
};

class cSC4WatercraftManager : public cIGZMessageTarget2 {
public:
    cSC4WatercraftManager() = default;                       // @ 0x1004edca0 (the deleting dtor at 0x1004fde84)
    virtual ~cSC4WatercraftManager() = default;              // @ 0x1004fe3d4 (the original dtor is virtual)

    // ---- cIGZUnknown (the original inherits these from the base; the port keeps its own refcount at the same slot +0x48)
    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x1004ef596 (thunks 0x1004ef5b4..)
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    std::uint32_t GetGZCLSID() const { return kGZCLSID_cSC4WatercraftManager; }   // @ 0x1004fdebc

    // ---- lifecycle
    bool Init();                                             // @ 0x1004eddb6
    bool Shutdown();                                         // @ 0x1004ede32
    bool DoMessage(cIGZMessage2* message) override;           // @ 0x1004edee6 (thunk 0x1004ef140)
    bool HandleTuningParametersChanged();                    // @ 0x1004ef4c2
    void DoAnimationTick(std::uint32_t data, float milliseconds);   // @ 0x1004f29a2 (PARTIAL)

    // ---- the per-kind manager entry points
    bool AddAutomaton(cSC4Watercraft* automaton);            // @ 0x1004ef220
    bool RemoveAutomaton(cSC4Watercraft* automaton, bool force);   // @ 0x1004ef274
    cSC4Watercraft* CreateAutomaton(std::uint32_t group, const cS3DVector3& position, std::uint32_t flags,
                                    const cGZPersistResourceKey& key, cIGZMessageTarget2* listener,
                                    std::uint8_t argument, const cS3DVector3* velocity);   // @ 0x1004f2054
    bool GetRandomCreationPoint(std::uint32_t group, const cS3DVector3& center, float minDistance, float maxDistance,
                                cS3DVector3& outPosition, std::uint32_t flags,
                                cS3DVector3* outDirection);   // @ 0x1004f4024

    // ---- occupant notifications (DoMessage 0x99ef1142 / 0x99ef1143)
    void HandleOccupantInserted(cISC4Occupant* occupant);    // @ 0x1004edfb4
    void HandleOccupantRemoved(cISC4Occupant* occupant);     // @ 0x1004eee5e

    // ---- queries the boats call
    cSC4Watercraft* LookForBoatsAtPosition(const cS3DVector3& position, cSC4Watercraft* exclude);   // @ 0x1004ef14e
    cSC4Watercraft* CollisionDetection(cSC4Watercraft* self, const cS3DVector3& position, float scale);   // @ 0x1004f4908
    bool ClearToLaunchBoat(int cellX, int cellZ, int unknown);   // @ 0x1004f1f2e
    bool GetPropPositionAndOrientation(cISC4Occupant* occupant, cS3DVector3& position, cS3DVector3& direction,
                                       bool offsetVariant);   // @ 0x1004f0e16
    std::int16_t GetLotBodyOfWater(cSC4Lot* lot);            // @ 0x1004f3f88
    int BoatPriority(cSC4Watercraft* boat);                  // @ 0x1004f3802

    // ---- the docking state machine
    bool RequestPermissionToDock(cSC4Lot* lot, cSC4Watercraft* boat);   // @ 0x1004f4690
    bool RequestPermissionToWait(cSC4Lot* lot, cSC4Watercraft* boat);   // @ 0x1004f472e
    void DoneDocking(cSC4Lot* lot, cSC4Watercraft* boat);    // @ 0x1004f47cc
    void DoneQueueing(cSC4Lot* lot, cSC4Watercraft* boat);   // @ 0x1004f4864

    // ---- ambient spawner
    void PopulateVisibleArea(float milliseconds);            // @ 0x1004f3900
    void GetEdgeCells(int& index, std::vector<std::pair<int, int>>& out);   // @ 0x1004f3d66

    // ---- persistence (stream version 6)
    bool Write(cIGZOStream& stream) const;                   // @ 0x1004ef61a (thunk 0x1004ef7f4)
    bool Read(cIGZIStream& stream);                          // @ 0x1004ef802 (thunk 0x1004f0e08)

    // ---- ferry / marina creation (PARTIAL: the traffic simulator's ferry routes / neighbour connections)
    FerryRouteInformation* CreateFerry(FerryTerminalInformation* stationA, FerryTerminalInformation* stationB,
                                       FerryRouteInformation* route);   // @ 0x1004f1144 (PARTIAL)
    void CreateFerryStruct(FerryTerminalInformation* station, FerryRouteInformation* route,
                           FerryStationInformation& stationA, FerryStationInformation& stationB);   // @ 0x1004f180e (PARTIAL)
    bool CreateMarinaBoat(cISC4Occupant* occupant, std::uint32_t group);   // @ 0x1004f212e (PARTIAL)

    // ---- the seams
    IWatercraftManagerHost* Host() const { return host_; }
    void SetHost(IWatercraftManagerHost* host) { host_ = host; }
    IWatercraftManagerBase* Base() const { return base_; }
    void SetBase(IWatercraftManagerBase* base) { base_ = base; }

    // The two fleet counts the boats read: docks = the ferry-terminal lots (+0xb28), queues = the marina occupants
    // (+0xb78).
    void FleetCounts(std::int32_t& docks, std::int32_t& queues) const;
    std::int32_t TerminalCount() const { return static_cast<std::int32_t>(terminals_.size()); }
    std::int32_t MarinaOccupantCount() const { return static_cast<std::int32_t>(marinaOccupants_.size()); }
    std::int32_t MarinaLotCount() const { return static_cast<std::int32_t>(marinaLots_.size()); }
    std::int32_t AmbientBoatCount() const { return static_cast<std::int32_t>(ambientBoats_.size()); }
    std::int32_t BoatCount() const { return static_cast<std::int32_t>(boats_.size()); }
    bool RoutesChanged() const { return routesChanged_; }
    std::uint32_t RefCount() const { return refCount_; }

    // test access
    using TerminalMap = std::unordered_map<cRZAutoRefCount<cSC4Lot>, FerryTerminalInformation, WatercraftRefHash<cSC4Lot>,
                                           WatercraftRefEqual<cSC4Lot>>;
    using MarinaLotMap = std::unordered_map<cRZAutoRefCount<cSC4Lot>, cRZAutoRefCount<cISC4Occupant>,
                                            WatercraftRefHash<cSC4Lot>, WatercraftRefEqual<cSC4Lot>>;
    using MarinaOccupantMap = std::unordered_map<cRZAutoRefCount<cISC4Occupant>, cRZAutoRefCount<cSC4Watercraft>,
                                                 WatercraftRefHash<cISC4Occupant>, WatercraftRefEqual<cISC4Occupant>>;
    const TerminalMap& Terminals() const { return terminals_; }
    const MarinaLotMap& MarinaLots() const { return marinaLots_; }
    const MarinaOccupantMap& MarinaOccupants() const { return marinaOccupants_; }
    TerminalMap& Terminals() { return terminals_; }
    MarinaLotMap& MarinaLots() { return marinaLots_; }
    MarinaOccupantMap& MarinaOccupants() { return marinaOccupants_; }
    const std::list<cRZAutoRefCount<cSC4Watercraft>>& Boats() const { return boats_; }
    const std::list<cRZAutoRefCount<cSC4Watercraft>>& AmbientBoats() const { return ambientBoats_; }

protected:
    IWatercraftManagerBase* base_ = nullptr;   // port seam for the inherited half (see above)
    IWatercraftManagerHost* host_ = nullptr;   // port seam (the original's SL::sp* globals)
    std::uint32_t refCount_ = 0;               // +0x48

    bool routesChanged_ = false;               // +0xb08

    // The four registries (the original's four containers at +0xb10 / +0xb38 / +0xb60 / +0xb88).
    TerminalMap terminals_;                    // +0xb10 lot -> terminal information
    MarinaLotMap marinaLots_;                  // +0xb38 lot -> terminal occupant
    MarinaOccupantMap marinaOccupants_;        // +0xb60 occupant -> boat
    std::list<cRZAutoRefCount<cSC4Watercraft>> boats_;         // the registered boats (port: the base +0x58 list)
    std::list<cRZAutoRefCount<cSC4Watercraft>> ambientBoats_;  // +0xb88 the ambient boats
};

}  // namespace sc4
