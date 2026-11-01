// cSC4DemandSimulator — the RCI demand simulator (53 symbols, 0x100223f6a..0x10022a826; class id 0x2990bff2,
// interface 0x47967e16, message target 0x452294aa, serializable 0xe4fda3d4, simulator type 0x279681d7).
//
// It owns the city's cSC4Demand channels (keyed by the exemplar instance id of the demand exemplar), the
// links between channels (the "economy index" table: one channel's new supply/demand feeds another, and the
// demand cap follows the supply/demand ratio), the census that turns occupied lots into supply, and the
// monthly export of the surplus demand to the region's connected cities.
//
// Per-tick behaviour (DoMessage):
//   0x66956810 (interval 0, once per city)                    SimulatorBegin
//   0x66956814 (day)                                          DoIdle(5) then DoDailySimulation
//   0x66956816 (month)                                        DoMonthlySimulation
//   0x6695681a (idle time; data3 = milliseconds)              DoIdle(data3)
//   0x8a0d15da / 0xea0d163e (neighbour connection made/lost)   drops the three payload reads, ResetRegionalDemands(false)
//   0xca1b151c                                                ReadSimulatorTunableValues
//   0x26c63343                                                DoDemandIndexLinks twice
//   0xe8bda2e7 (lot state changed)                            occupant funding -> agent effects + capacity
//
// DoIdle is a 4-state machine (+0x20): 3 = idle (until a month starts), 0 = refresh the tax/economy modifiers
// and zero the census, 1 = walk every city cell (census, +0x28 the cursor, +0x30 the per-cell sums, +0x48 the
// previous sums), 2 = push the census deltas into the supply/demand channels and record the history series,
// then 3 again. DoMonthlySimulation evaluates the regional connection utilization, exports the surplus to the
// region and returns the state machine to 0.
//
// Demand keys the code spells out itself (census / export / jobs channels):
//   0x1001..0x1031 residential channels, 0x1011/0x1021/0x1031 their census sums,
//   0x2111..0x2134 residential population by wealth and life-expectancy bucket (0x21<w><bucket>),
//   0x3111..0x3331 commercial and 0x4101..0x4401 industrial census sums,
//   0x21010..0x24400 the RCI demand channels updated by the economy/tax modifiers,
//   0x12010/0x12020/0x12030 the jobs channels, 0x11000/0x12000/0x13000/0x14000 the export channels,
//   0x3800/0x4800/0x4900 the connection-utilization channels.
//
// Everything the original reached through SL:: globals goes through IDemandSimulatorHost (one method per
// original call, with its vtable slot in the comment); the regional city, lot, growth developer and
// neighbour connection are seen through the small interfaces below.
//
// Quirks kept:
//   * Shutdown frees the +0x140 response curve and clears +0x138 a second time, leaving +0x140 dangling
//     (the destructor does not free the curves at all);
//   * DoIdle walks the census map and the previous-census map in lock step by position, not by key;
//   * the salary average divides an unsigned 64-bit weighted sum by an unsigned 64-bit value sum, so a
//     negative total produces a huge divisor;
//   * the tax history query reads the *maximum* of the last 90 days and feeds the response curve
//     `max * 0.1 / max(0.1, neutral tax)`;
//   * a link of type 0x47bb3f30 adds the owner's new supply to the *demand* of the target, while
//     0x47bb3f31 adds it to the supply (recovered from the switch, not the type names).
#pragma once
#include <cstdint>
#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "gz/cGZPersistResourceKey.h"
#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZSerializable.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/cSC4Demand.h"
#include "sc4/cSC4ResponseCurve.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4DemandSimulator = 0x2990bff2;
constexpr std::uint32_t kGZIID_cISC4DemandSimulator = 0x47967e16;
constexpr std::uint32_t kSimulatorTypeDemand = 0x279681d7;
constexpr std::uint32_t kExemplarTypeDemand = 3;                    // property 0x10 of a demand exemplar

