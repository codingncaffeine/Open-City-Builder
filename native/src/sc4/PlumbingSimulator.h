// cSC4PlumbingSimulator (class id 0x0990c075) — the water network: pipes (distressed / burst), water producers (pumps, towers,
// treatment plants), the per-cell flag map, the watered grids the lots and the data view read, neighbour water deals and the monthly
// water statistics.
//
// Ported from the Mac i386 build (field map: scratch/s42/plumbing-notes.md); the i386 offset is given per field. The city services the
// simulator reaches (occupant manager, lots, terrain, pollution, budget, neighbours, history, messages, the simulator clock) go
// through IPlumbingHost; the occupants themselves are the game's cISC4Occupant.
//
// Cell flag bits (byte map, [z * cellsX + x]): 1 W (x-1), 2 N (z-1), 4 E (x+1), 8 S (z+1) pipe connections, 0x10 pipe, 0x20 water
// producer footprint, 0x40 building with water demand; bits 0xe0 survive a pipe removal.
// Watered grid values: 1 watered, 2 building, 4 producer-only cell, 8 pipe.
#pragma once
#include <cstdint>
#include <deque>
#include <list>
#include <utility>
#include <vector>

#include "gz/cIGZMessage2.h"
#include "gz/cIGZSerializable.h"
#include "rz/cRZRandom.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/SimStreamOps.h"
#include "sc4/cSC4ResponseCurve.h"
#include "sc4/cSC4SimGrid.h"

class cIGZIStream;
class cIGZOStream;
class cIGZMessageTarget2;

