// cSC4GrowthDeveloper — the per (purpose, wealth) growth developer: it owns the tract-sized maps that
// drive growth (desirability, capacity, population, maximum capacity), turns the simulator's other maps
// into desirability once per "map update" through its response curves, and then hands the tract developer
// the list of tracts that should grow and the list that should decline, plus the demand, the thresholds
// and the style percentages it needs.
//
// Recovered from Sim City 4 Deluxe Editionsub 1.2.1 (0x100156058..0x10015b2a4, class id 0x0990c138,
// 0x248 bytes). It is one COM object with three views:
//   +0x00 cISC4GrowthDeveloper      iid 0xe7cdd662 (and iid 1)
//   +0x08 cIGZMessageTarget2        iid 0x452294aa
//   +0x10 cIGZSerializable         iid 0xe4fda3d4
// One reference count at +0x1c is shared by the three views (the secondary AddRef/Release thunks adjust
// their `this` by -8 / -0x10 before touching it). The building development simulator creates one of these
// per developer type (its growth planner owns them); cSC4TractDeveloper reads the maps and the lists.
//
// Original layout (0x248 bytes):
//   +0x18 initialised byte                    +0x1c reference count
//   +0x20 cISC4City                            +0x28 cIGZMessageServer2
//   +0x30 cSC4BuildingDevelopmentSimulator     +0x38 cISC4LandValueSimulator
//   +0x40 cISC4OccupantManager (city 0x110; stored but never used again)
//   +0x48 cISC4TrafficSimulator                +0x50 cISC4ZoneManager
//   +0x58 cISCPropertyHolder the ini exemplar  +0x60 cISC4Demand (the source demand channel)
//   +0x68 int purpose (0x87cd6332)             +0x6c int wealth (0x87cd6330)
//   +0x70 float (property 0x21's +0x14 side? see GetConstructionInfo; never written by this class)
//   +0x74 uint8 tract size (0x87cd6310, default 0x40)
//   +0x75/+0x76 uint8 construction-profile flags (0x87cd6333), +0x77 bool "use total land value"
//   +0x78 cSC4ResponseCurve* land value        +0x80 * slope
//   +0x88 std::vector<cSC4ResponseCurve*> the four pollution maps
//   +0xa0 * aura 0x70                          +0xa8/+0xb0/+0xb8 * residential wealth 1/2/3
//   +0xc0/+0xc8 * commercial 0x3320/0x3330     +0xd0/+0xd8 * residential 0x138/0x140
//   +0xe0 * crime   +0xe8 * traffic congestion  +0xf0 * traffic 0x48
//   +0xf8 * aura 0x78                          +0x100 * aura 0x68
//   +0x108 int32 desirability map value (0x87cd6357, default 0x50): the value the map starts at
//   +0x10c int32 desirability threshold (0x87cd6355, default 0x50): growth list prune threshold
//   +0x110 int32 decline threshold (0x87cd6354, default = +0x10c): decline list threshold
//   +0x114 bool add the population to the demand (0x87cd6356, default true)
//   +0x118 std::vector<cSC4ResponseCurve> style curves (0x87cd6360 count, 0x87cd6361 + i)
//   +0x130..0x133 uint8 supply caps (0x87cd6373 power, 0x87cd6370 water, 0x87cd6371 fire, 0x87cd6372 park)
//   +0x138 std::map<DensityType, float> abandonment thresholds (0x87cd6380, default 0.2)
//   +0x150 uint32 monopoly use (0x87cd6381, default 1)
//   +0x158 std::map<uint32_t, SC4Percentage> style percentages (0x87cd63a0)
//   +0x170 int32 sub type (property 0x21, the developer type)  +0x174 int32 type (0x87cd6300, 1/2/3)
//   +0x178 int32 map-update stage (0 = finished)               +0x17c int32 sim date the update started
//   +0x180 MapUpdateCursor* (0x20 bytes, see below)            +0x188 unused 8
//   +0x190 std::list<cRZAutoRefCount<cISC4Occupant>> the special buildings the landmark pass walks
//   +0x1a8 the iterator of that pass
//   +0x1b0 std::vector<GrowthItem> the growth list             +0x1c8 std::vector<GrowthItem> the decline list
//   +0x1e0 int32 total population                              +0x1e4 int32 total capacity
//   +0x1e8 cSC4SimGrid<uint8_t>*  per-tract count of cells where a building fits the zone, id 0x49b9e602
//   +0x1f0 cSC4SimGrid<int16_t>*  desirability, rebuilt every pass (id 0x49b9e605 + 0x2100000)
//   +0x1f8 cSC4SimGrid<int16_t>*  the desirability map the tract developer sees (0x2000000) - a copy of
//                                 +0x1f0 made at the start of the pass, and the one that is saved
//   +0x200 cSC4SimGrid<int16_t>*  population (0x3000000)       +0x208 cSC4SimGrid<uint16_t>* capacity (0x5000000)
//   +0x210 cSC4SimGrid<uint16_t>* maximum capacity (0x6000000)
//   +0x218 std::vector<uint32_t> subjective thresholds low (0xcab1ce3a)
//   +0x230 std::vector<uint32_t> subjective thresholds high (0xcab1ce3c)
// (the grid instance ids are the sub type + the constant in the comments)
//
// Quirks kept:
//   * Init stores the occupant manager (city 0x110) at +0x40 and never uses it (0x100156620).
//   * ReadSimulatorTunableValues skips the response-curve property read when the exemplar has no
//     *property* (not when the curve is missing) and deletes the old curve in that case (0x100156c0c);
//     an empty style-curve property becomes a single (0,0) point, not an empty curve.
//   * Release() destroys the object when the count is below 2 (Shutdown + deleting destructor), and the
//     destructor does *not* free the response curves: only Shutdown does, so a developer destroyed without
//     Init/Shutdown leaks them (0x10015638c vs 0x1001578b6).
//   * DoMessage treats data4 of a lot state message as the wealth and calls lot->GetBuildingType(true) with
//     the result discarded when the population delta is 0 (0x100157fd6).
//   * DoMessage 0x66956814 replies "not handled" when the map update is still running and more days have
//     passed since it started than the stage number (0x100157fd6).
//   * The map-update cursor wraps x back to +0x10 and z to +0x08 by the same step (+0xc), and the update
//     rect only gets real bounds from SetTractSize: the constructor leaves start = INT_MIN, max = INT_MAX
//     (0x10015606e, 0x100157dc0).
//   * UpdatePopulationCount calls lot->GetLotConfiguration()->GetID() and discards it when a cell's
//     capacity would go negative (0x100158140), and it ignores its `population` argument (the capacity comes
//     from the lot).
//   * Read overwrites the five map pointers without releasing the Init-time maps (0x10015afe2); the port
//     releases the old maps first.
//   * UpdateMaxCapacity only raises the end of the tract range to the start, so a rectangle that starts
//     outside the tract grid writes past the maps (0x100159800); the port clamps the range instead.
//   * The style index of the growth list is a *running* value across items, starts at the curve count and
//     is assigned as-is, so it can be count (out of range) or 1 when there is a single style (0x10015837c
//     case 0x14).
//   * DoIdle constructs and starts a cRZTimer it never reads (0x10015837c).
//   * The driver loop's "check the clock every 1024 cells" test always fires (the counter steps by 0x400
//     and the mask is 0x3ff, 0x100158f0a).
//
// The neighbours this class reaches through the SL::sp* globals are reached through IGrowthDeveloperHost,
// one method per original call with its vtable slot. cSC4SimGrid<T>, cSC4ResponseCurve, cSCPropertyHelp,
// cISCPropertyHolder and cSC4Lot are used directly (all recovered). The growth planner of
// cSC4BuildingDevelopmentSimulator (which owns these objects) is not recovered, so nothing constructs the
// class in the game yet.
#pragma once
#include "sc4/CommonIIDs.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <vector>

