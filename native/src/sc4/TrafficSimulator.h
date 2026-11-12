// cSC4TrafficSimulator — the data-driven core of the traffic simulator (class id 0x6990c1aa, simulator type
// 0x88027f0d). The original is a 15,000-line class (path finder, trip assignment, ferries, subnetwork flood
// fill); this file recovers the parts that do not depend on those: tunables, network-info grid, travel-time and
// capacity queries, funding -> road/rail damage, mass-transit strikes and budget feeds, the neighbour-connection
// tables (border slots, pools, map exits, the capacity handout and the roll-over) and the trip-pipeline setup.
// Still unrecovered: the path finder calls (FindPath / ApplyTraffic / GetCompressedRoute), the subnetwork flood
// fill (FloodSubnetwork / UpdateSubnetworkInfo), the DoDailySimulation stage machine and the streams.
// Everything that talks to the city (messages, departments, demand grids, ordinances, lots, neighbour
// connections) goes through ITrafficHost.
//
// Original layout (offsets in the 0x1000-byte object): +0x40/+0x44 city size in cells, +0xd8 network-info grid
// (x -> z -> chain of NetworkInfo by network type), +0x2c0 edge-density map, +0x350 traffic stats,
// +0x8a0 transit-switch map, +0x880 random, +0x8d0/+0x8d8 road / mass-transit budget departments,
// +0x908..0x938 budget counters, +0x96c strike state, +0x974.. road damage, +0x9d0.. rail damage,
// +0xbd0..0xf70 tunables (read from exemplar {0x6534284a, 0xe7e2c2db, 0xc9133286}).
#pragma once
#include <array>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZSerializable.h"
#include "rz/cRZRandom.h"
#include "sc4/CommonIIDs.h"
#include "sc4/MiscTypes.h"
#include "sc4/RecordTape.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/cSC4ResponseCurve.h"

