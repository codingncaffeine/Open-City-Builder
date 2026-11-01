// cSC4PlumbingSimulator (class id 0x0990c075) — the water network: pipes (burst / distressed), water producers
// (pumps, towers, treatment plants), the per-cell water map and the monthly water statistics. The original object is
// ~0x290 bytes (vtables +0 object, +8 message target, +0x10 serializable).
//
// Recovered here: tunables, the producer list and its queries, pipe / building change tracking (DoPipeChanged,
// DoBuildingChanged, DoOccupantInserted/Removed), UpdateWaterProduction, ApplyPipeFunding, AgeWaterProducer,
// FindWaterBuildings, NearestWater, consumption helpers, the statistics getters and the message dispatch.
// Not recovered: DoIdle (the 7 KB flood fill + neighbour trading, PLUMBING-1), Init/Shutdown service wiring
// (PLUMBING-2) and the streams Read/Write (PLUMBING-3).
//
// Layout used: +0x28 city, +0x38 budget, +0x48 neighbours, +0x50 random, +0x58 / +0x60 uint8 sim grids (tract size 1,
// class ids 0x49d5bb8c / 0x49d5bba1), +0xc8 per-cell flag bytes, +0xd0/+0xd4 city cells, +0xd8 bitmap, +0xe0 bitmap size,
// +0xe8 average pollution at the producers, +0xf0 department, +0xf8..+0x108 tunables, +0x110 producer list, +0x120 producer count,
// +0x128 countdown, +0x12c dirty flag, +0x178..+0x190 capacity / consumed / imported / exported (64-bit),
// +0x198 demand, +0x1c0/+0x1e0 age curves, +0x200 pipe list, +0x218 pipes, +0x21c distressed, +0x220 burst,
// +0x224 pipe funding accumulator, +0x228 burst variance, +0x22c buildings, +0x230 watered buildings,
// +0x23c polluted sources, +0x240 default pipe funding, +0x248 production-vs-funding curve, +0x268 pipe funding curve,
// +0x288 pipe cost factor.
//
// Cell flag bits (byte map): 1/2/4/8 pipe connections (E/S/W/N as the pipe reports them), 0x10 pipe, 0x20 water
// producer footprint, 0x40 building with water demand; bits 0xe0 survive a pipe removal.
#pragma once
#include <cstdint>
#include <list>
#include <vector>

#include "gz/cIGZMessage2.h"
#include "gz/cIGZSerializable.h"
#include "rz/cRZRandom.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/cSC4ResponseCurve.h"
#include "sc4/cSC4SimGrid.h"
#include "sc4/RetainedReference.h"

class cIGZIStream;
class cIGZOStream;

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4PlumbingSimulator = 0x0990c075;
constexpr std::uint32_t kGZIID_cISC4PlumbingSimulator = 0x47782ccc;
constexpr std::uint32_t kPlumbingOccupantBuilding = 0x278128a0, kPlumbingOccupantNetwork = 0xc772bf98;
constexpr std::uint32_t kPropBuildingWaterDemand = 0xc8ed2d84;    // exemplar: water the building consumes
constexpr std::uint32_t kPropWaterProductionCapacity = 0x88ed3303;
constexpr std::uint32_t kPropBuildingBudgetCost = 0x2a58d350;
constexpr std::uint32_t kPropProducerWear = 0xc910b4cf, kPropProducerAgeRate = 0xea1cf220, kPropProducerCurrentProduced = 0x08d9763d,
                        kPropProducerWearWarn = 0xea1cf221, kPropProducerWearBroken = 0xea1cf222, kPropProducerConditionCurve = 0x8a1e07ee,
                        kPropProducerGroundwater = 0x48f23a7e;
constexpr std::uint32_t kBudgetItemWater = 0xca58c9d5;
// Messages DoMessage reacts to (id, meaning).
constexpr std::uint32_t kMsgOccupantInserted = 0x99ef1142, kMsgOccupantRemoved = 0x99ef1143, kMsgReloadTunables = 0xca1b151c,
                        kMsgCellCleared = 0xea1f0dcd, kMsgNetworkChanged = 0xea418d8f, kMsgPlumbingFlag = 0x6ac284f3,
                        kMsgSimIdle1 = 0x66956814, kMsgFundingChanged = 0x66956816, kMsgSimIdle2 = 0x6695681a;