namespace sc4 {

class cISC4Occupant;
class cISC4NeighborDeal;
class cISC4NeighborConnection;

constexpr std::uint32_t kGZCLSID_cSC4PlumbingSimulator = 0x0990c075;
constexpr std::uint32_t kGZIID_cISC4PlumbingSimulator = 0x47782ccc;
constexpr std::uint32_t kPlumbingOccupantBuilding = 0x278128a0, kPlumbingOccupantNetwork = 0xc772bf98;
constexpr std::uint32_t kPlumbingPipeIid = 0x49c05b9f;            // cSC4PipeOccupant
constexpr std::uint32_t kPropBuildingWaterDemand = 0xc8ed2d84;    // exemplar: water the building consumes
constexpr std::uint32_t kPropWaterProductionCapacity = 0x88ed3303;
constexpr std::uint32_t kPropBuildingBudgetCost = 0x2a58d350;
constexpr std::uint32_t kPropProducerWear = 0xc910b4cf, kPropProducerAgeRate = 0xea1cf220, kPropProducerCurrentProduced = 0x08d9763d,
                        kPropProducerWearWarn = 0xea1cf221, kPropProducerWearBroken = 0xea1cf222, kPropProducerConditionCurve = 0x8a1e07ee,
                        kPropProducerWaterSource = 0x48f23a7e, kPropProducerCostPerUnit = 0xea1e3c53, kPropProducerExpenseByWear = 0x0911e2e7;
constexpr std::uint32_t kBudgetItemWater = 0xca58c9d5;
constexpr std::uint32_t kWaterDepartment = 0xaa4014b4, kWaterDepartmentGroup = 0x4a357eaf;
// neighbour deals: water the city buys (0x4773ffe2) and sells (0x4773ffe3); water connections are type 5
constexpr std::uint32_t kWaterDealImport = 0x4773ffe2, kWaterDealExport = 0x4773ffe3;
constexpr std::uint32_t kWaterConnectionType = 5;
// messages DoMessage reacts to
constexpr std::uint32_t kMsgPlumbingOccupantInserted = 0x99ef1142, kMsgPlumbingOccupantRemoved = 0x99ef1143,
                        kMsgPlumbingTunablesChanged = 0xca1b151c, kMsgPlumbingPipeBurst = 0xea1f0dcd, kMsgPlumbingDealsChanged = 0xea418d8f,
                        kMsgPlumbingCityEstablished = 0x6ac284f3, kMsgPlumbingTick = 0x66956814, kMsgPlumbingMonth = 0x66956816,
                        kMsgPlumbingTickFast = 0x6695681a;
constexpr std::uint32_t kMsgPlumbingUpdated = 0x6946dc34;           // sent at the end of every pass (void1 / unknown = the simulator)

struct PlumbingRect { int left = 0, top = 0, right = 0, bottom = 0; };   // inclusive cells

// What the budget simulator tracks for a water building (budget vslot 0xcc with 0xca58c9d5).
struct PlumbingBudgetEntry {
    bool tracked = false;
    float funding = 0.0f;
    bool hasExplicitFunding = false;
    std::int64_t monthlyCost = 0;
};

enum class WaterSourceType : int { Ground = 0, Surface = 1, None = 2 };

// The primary view the city holds for this simulator (cISC4PlumbingSimulator, iid 0x47782ccc); the query surface lives on the class.
class cISC4PlumbingSimulator : public cIGZUnknown {};

class IPlumbingHost {
public:
    virtual ~IPlumbingHost() = default;
    virtual int CityCellsX() = 0;                                    // city vslot 0x174
    virtual int CityCellsZ() = 0;                                    // city vslot 0x178
    virtual void PositionToCell(float x, float z, int& cellX, int& cellZ) = 0;   // city vslot 0x17c
    // occupant manager vslot 0xa0 (standard city cells, one type): the occupants of the cell (borrowed until the next query)
    virtual void OccupantsAtCell(int x, int z, std::uint32_t type, std::vector<cISC4Occupant*>& out) = 0;
    virtual bool IsWaterCell(int x, int z) = 0;                      // terrain (city vslot 0x150) vslot 0x88
    virtual int WaterPollutionAt(int x, int z) = 0;                  // pollution simulator vslot 0x20 (GetWaterValue)
    virtual float OrdinanceWaterFactor() { return 1.0f; }            // ordinance 0xa8f4eb0c GetCumulativeFactor
    virtual const cISCPropertyHolder* SimulatorExemplar() { return nullptr; }   // {0x6534284a, 0xe7e2c2db, 0xc911e35b}
    // ---- lots (lot manager vslot 0x14 the lot at a cell, 0x38 the lot of an occupant) ----
    virtual bool LotRectAt(int x, int z, PlumbingRect& out) = 0;     // lot vslot 0x38 GetBoundingRect
    // the occupant's lot needs power and has none (lot vslots 0x104 / 0x108): the producer makes nothing
    virtual bool ProducerLotUnpowered(cISC4Occupant* producer) = 0;
    virtual void SetLotConditionAt(int x, int z, int condition) = 0; // lot vslot 0x90
    // ---- the water department (+0xa4) and the budget ----
    virtual float DepartmentFundingFraction() = 0;                   // department vslot 0x28
    virtual void EnsureDepartment(float maxFunding) { (void)maxFunding; }   // Init: created and named when the city has none
    virtual void SetPipeCost(std::int64_t cost) { (void)cost; }      // department line item 2 SetFullExpenses
    virtual PlumbingBudgetEntry GetBudgetEntry(cISC4Occupant* occupant) { (void)occupant; return {}; }
    // DoBuildingChanged's budget bookkeeping (the building's 0xca58c9d5 item: line item by building type, its count and expense)
    virtual void ApplyBuildingBudget(cISC4Occupant* building, bool added) { (void)building; (void)added; }
    // state 8: the producer's monthly expense (0x2a58d350 scaled by its 0x0911e2e7 wear curve) into its budget item
    virtual void SetProducerExpense(cISC4Occupant* producer, std::int64_t expense) { (void)producer; (void)expense; }
    // ---- pipes (QI 0x49c05b9f: the network occupant view) ----
    virtual bool IsPipe(cISC4Occupant* occupant) = 0;                // a pipe, and not a preview (network flag 0x80000000)
    virtual std::uint32_t PipeFlags(cISC4Occupant* pipe) = 0;        // network vslot 0x30: 0x10000 distressed, 0x20000 burst
    virtual void SetPipeFlag(cISC4Occupant* pipe, std::uint32_t flag, bool on) = 0;   // vslots 0x28 / 0x2c
    virtual bool PipeConnects(cISC4Occupant* pipe, int edge) = 0;    // vslot 0xbc ConnectsToNeighbor(edge, 4)
    virtual void PipeCell(cISC4Occupant* pipe, int& x, int& z) = 0;  // vslot 0xec GetOccupiedCell
    virtual void SetPipeWatered(cISC4Occupant* pipe, bool watered) = 0;
    virtual void BurstPipeAt(int x, int z) { (void)x; (void)z; }     // disaster layer: the pipe-burst disaster 0x4a1332e8 at the cell
    // ---- neighbours (+0x28) ----
    virtual void Deals(std::uint32_t type, std::vector<cISC4NeighborDeal*>& out) { (void)type; (void)out; }   // vslot 0x74
    virtual void* DealCity(cISC4NeighborDeal* deal) { (void)deal; return nullptr; }
    virtual void Connections(void* city, std::vector<cISC4NeighborConnection*>& out) { (void)city; (void)out; }   // 0x44 / 0x48, type 5
    virtual cISC4NeighborConnection* UnvisitedConnection(void* city) { (void)city; return nullptr; }               // 0x3c
    virtual bool OtherSideActive(cISC4NeighborConnection* connection) { (void)connection; return false; }          // 0x38 -> 0x34
    virtual void RefreshNeighborDeals() {}                           // spNeighborsSim vslot 0x7c
    // ---- services ----
    virtual void PostMessage(std::uint32_t type, std::uint32_t data) { (void)type; (void)data; }
    virtual void SendUpdated(cIGZUnknown* simulator) { (void)simulator; }   // 0x6946dc34
    virtual void AddHistory(std::uint32_t id, std::int64_t value) { (void)id; (void)value; }
    virtual void AddAgent(cIGZMessageTarget2* target, int interval, const char* name, std::uint32_t flags) { (void)target; (void)interval; (void)name; (void)flags; }
    virtual void RemoveAgent(cIGZMessageTarget2* target) { (void)target; }
    virtual void AddNotification(cIGZMessageTarget2* target, std::uint32_t message) { (void)target; (void)message; }
    virtual void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t message) { (void)target; (void)message; }
    // ---- stream seams: the nested serializables through SC4Write/ReadGZSerializable ----
    virtual void WriteGrid(cIGZOStream&, cSC4SimGrid<std::uint8_t>*) {}
    virtual bool ReadGrid(cIGZIStream&, cSC4SimGrid<std::uint8_t>** out) { *out = nullptr; return false; }
    virtual void WriteDepartment(cIGZOStream&) {}
    virtual bool ReadDepartment(cIGZIStream&) { return false; }
    virtual void WriteOccupant(cIGZOStream&, cISC4Occupant*) {}
    // one reference (null when the reference is empty); iid 0x26bb767e occupant, 0x49c05b9f pipe
    virtual bool ReadOccupant(cIGZIStream&, std::uint32_t iid, cISC4Occupant** out) { (void)iid; *out = nullptr; return false; }
};