// messages
constexpr std::uint32_t kDemandMessageLotStateChanged = 0xe8bda2e7;
constexpr std::uint32_t kDemandMessageConnectionCreated = 0x8a0d15da;
constexpr std::uint32_t kDemandMessageConnectionDeleted = 0xea0d163e;
constexpr std::uint32_t kDemandMessageIndexLinks = 0x26c63343;
constexpr std::uint32_t kDemandMessageReadTunables = 0xca1b151c;
constexpr std::uint32_t kDemandMessageInterval0 = 0x66956810;
constexpr std::uint32_t kDemandMessageDay = 0x66956814;
constexpr std::uint32_t kDemandMessageMonth = 0x66956816;
constexpr std::uint32_t kDemandMessageIdleTime = 0x6695681a;
constexpr std::uint32_t kDemandDefaultDayInterval = 5;              // DoIdle(5) on the daily tick
constexpr int kDemandIdleMsTick = 5;

// demand exemplar properties
constexpr std::uint32_t kDemandPropInterfaceId = 0x11;             // default 0x0797a4e8
constexpr std::uint32_t kDemandPropId = 0x21;                       // overrides the exemplar instance id
// i386 CreateDemandFromExemplar 0x47bb3f20 -> SetSupplyValue (vslot 0x48), 0x47bb3f21 -> SetDemandValue (0x4c), both only
// for a new demand; 0x47bb3f10 -> SetActiveDemandMin (0x54), 0x47bb3f11 -> SetActiveDemandMax (0x50), always. (The port had
// the two pairs crossed: every demand kept the unset caps and the RCI bars ran off the scale.)
constexpr std::uint32_t kDemandPropSupply = 0x47bb3f20;
constexpr std::uint32_t kDemandPropDemand = 0x47bb3f21;
constexpr std::uint32_t kDemandPropMin = 0x47bb3f10;
constexpr std::uint32_t kDemandPropMax = 0x47bb3f11;
constexpr std::uint32_t kDemandPropLinkLo = 0x47bb3f31;             // pairs (target, percent*100)
constexpr std::uint32_t kDemandPropLinkSupply = 0x47bb3f30;
constexpr std::uint32_t kDemandPropLinkHi = 0x47bb3f32;
constexpr std::uint32_t kDemandPropLinkCap = 0x47bb3f41;            // ids only
constexpr std::uint32_t kDemandPropLinkSum = 0x47bb3f50;            // ids only
constexpr std::uint32_t kDemandPropRegionUse = 0x47bb3f60;

// demand-job exemplar properties (0x6534284a / 0xc7bb4816 / <demand id>)
constexpr std::uint32_t kDemandJobsProperty = 0x47bb3f30;

// building exemplar property the lot-state message reads
constexpr std::uint32_t kDemandPropBuildingIgnoreCapacity = 0x27812844;

// tunable exemplar {0x6534284a, 0xe7e2c2db, 0x4a1f3ce6}
constexpr std::uint32_t kDemandTuningType = 0x6534284a;
constexpr std::uint32_t kDemandTuningGroup = 0xe7e2c2db;
constexpr std::uint32_t kDemandTuningInstance = 0x4a1f3ce6;
constexpr std::uint32_t kDemandTunePropTaxCurve = 0x4a1f38b5;
constexpr std::uint32_t kDemandTunePropEconomyCurve = 0x4a1f38b6;
constexpr std::uint32_t kDemandTunePropHistoryDays = 0x4a1f38b7;    // 0x5a
constexpr std::uint32_t kDemandTunePropUtilization = 0x4a1f38c5;    // 13 floats
constexpr std::uint32_t kDemandTunePropAirportFactor = 0x4a1f38c6;  // 1.0
constexpr std::uint32_t kDemandTunePropSeaportFactor = 0x4a1f38c7;  // 2.0
constexpr std::uint32_t kDemandTunePropConnectionFactor = 0x4a1f38c8;   // 20.0
constexpr std::uint32_t kDemandTunePropTrafficFactor = 0x4a1f38c9;      // 0.1
constexpr std::uint32_t kDemandTunePropSeaConnectionFactor = 0x4a1f38ca;  // 20.0
constexpr std::uint32_t kDemandTunePropSalaryWeights = 0x8a63f2a1;  // 12 ints (default 100)