struct PlumbingRect { int left = 0, top = 0, right = 0, bottom = 0; };   // inclusive

class IPlumbingPipe;

// cISC4Occupant (the parts the water simulator uses).
class IPlumbingOccupant {
public:
    virtual ~IPlumbingOccupant() = default;
    virtual void AddRef() = 0;                                       // slot 0x08
    virtual void Release() = 0;                                      // slot 0x10
    virtual cISCPropertyHolder* PropertyHolder() = 0;                // slot 0x30
    virtual std::uint32_t OccupantType() = 0;                        // slot 0x38
    virtual void GetPosition(float& x, float& z) = 0;                // slot 0x40 (3 floats, x and z used)
    virtual void GetFootprint(float min[3], float max[3]) = 0;       // slot 0x50 (two 3-float points, x and z used)
    virtual IPlumbingPipe* AsPipe() = 0;                             // QueryInterface(0x49c05b9f), null when not a pipe
    virtual bool HasBudgetInterface() = 0;                           // QueryInterface(0x87dda3a9)
};

// cSC4PipeOccupant (the network-occupant view).
class IPlumbingPipe : public IPlumbingOccupant {
public:
    virtual bool TestFlag(std::uint32_t flag) = 0;                   // slot 0x40
    virtual std::uint32_t GetFlags() = 0;                            // slot 0x60: 0x10000 distressed, 0x20000 burst
    virtual void SetFlags(std::uint32_t flags) = 0;                  // slot 0x50
    virtual void ClearFlags(std::uint32_t flags) = 0;                // slot 0x58
    virtual void GetCellBounds(PlumbingRect& out) = 0;               // view slot 0x58
    virtual bool HasConnection(int direction, int networkType) = 0;  // slot 0x178
    virtual void GetCell(int& x, int& z) = 0;                        // slot 0x1d8
};

// cISC4NeighborConnection
class IPlumbingNeighborConnection {
public:
    virtual ~IPlumbingNeighborConnection() = default;
    virtual void GetCell(int& x, int& z) = 0;                        // slot 0x30
    virtual void SetConnected(bool connected) = 0;                   // slot 0x60
};

// What the original reaches through the global city / budget / lot / neighbour objects.
struct PlumbingBudgetEntry {
    bool tracked = false;               // budget sim 0x198 answered true
    float funding = 0.0f;               // entry +0x1c
    bool hasExplicitFunding = false;    // department 0xe0
    std::int64_t monthlyCost = 0;
};

class IPlumbingWaterMap {
public:
    virtual ~IPlumbingWaterMap() = default;
    virtual int Width() = 0;                                         // slot 0x38
    virtual int Height() = 0;                                        // slot 0x40
    virtual bool IsWater(int x, int z) = 0;                          // slot 0x110
};

// The primary view the city holds for this simulator (cISC4PlumbingSimulator, iid 0x47782ccc).
class cISC4PlumbingSimulator : public cIGZUnknown {};