// One water producer (list node + 8; i386 producer offsets).
struct WaterProducer {
    int x = 0, z = 0;                        // +0x00 the building's cell (FindWaterProducer matches it exactly)
    PlumbingRect rect;                       // +0x08 its lot
    bool counted = false;                    // +0x18 its output is in this pass's network
    bool shareWritten = false;               // +0x19 its share of the network's use is written (0x08d9763d)
    std::uint32_t capacity = 0;              // +0x1c rated output (0x88ed3303)
    std::uint32_t production = 0;            // +0x20 output after the water source, funding, pollution and wear
    float percent = 0.0f;                    // +0x24
    bool polluted = false;                   // +0x28
    cISC4Occupant* occupant = nullptr;       // +0x2c (one reference)
};

class cSC4PlumbingSimulator : public cISC4PlumbingSimulator,
                              public cIGZMessageTarget2,
                              public cIGZSerializable {
public:
    cSC4PlumbingSimulator(IPlumbingHost* host, cRZRandom* random);
    virtual ~cSC4PlumbingSimulator();

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4PlumbingSimulator; }
    bool Write(cIGZOStream& stream) const override;                  // i386 0x00074c46
    bool Read(cIGZIStream& stream) override;                         // i386 0x00074f7c
    bool DoMessage(cIGZMessage2* message) override;                  // i386 0x00074956

    bool Init();                                                     // i386 0x00075c0c
    bool Shutdown();                                                 // i386 0x00071694
    bool IsInitialized() const { return initialised_; }
    // Defaults, then the exemplar {0x6534284a, 0xe7e2c2db, 0xc911e35b} when given (i386 0x00071cfc).
    void ReadSimulatorTunableValues(const cISCPropertyHolder* exemplar);

    // ---- the pass ----
    bool DoIdle(std::uint32_t milliseconds);                         // i386 0x0007208a (states 0..9)
    void DoMonthlySimulation();                                      // i386 0x0007491e
    int IdleState() const { return state_; }
    bool Enabled() const { return enabled_; }
    void SetEnabled(bool enabled) { enabled_ = enabled; }
    void RequestRefresh() { refresh_ = true; }
    void DoOccupantInserted(cISC4Occupant* occupant);
    void DoOccupantRemoved(cISC4Occupant* occupant);
    void DoPipeChanged(cISC4Occupant* pipe, bool added);             // i386 0x00070ee8
    void DoBuildingChanged(cISC4Occupant* building, bool added);     // i386 0x00071150
    void ApplyPipeFunding(const SC4Percentage& funding);             // i386 0x000745e2
    void UpdateWaterProduction();                                    // i386 0x0007092c
    void AgeWaterProducer(WaterProducer& producer);                  // i386 0x00070292

    // ---- queries ----
    int BuildingConsumption(int x, int z);
    void FillGridAtOccupant(cSC4SimGrid<std::uint8_t>& grid, cISC4Occupant* occupant, std::uint8_t mask);
    bool GetOccupantMonthlyUpkeepCost(cISC4Occupant* occupant, std::int64_t& cost);
    void FindWaterBuildings(cISC4Occupant** leastFunded, SC4Percentage* funding, cISC4Occupant** mostWorn, int* wearPercent);
    void GetWaterProducers(std::list<cISC4Occupant*>& out);
    WaterProducer* FindWaterProducer(int x, int z);
    std::uint8_t GetSupplyValue(int x, int z) const { return InGrid(x, z) ? published_->GetTractValue(x, z) : 0; }
    cSC4SimGrid<std::uint8_t>* GetWateredGrid() const { return published_; }
    bool IsCellWatered(int x, int z) const { return (GetSupplyValue(x, z) & 1) != 0; }
    bool AreWateredBuildingsInCell(int x, int z) const { return (GetSupplyValue(x, z) & 3) == 3; }
    bool IsBuildingWatered(cISC4Occupant* building);
    std::uint32_t GetWaterProduction(cISC4Occupant* occupant);
    bool GetWaterProducerInfo(cISC4Occupant* occupant, std::uint32_t* production, std::uint32_t* capacity, SC4Percentage* percent,
                              bool* polluted, std::uint32_t* currentProduced);
    bool GetWaterProducerConditionPercent(cISC4Occupant* occupant, SC4Percentage& out);
    // Spiral ring to the nearest water cell within `radius`; -1 when none, 0 for WaterSourceType::None.
    int NearestWater(int x, int z, WaterSourceType type, int radius);
    bool IsNeighborConnectedToCurrentNetwork(void* city, bool markVisited);
    void GetConnectionCell(cISC4NeighborConnection* connection, int& x, int& z) const;

    // ---- statistics (cISC4PlumbingSimulator getters) ----
    std::int64_t GetWaterDemand() const { return demand_; }
    int GetNumWaterSources() const { return int(producers_.size()); }
    int GetNumPollutedWaterSources() const { return pollutedSources_; }
    std::int64_t GetWaterProductionCapacity() const { return last_.production; }
    std::int64_t GetWaterConsumed() const { return last_.consumed; }
    std::int64_t GetWaterImported() const { return last_.imported; }
    std::int64_t GetWaterExported() const { return last_.exported; }
    int GetNumPipes() const { return pipeCount_; }
    int GetNumDistressedPipes() const { return distressedPipes_; }
    int GetNumBurstPipes() const { return burstPipes_; }
    int GetNumWateredBuildings() const { return lastWatered_; }
    int GetNumUnwateredBuildings() const { return lastBuildings_ - lastWatered_; }

    // ---- state exposed for tests ----
    std::list<WaterProducer>& Producers() { return producers_; }
    std::list<cISC4Occupant*>& Pipes() { return pipes_; }
    std::vector<std::uint8_t>& CellFlags() { return cellFlags_; }
    cSC4SimGrid<std::uint8_t>* WorkingGrid() const { return working_; }
    bool RefreshRequested() const { return refresh_; }
    std::uint32_t AveragePollution() const { return averagePollution_; }

    struct Totals { std::int64_t production = 0, consumed = 0, imported = 0, exported = 0; };
    Totals pass_, last_;                    // +0xf0..+0x10c this pass, +0x110..+0x12c the last finished one

    // ---- tunables (ReadSimulatorTunableValues) ----
    std::uint32_t serviceRadius = 7;        // +0xa8 (0x68f5436a) rings around the pipes a pass waters
    std::uint32_t groundSearchRadius = 2;   // +0xac (0x68f5436b)
    std::uint32_t surfaceSearchRadius = 1;  // +0xb0 (0x68f5436c)
    std::uint32_t farGroundPercent = 20;    // +0xb4 (0x68f5436d) output a pump gets when the water is more than one ring away
    std::uint32_t pollutionThreshold = 100; // +0xb8 (0xaa161f81) water pollution above which a producer stops
    float burstVariance = 0.3f;             // +0x188 (0x8a235c56)
    SC4Percentage maxFunding{1.2f};         // +0x1a0 (0xea4025a9) the department's funding cap
    float pipeCostFactor = 0.0f;            // +0x1c4 (0x8a84778f)
    cSC4ResponseCurve wearByFundingCurve;   // +0x150 (0x0911e2e8)
    cSC4ResponseCurve wearByUseCurve;       // +0x160 (0x0911e2e9)
    cSC4ResponseCurve productionFundingCurve;   // +0x1a4 (0x6a410bf2)
    cSC4ResponseCurve pipeFundingCurve;     // +0x1b4 (0xca7138c4)