// agent-effect exemplar properties (the occupant exemplar referenced by a building type)
constexpr std::uint32_t kDemandPropEffectDemand = 0x27812841;       // ids + values, feeding demand
constexpr std::uint32_t kDemandPropEffectDemandAux = 0x27812843;    // per-id float factors
constexpr std::uint32_t kDemandPropEffectSupply = 0x27812840;       // ids + values, feeding supply
constexpr std::uint32_t kDemandPropEffectSupplyAux = 0x27812842;

// history series
constexpr std::uint32_t kDemandHistoryResidential = 0xaa772f1d;
constexpr std::uint32_t kDemandHistoryCommercial = 0x6a774f64;
constexpr std::uint32_t kDemandHistoryIndustrial = 0xca774f69;
constexpr std::uint32_t kDemandHistoryTaxBase = 0xaabaaa9f;         // + demand-type index
constexpr std::uint32_t kDemandHistoryDailyBase = 0x2a67a399;       // 12 daily series, keyed by channel
constexpr std::uint32_t kDemandHistoryCensusBase = 0x2a4e2003;      // residential 0x1001..0x1031
constexpr std::uint32_t kDemandHistoryJobsBase = 0x8a5e950e;        // per-demand job counts
constexpr std::uint32_t kDemandHistorySalary = 0x8a63f30c;
constexpr std::uint32_t kDemandHistoryEmployment = 0x8a931172;
constexpr std::uint32_t kDemandHistoryWorkforce = 0x6a931184;

// the 24 census keys ZeroCensusInfo clears
extern const std::uint32_t kDemandCensusKeys[24];
// the 11 demand keys whose active values feed the salary average
extern const std::uint32_t kDemandSalaryKeys[11];
// the 12 RCI channels DoIdle/DoDailySimulation/UpdateTaxAndEconomyModifiers walk
struct DemandChannel {
    std::uint32_t demandId;       // key in the demand map
    std::uint32_t dailyHistory;   // DoDailySimulation series
    std::uint32_t taxHistory;     // tax history series
    std::uint32_t ordinanceProp;  // 0x2a650000 + purpose
    std::uint32_t purpose;        // nation modifier purpose
    int extraEconomy;             // 0 none, 1 value128, 2 value12c
};
extern const DemandChannel kDemandChannels[12];

// ---------------------------------------------------------------------------------------------------------
// The engine neighbours, one interface per original object, with the original vtable slots.
// ---------------------------------------------------------------------------------------------------------

// cISC4RegionalCity as this simulator sees it (the real class is src/sc4/RegionalCity.h).
class IDemandRegionCity {
public:
    virtual ~IDemandRegionCity() = default;
    virtual void GetPosition(int& x, int& z) const = 0;                              // +0x28
    virtual void GetCitySize(int& width, int& height) const = 0;                     // +0x38
    virtual float WorkforcePercentage() const = 0;                                   // +0x60
    virtual float GetTaxRate(std::uint32_t purpose) const = 0;                       // +0x78
    virtual int GetPopulation(std::uint32_t purpose) const = 0;                      // +0x80
    virtual int GetExtrapolatedPopulation(std::uint32_t purpose) const = 0;          // +0x88
    virtual int GetAllowableExtrapolation(std::uint32_t purpose) const = 0;           // +0x90
    virtual void ExtrapolateGrowth(std::uint32_t purpose, float amount) = 0;         // +0x98
};

// cISC4NeighborConnection as this simulator sees it.
class IDemandConnection {
public:
    virtual ~IDemandConnection() = default;
    virtual cISCPropertyHolder* PropertyHolder() = 0;      // +0x18 AsISCPropertyHolder
    virtual std::uint32_t GetType() const = 0;             // +0x28
    virtual const void* CityOnOtherSide() const = 0;       // +0x48
};

// cISC4Lot as the census reads it.
class IDemandLot {
public:
    virtual ~IDemandLot() = default;
    virtual bool HasCapacity() const = 0;                                             // +0x130
    virtual std::uint32_t GetBuildingType(bool ignoreFamily) const = 0;               // +0x170
    virtual std::int32_t GetOccupantWealth() const = 0;                               // +0x148
    virtual std::uint16_t GetCapacity(std::uint32_t developerType, bool ignorePartial) const = 0;  // +0x198
};

