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
struct cSC4TripData;

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
struct NetworkInfo {
    float inverseCapacity = 0.0f;   // +0: capacity = 1 / this
    std::uint8_t connectionCount = 0;  // +4
    std::uint8_t networkType = 0;   // +6
    std::uint8_t flag7 = 0;         // +7
    float potholePenalty = 0.0f;    // +8: set while the cell has a pothole
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

// Value part of an edge-density map entry; only the density word is read by the recovered code.
struct TrafficEdgeDensity { std::uint16_t density = 0; };

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
    // +0: per travel type, four bits per map side: bit (side + 4 * lane) with side 0 west, 1 north, 2 east, 3 south.
    std::array<std::uint16_t, 9> masks{};
    std::array<std::uint8_t, 0x2e - 0x12> rest{};
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
    virtual void RefreshTrafficSource(void* source) = 0;            // source vslot 0x20
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
    // The transit-switch grid (+0x890, a cSC4SimGrid<uint32>) vslot 0x38: the switch id stored at a cell, 0 when none.
    virtual std::uint32_t TransitSwitchIdAtCell(int x, int z) { (void)x; (void)z; return 0; }
    // vslot 0x30 of the object a switch entry keeps at +0: its property holder (the budget item is looked up on it).
    virtual cISCPropertyHolder* TransitSwitchProperties(void* occupant) { (void)occupant; return nullptr; }
    // The lot's building (lot vslot 0x180 -> 0x18 -> 0x30): its property holder; null when the lot has none.
    virtual cISCPropertyHolder* LotBuildingProperties(ITrafficLot* lot) { (void)lot; return nullptr; }
    // Budget simulator vslot 0x198 GetBudgetItemForPurpose (department id and monthly cost) and 0xd8 -> 0x50 (funding fraction).
    virtual bool BudgetItemForPurpose(cISCPropertyHolder* holder, std::uint32_t purpose, std::uint32_t& department, std::int64_t& cost) {
        (void)holder; (void)purpose; department = 0; cost = 0; return false;
    }
    virtual bool DepartmentFundingFraction(std::uint32_t department, float& fraction) { (void)department; fraction = 0.0f; return false; }
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
    ~cSC4TrafficSimulator() override = default;

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
    struct TransitSwitch { void* occupant = nullptr; float traffic = 0, cost = 0, capacity = 1; float field14 = 0, usage = 0, percent = 0; };
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

    // ---- subnetworks (lots hold property 0x2a4f9c74 with the ids of the subnetworks they touch) ----
    void ClearSubnetworks();                                        // 0x100082bf4: next id back to 1, every cell id to 0
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
};

}  // namespace sc4