#include "gz/cIGZMessage2.h"
#include "gz/cIGZSerializable.h"
#include "rz/cRZAutoRefCount.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/Lot.h"
#include "sc4/Occupant.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/TractDeveloper.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4ResponseCurve.h"
#include "sc4/cSC4SimGrid.h"

namespace sc4 {

class cSC4GrowthDeveloper;
class cISC4GrowthDeveloper;

constexpr std::uint32_t kGrowthDeveloperClassId = 0x0990c138;
constexpr std::uint32_t kGZIID_cISC4GrowthDeveloper = 0xe7cdd662;
constexpr std::uint32_t kGZIID_cISC4GrowthDeveloperSerializable = 0xe4fda3d4;

// cISC4GrowthDeveloper::GrowthItem (0xc bytes): one tract of a growth/decline list. `style` is the
// per-item style index the last map-update stage assigned (see the quirk above).
struct GrowthItem {
    std::int32_t x = 0;        // +0x00 tract x
    std::int32_t z = 0;        // +0x04 tract z
    std::int32_t style = 1;    // +0x08 style index (starts at 1 in the list-building stage)
};
static_assert(sizeof(GrowthItem) == 0xc, "GrowthItem must stay 0xc bytes");

// cISC4BuildingOccupant::DensityType, the key of the abandonment thresholds. The port has no density enum
// yet, so the class declares the three values the original stores (1, 2, 3).
enum GrowthDensityType : std::int32_t {
    kGrowthDensityLow = 1,
    kGrowthDensityMedium = 2,
    kGrowthDensityHigh = 3,
};

// cISC4GrowthDeveloper::Type (+0x174, property 0x87cd6300).
enum GrowthDeveloperType : std::int32_t {
    kGrowthTypeResidential = 1,
    kGrowthTypeCommercial = 2,
    kGrowthTypeIndustrial = 3,
};

// cSC4GrowthDeveloper's map-update cursor (+0x180, 0x20 bytes). DoIdle's driver loop walks the tracts of
// the city in row-major order, `step` at a time, and reports its own state: 0 not started, 1 walking,
// 2 finished. `startX`/`startZ`/`maxX`/`maxZ` are written by SetTractSize only.
struct GrowthMapUpdateCursor {
    std::uint8_t state = 0;      // +0x00
    std::uint8_t reserved01[3] = {};
    std::int32_t x = 0;          // +0x04
    std::int32_t z = 0;          // +0x08
    std::int32_t step = 1;       // +0x0c
    std::int32_t startX = 0;     // +0x10
    std::int32_t startZ = 0;     // +0x14
    std::int32_t maxX = 0;       // +0x18
    std::int32_t maxZ = 0;       // +0x1c
};
static_assert(sizeof(GrowthMapUpdateCursor) == 0x20, "GrowthMapUpdateCursor must stay 0x20 bytes");

// ---------------------------------------------------------------------------------------------------
// Services (one small view per original global; AddRef/Release/Init/Shutdown mirror the COM calls the
// original makes on the stored pointer, like cSC4TractDeveloper's ITractDeveloperService).
// ---------------------------------------------------------------------------------------------------
class IGrowthDeveloperService {
public:
    virtual ~IGrowthDeveloperService() = default;
    virtual void AddRef() {}       // cIGZUnknown slot 0x08
    virtual void Release() {}      // cIGZUnknown slot 0x10
    virtual void Init() {}         // slot 0x18
    virtual void Shutdown() {}     // slot 0x20
};

class IGrowthCityApi : public IGrowthDeveloperService {
public:
    virtual IGrowthDeveloperService* OccupantManager() = 0;                   // city vslot 0x110
    // city vslot 0x190 -> the developer registry's vslot 0xa8 (remove a developer by its view).
    virtual void RemoveDeveloper(cIGZMessageTarget2* view) = 0;
    virtual void PositionToCell(float x, float z, int& cellX, int& cellZ) = 0;   // city vslot 0x2f8
};

class IGrowthBuildingDevelopmentApi : public IGrowthDeveloperService {
public:
    virtual const BuildingProfile& GetBuildingProfile(std::uint32_t developerType) = 0;   // vslot 0x138
    // vslot 0x160: the "special buildings" occupant list the landmark/aura pass walks.
    virtual void GetSpecialBuildings(std::list<cRZAutoRefCount<cISC4Occupant>>& out) = 0;
    // vslot 0x168: the maximum capacity of one zone type for a (purpose, wealth).
    virtual float GetMaxCapacity(int zoneType, int purpose, int wealth) = 0;
    // vslot 0x1a8 GetStagePopulationMultiplier(); the original reads a current-stage member (no argument),
    // while the recovered class exposes it per stage.
    virtual float GetStagePopulationMultiplier() = 0;
    // vslot 0x118 (the tract developer) -> its vslot 0x38 UpdatePopulation(GrowthProfile{developer, x, z}).
    // The profile's stage byte is left uninitialised by the original.
    virtual void UpdateTractPopulation(cISC4GrowthDeveloper* developer, int tractX, int tractZ) = 0;
};

class IGrowthLandValueApi : public IGrowthDeveloperService {
public:
    virtual cSC4SimGrid<std::uint8_t>* TotalLandValueMap() = 0;       // vslot 0x40 (GetTotalLandValueMap)
    virtual cSC4SimGrid<std::uint8_t>* IntrinsicLandValueMap() = 0;   // vslot 0x38 (GetIntrinsicLandValueMap)
    virtual cSC4SimGrid<float>* SlopeMap() = 0;                       // vslot 0x48
};

class IGrowthZoneManagerApi : public IGrowthDeveloperService {
public:
    virtual int ZoneTypeAt(int x, int z) = 0;                         // vslot 0x60
};

class IGrowthResidentialApi : public IGrowthDeveloperService {
public:
    virtual cSC4SimGrid<std::uint8_t>* WealthMap(int wealth) = 0;     // vslot 0x28(wealth 1..3)
    virtual cSC4SimGrid<float>* Map138() = 0;                         // vslot 0x138
    virtual cSC4SimGrid<float>* Map140() = 0;                         // vslot 0x140
};

class IGrowthCommercialApi : public IGrowthDeveloperService {
public:
    virtual cSC4SimGrid<std::uint8_t>* DeveloperMap(std::uint32_t developerType) = 0;   // vslot 0x28
};

class IGrowthPollutionApi : public IGrowthDeveloperService {
public:
    virtual cSC4SimGrid<std::int16_t>* TypeMap(int type) = 0;         // vslot 0x28(type 0..3)
};

class IGrowthAuraApi : public IGrowthDeveloperService {
public:
    virtual cSC4SimGrid<std::int8_t>* Int8Map() = 0;                  // vslot 0x68
    virtual cSC4SimGrid<std::int16_t>* Int16MapA() = 0;               // vslot 0x70
    virtual cSC4SimGrid<std::int16_t>* Int16MapB() = 0;               // vslot 0x78
};

class IGrowthCrimeApi : public IGrowthDeveloperService {
public:
    virtual cSC4SimGrid<std::uint8_t>* CrimeMap() = 0;                // vslot 0x28
};

// The traffic density object traffic simulator vslot 0x68 returns (one call here).
class IGrowthTrafficDensityApi {
public:
    virtual ~IGrowthTrafficDensityApi() = default;
    virtual std::uint8_t GetValue(int x, int z) const = 0;            // vslot 0x38
};

class IGrowthTrafficApi : public IGrowthDeveloperService {
public:
    virtual IGrowthTrafficDensityApi* Density() = 0;                  // vslot 0x68 GetTripLengthMap (its cell getter)
    virtual cSC4SimGrid<std::uint8_t>* CongestionMap() = 0;           // vslot 0x48 GetCommercialTrafficMap (Mac i386 DoIdle stage 13)
};

// One RCI demand channel (cSC4Demand, recovered; the demand simulator is not).
class IGrowthDemandApi {
public:
    virtual ~IGrowthDemandApi() = default;
    virtual void AddRef() {}                                          // cIGZUnknown slot 0x08
    virtual void Release() {}                                         // cIGZUnknown slot 0x10
    virtual float GetValue() const = 0;                               // vslot 0x40
    virtual void AddToSupply(float amount) = 0;                       // vslot 0x88
    virtual float GetActiveValue() const { return 0.0f; }             // vslot 0x60 (the active demand Grow weighs)
};

class IGrowthDemandSimulatorApi : public IGrowthDeveloperService {
public:
    virtual IGrowthDemandApi* GetChannel(std::uint32_t type, std::uint32_t flags) = 0;   // vslot 0x30
};

class IGrowthNeighborsApi : public IGrowthDeveloperService {
public:
    // neighbors simulator vslot 0xa8 GetConnectedCities(list, false): the number of established neighbours.
    virtual int ConnectedCityCount() = 0;
    // vslot 0xb0 GetAllConnectedCities(list, false): the number of neighbours (0 = the demand is local).
    virtual int AllCityCount() = 0;
};

// Everything outside the object. One method per original call; the global/slot is in the comment.
class IGrowthDeveloperHost {
public:
    virtual ~IGrowthDeveloperHost() = default;