// The building occupant of a lot (cISC4Occupant): only the property-holder access is used.
class IDemandOccupant {
public:
    virtual ~IDemandOccupant() = default;
    virtual cISCPropertyHolder* AsPropertyHolder() = 0;              // +0x30
};

// cISC4Lot as the lot-state message sees it (the message carries the lot as cIGZUnknown).
class IDemandLotState : public cIGZUnknown {
public:
    virtual std::uint32_t GetBuildingType(bool ignoreFamily) = 0;    // +0x170
    virtual IDemandOccupant* GetBuilding() const = 0;                // +0x180
};

// cSC4GrowthDeveloper as this simulator sees it.
class IDemandDeveloper {
public:
    virtual ~IDemandDeveloper() = default;
    virtual std::uint32_t GetType() const = 0;                       // +0x28 1 residential, 2 commercial, 3 industrial
    virtual std::uint32_t GetDeveloperType() const = 0;              // +0x30
    virtual cSC4Demand* GetSourceDemand() const = 0;                 // +0x90
    virtual std::int16_t DesirabilityAt(unsigned x, unsigned z) const = 0;  // +0x68 -> grid +0x38
};

// The demand simulator's interface (cISC4DemandSimulator, iid 0x47967e16, slots 3..11).
class cISC4DemandSimulator : public cIGZUnknown {
public:
    virtual std::uint32_t GetSimulatorType() = 0;                                       // 0x28
    virtual cSC4Demand* GetDemand(std::uint32_t id, std::uint32_t keyOffset) = 0;        // 0x30 (key = id + keyOffset)
    virtual void UpdateOccupantEffects(std::uint32_t occupantType, const SC4Percentage& oldFunding,
                                       const SC4Percentage& newFunding) = 0;             // 0x38
    virtual void CalculateJobsPerUnitOfDemand(float* out, std::uint32_t demandId, std::uint32_t kind) = 0;  // 0x40
    virtual std::int64_t GetJobsBySensus(std::uint32_t demandKey) = 0;                   // 0x48
    virtual float GetNeutralTaxRate() = 0;                                               // 0x50
    virtual void GetLocalPopulationSummary(std::map<std::uint32_t, int>& out) const = 0; // 0x58
    virtual bool AddDemandToSimulator(cSC4Demand* demand) = 0;                           // 0x90
};

// A structural copy of the building-development simulator's BuildingProfile: only the fields the census reads.
struct DemandBuildingProfile {
    std::uint32_t purpose = 0;                                                // +0x00
    std::uint32_t wealth = 0;                                                 // +0x04
    std::uint16_t capacity = 0;                                               // +0x08
    std::uint8_t growthStage = 0;                                             // +0x0a
    std::vector<std::map<std::uint32_t, std::uint16_t>> capacities;           // +0x10
};