class IPlumbingHost {
public:
    virtual ~IPlumbingHost() = default;
    virtual int CityCellsX() = 0;                                    // city slot 0x2e8
    virtual int CityCellsZ() = 0;                                    // city slot 0x2f0
    virtual IPlumbingWaterMap* WaterMap() = 0;                       // city slot 0x2a0 (may be null)
    virtual void PositionToCell(float x, float z, int& cellX, int& cellZ) = 0;   // city slot 0x2f8
    virtual float DepartmentFundingFraction() = 0;                   // department +0x50
    virtual std::uint32_t PollutionAt(int x, int z) = 0;             // object at city slot 0x210, slot 0x40
    // Occupants of type 0x278128a0 in cell (x, z) (occupant manager slot 0x140); the caller releases each.
    virtual std::vector<IPlumbingOccupant*> BuildingsAtCell(int x, int z) = 0;
    virtual PlumbingBudgetEntry GetBudgetEntry(cISCPropertyHolder* holder) = 0;  // budget sim 0x198 / department 0xe0
    virtual void SetPipeInfrastructureCost(int cost) = 0;            // department sub-item 0xa0(2)->0x60
    // Budget bookkeeping DoBuildingChanged performs for a building with a water budget item (0x1000a0b6f..0x1000a0cf8).
    virtual void ApplyBuildingBudget(IPlumbingOccupant* building, bool added) = 0;
    virtual bool GetLotRect(int cellX, int cellZ, PlumbingRect& out) = 0;        // lot manager 0x28 + lot 0x70
    virtual std::int32_t LotWaterCapacity(IPlumbingOccupant* building) = 0;      // property 0x88ed3303
    virtual void SetLotCondition(int cellX, int cellZ, int state) = 0;           // lot slot 0x120 (2 warning, 4 broken)
    virtual void BurstPipeAt(int cellX, int cellZ) = 0;              // pipe-burst disaster manager slot 0x90
    // lot manager 0x70(occupant): the lot exists, its slot 0x208 answers true and its slot 0x210 false (production is zeroed).
    virtual bool ProducerLotDisabled(IPlumbingOccupant* occupant) = 0;
    virtual std::vector<IPlumbingNeighborConnection*> NeighborConnections(void* regionalCity) = 0;   // neighbours 0x90(5, city)
    // ---- stream seams: nested serializables reached through the host (keeps the sim free of the DB-segment layer) ----
    virtual void WriteGrid(cIGZOStream&, cSC4SimGrid<std::uint8_t>*) {}   // SC4WriteGZSerializable(stream, grid)
    virtual bool ReadGrid(cIGZIStream&, cSC4SimGrid<std::uint8_t>**) { return false; }
    virtual void WriteWaterGrid(cIGZOStream&) {}                          // +0xf0 int16 water grid (0x223e2b67)
    virtual bool ReadWaterGrid(cIGZIStream&) { return false; }
    virtual void WriteOccupant(cIGZOStream&, IPlumbingOccupant*) {}       // SC4WriteGZSerializable(stream, occupant)
    virtual bool ReadOccupant(cIGZIStream&, IPlumbingOccupant**) { return false; }
    virtual void WritePipe(cIGZOStream&, IPlumbingPipe*) {}               // SC4WriteGZSerializable(stream, pipe)
    virtual bool ReadPipe(cIGZIStream&, IPlumbingPipe**) { return false; }
};

// cIGZMessage2 (the parts DoMessage reads).
class IPlumbingMessage {
public:
    virtual ~IPlumbingMessage() = default;
    virtual std::uint32_t Type() = 0;                                // slot 0x20
    virtual long Data1() = 0;                                        // slot 0x48 (cell x for 0xea1f0dcd; 0 for the network messages)
    virtual int Data2() = 0;                                         // slot 0x70 (cell z)
    virtual IPlumbingOccupant* Occupant() = 0;                       // slot 0x50
    virtual std::uint32_t IdleParameter() = 0;                       // slot 0x98
    // QueryInterface(0x773fef1) on the payload (slot 0x100): the network-change record's type (slot 0x40).
    virtual bool GetNetworkChangeType(std::uint32_t& type) = 0;
};

struct WaterProducer {
    int x = 0, z = 0;                 // +0x00 anchor cell
    PlumbingRect rect;                // +0x08 lot rectangle
    int capacity = 0;                 // +0x1c
    int production = 0;               // +0x20 current output
    float percent = 0.0f;             // +0x24 output / capacity factor
    bool polluted = false;            // +0x28
    IPlumbingOccupant* occupant = nullptr;   // +0x30 (holds a reference)
};

enum class WaterSourceType : int { Ground = 0, Surface = 1, None = 2 };