    virtual cIGZMessageServer2* MessageServer() = 0;                          // SL::spMS2
    virtual IGrowthCityApi* City() = 0;                                       // SL::spCity
    virtual IGrowthBuildingDevelopmentApi* BuildingDevelopment() = 0;         // SL::spBuildingDevelopmentSim
    virtual IGrowthLandValueApi* LandValue() = 0;                             // SL::spLandValueSim
    virtual IGrowthTrafficApi* Traffic() = 0;                                 // SL::spTrafficSim
    virtual IGrowthZoneManagerApi* ZoneManager() = 0;                         // SL::spZoneManager
    virtual IGrowthResidentialApi* Residential() = 0;                         // SL::spResidentialSim
    virtual IGrowthCommercialApi* Commercial() = 0;                           // SL::spCommercialSim
    virtual IGrowthPollutionApi* Pollution() = 0;                             // SL::spPollutionSim
    virtual IGrowthAuraApi* Aura() = 0;                                       // SL::spAuraSim
    virtual IGrowthCrimeApi* Crime() = 0;                                     // SL::spCrimeSim
    virtual IGrowthDemandSimulatorApi* DemandSimulator() = 0;                 // SL::spDemandSim
    virtual IGrowthNeighborsApi* Neighbors() = 0;                             // SL::spNeighborsSim