class IDemandSimulatorHost {
public:
    virtual ~IDemandSimulatorHost() = default;
    // framework
    virtual void AddShutdownCallback(bool (*function)(void*), void* context) = 0;   // SL::spSC4 +0x218
    virtual void AddNotification(cIGZMessageTarget2* target, std::uint32_t message) = 0;      // SL::spMS2 +0x28
    virtual void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t message) = 0;   // SL::spMS2 +0x30
    virtual void AddSimulatorAgent(cIGZMessageTarget2* target, int interval) = 0;   // simulator +0x98, priority 2
    virtual void RemoveSimulatorAgent(cIGZMessageTarget2* target) = 0;              // simulator +0xa8
    virtual void PostMessage(std::uint32_t type, std::uint32_t data1, std::int32_t data2) = 0;  // cISC4Demand::CalculateActiveDemand
    virtual std::uint32_t CurrentDayNumber() = 0;                                   // simulator +0x30 -> date +0xd0
    virtual std::uint64_t NowCycles() = 0;                                          // mach_absolute_time
    virtual float MillisecondsToCycles() = 0;                                       // cSC4LimitTimer::sMillisecsToCyclesCoeff
    // persistence / exemplars
    virtual void ExemplarKeysOfType(std::uint32_t type, std::vector<cGZPersistResourceKey>& out) = 0;  // RM +0x90
    virtual cISCPropertyHolder* LoadExemplar(const cGZPersistResourceKey& key) = 0;  // RM +0x18 (iid 0xa52160f5)
    virtual void ReleaseExemplar(cISCPropertyHolder* holder) = 0;
    // city
    virtual void CityCellCount(int& x, int& z) = 0;                                 // SL::spCity +0x2e8 / +0x2f0
    virtual std::uint32_t AirportTrips() = 0;                                       // GetAirportDeveloper +0x68
    virtual std::uint32_t SeaportTrips() = 0;                                       // GetSeaportDeveloper +0x68
    // region / history / nation / ordinances
    virtual IDemandRegionCity* CurrentRegionCity() = 0;                             // SL::spSC4 +0x120
    virtual void HistoryMinMaxAverage(std::uint32_t series, std::uint32_t fromDay, std::uint32_t toDay,
                                      float& minimum, float& maximum, float& average) = 0;   // +0x38
    virtual void HistoryRecord(std::uint32_t series, std::int32_t value) = 0;       // +0x60
    virtual void HistoryQueueLength(std::uint32_t series, std::uint32_t length) = 0;  // +0x68
    virtual float NationDemandModifier(std::uint32_t purpose) = 0;                  // SL::spSC4 +0x110 -> nation +0x80
    virtual float OrdinanceFactor(std::uint32_t property, std::uint32_t index) = 0; // ordinance sim +0xc8
    // neighbours / traffic
    virtual void AllConnections(std::vector<IDemandConnection*>& out) = 0;          // neighbours +0x80
    virtual void ConnectionsOfType(std::uint32_t type, std::vector<IDemandConnection*>& out) = 0;  // +0x88
    virtual void AllConnectedCities(std::list<IDemandRegionCity*>& out) = 0;        // +0xb0 (false)
    virtual void ConnectedCities(std::list<IDemandRegionCity*>& out) = 0;           // +0xa8 (false)
    virtual std::uint32_t TrafficArrived(cISCPropertyHolder* holder, std::uint32_t kind) = 0;  // traffic sim +0xa8
    // building development / lots / developers
    virtual const DemandBuildingProfile* BuildingProfile(std::uint32_t buildingType) = 0;  // +0x138
    virtual std::uint32_t DeveloperType(std::uint32_t purpose, std::int32_t wealth) = 0;    // +0xf0
    virtual IDemandDeveloper* Developer(std::uint32_t developerType) = 0;                  // +0x108
    virtual void Developers(std::vector<IDemandDeveloper*>& out) = 0;                      // +0x110
    virtual void AllBuildingTypes(std::vector<std::uint32_t>& out) = 0;                     // +0x50
    virtual bool BuildingKeyFromType(std::uint32_t type, cGZPersistResourceKey& key) = 0;   // +0x68
    virtual std::int32_t DayOfLastSuccessfulBuild(std::uint32_t developerType) = 0;         // +0x1a0
    virtual IDemandLot* LotAt(int x, int z, int ignore) = 0;                                // lot manager +0x28
    // residential simulator
    virtual float CellWorkforcePercent(unsigned x, unsigned z) = 0;                 // +0xf0
    virtual float CellEQ(unsigned x, unsigned z) = 0;                               // +0x118
};
IDemandSimulatorHost* DemandSimulatorHost();
void SetDemandSimulatorHost(IDemandSimulatorHost* host);

// ---------------------------------------------------------------------------------------------------------

class cSC4DemandSimulator;

// One entry of the agent-effect maps: multiply by the occupant's funding then add to supply (+4) / demand (+8).
struct DemandAgentEffect {
    std::uint32_t id = 0;
    float supply = 0.0f;      // value at +0x14 of the original node (added to the target's supply)
    float demand = 0.0f;      // value at +0x18 (added to the target's demand)
};