class cSC4PlumbingSimulator : public cISC4PlumbingSimulator,
                              public cIGZMessageTarget2,
                              public cIGZSerializable {
public:
    cSC4PlumbingSimulator(IPlumbingHost* host, cRZRandom* random);
    virtual ~cSC4PlumbingSimulator();

    // Defaults, then the exemplar {0x6534284a, 0xe7e2c2db, 0xc911e35b} when given (null = defaults only).
    void ReadSimulatorTunableValues(const cISCPropertyHolder* exemplar);
    // Allocates the per-cell byte map and the bitmap and the two uint8 grids (the grid part of Init, 0x10009c1b8).
    void InitCellMap();
    // The data side of Shutdown (0x10009cb48): drops producer / pipe references, grids and maps. Service wiring: PLUMBING-2.
    bool Shutdown();
    void SetInitialized(bool initialized) { initialized_ = initialized; }
    // cIGZUnknown over the original's three views.
    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;   // deletes the simulator when the count drops below 2
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4PlumbingSimulator; }
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;

    // ---- messages ----
    // Dispatches a simulator message. DoIdle (the flood-fill pass) is a virtual hook that is not recovered (PLUMBING-1).
    bool DoMessage(cIGZMessage2* message) override;   // cIGZMessageTarget2 entry (routing unrecovered)
    bool DoMessage(IPlumbingMessage& message);
    virtual bool DoIdle(std::uint32_t idleTime);
    void DoOccupantInserted(IPlumbingOccupant* occupant);
    void DoOccupantRemoved(IPlumbingOccupant* occupant);
    void DoPipeChanged(IPlumbingPipe* pipe, bool added);
    void DoBuildingChanged(IPlumbingOccupant* building, bool added);

    // ---- per-month / per-pass work ----
    void ApplyPipeFunding(const SC4Percentage& funding);
    void UpdateWaterProduction();
    void AgeWaterProducer(WaterProducer& producer);

    // ---- queries ----
    int BuildingConsumption(int cellX, int cellZ);
    void FillGridAtOccupant(cSC4SimGrid<std::uint8_t>& grid, IPlumbingOccupant* occupant, std::uint8_t mask);
    bool GetOccupantMonthlyUpkeepCost(IPlumbingOccupant* occupant, std::int64_t& cost);
    void FindWaterBuildings(IPlumbingOccupant** leastFunded, SC4Percentage* funding, IPlumbingOccupant** mostWorn, int* wearPercent);
    void GetWaterProducers(std::list<IPlumbingOccupant*>& out);
    int GetSupplyValue(int x, int z) const { return supplyGrid_->GetTractValue(x, z); }
    void GetSupplyValueMapPtr(cSC4SimGrid<std::uint8_t>*& out) { out = supplyGrid_; }
    bool AreWateredBuildingsInCell(int x, int z) const { return (supplyGrid_->GetGridData()[std::size_t(x)][std::size_t(z)] & 3) == 3; }
    bool IsBuildingWatered(IPlumbingOccupant* building);
    int GetWaterProduction(IPlumbingOccupant* occupant);
    bool IsCellWatered(int x, int z) const { return (supplyGrid_->GetCellValue(x, z) & 1) != 0; }
    bool GetWaterProducerInfo(IPlumbingOccupant* occupant, std::uint32_t* production, std::uint32_t* capacity, SC4Percentage* percent,
                              bool* polluted, std::uint32_t* currentProduced);
    bool GetWaterProducerConditionPercent(IPlumbingOccupant* occupant, SC4Percentage& out);
    // Spiral distance (ring number) to the nearest water cell within `radius`; -1 when none / no map, 0 for None.
    int NearestWater(int x, int z, WaterSourceType type, int radius);
    bool IsNeighborConnectedToCurrentNetwork(void* regionalCity, bool markConnected);
    void GetConnectionCell(IPlumbingNeighborConnection* connection, int& x, int& z);

    // statistics getters
    std::int64_t GetWaterDemand() const { return demand_; }
    cSC4SimGrid<std::uint8_t>* GetWateredGrid() const { return supplyGrid_; }
    int GetNumWaterSources() const { return int(producers_.size()); }
    int GetNumPollutedWaterSources() const { return pollutedSources_; }
    int GetWaterProductionCapacity() const { return int(capacity_); }
    int GetWaterConsumed() const { return int(consumed_); }
    int GetWaterImported() const { return int(imported_); }
    int GetWaterExported() const { return int(exported_); }
    int GetNumPipes() const { return int(pipes_.size()); }
    int GetNumDistressedPipes() const { return distressedPipes_; }
    int GetNumBurstPipes() const { return burstPipes_; }
    int GetNumWateredBuildings() const { return wateredBuildings_; }
    int GetNumUnwateredBuildings() const { return totalBuildings_ - wateredBuildings_; }

    // ---- state exposed for tests and the not-yet-recovered passes ----
    std::list<WaterProducer>& Producers() { return producers_; }
    std::list<IPlumbingPipe*>& Pipes() { return pipes_; }
    std::vector<std::uint8_t>& CellFlags() { return cellFlags_; }
    std::vector<std::uint32_t>& Bitmap() { return bitmap_; }   // +0xd8, bitmapSize x bitmapSize bits (the flood-fill visit map)
    int BitmapSize() const { return bitmapSize_; }
    bool Dirty() const { return dirty_; }
    bool DirtySecond() const { return dirtySecond_; }
    int Countdown() const { return countdown_; }
    int AveragePollution() const { return averagePollution_; }
    std::int64_t capacity_ = 0, consumed_ = 0, imported_ = 0, exported_ = 0, demand_ = 0;
    int totalBuildings_ = 0, wateredBuildings_ = 0;
    int tunableF8 = 7;              // +0xf8 (read by DoIdle)
    int groundSearchRadius = 2;     // +0xfc  NearestWater radius for ground water
    int surfaceSearchRadius = 1;    // +0x100 NearestWater radius for surface water
    int farGroundPercent = 20;      // +0x104 share of capacity a ground pump yields when the water is more than 1 ring away
    int pollutionThreshold = 100;   // +0x108 pollution above which a producer shuts down
    float burstVariance = 0.3f;                                                                // +0x228
    float pipeCostFactor = 0.0f;                                                               // +0x288
    SC4Percentage departmentFunding{1.2f};                                                     // +0x240
    cSC4ResponseCurve wearByAgeCurve, wearByUseCurve, productionFundingCurve, pipeFundingCurve;   // +0x1c0, +0x1e0, +0x248, +0x268
    float pipeFundingAccum = 0.0f;                                                             // +0x224
    bool flag20 = false;                                                                       // +0x20 (set by message 0x6ac284f3)