namespace sc4 {

class cISC4PathFinder;
class cSC4PathFinder;
class cSC4NetworkOccupant;
struct cSC4TripData;
template <class T> class cSC4SimGrid;

constexpr std::uint32_t kGZCLSID_cSC4TrafficSimulator = 0x6990c1aa;
constexpr std::uint32_t kGZIID_cISC4TrafficSimulator = 0x68027f37;
constexpr std::uint32_t kBudgetPurposeTransitStation = 0xca550301;   // GetTransitSwitchQueryData's budget item
constexpr std::uint32_t kTrafficSimulatorType = 0x88027f0d;

// messages posted to the city
constexpr std::uint32_t kTrafficMessageStrikeAverted = 0x69247dc3, kTrafficMessageStrikeBegan = 0x69247dc4,
                        kTrafficMessageStrikeEnded = 0x69247dc5, kTrafficMessageStrikeThreatened = 0x69247dc6,
                        kTrafficMessagePotholeChanged = 0x69247dc8;
// exemplar property holding the 12 trip-capacity counters of a lot (16-bit array, widened on overflow)
constexpr std::uint32_t kTripCapacityProperty = 0x8a0b4100;

// Per-cell, per-network-type traffic data (the original chains these per cell).
// `networkType` is the record's key: the transit network n (0 pedestrian, 1 car, 2 train, 3 subway, 4 el-train, 5 monorail);
// `pieceType` is the network type 0..12 of the piece that carries it (traffic-network.md §2.2).
struct NetworkInfo {
    float inverseCapacity = 0.0f;   // +0: capacity = 1 / this
    std::uint8_t connectionCount = 0;  // +4
    std::uint8_t networkType = 0;   // +6
    std::uint8_t flag7 = 0;         // +7: bit 0 = a piece here has network flag 0x40000 (the bridge statistics row)
    float potholePenalty = 0.0f;    // +8: the extra step cost, set while the cell has a pothole
    std::uint8_t pieceType = 0;
    std::array<std::uint16_t, 4> subnetwork{};   // +0xc: flood-fill subnetwork id per lane group (0 = unassigned)
};

// Lot property listing the subnetwork ids the lot touches (32-bit array).
constexpr std::uint32_t kLotSubnetworksProperty = 0x2a4f9c74;

// One connected piece of road network: occupants reachable on it by occupant type, and the four destination
// counters (0x28 bytes in the original: a std::map plus four counters at +0x18).
struct SubnetworkInfo {
    std::map<std::uint32_t, std::uint32_t> occupantsByType;
    std::array<std::uint32_t, 4> destinations{};
};

struct TrafficCellRect { int x0 = 0, z0 = 0, x1 = 0, z1 = 0; };   // inclusive cells (SC4Rect<int>)
// SC4CellRegion<int>: bounds plus a bit mask indexed [x - x0][z - z0].
struct TrafficCellRegion {
    TrafficCellRect bounds;
    std::vector<std::vector<bool>> mask;
    bool Contains(int x, int z) const {
        unsigned dx = unsigned(x - bounds.x0), dz = unsigned(z - bounds.z0);
        return dx < mask.size() && dz < mask[dx].size() && mask[dx][dz];
    }
};

// Value part of an edge-density map entry (33 u16, traffic-simulator.md §4.8): last[4 * entry + exit], the usage (`density`, the
// sum of last at the dump) and current[16], which the routes add to.
struct TrafficEdgeDensity {
    std::array<std::uint16_t, 16> last{};
    std::uint16_t density = 0;
    std::array<std::uint16_t, 16> current{};
};

// A set of packed cells ((x << 8) | z) with O(1) insert, erase and uniform random pick. The original uses an
// unordered_set and walks to a random position; the distribution is the same.
class CellSet {
public:
    bool Insert(std::uint16_t cell);
    bool Erase(std::uint16_t cell);
    bool Contains(std::uint16_t cell) const { return index_.count(cell) != 0; }
    std::size_t Size() const { return items_.size(); }
    std::uint16_t At(std::size_t i) const { return items_[i]; }
    void Clear() { items_.clear(); index_.clear(); }
private:
    std::vector<std::uint16_t> items_;
    std::unordered_map<std::uint16_t, std::size_t> index_;
};

// One border-connection slot (8 bytes): the connection types that touch that border cell, the neighbour pool that
// holds the trip capacities of the city on the other side, and the travel cost of the connection. The four tables
// are vectors at +0xf0 west[z], +0x108 north[x], +0x120 east[z] and +0x138 south[x].
struct TrafficNeighborConnection {
    std::uint16_t types = 0;   // +0: bit per cISC4NeighborConnection type (kValidNeighborConnectionTypes...)
    std::uint16_t pool = 0;    // +2: key of the pool map (+0x150); 0 = the default pool (+0x168)
    float cost = 0.0f;         // +4: trip scale or the far side's property 0xca947077
};

// Per-cell record of the 0x2e-byte connection table at +0x98, one record per packed cell ((x << 8) | z) of the
// fixed 0x10000-cell table. Only the nine masks are read by the recovered code; the rest of the record is unused.
struct TrafficCellConnections {
    // conn[t]: bit (exit + 4 * entry) set = a type-t traveller that entered from side `entry` may leave through `exit`
    // (sides 0 west, 1 north, 2 east, 3 south). traffic-network.md §2.1.
    std::array<std::uint16_t, 9> masks{};
    // switch[t]: nibble `exit` = the travel type a type-t traveller becomes when it leaves through that side (0xf = none).
    std::array<std::uint16_t, 9> switches{0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff};
    // priorCar[entry]: bit (prev + 4 * exit) = a car that entered from `entry` may leave through `exit` when it entered the
    // previous cell from `prev`.
    std::array<std::uint16_t, 4> priorCar{0xffff, 0xffff, 0xffff, 0xffff};
    std::uint8_t wormhole = 0;   // bit 1 subway station (never set), 2 tunnel portal, 4 ferry terminal
};

// One trip search in progress (the original's cSC4TrafficSimulator::StandardTripParams at +0xa70).
struct StandardTripParams {
    int sourceX = 0, sourceZ = 0;            // +0/+4: the source cell
    cISC4PathFinder* primary = nullptr;      // +8
    cISC4PathFinder* secondary = nullptr;    // +0x10
    void* source = nullptr;                  // +0x18: cISC4TrafficSource
    std::uint8_t flag51 = 0;                 // +0x51: set when an earlier trip of this source was split
    std::uint32_t started = 0;               // +0x54
    std::uint8_t findersReset = 0;           // +0x58
    std::uint8_t secondPhase = 0;            // +0x59
    std::uint32_t workTime = 0;              // +0x5c: milliseconds handed to both path finders
    std::uint32_t useSecondary = 0;          // +0x60
};

// The cISC4Lot view the traffic simulator uses: its property holder (vslot 0x18), occupied rectangle (vslot
// 0x70) and traffic source (vslot 0x20; its vslot 0x18 is the lot's trip list, its vslot 0x20 refreshes that
// list when it is empty, and its vslot 0x28 is the start rectangle). Cached trip capacities live on the holder.
class ITrafficLot {
public:
    virtual ~ITrafficLot() = default;
    virtual cISCPropertyHolder* Properties() = 0;
    virtual void OccupiedRect(SC4RectInt32& rect) = 0;
    virtual void* TrafficSource() = 0;                              // lot vslot 0x20
    virtual std::list<cSC4TripData>* TripList(void* source) = 0;    // source vslot 0x18
    virtual void RefreshTrafficSource(void* source) = 0;            // source vslot 0x20 (UpdateTrafficVolume)
    // ---- what the work cycle reads (lot slots 21, 2, 28, 59, 12/13) ----
    virtual void Location(int& x, int& z) { SC4RectInt32 r; OccupiedRect(r); x = r.x0; z = r.z0; }
    virtual int ZoneType() { return 0; }
    virtual bool HasRoadAccess() { return false; }
    virtual ITrafficLot* LinkedLot() { return nullptr; }            // slot 59: the lot whose trips this one shares (none natively)
    virtual int Facing() { return 2; }                              // slot 23: 0..3 (2 leaves a transit switch's rule sides as stored)
    virtual void* Building() { return nullptr; }                    // the lot's building occupant, as an identity only
    virtual void ClearTripFlag() {}                                 // flag 0x80 (FinishTripMap)
    virtual bool IsSameLot(ITrafficLot* other) { return other == this; }
    // lot slot 54 GetPopulations: occupant counts by developer type, one map per occupant group (none: empty)
    virtual void Populations(std::vector<std::map<std::uint32_t, std::uint16_t>>& out) { out.clear(); }
};

// One budget department (road or mass transit) line items by index.
class ITrafficHost {
public:
    virtual ~ITrafficHost() = default;
    virtual void PostMessage(std::uint32_t message) = 0;
    virtual void PostPotholeMessage(int x, int z, bool potholed) = 0;      // kTrafficMessagePotholeChanged
    virtual void SetRoadBudgetExpense(int line, int amount) = 0;           // road department line item
    // Mass transit department; line 3 is created on first use (ids 0x6a3ff01c / 0x6a8b0cfb) when missing.
    virtual void SetMassTransitBudgetExpense(int line, int amount) = 0;
    virtual float MassTransitFundingFraction() = 0;                        // department +0x50
    virtual float CityCellSize() = 0;                                      // metres per cell
    virtual bool IsRightHandTraffic() = 0;                                 // first lot-manager flag, true when absent
    virtual short DemandGridValue(std::uint32_t gridId, int x, int z) = 0; // building-development grids
    // property value (SC4Percentage as 0..1 per entry) of every active ordinance's travel-strategy adjustments
    virtual void ActiveOrdinanceStrategyAdjustments(std::vector<std::vector<std::int32_t>>& out) = 0;

    // ---- the lot manager and cISC4Lot (lot manager vslot 0x28, lot vslots 0x18 / 0x70) ----
    // `filter` is the second argument the original passes: 1 in the commute-map and trip-destination sweeps,
    // 0 in the single-cell lookups.
    virtual ITrafficLot* LotAt(int x, int z, int filter) = 0;