private:
    using QueuePoint = std::pair<std::uint8_t, std::uint8_t>;   // x, z
    std::uint8_t& CellFlag(int x, int z) { return cellFlags_[std::size_t(z) * std::size_t(cellsX_) + std::size_t(x)]; }
    bool InGrid(int x, int z) const { return published_ && published_->TractIsInBounds(x, z); }
    bool Visited(int x, int z) const;
    void Visit(int x, int z);
    void Seed(int x, int z, std::int64_t supply);
    void FloodPipes();                       // state 3's pipe walk from the queued seed
    void ServeNetwork();                     // state 4
    void ResetScan();
    cSC4SimGrid<std::uint8_t>* MakeGrid(std::uint32_t instance);

    IPlumbingHost* host_;
    cRZRandom* random_;
    std::uint32_t refCount_ = 0;
    bool initialised_ = false;               // +0x0c
    bool gridsInitialised_ = false;          // +0x0d (set by Read too: a loaded city keeps its maps)
    bool enabled_ = false;                   // +0x14 (0x6ac284f3: the city is established)
    cSC4SimGrid<std::uint8_t>* working_ = nullptr;     // +0x30 the grid the pass builds (0x49d5bb8c)
    cSC4SimGrid<std::uint8_t>* published_ = nullptr;   // +0x34 the grid the city reads (0x49d5bba1); state 6 swaps them
    std::deque<QueuePoint> pipeQueue_;       // +0x38
    std::deque<QueuePoint> serviceQueue_;    // +0x60
    std::vector<std::uint8_t> cellFlags_;    // +0x88
    int cellsX_ = 0, cellsZ_ = 0;            // +0x8c / +0x90
    std::vector<std::uint32_t> visited_;     // +0x94 bitmap, visitedSize_ squared bits
    int visitedSize_ = 0;                    // +0x98
    std::int32_t month_ = 0;                 // +0x9c
    std::uint32_t averagePollution_ = 0;     // +0xa0
    std::list<WaterProducer> producers_;     // +0xbc
    int state_ = 9;                          // +0xc4
    bool refresh_ = false;                   // +0xc8
    bool ageProducers_ = false;              // +0xc9
    std::int64_t supply_ = 0;                // +0xcc this network's supply
    std::int64_t consumed_ = 0;              // +0xd4 this network's use
    std::int64_t imported_ = 0;              // +0xdc this network's seed deal
    int ringRemaining_ = 0;                  // +0xe4
    std::uint32_t ring_ = 0;                 // +0xe8
    float waterFactor_ = 1.0f;               // +0xec
    std::int64_t demand_ = 0;                // +0x130
    int pipeCount_ = 0;                      // +0x138 pipes counted by the last pass
    int value13c_ = 0;                       // +0x13c (zeroed with the pipe count, recorded as history 0xca14ec19)
    WaterProducer* current_ = nullptr;       // +0x140 the producer whose network the pass is serving
    ScanningIterator scan_;                  // +0x144
    std::int64_t waterCost_ = 0;             // +0x148 sum of the producers' output by their cost per unit
    std::list<cISC4Occupant*> pipes_;        // +0x170 (one reference each)
    int pipeListCount_ = 0;                  // +0x178
    int distressedPipes_ = 0;                // +0x17c
    int burstPipes_ = 0;                     // +0x180
    float pipeFunding_ = 0.0f;               // +0x184 (ApplyPipeFunding's accumulator)
    int lastBuildings_ = 0, lastWatered_ = 0;   // +0x18c / +0x190
    int buildings_ = 0, watered_ = 0;        // +0x194 / +0x198
    int pollutedSources_ = 0;                // +0x19c
    std::vector<std::uint8_t> loadedWatered_;   // the published grid the city file brought, [x * cellsZ + z] (SC4_WATERDEBUG)
    bool captureLoaded_ = false;
};

}  // namespace sc4