    virtual std::int32_t SimDateNumber() = 0;                                 // simulator vslot 0x40
    virtual bool SimulatorPresent() = 0;                                      // SL::spSimulator != null
    // simulator vslot 0x98 AddAgent(target, interval, name, flags): intervals 4 (day) and 10 (idle time).
    virtual void AddSimulatorAgent(cIGZMessageTarget2* target, int interval, const char* name,
                                   std::uint32_t flags) = 0;

    // cSC4BuildingDevelopmentSimulator::BuildingFitsInZone @ 0x100150a86 (a static predicate that the
    // recovered class does not carry yet, although the ledger row for it exists). It maps a building's
    // purpose to the zone types it accepts.
    virtual bool BuildingFitsInZone(std::uint32_t buildingPurpose, std::uint32_t zoneType) = 0;

    // cSC4DynamicLimitTimer(ms) + Start / the inline mach_absolute_time expiry test of the driver loop.
    virtual std::uint64_t BeginSlice(std::uint32_t milliseconds) = 0;
    virtual bool SliceExpired(std::uint64_t slice) = 0;

    virtual std::uint32_t RandomUint32Raw() = 0;                              // cRZRandom::RandomUint32UniformRaw(sRandom)

    // message slot 0x100 (GetIGZUnknown): the lot a lot-state message carries. The port's cSC4Lot has no
    // COM base yet, so the host unwraps the message instead of the class casting the unknown pointer.
    virtual cSC4Lot* MessageLot(cIGZMessage2* message) = 0;