    // ---- neighbour connections: cSC4NeighborsSimulator / cISC4NeighborConnection / cISC4RegionalCity ----
    virtual void* NeighborFindConnection(int connectionType, int x, int z) = 0;       // simulator vslot 0x58
    virtual void* NeighborFindConnectionOnOtherSide(void* connection) = 0;           // simulator vslot 0x70
    virtual void NeighborGetAllConnections(std::vector<void*>& out) = 0;             // simulator vslot 0x80
    virtual void NeighborGetConnectedCities(std::vector<void*>& out) = 0;            // simulator vslot 0xa8 (no filter)
    virtual cISCPropertyHolder* NeighborConnectionProperties(void* connection) = 0;  // connection vslot 0x18
    virtual void NeighborConnectionCell(void* connection, int& x, int& z) = 0;       // connection vslot 0x30
    virtual int NeighborConnectionType(void* connection) { (void)connection; return -1; }   // connection vslot 0x18
    virtual void* NeighborConnectionCity(void* connection) = 0;                      // connection vslot 0x48
    virtual bool NeighborCityEstablished(void* city) = 0;                            // city vslot 0x158
    virtual void NeighborCityRegionCell(void* city, int& x, int& z) = 0;             // city vslot 0x28
    // city vslot 0x88 (0x2010 / 0x2020 / 0x2030 are the population purposes the original asks for)
    virtual int NeighborCityExtrapolatedPopulation(void* city, std::uint32_t purpose) = 0;
    // demand simulator vslot 0x30 grid lookup for that id, then the grid's total (vslot 0x48)
    virtual int RegionDemandTotal(std::uint32_t gridId) = 0;
    // cISC4TrafficSource vslot 0x28: the rectangle the source's trips start from
    virtual SC4RectInt32 TrafficSourceStartRect(void* source) = 0;

    // ---- GetTransitSwitchQueryData (the query tool's transit station rows); defaults answer "nothing" ----
    // vslot 0x30 of the object a switch entry keeps at +0: its property holder (the budget item is looked up on it).
    virtual cISCPropertyHolder* TransitSwitchProperties(void* occupant) { (void)occupant; return nullptr; }
    // The lot's building (lot vslot 0x180 -> 0x18 -> 0x30): its property holder; null when the lot has none.
    virtual cISCPropertyHolder* LotBuildingProperties(ITrafficLot* lot) { (void)lot; return nullptr; }
    // Budget simulator vslot 0x198 GetBudgetItemForPurpose (department id and monthly cost) and 0xd8 -> 0x50 (funding fraction).
    virtual bool BudgetItemForPurpose(cISCPropertyHolder* holder, std::uint32_t purpose, std::uint32_t& department, std::int64_t& cost) {
        (void)holder; (void)purpose; department = 0; cost = 0; return false;
    }
    virtual bool DepartmentFundingFraction(std::uint32_t department, float& fraction) { (void)department; fraction = 0.0f; return false; }

    // ---- the live simulator: agents, the city, the lots and the network pieces (defaults answer "nothing") ----
    virtual void CityCellCount(int& x, int& z) { x = z = 0; }                                     // city slots 93 / 94
    virtual void AddAgent(cIGZMessageTarget2* target, int level, const char* name, std::uint32_t flags) { (void)target; (void)level; (void)name; (void)flags; }
    virtual void RemoveAgent(cIGZMessageTarget2* target) { (void)target; }
    virtual void AddNotification(cIGZMessageTarget2* target, std::uint32_t message) { (void)target; (void)message; }
    virtual void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t message) { (void)target; (void)message; }
    virtual const cISCPropertyHolder* SimulatorExemplar() { return nullptr; }                    // {0x6534284a, 0xe7e2c2db, 0xc9133286}
    virtual int DayOfMonth() { return 0; }                                                         // simulator GetSimDate
    virtual std::uint32_t SimDateNumber() { return 0; }
    // Every lot in the lot manager's list order (slot 15), as views the simulator owns.
    virtual void AllLots(std::vector<std::unique_ptr<ITrafficLot>>& out) { (void)out; }
    // The real lot behind a lot-state message's object (a copy of the lot): the lot at its corner, or null.
    virtual ITrafficLot* LotFromMessage(cIGZUnknown* object) { (void)object; return nullptr; }
    // The network pieces (occupant type 0xc772bf98) over an inclusive cell rect, one reference each.
    virtual void NetworkOccupantsInRect(int x0, int z0, int x1, int z1, std::vector<cSC4NetworkOccupant*>& out) {
        (void)x0; (void)z0; (void)x1; (void)z1; (void)out;
    }
    virtual void PositionToCell(float x, float z, int& cellX, int& cellZ) { cellX = int(x / 16.0f); cellZ = int(z / 16.0f); }   // city slot 95
    virtual float RoadFundingFraction() { return 1.0f; }                                           // roads department +0x50
    virtual void SetBudgetIncome(bool massTransit, int line, std::int64_t amount) { (void)massTransit; (void)line; (void)amount; }
    virtual void RecordHistory(std::uint32_t id, std::uint32_t value) { (void)id; (void)value; }   // history warehouse slot 12
    virtual bool OrdinanceOn(std::uint32_t id) { (void)id; return false; }                         // carpool 0xa2bf1ddc
    virtual float ResidentialWorkforcePercent(int x, int z) { (void)x; (void)z; return 1.0f; }    // residential slot 30
    virtual void PostTrafficMapChanged() {}                                                         // 0x69247dc7
    virtual void PostPotholeMessage3(int x, int z, std::uint32_t data3) { (void)x; (void)z; (void)data3; }   // 0x69247dc8 with data3
    // The end of a day's work: adapters the host handed out (LotAt) may be dropped now.
    virtual void EndOfWork() {}
};

// cISC4TrafficSimulator::TransitSwitchQueryData (0x14 bytes): what the query tool shows for a transit station.
struct TransitSwitchQueryData {
    std::int32_t served = 0;        // +0
    std::int32_t capacity = 0;      // +4
    float percent = 0.0f;           // +8 (an SC4Percentage)
    std::int32_t usage = 0;         // +0xc
    std::int32_t maintenance = 0;   // +0x10
};

// The part of the 0x1e8 list's records this port reads: the original record continues with the trip lists (Write/Read
// 0x10008e... walk them); only the station capacity (+0x40) and cell (+0x44 / +0x45) are modelled here.
struct TransitStationRecord {
    std::uint16_t capacity = 0;   // +0x40
    std::uint8_t x = 0, z = 0;    // +0x44, +0x45
};