private:
    IPlumbingHost* host_;
    cRZRandom* random_;
    int cellsX_ = 0, cellsZ_ = 0;
    std::vector<std::uint8_t> cellFlags_;       // +0xc8
    std::vector<std::uint32_t> bitmap_;         // +0xd8
    int bitmapSize_ = 0;                        // +0xe0
    cSC4SimGrid<std::uint8_t>* auxGrid_ = nullptr;   // +0x58 (a second grid, id 0x49d5bb8c, used only to convert positions to cells here)
    cSC4SimGrid<std::uint8_t>* supplyGrid_ = nullptr;    // +0x60
    std::list<WaterProducer> producers_;        // +0x110
    WaterProducer* currentProducer_ = nullptr;  // +0x1a8
    std::list<IPlumbingPipe*> pipes_;           // +0x200
    // the references a city file carries that the port does not resolve yet (kept as read, written back on save)
    cRetainedReference fileAuxGrid_, fileSupplyGrid_, fileWaterGrid_, fileCurrentOccupant_;
    cRetainedReferences fileProducerOccupants_, filePipes_;
    int distressedPipes_ = 0, burstPipes_ = 0, pollutedSources_ = 0;
    int averagePollution_ = 0;                  // +0xe8 average pollution over the producers (UpdateWaterProduction)
    bool initialized_ = false;                  // +0x18 service wiring done (Init phase one)
    bool gridsInitialized_ = false;             // +0x19 grids / maps allocated (Init phase two)
    std::uint32_t refCount_ = 0;                // +0x1c
    int countdown_ = 0;                         // +0x128
    bool dirty_ = false, dirtySecond_ = false;  // +0x12c, +0x12d
    std::int32_t valueE4_ = 0;                  // +0xe4 (streamed scalar; the pass that maintains it is unrecovered)
    std::int64_t value1b8_ = 0;                 // +0x1b8 (streamed scalar, 64-bit; unrecovered)

    std::uint8_t& CellFlag(int x, int z) { return cellFlags_[std::size_t(cellsX_ * z + x)]; }
};

}  // namespace sc4