// One economy link of a demand channel (the multimap value of mEconomyLinkMap at +0xb0).
struct DemandEconomyLink {
    std::uint32_t id = 0;             // +0x00 owner id (the multimap key as well)
    std::uint32_t linkId = 0;         // +0x04 unique id (local static counter + 10000/20000/30000/40000/50000)
    std::uint32_t ownerId = 0;        // +0x08
    std::uint32_t targetId = 0;       // +0x0c the other channel
    std::uint32_t type = 0;           // +0x10 0x47bb3f30/31/32/41/50
    float percent = 0.0f;             // +0x14
};

class cSC4DemandSimulator final : public cISC4DemandSimulator,
                                  public cIGZMessageTarget2,
                                  public cIGZSerializable,
                                  public IDemandHost {
public:
    cSC4DemandSimulator();
    ~cSC4DemandSimulator() override;

    // cIGZUnknown / cIGZMessageTarget2 / cIGZSerializable
    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    bool DoMessage(cIGZMessage2* message) override;
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4DemandSimulator; }

    // IDemandHost (a cSC4Demand posts its 0x426840a0 change notification through this)
    void PostMessage(std::uint32_t message, std::uint32_t data1, std::int32_t data2) override {
        host()->PostMessage(message, data1, data2);
    }

    // cISC4DemandSimulator
    std::uint32_t GetSimulatorType() override { return kSimulatorTypeDemand; }
    cSC4Demand* GetDemand(std::uint32_t id, std::uint32_t keyOffset) override;         // @ 0x100229608
    void UpdateOccupantEffects(std::uint32_t occupantType, const SC4Percentage& oldFunding,
                               const SC4Percentage& newFunding) override;              // @ 0x10022a7c6
    void CalculateJobsPerUnitOfDemand(float* out, std::uint32_t demandId, std::uint32_t kind) override;  // @ 0x100229678
    std::int64_t GetJobsBySensus(std::uint32_t demandKey) override;                    // @ 0x1002270de
    float GetNeutralTaxRate() override;                                                // @ 0x100225e44
    void GetLocalPopulationSummary(std::map<std::uint32_t, int>& out) const override;  // @ 0x10022a096
    bool AddDemandToSimulator(cSC4Demand* demand) override;                            // @ 0x100229568

    // the rest of the original class
    bool Init();                                                                       // @ 0x100224520
    bool Shutdown();                                                                   // @ 0x100225776
    void DoIdle(std::uint32_t elapsedMs);                                              // @ 0x1002264b2
    void DoDailySimulation();                                                          // @ 0x100226b92
    void DoMonthlySimulation();                                                        // @ 0x100226e40
    void SimulatorBegin();                                                             // @ 0x100226e6a
    void DoDemandIndexLinks();                                                         // @ 0x100225380
    void ResetRegionalDemands(bool reset);                                             // @ 0x1002250a4
    void ReadSimulatorTunableValues();                                                 // @ 0x100224cb2
    void EvaluateConnectionUtilization();                                              // @ 0x100227ea6
    void ExportDemand();                                                               // @ 0x100228f18
    void PerformCensus(int x, int z);                                                  // @ 0x100227bfe
    void UpdateTaxAndEconomyModifiers();                                               // @ 0x10022718a
    void UpdateCapacity(std::uint32_t buildingType, std::uint32_t wealth, bool add);    // @ 0x100226fd6
    void AddAgentEffect(std::uint32_t type, bool add, const SC4Percentage& scale);      // @ 0x100226ed2
    bool CreateNewDemand(std::uint32_t id, std::uint32_t iid, cSC4Demand** out);       // @ 0x100229458
    void CreateDemandFromExemplar(std::uint32_t exemplarId, cISCPropertyHolder* holder);  // @ 0x1002297e2
    void ExtrapolateResidentialCensus(IDemandRegionCity* city, std::uint32_t demandType, float amount);  // @ 0x10022a2ec
    void ExtrapolateBusinessCensus(IDemandRegionCity* city, std::uint32_t demandType, float amount);     // @ 0x10022a588
    void ZeroCensusInfo(std::map<std::uint32_t, float>& census);                       // @ 0x100224a42

    // statics
    static bool StaticInit(void* context);                                             // @ 0x1002243a0
    static bool StaticShutdown(void* context);                                         // @ 0x10022446e
    static void AddToAgentEffectMap(std::uint32_t buildingType, const cGZPersistResourceKey& key);  // @ 0x100225ef4
    static void InitDynamicDataFromExemplar(const cGZPersistResourceKey& key, void* context);       // @ 0x100224982
    static std::map<std::uint32_t, std::list<DemandAgentEffect>>& AgentEffectMap();
    static std::map<std::uint32_t, std::list<DemandAgentEffect>>& ConnectionEffectMap();
    static void ResetStaticStateForTest();

    // test hooks
    bool IsInitialised() const { return initialized_; }
    int State() const { return state_; }
    std::uint32_t RefCountForTest() const { return refCount_; }
    const std::map<std::uint32_t, cSC4Demand*>& DemandsForTest() const { return demands_; }
    const std::multimap<std::uint32_t, DemandEconomyLink>& LinksForTest() const { return links_; }
    const std::map<std::uint32_t, float>& CensusForTest() const { return censusMap_; }
    const std::map<std::uint32_t, float>& PreviousCensusForTest() const { return previousCensus_; }
    cSC4ResponseCurve* TaxCurve() const { return taxCurve_; }
    cSC4ResponseCurve* EconomyCurve() const { return economyCurve_; }
    float Value130() const { return value130_; }
    float Value134() const { return value134_; }
    std::uint32_t HistoryDays() const { return historyDays_; }
    const std::vector<float>& UtilizationWeights() const { return utilizationWeights_; }
    const std::int32_t* SalaryWeights() const { return salaryWeights_; }