struct TrafficTunables {
    float value_bcc = 0.0f;                            // 0x491332e6
    float value_bd0 = 0.03125f;                        // 0x29136789
    float value_bd4 = 0.0f;                            // 0x29136788
    std::array<std::array<float, 13>, 9> cellsPerMinute{};        // +0xbd8: speed per class, per travel type
    std::array<std::array<float, 13>, 9> inverseMaximumSpeed{};   // mfInverseMaximumSpeed
    std::array<float, 13> budgetScale{};               // +0xdac, 0x6a84493e
    float value_de0 = 1.2f, value_de4 = 1.2f;          // 0xa92356ae / af (percentages)
    float potholePenalty = 32.0f;                      // +0xde8, 0xa92356b0
    float strikeEndFunding = 1.0f;                     // +0xdec: a strike lasts while funding stays below this
    float strikeThreatFunding = 0.7f;                  // +0xdf0: below this a strike may be threatened
    std::uint32_t strikeMaxDays = 24;                  // +0xdf4
    float strikeChanceScale = 1.0f;                    // +0xdf8
    std::array<float, 9> values_dfc{};                 // 0xa92356b1
    std::uint8_t value_e20 = 0x3c;
    std::array<float, 13> values_e24{}, inverse_e58{}; // 0xa92356b3 and reciprocals
    std::array<bool, 9> travelTypeEnabled{};           // +0xe8c, 0xa92356b4 (true beyond the property)
    std::array<bool, 9> travelTypeFlag_e95{};          // 0xa92356b5
    std::uint32_t value_ea0 = 365, value_ea4 = 128, value_ea8 = 1;
    float tripScale = 90.0f;                           // +0xeac, 0xa92356ba
    float value_eb0 = 64.0f;                           // 255 / (last congestion value - 1)
    std::uint8_t count_ec0 = 0;
    std::vector<float> inverse_ec8;                    // 0xa92356bb reciprocals
    std::array<float, 9> values_ee0{}, values_f04{}, values_f28{};
    float value_f4c = 1.0f;
    std::array<float, 3> backgroundTrafficWeights{1.0f, 1.0f, 1.0f};  // +0xf50, 0xa92356be
    float freightScalingFactor = 0.0f;                 // +0xf5c
    float value_f60 = 0.75f, value_f64 = 0.5f, value_f68 = 4.0f;
    float railAccidentChance = 1.0f;                   // +0xf6c, 0x0bd86fd5
    float value_f70 = 0.001f;                          // 0x8c3caef5
    float value_ff8 = 10.0f, value_ffc = 60.0f;
    float tripScaleForDisplay = 0.0f;                  // +0xf64 (also tunable 0xca76013b)
};

// The traffic simulator's own interface (class id 0x6990c1aa, iid 0x68027f37). The recovered port calls
// the concrete methods directly, so the interface carries only the COM identity.
class cISC4TrafficSimulator : public cIGZUnknown {
public:
    virtual ~cISC4TrafficSimulator() = default;
};