    // SC4WriteGZSerializable / SC4ReadGZSerializable(0xc9bd939a, ..., false) of one grid. The grids are
    // cIGZSerializable objects (one reference comes back from the read).
    virtual void WriteGrid(cIGZOStream& stream, cIGZSerializable* grid) = 0;
    virtual cIGZSerializable* ReadGrid(cIGZIStream& stream) = 0;
};
IGrowthDeveloperHost* GrowthDeveloperHost();
void SetGrowthDeveloperHost(IGrowthDeveloperHost* host);

// ---------------------------------------------------------------------------------------------------
// cISC4GrowthDeveloper: the interface of the primary COM view (37 slots, vtable 0x100a76390).
// ---------------------------------------------------------------------------------------------------
class cISC4GrowthDeveloper : public cIGZUnknown {
public:
    virtual bool Init(cISCPropertyHolder* exemplar) = 0;                              // [3]  0x18
    virtual bool Shutdown() = 0;                                                      // [4]  0x20
    virtual std::int32_t GetType() const = 0;                                         // [5]  0x28
    virtual std::int32_t GetSubType() const = 0;                                      // [6]  0x30
    virtual std::int32_t GetPurpose() const = 0;                                      // [7]  0x38
    virtual std::int32_t GetWealth() const = 0;                                       // [8]  0x40
    virtual void GetConstructionInfo(ConstructionProfile& out) const = 0;             // [9]  0x48
    virtual bool GrowthAndDeclineListsAreValid() const = 0;                           // [10] 0x50
    virtual const std::vector<GrowthItem>& GetGrowthList() const = 0;                 // [11] 0x58
    virtual const std::vector<GrowthItem>& GetDeclineList() const = 0;                // [12] 0x60
    virtual cSC4SimGrid<std::int16_t>* GetDesirabilityMap() const = 0;                // [13] 0x68
    virtual cSC4SimGrid<std::uint16_t>* GetCapacityMap() const = 0;                   // [14] 0x70
    virtual cSC4SimGrid<std::int16_t>* GetPopulationMap() const = 0;                  // [15] 0x78
    virtual cSC4SimGrid<std::uint16_t>* GetMaxCapacityMap() const = 0;                // [16] 0x80
    virtual std::int32_t GetTotalCapacity() const = 0;                                // [17] 0x88
    virtual IGrowthDemandApi* GetSourceDemand() const = 0;                            // [18] 0x90
    virtual std::uint8_t GetPowerSupplyCap() const = 0;                               // [19] 0x98
    virtual std::uint8_t GetWaterSupplyCap() const = 0;                               // [20] 0xa0
    virtual std::uint8_t GetFireCoverageCap() const = 0;                              // [21] 0xa8
    virtual std::uint8_t GetParkCap() const = 0;                                      // [22] 0xb0
    virtual std::int32_t GetDesirabilityThreshold() const = 0;                        // [23] 0xb8
    virtual float GetAbandonmentThreshold(std::int32_t density) const = 0;            // [24] 0xc0
    virtual std::uint32_t GetMonopolyUse() const = 0;                                 // [25] 0xc8
    virtual const std::map<std::uint32_t, SC4Percentage>& GetStylePercentages() const = 0;  // [26] 0xd0
    virtual void StartMapUpdate() = 0;                                                // [27] 0xd8
    virtual bool IsMapUpdateFinished() const = 0;                                     // [28] 0xe0
    virtual std::int32_t EstimateDesirability(int x, int z) const = 0;                // [29] 0xe8
    virtual void GetSubjectiveThresholds(int index, int& low, int& high) const = 0;   // [30] 0xf0
    virtual ~cISC4GrowthDeveloper() = default;                                        // [31]/[32]
    virtual bool DoMessage(cIGZMessage2* message) = 0;                                // [33] 0x108
    virtual bool Write(cIGZOStream& stream) const = 0;                                // [34] 0x110
    virtual bool Read(cIGZIStream& stream) = 0;                                       // [35] 0x118
    virtual std::uint32_t GetGZCLSID() const = 0;                                     // [36] 0x120
};

// ---------------------------------------------------------------------------------------------------
// The object.
// ---------------------------------------------------------------------------------------------------
class cSC4GrowthDeveloper final : public cISC4GrowthDeveloper,
                                  public cIGZMessageTarget2,
                                  public cIGZSerializable {
public:
    cSC4GrowthDeveloper();
    ~cSC4GrowthDeveloper() override;

    // cIGZUnknown (one shared count at +0x1c)
    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override;
    std::uint32_t Release() override;

    // cISC4GrowthDeveloper
    bool Init(cISCPropertyHolder* exemplar) override;
    bool Shutdown() override;
    std::int32_t GetType() const override { return type_; }                     // @ 0x10015b130
    std::int32_t GetSubType() const override { return subType_; }               // @ 0x10015b13c
    std::int32_t GetPurpose() const override { return purpose_; }               // @ 0x10015b148
    std::int32_t GetWealth() const override { return wealth_; }                 // @ 0x10015b152
    void GetConstructionInfo(ConstructionProfile& out) const override;          // @ 0x10015a6da
    bool GrowthAndDeclineListsAreValid() const override { return mapUpdateStage_ < 0x11; }  // @ 0x10015b15c
    const std::vector<GrowthItem>& GetGrowthList() const override { return growthList_; }    // @ 0x10015b16c
    const std::vector<GrowthItem>& GetDeclineList() const override { return declineList_; }  // @ 0x10015b17a
    cSC4SimGrid<std::int16_t>* GetDesirabilityMap() const override { return desirabilityMap_; }  // @ 0x10015b188
    cSC4SimGrid<std::uint16_t>* GetCapacityMap() const override { return capacityMap_; }      // @ 0x10015b196
    cSC4SimGrid<std::int16_t>* GetPopulationMap() const override { return populationMap_; }   // @ 0x10015b1a4
    cSC4SimGrid<std::uint16_t>* GetMaxCapacityMap() const override { return maxCapacityMap_; }  // @ 0x10015b1b2
    std::int32_t GetTotalCapacity() const override { return totalCapacity_; }   // @ 0x10015b1c0
    IGrowthDemandApi* GetSourceDemand() const override { return sourceDemand_; }  // @ 0x10015b1cc
    std::uint8_t GetPowerSupplyCap() const override { return powerSupplyCap_; }  // @ 0x10015b1d6
    std::uint8_t GetWaterSupplyCap() const override { return waterSupplyCap_; }  // @ 0x10015b1e4
    std::uint8_t GetFireCoverageCap() const override { return fireCoverageCap_; }  // @ 0x10015b1f2
    std::uint8_t GetParkCap() const override { return parkCap_; }                // @ 0x10015b200
    std::int32_t GetDesirabilityThreshold() const override { return desirabilityThreshold_; }  // @ 0x10015b20e
    float GetAbandonmentThreshold(std::int32_t density) const override;         // @ 0x10015b21a
    std::uint32_t GetMonopolyUse() const override { return monopolyUse_; }      // @ 0x10015b242
    const std::map<std::uint32_t, SC4Percentage>& GetStylePercentages() const override {  // @ 0x10015b24e
        return stylePercentages_;
    }
    void StartMapUpdate() override;                                            // @ 0x10015b25c
    bool IsMapUpdateFinished() const override { return mapUpdateStage_ == 0; }  // @ 0x10015b276
    void GetSubjectiveThresholds(int index, int& low, int& high) const override;  // @ 0x10015b0cc
    std::int32_t EstimateDesirability(int x, int z) const override;             // @ 0x10015aace

    // cIGZMessageTarget2 / cIGZSerializable
    bool DoMessage(cIGZMessage2* message) override;                             // @ 0x100157fd6
    bool Write(cIGZOStream& stream) const override;                             // @ 0x10015af3a
    bool Read(cIGZIStream& stream) override;                                    // @ 0x10015afe2
    std::uint32_t GetGZCLSID() const override { return kGrowthDeveloperClassId; }  // @ 0x10015b286

    // Non-virtual helpers of the original, recovered.
    void ReadSimulatorTunableValues();                     // @ 0x100156c0c
    void SetTractSize(std::uint8_t size);                  // @ 0x100157dc0
    void UpdatePopulationCount(cSC4Lot* lot, std::uint32_t buildingType, bool add);   // @ 0x100158140
    void DoIdle(std::uint32_t budgetMilliseconds);         // @ 0x10015837c
    void UpdateMaxCapacity(const SC4Rect<int>& rect);      // @ 0x100159800
    static std::int32_t ConvertZoneToType(int zoneType);   // @ 0x10015a6bc

    // Accessors the test and the neighbours use.
    bool IsInitialized() const { return initialized_; }
    std::uint32_t RefCount() const { return refCount_; }
    cISCPropertyHolder* Exemplar() const { return exemplar_; }
    std::int32_t MapUpdateStage() const { return mapUpdateStage_; }
    void SetMapUpdateStage(std::int32_t stage) { mapUpdateStage_ = stage; }
    GrowthMapUpdateCursor* Cursor() const { return cursor_; }
    cSC4SimGrid<std::uint8_t>* ZoneCellCountMap() const { return zoneCellCountMap_; }
    cSC4SimGrid<std::int16_t>* DesirabilityWorkMap() const { return desirabilityWorkMap_; }
    const cSC4ResponseCurve* LandValueCurve() const { return landValueCurve_; }
    const cSC4ResponseCurve* SlopeCurve() const { return slopeCurve_; }
    const std::vector<cSC4ResponseCurve*>& PollutionCurves() const { return pollutionCurves_; }
    // the query tool's rating curves (i386 +0x74 school, +0x78 hospital, +0x7c crime, +0x80 trip length, +0x84 traffic)
    const cSC4ResponseCurve* SchoolGradeCurve() const { return residentialMap138Curve_; }
    const cSC4ResponseCurve* HospitalGradeCurve() const { return residentialMap140Curve_; }
    const cSC4ResponseCurve* CrimeCurve() const { return crimeCurve_; }
    const cSC4ResponseCurve* TripLengthCurve() const { return trafficCurve_; }
    const cSC4ResponseCurve* TrafficCurve() const { return trafficMap48Curve_; }
    const std::vector<cSC4ResponseCurve>& StyleCurves() const { return styleCurves_; }
    const std::map<std::int32_t, float>& AbandonmentThresholds() const { return abandonmentThresholds_; }
    std::int32_t PopulationTotal() const { return populationTotal_; }
    std::int32_t DeclinedThreshold() const { return declineThreshold_; }
    std::int32_t MapUpdateStartDate() const { return mapUpdateStartDate_; }
    bool AddPopulationToDemand() const { return addPopulationToDemand_; }
    std::uint8_t TractSize() const { return tractSize_; }
    cIGZMessageServer2* MessageServerForTest() const { return messageServer_; }
    IGrowthCityApi* CityForTest() const { return city_; }
    IGrowthBuildingDevelopmentApi* BuildingDevelopmentForTest() const { return buildingDevelopment_; }

    // Original field offsets, asserted by the test.
    static constexpr std::uint32_t kOffsetCity = 0x20;
    static constexpr std::uint32_t kOffsetMessageServer = 0x28;
    static constexpr std::uint32_t kOffsetBuildingDevelopment = 0x30;
    static constexpr std::uint32_t kOffsetLandValue = 0x38;
    static constexpr std::uint32_t kOffsetOccupantManager = 0x40;
    static constexpr std::uint32_t kOffsetTraffic = 0x48;
    static constexpr std::uint32_t kOffsetZoneManager = 0x50;
    static constexpr std::uint32_t kOffsetExemplar = 0x58;
    static constexpr std::uint32_t kOffsetSourceDemand = 0x60;
    static constexpr std::uint32_t kOffsetTractSize = 0x74;
    static constexpr std::uint32_t kOffsetStyleCurves = 0x118;
    static constexpr std::uint32_t kOffsetStylePercentages = 0x158;
    static constexpr std::uint32_t kOffsetCursor = 0x180;
    static constexpr std::uint32_t kOffsetSpecialBuildings = 0x190;
    static constexpr std::uint32_t kOffsetGrowthList = 0x1b0;
    static constexpr std::uint32_t kOffsetDeclineList = 0x1c8;
    static constexpr std::uint32_t kOffsetTotals = 0x1e0;
    static constexpr std::uint32_t kOffsetZoneCellCountMap = 0x1e8;
    static constexpr std::uint32_t kOffsetDesirabilityWorkMap = 0x1f0;
    static constexpr std::uint32_t kOffsetDesirabilityMap = 0x1f8;
    static constexpr std::uint32_t kOffsetPopulationMap = 0x200;
    static constexpr std::uint32_t kOffsetCapacityMap = 0x208;
    static constexpr std::uint32_t kOffsetMaxCapacityMap = 0x210;
    static constexpr std::uint32_t kOffsetSubjectiveThresholds = 0x218;
    static constexpr std::uint32_t kClassSize = 0x248;

private:
    // +0x78..+0x100: one response curve property, deleted and recreated like the original.
    void ReadCurveProperty(std::uint32_t propertyId, cSC4ResponseCurve*& curve);
    void DeleteCurve(cSC4ResponseCurve*& curve);
    void CreateMaps();
    void ReleaseServices();
    // The growth-list pruning shared by the last stage (0x1001592xx).
    void PruneGrowthList();

    cSC4SimGrid<std::uint8_t>* MakeUint8Map(std::uint32_t instanceId);
    cSC4SimGrid<std::int16_t>* MakeInt16Map(std::uint32_t instanceId);
    cSC4SimGrid<std::uint16_t>* MakeUint16Map(std::uint32_t instanceId);

    // +0x18
    bool initialized_ = false;
    std::uint32_t refCount_ = 0;                      // +0x1c
    IGrowthCityApi* city_ = nullptr;                  // +0x20
    cIGZMessageServer2* messageServer_ = nullptr;     // +0x28
    IGrowthBuildingDevelopmentApi* buildingDevelopment_ = nullptr;   // +0x30
    IGrowthLandValueApi* landValue_ = nullptr;        // +0x38
    IGrowthDeveloperService* occupantManager_ = nullptr;   // +0x40
    IGrowthTrafficApi* traffic_ = nullptr;            // +0x48
    IGrowthZoneManagerApi* zoneManager_ = nullptr;    // +0x50
    cISCPropertyHolder* exemplar_ = nullptr;          // +0x58
    IGrowthDemandApi* sourceDemand_ = nullptr;        // +0x60
    std::int32_t purpose_ = 0;                        // +0x68
    std::int32_t wealth_ = 0;                         // +0x6c
    float constructionValue_ = 0;                     // +0x70
    std::uint8_t tractSize_ = 0x40;                   // +0x74
    std::uint8_t constructionFlag10_ = 0;             // +0x75
    std::uint8_t constructionFlag11_ = 0;             // +0x76
    bool useIntrinsicLandValue_ = false;              // +0x77
    cSC4ResponseCurve* landValueCurve_ = nullptr;     // +0x78
    cSC4ResponseCurve* slopeCurve_ = nullptr;         // +0x80
    std::vector<cSC4ResponseCurve*> pollutionCurves_; // +0x88 (four entries)
    cSC4ResponseCurve* auraCurve70_ = nullptr;        // +0xa0
    cSC4ResponseCurve* residentialCurve1_ = nullptr;  // +0xa8
    cSC4ResponseCurve* residentialCurve2_ = nullptr;  // +0xb0
    cSC4ResponseCurve* residentialCurve3_ = nullptr;  // +0xb8
    cSC4ResponseCurve* commercialCurve3320_ = nullptr;  // +0xc0
    cSC4ResponseCurve* commercialCurve3330_ = nullptr;  // +0xc8
    cSC4ResponseCurve* residentialMap138Curve_ = nullptr;  // +0xd0
    cSC4ResponseCurve* residentialMap140Curve_ = nullptr;  // +0xd8
    cSC4ResponseCurve* crimeCurve_ = nullptr;         // +0xe0
    cSC4ResponseCurve* trafficCurve_ = nullptr;       // +0xe8
    cSC4ResponseCurve* trafficMap48Curve_ = nullptr;  // +0xf0
    cSC4ResponseCurve* auraCurve78_ = nullptr;        // +0xf8
    cSC4ResponseCurve* auraCurve68_ = nullptr;        // +0x100
    std::int32_t desirabilityMapValue_ = 0x50;        // +0x108
    std::int32_t desirabilityThreshold_ = 0x50;       // +0x10c
    std::int32_t declineThreshold_ = 0x50;           // +0x110
    bool addPopulationToDemand_ = true;              // +0x114
    std::vector<cSC4ResponseCurve> styleCurves_;     // +0x118
    std::uint8_t powerSupplyCap_ = 0;                // +0x130
    std::uint8_t waterSupplyCap_ = 3;                // +0x131
    std::uint8_t fireCoverageCap_ = 3;               // +0x132
    std::uint8_t parkCap_ = 6;                       // +0x133
    // mutable because GetAbandonmentThreshold is const and the original grows the map on lookup (+0x138).
    mutable std::map<std::int32_t, float> abandonmentThresholds_;   // +0x138
    std::uint32_t monopolyUse_ = 1;                  // +0x150
    std::map<std::uint32_t, SC4Percentage> stylePercentages_;   // +0x158
    std::int32_t subType_ = 0;                       // +0x170
    std::int32_t type_ = 0;                          // +0x174
    std::int32_t mapUpdateStage_ = 0;                // +0x178
    std::int32_t mapUpdateStartDate_ = 0;            // +0x17c
    GrowthMapUpdateCursor* cursor_ = nullptr;        // +0x180 (allocated by Init)
    [[maybe_unused]] std::uint64_t reserved188_ = 0; // +0x188
    std::list<cRZAutoRefCount<cISC4Occupant>> specialBuildings_;   // +0x190
    std::list<cRZAutoRefCount<cISC4Occupant>>::iterator specialCursor_;   // +0x1a8 (valid after the fill)
    std::vector<GrowthItem> growthList_;             // +0x1b0
    std::vector<GrowthItem> declineList_;            // +0x1c8
    std::int32_t populationTotal_ = 0;               // +0x1e0
    std::int32_t totalCapacity_ = 0;                 // +0x1e4
    cSC4SimGrid<std::uint8_t>* zoneCellCountMap_ = nullptr;   // +0x1e8
    cSC4SimGrid<std::int16_t>* desirabilityWorkMap_ = nullptr;  // +0x1f0
    cSC4SimGrid<std::int16_t>* desirabilityMap_ = nullptr;    // +0x1f8
    cSC4SimGrid<std::int16_t>* populationMap_ = nullptr;      // +0x200
    cSC4SimGrid<std::uint16_t>* capacityMap_ = nullptr;       // +0x208
    cSC4SimGrid<std::uint16_t>* maxCapacityMap_ = nullptr;    // +0x210
    std::vector<std::uint32_t> subjectiveThresholdLow_;       // +0x218
    std::vector<std::uint32_t> subjectiveThresholdHigh_;      // +0x230
};

// The two list comparators of the original (0x10015a615 / 0x10015a64e), exported for the test. They are
// file-scope functions in the original TU but the growth list sort needs them.
bool GrowthDeveloperCompareDesirability(const GrowthItem& a, const GrowthItem& b);   // @ 0x10015a615
bool GrowthDeveloperCompareRandomize(const GrowthItem& a, const GrowthItem& b);      // @ 0x10015a64e

}  // namespace sc4