private:
    IDemandSimulatorHost* host() const { return host_ ? host_ : DemandSimulatorHost(); }
    struct CensusInfo {
        std::uint8_t state = 0;      // +0x00
        int x = 0;                   // +0x04
        int z = 0;                   // +0x08
        int step = 1;                // +0x0c
        int xBase = 0;               // +0x10
        int zBase = 0;               // +0x14
        int maxX = 0x7fffffff;       // +0x18
        int maxZ = 0x7fffffff;       // +0x1c
    };
    cSC4Demand* DemandAt(std::uint32_t key);   // map::operator[] like the original (nullptr when absent)
    void RecordChannelHistory();
    void ApplyCensusDeltas();

    IDemandSimulatorHost* host_ = nullptr;
    bool readFromStream_ = false;        // +0x18
    bool initialized_ = false;           // +0x19
    std::uint32_t refCount_ = 0;         // +0x1c
    int state_ = 3;                      // +0x20
    CensusInfo* census_ = nullptr;       // +0x28
    std::map<std::uint32_t, float> censusMap_;          // +0x30
    std::map<std::uint32_t, float> previousCensus_;     // +0x48
    std::map<std::uint32_t, cSC4Demand*> demands_;      // +0x98
    std::multimap<std::uint32_t, DemandEconomyLink> links_;  // +0xb0
    std::int32_t salaryWeights_[12] = {100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100};  // +0xc8
    float value128_ = 0.0f;              // +0x128
    float value12c_ = 0.0f;              // +0x12c
    float value130_ = 0.0f;              // +0x130
    float value134_ = 0.0f;              // +0x134
    cSC4ResponseCurve* taxCurve_ = nullptr;        // +0x138
    cSC4ResponseCurve* economyCurve_ = nullptr;    // +0x140
    std::uint32_t historyDays_ = 0x5a;             // +0x148
    std::vector<float> utilizationWeights_;        // +0x150 (13 floats)
    float airportFactor_ = 1.0f;         // +0x168
    float seaportFactor_ = 2.0f;         // +0x16c
    float connectionFactor_ = 20.0f;     // +0x170
    float trafficFactor_ = 0.1f;         // +0x174
    float seaConnectionFactor_ = 20.0f;  // +0x178
    std::list<IDemandRegionCity*> connectedCities_;   // +0x110 (working list)
    std::list<IDemandRegionCity*> regionalCities_;    // +0xf8
};

}  // namespace sc4