class cSC4TrafficSimulator : public cISC4TrafficSimulator, public cIGZMessageTarget2, public cIGZSerializable {
public:
    cSC4TrafficSimulator(ITrafficHost* host, cRZRandom* random);
    ~cSC4TrafficSimulator() override;

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    bool DoMessage(cIGZMessage2* message) override;   // unrecovered
    bool Write(cIGZOStream& stream) const override;   // unrecovered (stub)
    bool Read(cIGZIStream& stream) override;          // unrecovered (stub)
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4TrafficSimulator; }

    // ---- tunables (0x100078e16, 0x10008242c, 0x100082660 is not recovered) ----
    // Returns false when the simulator exemplar is missing (nothing changes).
    bool ReadSimulatorTunableValues(const cISCPropertyHolder* exemplar);
    void UpdateTravelStrategies(const cISCPropertyHolder* exemplar);
    const TrafficTunables& Tunables() const { return tunables_; }
    TrafficTunables& MutableTunables() { return tunables_; }
    cSC4ResponseCurve& CongestionToInverseSpeedCurve() { return congestionToInverseSpeed_; }
    cSC4ResponseCurve& FundingCurve() { return fundingCurve_; }

    // ---- network info grid ----
    void InitGrid(int cellsX, int cellsZ);                       // sets +0x40/+0x44 and clears the grid
    NetworkInfo* AddNetworkInfo(int networkType, int x, int z);  // valid until the next Add on that cell
    NetworkInfo* GetNetworkInfo(int networkType, int x, int z);
    const NetworkInfo* GetNetworkInfo(int networkType, int x, int z) const;
    void RemoveNetworkInfo(int networkType, int x, int z);
    float GetCapacity(int networkType, int x, int z) const;
    // i386 0x00052662: a new path finder over this simulator's network, initialised; the caller owns its one reference.
    // Null before Init built the network graph.
    cISC4PathFinder* CreatePathFinder();
    std::uint8_t GetConnectionCount(int networkType, int x, int z) const;
    float GetTravelTimeRatio(int x, int z, int travelType) const;
    float GetBackgroundTraffic(int x, int z) const;

    // ---- edge densities ----
    static std::uint32_t EdgeDensityKey(int x, int z, int travelType, bool secondDirection);
    TrafficEdgeDensity& EdgeDensityEntry(int x, int z, int travelType, bool secondDirection);
    const TrafficEdgeDensity& GetTrafficEdgeDensity(int x, int z, int travelType, bool secondDirection) const;

    // ---- transit switches ----
    // The map value (the original's operator[] result): +0 the switch's object, +8 traffic, +0xc cost, +0x10 capacity, +0x18 usage,
    // +0x1c percent. +0x14 is not read.
    // One rule of a switch (traffic-network.md §2.4): on the city sides `side`, a type-`from` traveller becomes type `to`; kind 0
    // on the way INTO the switch's lot from the neighbour cell, kind 1 on the way OUT of it.
    struct TransitSwitchRule { std::uint8_t kind = 0; bool side[4] = {false, false, false, false}; std::uint8_t from = 0, to = 0; };
    struct TransitSwitch {
        void* occupant = nullptr; float traffic = 0, cost = 0, capacity = 1; float field14 = 0, usage = 0, percent = 0;
        std::vector<TransitSwitchRule> rules;
        std::uint8_t flags = 0;                 // from the rules' "to" types: bus 1, train 2, subway 4, el train 8, monorail 0x10; 0x40 subway <-> el
        std::uint16_t toMask = 0;               // 1 << to over the kind-0 rules
        float toll = 0;                         // building property 0x0bfc0a4c: the fee per unit of traffic through it
        bool countsAsIntersection = false;      // building property 0x4bfc47b0
        bool hasAccessPoint = false;            // a subway switch's access cell
        std::uint8_t accessX = 0, accessZ = 0;
    };
    // The switch id grid and the mode mask grid (1 << from over the kind-0 rules), written over a switch's lot.
    std::uint32_t SwitchIdAt(int x, int z) const;
    std::uint8_t SwitchModeMaskAt(int x, int z) const;
    const std::unordered_map<std::uint32_t, TransitSwitch>& TransitSwitches() const { return transitSwitches_; }
    // traffic-network.md §4.7.2: the rules of a property's raw bytes (4 per rule) for a lot facing, with the flags and the "to" mask.
    static void ReadTransitSwitchRules(TransitSwitch& out, const std::vector<std::uint8_t>& bytes, int facing);
    bool ActivateTransitSwitch(ITrafficLot* lot);                  // §4.7.1 steps 1-5; true when a switch was made
    bool DeactivateTransitSwitch(const SC4RectInt32& rect);        // §4.7.5 step 1 without the rebuild
    void ApplyTransitSwitches(const SC4RectInt32& r1);             // §4.7.4, the switch phase of DoConnectionsChanged
    // An id over one cell of the switch grid (what a loaded record's grid gives).
    void SetSwitchIdAt(int x, int z, std::uint32_t id);
    std::list<TransitStationRecord>& TransitStations() { return transitStations_; }   // +0x1e8
    // 0x10008d8bc: the switch under the cell, or the station whose lot covers it; false when there is neither.
    bool GetTransitSwitchQueryData(int x, int z, TransitSwitchQueryData& out);
    TransitSwitch& TransitSwitchEntry(std::uint32_t id) { return transitSwitches_[id]; }
    float TransitSwitchEntryCost(std::uint32_t id);   // FLT_MAX-like 1.7014117e38 when saturated
    float transitSwitchCostScale = 1.0f;               // +0x8c8

    // ---- roads / rail damage and funding ----
    CellSet& HealthyRoadCells() { return healthyRoads_; }        // +0x980
    CellSet& DamagedRoadCells() { return damagedRoads_; }        // +0x9a8
    CellSet& HealthyRailCells() { return healthyRail_; }         // +0x9e0
    CellSet& DamagedRailCells() { return damagedRail_; }         // +0xa08
    void SetRoadPotholeCapacity(std::uint32_t total) { roadPotholeTotal_ = total; }
    void SetRailPotholeCapacity(std::uint32_t total) { railPotholeTotal_ = total; }
    void ApplyRoadFunding(const SC4Percentage& funding);
    void ApplyMassTransitFunding(const SC4Percentage& funding);
    void ApplyPotholeChanges(CellSet& healthy, CellSet& damaged, int delta, int networkType);
    bool IsRoadDamaged(int x, int z) const { return damagedRoads_.Contains(std::uint16_t((x << 8) | z)); }
    bool CheckRailAccident(int x, int z) const;
    int RoadPotholeCount() const { return roadPotholeCount_; }
    int RailPotholeCount() const { return railPotholeCount_; }
    short StatRoadPotholes() const { return statRoadPotholes_; }

    // ---- strikes and budgets ----
    enum StrikeState { kStrikeThreatened = 0, kStrikeOn = 1, kStrikeNone = 2 };
    void UpdateStrikes();
    StrikeState GetStrikeState() const { return strikeState_; }
    int StrikeDays() const { return strikeDays_; }
    void SetStrikeEligible(std::int64_t count) { strikeEligible_ = count; }
    bool StrikeActiveFlag() const { return strikeActive_; }
    float StrikeChance() const { return strikeChance_; }
    struct BudgetCounters { std::uint32_t c908 = 0, c90c = 0, c910 = 0, c914 = 0, c920 = 0, c924 = 0, c928 = 0, c92c = 0, c930 = 0, c938 = 0; };
    BudgetCounters& Counters() { return counters_; }
    void UpdateRoadsBudget();
    void UpdateMassTransitBudget();

    // ---- trip capacity (lot property 0x8a0b4100: 12 counters, max at [0..], used at [+4], arrived at [+8]) ----
    static std::uint32_t GetTripCapacity(const cISCPropertyHolder* holder, std::uint32_t index);
    static void SetTripCapacity(cISCPropertyHolder* holder, std::uint32_t index, std::uint32_t value);
    static std::uint32_t GetMaxTripCapacity(const cISCPropertyHolder* h, std::uint32_t i) { return GetTripCapacity(h, i); }
    static void SetMaxTripCapacity(cISCPropertyHolder* h, std::uint32_t i, std::uint32_t v) { SetTripCapacity(h, i, v); }
    static std::uint32_t GetUsedTripCapacity(const cISCPropertyHolder* h, std::uint32_t i) { return GetTripCapacity(h, i + 4); }
    static std::uint32_t GetTrafficArrived(const cISCPropertyHolder* h, std::uint32_t i) { return GetTripCapacity(h, i + 8); }

    // ---- neighbour connection geometry (city border cells are outside [0, size]) ----
    // Returns the border side (0 west, 1 north, 2 east, 3 south) and the nearest inner cell, or -1 when (x, z)
    // is not a border neighbour cell.
    int GetNeighborConnectionInnerCell(int x, int z, int& innerX, int& innerZ) const;
    void FindHighwayReturnTile(int& x, int& z, bool flag) const;
    int LastCellX() const { return lastX_; }    // +0x40, highest valid x index
    int LastCellZ() const { return lastZ_; }    // +0x44

    // ---- neighbour connection tables (0x1000820ac, 0x100082358, 0x1000828a2, 0x100082ae2, 0x1000884e8,
    //      0x100089830, 0x10008a090) ----
    // Side of the map a border cell belongs to (0 west, 1 north, 2 east, 3 south; false when not a border cell).
    static bool NeighborSide(int x, int z, int lastX, int lastZ, int& side, int& index);
    // The slot GetTrafficNeighborConnection returns; null when (x, z) is not a border cell.
    TrafficNeighborConnection* GetTrafficNeighborConnection(int x, int z);
    std::array<std::vector<TrafficNeighborConnection>, 4>& NeighborConnections() { return neighborConnections_; }
    // +0x150: the original stores refcounted cSCPropertyHolder*; the port's map owns the holders so that a
    // simulator destroyed without RemoveUnusedPools does not leak them.
    std::map<std::uint16_t, cSCPropertyHolder>& NeighborPools() { return neighborPools_; }
    cISCPropertyHolder* DefaultNeighborPool() { return defaultNeighborPoolValid_ ? &defaultNeighborPool_ : nullptr; }  // +0x168
    void AddNeighborConnection(int type, int x, int z);      // records the type, the pool and the cost
    void RemoveNeighborConnection(int type, int x, int z);
    void RemoveUnusedPools();
    void* GetNeighborConnection(int travelMode, int x, int z);        // the engine's cISC4NeighborConnection
    cISCPropertyHolder* GetNeighborPoolPropertyHolder(int travelMode, int x, int z);
    bool HasMapExit(int x, int z);
    // +0x98: per-cell connection masks (fixed 0x10000-cell table). Only HasMapExit reads them back so far.
    TrafficCellConnections& CellConnections(int x, int z) { return cellConnections_[(std::size_t(x) << 8) | (std::size_t(z) & 0xff)]; }
    const TrafficCellConnections& CellConnections(int x, int z) const { return cellConnections_[(std::size_t(x) << 8) | (std::size_t(z) & 0xff)]; }
    // +0x80: trip-capacity flag byte per cell (bit per lane group), rows x + 1, columns z + 1
    std::vector<std::vector<std::uint8_t>>& CellTripFlags() { return cellTripFlags_; }
    std::array<int, 4>& ConnectionCounts() { return connectionCounts_; }          // +0x31c
    std::array<int, 4>& TripCapacityTotals() { return tripCapacityTotals_; }      // +0x32c
    void RecalculateNeighborConnectionCapacities();                              // 0x100087336

    // ---- trip pipeline ----
    // 0x1000885b8 / the original's vslot 0xa0: the lot's trip capacity still available for lane group `kind`.
    std::uint32_t GetMaxTripCapacitySlot(const cISCPropertyHolder* holder, std::uint32_t kind) const {
        return GetTripCapacity(holder, kind);
    }
    bool CapacityStillAvailable(const cSC4TripData& trip);                       // 0x100087c06
    void ApplyTrafficToDestination(std::uint32_t count, int x, int z, std::uint8_t networkType,
                                   std::uint8_t tripType);                       // 0x1000895c6
    void WorkOnClearTripDestinations();                                          // 0x100083508
    void WorkOnRefreshCommuteMap();                                              // 0x1000836ba
    void WorkOnUpdateNeighborConnections();                                      // 0x100086086
    // 0x100087cc4: hand one trip to the two path finders (the source rect comes from the host seam).
    void SetupTrip(StandardTripParams& params, cSC4TripData& trip);
    // 0x10008bf74: configure a path finder for a lot's dominant trip type; false when the lot has no trips.
    bool SetupPathFinderForLot(cISC4PathFinder* pathFinder, ITrafficLot* lot);
    StandardTripParams& TripParams() { return tripParams_; }                     // +0xa70

    // ---- the live simulator (traffic-simulator.md §1, §3, §4; traffic-network.md §1, §4.2) ----
    bool Init();                                                   // 0x0071E7E0
    bool Shutdown();                                               // 0x00719290
    bool IsInitialized() const { return initialized_; }
    void DoPostCityInit();                                         // 0x00720430
    bool DoDailySimulation();                                      // 0x007215C0 (false: not done for today)
    void DoMonthlySimulation();                                    // 0x00718AC0
    // 0x00720F00 with a budget of work units instead of the 4 ms deadline (§3.4 [choice]); true when a phase finished.
    bool WorkOnSimCycle();
    void DoConnectionsChanged(int x1, int z1, int x2, int z2);     // 0x0071A860
    void DoNetworkOccupantChanged(cSC4NetworkOccupant* occupant, bool inserted);   // DoMessage 0x99ef1142 / 0x99ef1143
    void DoLotActivated(ITrafficLot* lot);                         // 0x00723AF0 (no transit switches or ferries yet)
    void DoLotDeactivated(ITrafficLot* lot);                       // 0x0071E2F0
    void UpdateIntersectionData();                                 // 0x0071B9A0
    bool ConfirmRoute(const std::vector<std::uint8_t>& route) const;   // 0x0070E320
    // 0x00719910: mode 0 outbound, 1 return leg, 2 both directions.
    void ApplyRoute(const std::vector<std::uint8_t>& route, std::uint32_t count, const SC4RectInt32& sourceRect, int destX, int destZ,
                    int mode, std::uint8_t type);
    std::uint32_t CycleState() const { return cycleState_; }
    void SetUnitsPerSlice(int units) { unitsPerSlice_ = units < 1 ? 1 : units; }

    cSC4SimGrid<std::uint8_t>* GetCongestionMap() const { return congestionMap_; }          // slot 10
    cSC4SimGrid<std::uint8_t>* GetAirPollutingTrafficMap() const { return airMap_; }        // slot 8
    cSC4SimGrid<std::uint8_t>* GetCommercialTrafficMap() const { return commercialMap_; }   // slot 9
    cSC4SimGrid<std::uint8_t>* GetTripLengthMap() const { return tripLengthMap_; }          // slot 13
    cSC4SimGrid<float>* GetVolumeMap(int network) const { return unsigned(network) < 6 ? volume_[network] : nullptr; }
    // One statistics row (§4.9): average volume/capacity, congested cells, the busiest cell and its ratio, records counted.
    struct TrafficStatsRow { float average = 0; std::uint32_t congested = 0; int maxX = 0, maxZ = 0; float max = 0; std::uint16_t records = 0; };
    const std::array<TrafficStatsRow, 14>& PublishedStats() const { return statsPublished_; }
    float AverageCarCommute() const { return avgCarCommute_; }
    std::uint32_t PassTotal() const { return passTotal_; }
    const std::array<std::uint32_t, 3>& CommutersAttempted() const { return attempted_; }
    const std::array<std::uint32_t, 3>& CommutersRouted() const { return routed_; }
    const std::array<std::uint32_t, 9>& TravelTypeCounts() const { return typeCountsPublished_; }
    std::uint32_t CellCount(int type) const { return unsigned(type) < 13 ? cellCounts_[type] : 0; }
    std::uint8_t DestinationByte(int x, int z) const;
    std::uint32_t FreeDestinations(int k) const { return unsigned(k) < 4 ? std::uint32_t(connectionCounts_[k]) : 0; }

    // ---- subnetworks (lots hold property 0x2a4f9c74 with the ids of the subnetworks they touch) ----
    void ClearSubnetworks();                                        // 0x100082bf4: next id back to 1, every cell id to 0
    // traffic-network.md §4.11 (UpdateSubnetworks 0x007183B0, work state 12): label the car network's (cell, entry side) states by
    // flood fill from the seeds; §4.12 (UpdateSubnetworkInfo 0x00716180, state 13): each lot's subnetworks (property 0x2a4f9c74),
    // their populations and trip capacities, plus the neighbour connections, published at the end. Both run whole in one call.
    void UpdateSubnetworks();
    void FloodSubnetwork(int x, int z, int entry);
    void UpdateSubnetworkInfo();
    // Output order is first-found; the original's hash-set order is unspecified.
    void GetSubnetworksForLot(const cISCPropertyHolder* lot, std::vector<std::uint32_t>& out) const;
    void GetSubnetworksInRectangle(const TrafficCellRect& rect, std::vector<std::uint32_t>& out) const;
    void GetSubnetworksInRegion(const TrafficCellRegion& region, std::vector<std::uint32_t>& out) const;
    bool AreLotsConnected(const cISCPropertyHolder* a, const cISCPropertyHolder* b) const;
    std::uint32_t GetConnectedOccupantCount(const cISCPropertyHolder* lot, std::uint32_t occupantType) const;
    std::uint32_t GetConnectedDestinationCount(const cISCPropertyHolder* lot, int destinationType) const;
    void GetOccupantCountForAllSubnetworks(std::uint32_t occupantType, std::vector<std::uint32_t>& out) const;
    std::vector<SubnetworkInfo>& WorkingSubnetworks() { return workingSubnetworks_; }       // +0xa8
    // WorkOnUpdateSubnetworkInfo's tail: the finished working vector replaces the published one.
    void PublishSubnetworks() { subnetworks_ = workingSubnetworks_; }
    std::vector<SubnetworkInfo>& Subnetworks() { return subnetworks_; }                      // +0xc0
    std::uint16_t NextSubnetworkId() const { return nextSubnetworkId_; }

    // ---- travel strategy percentages (4 wealth classes x 3 strategies, percent bytes) ----
    void GetTravelStrategyPercentages(unsigned wealth, std::vector<SC4Percentage>& out) const;
    const std::array<std::uint8_t, 12>& TravelStrategyBytes() const { return travelStrategies_; }

    // ---- accessors ----
    static constexpr std::uint32_t GetSimulatorType() { return kTrafficSimulatorType; }
    float GetTripScale() const { return tunables_.tripScale; }
    float GetTripScaleForDisplay() const { return tunables_.tripScaleForDisplay; }
    float GetFreightScalingFactor() const { return tunables_.freightScalingFactor; }
    int GetFerryTerminalCount(unsigned ferryType) const { return ferryTerminalCount_[ferryType]; }
    void SetFerryTerminalCount(unsigned ferryType, int n) { ferryTerminalCount_[ferryType] = n; }
    int GetLastMonthlyIncome() const { return lastMonthlyIncome_; }
    void SetLastMonthlyIncome(int v) { lastMonthlyIncome_ = v; }
    // x/z packed into one value when the lot-insertion mode is 1, else 0
    std::uint64_t GetDesiredLotInsertionPoint() const { return insertionMode_ == 1 ? insertionPoint_ : 0; }
    void SetDesiredLotInsertion(int mode, std::uint64_t point) { insertionMode_ = mode; insertionPoint_ = point; }
    void InvalidateIntersectionData(int x0, int z0, int x1, int z1);
    bool IntersectionDirty() const { return intersectionDirty_; }
    void GetIntersectionDirtyRect(int& x0, int& z0, int& x1, int& z1) const { x0 = dirtyX0_; z0 = dirtyZ0_; x1 = dirtyX1_; z1 = dirtyZ1_; }

    static const std::array<std::uint8_t, 9> kTravelModeToTransitNetwork;

private:
    cRecordTape cityRecord_;   // the city-file record as read (the port models none of it yet)
    NetworkInfo* FindNetworkInfo(int networkType, int x, int z) const;
    std::vector<NetworkInfo>* Cell(int x, int z) const;

    ITrafficHost* host_;
    cRZRandom* random_;
    std::uint32_t refCount_ = 0;                        // COM refcount (shared by the three sub-objects)
    TrafficTunables tunables_;
    cSC4ResponseCurve congestionToInverseSpeed_;
    cSC4ResponseCurve curveF78_, fundingCurve_, curveFB8_, curveFD8_;
    std::array<bool, 9> travelTypeAffectedByTraffic_{};
    int lastX_ = -1, lastZ_ = -1;
    mutable std::vector<std::vector<std::vector<NetworkInfo>>> grid_;
    std::unordered_map<std::uint32_t, TrafficEdgeDensity> edgeDensities_;
    std::unordered_map<std::uint32_t, TransitSwitch> transitSwitches_;
    std::vector<std::uint32_t> switchIds_;      // empty, or 0x10000 cells packed (x << 8) | z; 0 = no switch
    std::vector<std::uint8_t> switchModes_;
    std::uint32_t nextSwitchId_ = 1;

    std::list<TransitStationRecord> transitStations_;   // +0x1e8
    TrafficEdgeDensity emptyEdgeDensity_{};
    std::uint16_t nextSubnetworkId_ = 1;                 // +0xa4
    std::vector<SubnetworkInfo> workingSubnetworks_, subnetworks_;   // +0xa8, +0xc0

    CellSet healthyRoads_, damagedRoads_, healthyRail_, damagedRail_;
    std::uint32_t roadPotholeTotal_ = 0, railPotholeTotal_ = 0;
    int roadPotholeCount_ = 0, railPotholeCount_ = 0;
    float roadFundingAccum_ = 0, railFundingAccum_ = 0;
    short statRoadPotholes_ = 0;

    StrikeState strikeState_ = kStrikeNone;
    int strikeDays_ = 0;
    std::int64_t strikeEligible_ = 0;
    bool strikeActive_ = false;
    float strikeChance_ = 0;
    BudgetCounters counters_;

    std::array<std::uint8_t, 12> travelStrategies_{};
    std::array<int, 3> ferryTerminalCount_{};
    int lastMonthlyIncome_ = 0;
    int insertionMode_ = 0;
    std::uint64_t insertionPoint_ = 0;
    bool intersectionDirty_ = false;
    int dirtyX0_ = 0, dirtyZ0_ = 0, dirtyX1_ = 0, dirtyZ1_ = 0;

    // neighbour connections: the four border tables, the pool map and its default pool
    std::array<std::vector<TrafficNeighborConnection>, 4> neighborConnections_;   // +0xf0, +0x108, +0x120, +0x138
    std::map<std::uint16_t, cSCPropertyHolder> neighborPools_;                    // +0x150
    cSCPropertyHolder defaultNeighborPool_;                                       // +0x168
    bool defaultNeighborPoolValid_ = false;
    std::array<int, 4> connectionCounts_{};                                       // +0x31c: cells per lane group
    std::array<int, 4> tripCapacityTotals_{};                                     // +0x32c: destination capacity per group
    std::vector<TrafficCellConnections> cellConnections_;                         // +0x98, fixed 0x10000 cells
    std::vector<std::vector<std::uint8_t>> cellTripFlags_;                        // +0x80, [x + 1][z + 1]
    StandardTripParams tripParams_;                                               // +0xa70

    // ---- the live simulator ----
    class PathGraph;
    friend class PathGraph;
    PathGraph* graph_ = nullptr;   // owned (TrafficCycle.cpp)
    cSC4PathFinder* pf_ = nullptr;      // outbound
    cSC4PathFinder* pf2_ = nullptr;     // return leg
    bool initialized_ = false;
    std::uint32_t cycleState_ = 0;
    int unitsPerSlice_ = 64;
    int cellsX_ = 0, cellsZ_ = 0;
    cSC4SimGrid<std::uint8_t>* airMap_ = nullptr;           // 0x89d5c3a2
    cSC4SimGrid<float>* peakVolume_ = nullptr;              // 0x69d5c3ac
    cSC4SimGrid<std::uint8_t>* commercialMap_ = nullptr;    // 0xcabd2925 (tract 4)
    cSC4SimGrid<std::uint8_t>* commercialWork_ = nullptr;   // 0xcabd2926
    cSC4SimGrid<std::uint8_t>* tripLengthMap_ = nullptr;    // 0x69d5c3bf
    std::array<cSC4SimGrid<float>*, 6> volume_{};           // 0x69d5c3e0 + n
    cSC4SimGrid<std::uint8_t>* congestionMap_ = nullptr;    // 0x69d5c402
    cSC4SimGrid<float>* peakRatio_ = nullptr;               // 0x69d5c40e
    std::array<std::uint32_t, 13> cellCounts_{};            // per network type (§3.6)
    double incomeAcc_ = 0, feeAcc_ = 0;
    float transitIncome_ = 0, tollIncome_ = 0;
    std::array<std::uint32_t, 3> attempted_{}, routed_{};
    float carLenSum_ = 0, freightLenSum_ = 0;
    std::uint32_t carLenCount_ = 0, freightCount_ = 0;
    float avgCarCommute_ = 0;
    std::uint8_t freightByte_ = 0;
    std::array<std::uint32_t, 9> typeCountsBuilding_{}, typeCountsPublished_{};
    std::uint32_t passTotal_ = 0, processed_ = 0, dayIndex_ = 0, total_ = 0, days_ = 0, toReRoute_ = 0, toSearch_ = 0;
    std::array<std::uint32_t, 10> passCounters_{};
    std::uint8_t rerouteBucket_ = 0;
    int lastDayOfMonth_ = -1;
    std::uint32_t strategyBucketCounter_ = 0, strategyBucket_ = 0;
    std::multimap<std::uint32_t, std::pair<int, int>> strategyBuckets_;
    std::vector<std::unique_ptr<ITrafficLot>> sources_;     // the traffic-source snapshot (lots; neighbour connections: none yet)
    std::size_t sourceIndex_ = 0;
    bool continueSearch_ = false;
    std::array<TrafficStatsRow, 14> statsWork_{}, statsPublished_{};
    std::array<float, 14> statsCompare_{};
    std::vector<std::pair<std::uint16_t, float>> accidentList_, accidentCheck_;
    bool networkChanged_ = false, subnetworksStale_ = false;

    void EnterState(std::uint32_t state);
    void ResetSources();
    bool ConfirmTripsPhase(int& units);              // state 16
    bool SearchPhase(bool reRoute, int& units);      // states 17 / 18
    void ConfirmSourceTrips(ITrafficLot& source);    // 0x0071D920
    int PerformPathSearch(ITrafficLot& source, cSC4TripData& trip);   // 0x0071C6C0: 1 found, 0 not found
    void SetupTripFor(ITrafficLot& source, cSC4TripData& trip);       // 0x0070EC40
    void CompleteTripSourceCalculations(ITrafficLot& source);         // 0x0070F0A0
    void FinishTripMap();                            // state 15
    void CreateMaps();                               // state 2
    void CreateFinalMap();                           // state 4
    void StartRecalc();                              // state 0
    void FinalCalculations();                        // state 8
    void DumpEdgeDensities();                        // state 7
    void ReduceEdgeDensities();                      // state 6
    void ReduceTransitSwitches();                    // state 3
    void BumpCellCount(int type, bool inserted);
    void NetworkBookkeeping(cSC4NetworkOccupant* occupant, bool inserted, bool budgets);   // §3.6
    void SyncBudgetCounters();
    template <class T> cSC4SimGrid<T>* MakeGrid(std::uint32_t id, int tract);
    template <class T> void AdoptSavedGrid(cSC4SimGrid<T>*& member, std::uint32_t id);   // Init keeps what Read loaded
    // §3.5: the record's cycle position and accumulators, kept by Read (which runs before Init) for DoPostCityInit to resume from
    struct LoadedCycle {
        bool valid = false;
        std::uint32_t state = 0;
        std::int16_t phaseX = 0, phaseZ = 0;
        std::uint32_t strategyBucketCounter = 0, strategyBucket = 0;
        std::uint32_t processed = 0, dayIndex = 0, total = 0, days = 0, passTotal = 0, toReRoute = 0, toSearch = 0;
        std::array<std::uint32_t, 10> passCounters{};
        std::array<std::uint32_t, 3> jobCapacity{}, attempted{}, routed{};
        float carLenSum = 0, freightLenSum = 0;
        std::uint32_t carLenCount = 0, freightCount = 0;
        std::array<std::uint32_t, 9> typeCountsPublished{}, typeCountsBuilding{};
        std::vector<std::pair<std::uint32_t, TrafficEdgeDensity>> edgeDensities;
        std::vector<std::pair<std::uint16_t, cSCPropertyHolder>> pools;   // capacities and arrivals per neighbouring city
        std::vector<std::vector<std::uint8_t>> destinations;                // the destination map, [x + 1][z + 1]
        std::array<std::uint32_t, 4> freeDestinations{};
    } loaded_;
    void ResumeLoadedCycle();                        // §3.5 steps 11-13
    std::vector<std::uint8_t> savedCongestion_, savedTripLength_;   // the file's maps as loaded (SC4_TRAFFICDEBUG scoring)
    void ScoreAgainstSaved();
    void DumpTrips(const char* path);                // SC4_TRAFFICDUMP: every lot's trips, one line each
};

}  // namespace sc4
