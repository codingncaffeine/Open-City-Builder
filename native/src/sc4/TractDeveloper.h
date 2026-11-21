// cSC4TractDeveloper — the tract developer: it groups the lots of one coarse city "tract" and decides
// whether the tract grows (build a lot, subdivide or aggregate the existing lots) or declines. The
// building development simulator owns one of these and calls Grow/Decline/UpdatePopulation for every
// tract that belongs to a developer type; the zone developer and the lot manager answer the lot queries
// it uses.
//
// Recovered from Sim City 4 Deluxe Editionsub 1.2.1 (0x100134bfa..0x10013c1f0, class id 0x2990c142,
// 0x268 bytes). It is one COM object with three views:
//   +0x00 cISC4TractDeveloper      iid 0xa818ed1b (and iid 1)
//   +0x08 cIGZMessageTarget2       iid 0x452294aa
//   +0x10 cIGZSerializable         iid 0xe4fda3d4
// One reference count at +0x1c is shared by the three views (the two secondary AddRef/Release thunks
// subtract 8 / 0x10 from their `this` before touching it).
//
// Original layout (0x268 bytes):
//   +0x18 initialised byte                      +0x1c reference count
//   +0x20 cIGZMessageServer2                    +0x28 cISC4City
//   +0x30 cSC4BuildingDevelopmentSimulator      +0x38 cISC4LandValueSimulator
//   +0x40 cSC4LotConfigurationManager           +0x48 cSC4LotDeveloper
//   +0x50 cISC4LotManager                       +0x58 cISC4PlumbingSimulator
//   +0x60 cISC4PowerSimulator                   +0x68 cISC4TrafficSimulator
//   +0x70 cISC4ZoneManager                      +0x74 unused int
//   +0x78 float 1.5   capacity/attempts factor  +0x7c float 0.0  decline factor
//   +0x80 float 1.2   build bias                +0x84 float 1.5  second capacity factor
//   +0x88 float 1.0   existing-capacity weight  +0x8c float 0.2  abandoned-capacity weight
//   +0x90 float 10.0  aggregation penalty       +0x94 float 5.0  subdivision penalty
//   +0x98 float 0.2   population jitter         +0x9c float 0.01 decline jitter low
//   +0xa0 float 0.1   decline jitter high       +0xa4 bool 1    may take over abandoned lots
//   +0xa8 std::vector<uint32_t> tunable style ids (the styles file)
//   +0xc0 int years between style changes (0 until the tunables are read, then 0x28 by default)
//   +0xc8 cISC4GrowthDeveloper* of the tract being grown
//   +0xd0/+0xd4 tract x/z                       +0xd8 the growth profile's flag byte
//   +0xdc ConstructionProfile (0x12 bytes)      +0xf0..0xff unused
//   +0x100 int capacity                         +0x104 int maximum build attempts
//   +0x108 int second attempt limit             +0x10c int capacity built by the last Grow
//   +0x114 byte "this tract was attempted"      +0x118 std::set<cISC4Lot*> existing lots
//   +0x130 std::set<cISC4LotConfiguration*> suitable configurations
//   +0x148 std::vector<CandidateLot*> candidate lots
//   +0x160 uint64 configuration data saved for the attempt record
//   +0x168 int object id being built            +0x16c/+0x170/+0x174 int attempt outcomes
//   +0x178 std::list<Outcome>* the caller's outcome list (borrowed)
//   +0x180 byte "the tract could not grow"      +0x188 std::map<cISC4Lot*, Outcome> outcomes per lot
//   +0x1a0 std::vector<uint32_t> active styles  +0x1b8 int style index, +0x1bc int years in style
//   +0x1c0 byte "use all styles at once" (inverted)
//   +0x1c8 std::map<std::pair<int,int>, cISC4LotConfigurationFilter*> filter cache, key {wealth, purpose}
//   +0x1e0 the GrowthAttemptRecord of the pass being run (0x70 bytes, see GrowthAttemptRecord)
//   +0x250 std::map<DeveloperType, std::map<std::pair<int,int>, GrowthRecord>> growth database
//
// Quirks kept:
//   * Grow counts a pass as "the whole tract" only when ListPossibleLotConfigurations returns 27; the
//     pass runs at most n*n+1 times where n is the desirability map's tract size (0x100136070).
//   * RecordGrowthAttempt stores the pass in the *grown* record when something was built and in the
//     *failed* record when nothing was, replacing only that kind of record (0x1001372b4); the other
//     record keeps its own last attempt.
//   * Read does not validate the stored style count (0x100135c32) and Write stores version 3 while
//     Read accepts 0..3.
//   * The candidate-lot pool is a class-wide static (sPool at 0x100b225a8); NewCandidateLot allocates
//     ten at once and copies the static template over the popped object (0x100138eae).
//   * cLotSizesFilter::Release returns 1 without deleting, so the configuration filters leak in the
//     original; this port deletes them in Shutdown (address 0x10013c4ae).
//   * The original visits the tract's cells with a cRZRandomIterator (random order); the order only
//     matters for which lot of a shared cell is inserted first, so it is delegated to the host.
//
// The neighbours this class reaches through the SL::sp* globals are reached through
// ITractDeveloperHost, one method per original call with its vtable slot. cSC4Lot and
// cSC4LotConfiguration are used directly (recovered in Lot.h / LotConfiguration.h).
//
// Recovered growth path: Grow's pass loop, the existing-lot candidate search (ListCandidateLots_Existing, IsLotSuitable,
// IsLotConfigurationCompatible, TestForCornerLot, FindNetworks) and the build (Build, PickBuilding, IsBuildingCompatible). They
// are transcriptions of the decompile and disassembly; the engine calls go through ITractDeveloperHost, so they are not
// byte-exact. TakeOverAbandoned and Decline are recovered too. Not recovered (PARTIAL, to-do TRACT-2): the aggregation and
// subdivision searches and their Perform* steps, IsAggregationLegal, BuildFarm and UpdatePopulation.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gz/cGZPersistResourceKey.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZSerializable.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/Lot.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {

class cSC4Lot;
class cSC4LotConfiguration;
class cSC4LotConfigurationManager;
class cISC4LotConfigurationFilter;
class cSC4TractDeveloper;

constexpr std::uint32_t kTractDeveloperClassId = 0x2990c142;
constexpr std::uint32_t kGZIID_cISC4TractDeveloper = 0xa818ed1b;
constexpr std::uint32_t kGZIID_cISC4TractDeveloperSerializable = 0xe4fda3d4;

// cISC4TractDeveloper::Outcome. The passes record one of these per attempt; 27 means "grew", every
// other value is a rejection reason whose name is only visible in the pass that produced it. The values
// the recovered code produces itself are named here; the rest are carried verbatim.
enum TractOutcome : int {
    kTractOutcomeNone = 0,
    kTractOutcomeNoCandidates = 3,
    kTractOutcomeNoConfiguration = 5,
    kTractOutcomePurposeMismatch = 7,
    kTractOutcomeWealthMismatch = 8,
    kTractOutcomeStyleMismatch = 9,
    kTractOutcomeCapacityTooLow = 0xb,
    kTractOutcomeCapacityTooHigh = 0xd,
    kTractOutcomeGrowthStageTooHigh = 0xe,
    kTractOutcomeBuildFailed = 0x14,
    kTractOutcomeSuccess = 0x1b,
    kTractOutcomeCount = 28,
};

// cISC4BuildingDevelopmentSimulator::ConstructionProfile, written by the growth developer's
// GetConstructionInfo (0x10015a6da): +0x00 building purpose, +0x04 a construction value, +0x08 the
// developer's wealth (an int stored in the float's bits), +0x0c the developer's purpose, +0x10/+0x11 the
// land value range. The tract developer copies it out of the growth developer once per pass.
struct ConstructionProfile {
    std::uint32_t purpose = 0;      // +0x00
    float value4 = 0;               // +0x04
    float value8 = 0;               // +0x08
    std::uint32_t value12 = 0;      // +0x0c
    std::uint8_t flag10 = 0;        // +0x10
    std::uint8_t flag11 = 0;        // +0x11
};

// A per-tract value grid as the growth developer exposes it (cSC4SimGrid<T>, recovered in
// cSC4SimGrid.h). The original returns the typed value; every caller widens it to an int, so the seam
// returns the widened value.
class ITractValueMap {
public:
    virtual ~ITractValueMap() = default;
    virtual std::int32_t GetTractSize() const = 0;                        // vslot 0x50 GetTractSize
    virtual std::int32_t GetTractShift() const = 0;                       // vslot 0x58 GetTractShift
    virtual std::int32_t GetCellValue(int x, int z) const = 0;            // vslot 0xb0 GetCellValue
};

// The demand object the growth developer hands out (its own +0x60 field); vslot 0x60 is the value.
class ITractDemandApi {
public:
    virtual ~ITractDemandApi() = default;
    virtual float GetValue() const = 0;                                   // vslot 0x60
};

// cISC4GrowthDeveloper, restricted to the calls this class makes (slots from the cSC4GrowthDeveloper
// vtable at 0x100a76390). Unrecovered: the growth planner of cSC4BuildingDevelopmentSimulator owns it.
class IGrowthDeveloperApi {
public:
    virtual ~IGrowthDeveloperApi() = default;
    virtual std::int32_t GetDeveloperType() const = 0;                    // vslot 0x30 GetSubType()
    virtual std::int32_t GetPurpose() const = 0;                          // vslot 0x38
    virtual std::int32_t GetWealth() const = 0;                           // vslot 0x40
    virtual void GetConstructionInfo(ConstructionProfile& out) const = 0; // vslot 0x48
    virtual ITractValueMap* GetDesirabilityMap() const = 0;               // vslot 0x68
    virtual ITractValueMap* GetCapacityMap() const = 0;                   // vslot 0x70
    virtual ITractValueMap* GetPopulationMap() const = 0;                 // vslot 0x78
    virtual ITractValueMap* GetMaxCapacityMap() const = 0;                // vslot 0x80
    virtual ITractDemandApi* GetSourceDemand() const = 0;                 // vslot 0x90
    virtual std::int32_t GetType() const = 0;                             // vslot 0x28 (3 = agriculture-free industry)
    virtual std::uint8_t GetPowerSupplyCap() const = 0;                   // vslot 0x98
    virtual std::uint8_t GetWaterSupplyCap() const = 0;                   // vslot 0xa0
    virtual std::uint8_t GetFireCoverageCap() const = 0;                  // vslot 0xa8
    virtual std::uint8_t GetParkCap() const = 0;                          // vslot 0xb0
    virtual std::uint32_t GetMonopolyUse() const = 0;                     // vslot 0xc8
    virtual const std::map<std::uint32_t, SC4Percentage>& GetStylePercentages() const = 0;  // vslot 0xd0
};

// cISC4BuildingDevelopmentSimulator::GrowthProfile, passed by the growth planner: the developer of the
// tract, the tract coordinates and the growth stage the tract has reached (the original reads
// +0x00/+0x08/+0x0c/+0x10).
struct GrowthProfile {
    IGrowthDeveloperApi* developer = nullptr;   // +0x00
    std::int32_t tractX = 0;                    // +0x08
    std::int32_t tractZ = 0;                    // +0x0c
    std::uint8_t growthStage = 0;               // +0x10 the stage the tract has reached so far
};

// ---------------------------------------------------------------------------------------------------
// Services (one small view per original global; AddRef/Release/Init/Shutdown mirror the COM calls the
// original makes on the stored pointer, like cSC4ZoneDeveloper's IZoneDeveloperService).
// ---------------------------------------------------------------------------------------------------
class ITractDeveloperService {
public:
    virtual ~ITractDeveloperService() = default;
    virtual void AddRef() {}       // cIGZUnknown slot 0x08
    virtual void Release() {}      // cIGZUnknown slot 0x10
    virtual void Init() {}         // slot 0x18 (where the owner calls it)
    virtual void Shutdown() {}     // slot 0x20
};

class ITractCityApi : public ITractDeveloperService {
public:
    virtual int CellCountX() = 0;                                          // city vslot 0x2e8
    virtual int CellCountZ() = 0;                                          // city vslot 0x2f0
    // city vslot 0x190 -> the developer registry's vslot 0xa8 (remove a developer by its view).
    virtual void RemoveDeveloper(cIGZMessageTarget2* view) = 0;
};

class ITractLotManagerApi : public ITractDeveloperService {
public:
    virtual cSC4Lot* GetLot(int x, int z, bool exactCell) = 0;              // vslot 0x28
    virtual int GetLotsInCellRect(std::list<cRZAutoRefCount<cSC4Lot>>& out, int x0, int z0, int x1,
                                  int z1, bool unique) = 0;                 // vslot 0x38
    virtual int CanCreateLot(int x, int z, int sizeX, int sizeZ, bool param5, bool param6,
                             cSC4LotConfiguration* configuration) = 0;      // vslot 0xa0
};

class ITractLotDeveloperApi : public ITractDeveloperService {
public:
    virtual void StartConstruction(cSC4Lot* lot, cSC4LotConfiguration* configuration,
                                   std::uint32_t buildingType, bool flag) = 0;   // vslot 0x30
    virtual void AbandonBuilding(cSC4Lot* lot, int vacateReason) = 0;       // vslot 0x40
    virtual void ReoccupyBuilding(cSC4Lot* lot, std::uint32_t developerType) = 0;  // vslot 0x48
    virtual float GetAreaSlope(const std::int32_t rect[4], bool includeWater) = 0; // vslot 0xe8
};

class ITractBuildingDevelopmentApi : public ITractDeveloperService {
public:
    virtual bool CheatCodesEnabled() = 0;                                   // vslot 0x1c8
    virtual const BuildingProfile& GetBuildingProfile(std::uint32_t type) const = 0;  // vslot 0x138
    virtual std::uint16_t GetFreeConstructionResources() = 0;               // vslot 0x180
    virtual bool ClaimConstructionResources(std::uint16_t amount) = 0;      // vslot 0x190
    virtual void ReleaseConstructionResources(std::uint16_t amount) = 0;    // vslot 0x198
    virtual bool GetBuildingKeyFromType(std::uint32_t type, cGZPersistResourceKey& key) = 0;    // vslot 0x68
    virtual void GetBuildingTypesByFamily(std::unordered_set<std::uint32_t>& out, std::uint32_t family) = 0;  // vslot 0x88
    virtual bool CheatIgnorePlumbing() = 0;                                 // vslot 0x1b0
    virtual bool CheatIgnorePower() = 0;                                    // vslot 0x1b8
    virtual bool CheatIgnoreCaps() = 0;                                     // vslot 0x1c0
    // vslot 0x120 GetLotDeveloper() -> the lot developer's vslot 0x120 NeedsAnchorBuilding(rect, purpose).
    virtual bool NeedsAnchorBuilding(const std::int32_t rect[4], std::int32_t purpose) = 0;
    // vslot 0x100 GetDeveloper(purpose, wealth): the growth developer of a building purpose and wealth (null when none).
    virtual IGrowthDeveloperApi* GetDeveloper(std::uint32_t purpose, std::int32_t wealth) = 0;
};

class ITractZoneManagerApi : public ITractDeveloperService {
public:
    virtual int ZoneTypeAt(int x, int z) = 0;                               // vslot 0x60
};

// The per-cell network lookup cSC4TractDeveloper::FindNetworks uses (city vslot 0x140 -> network
// manager vslot 0x50), and the found cell's network-type test (vslot 0xb0).
class ITractNetworkCellApi {
public:
    virtual ~ITractNetworkCellApi() = default;
    virtual bool HasNetworkType(int networkType) const = 0;                 // vslot 0xb0
};

class ITractNetworkManagerApi : public ITractDeveloperService {
public:
    virtual ITractNetworkCellApi* GetNetworkAt(int x, int z, std::uint32_t typeMask, int param) = 0;  // vslot 0x50
};

// The tunables cSC4TractDeveloper::ReadSimulatorTunableValues reads from the tract-developer ini
// exemplar (property ids 0x47e2c500..0x47e2c551). The struct arrives pre-filled with the constructor
// defaults; the host overwrites the properties the exemplar actually has.
struct TractTunables {
    float capacityFactor = 1.5f;             // 0x47e2c500 -> +0x78
    float declineFactor = 0.0f;              // 0x47e2c501 -> +0x7c
    float buildBias = 1.2f;                  // 0x47e2c501 -> +0x80
    float capacityFactor2 = 1.5f;            // 0x47e2c502 -> +0x84
    float existingCapacityWeight = 1.0f;     // 0x47e2c510 -> +0x88
    float abandonedCapacityWeight = 0.2f;    // 0x47e2c510 -> +0x8c
    float aggregationPenalty = 10.0f;        // 0x47e2c511 -> +0x90
    float subdivisionPenalty = 5.0f;         // 0x47e2c511 -> +0x94
    float populationJitter = 0.2f;           // 0x47e2c530 -> +0x98
    float declineJitterLow = 0.01f;          // 0x47e2c531 -> +0x9c
    float declineJitterHigh = 0.1f;          // 0x47e2c531 -> +0xa0
    bool allowTakeOver = true;               // 0x47e2c540 -> +0xa4
    std::vector<std::uint32_t> styles;       // 0x47e2c550 -> +0xa8
    bool hasYearsBetweenStyleChanges = false;
    std::int32_t yearsBetweenStyleChanges = 0x28;   // 0x47e2c551 -> +0xc0 when the int was 0
};

// Everything outside the object. One method per original call; the global/slot is in the comment.
class ITractDeveloperHost {
public:
    virtual ~ITractDeveloperHost() = default;

    virtual cIGZMessageServer2* MessageServer() = 0;                          // SL::spMS2
    virtual bool SimulatorPresent() = 0;                                      // SL::spSimulator != null
    // simulator vslot 0x98: register a message target for one of the simulator's tick messages.
    virtual void RegisterSimulatorCallback(int messageId, cIGZMessageTarget2* target,
                                           const char* name) = 0;
    virtual ITractCityApi* City() = 0;                                        // SL::spCity
    virtual ITractBuildingDevelopmentApi* BuildingDevelopment() = 0;          // SL::spBuildingDevelopmentSim
    virtual ITractDeveloperService* LandValue() = 0;                           // SL::spLandValueSim
    virtual cSC4LotConfigurationManager* LotConfigurationManager() = 0;       // SL::spLotConfigurationManager
    virtual ITractLotDeveloperApi* LotDeveloper() = 0;                        // SL::spLotDeveloper
    virtual ITractLotManagerApi* LotManager() = 0;                            // SL::spLotManager
    virtual ITractDeveloperService* Plumbing() = 0;                           // SL::spPlumbingSim
    virtual ITractDeveloperService* Power() = 0;                              // SL::spPowerSim
    virtual ITractDeveloperService* Traffic() = 0;                            // SL::spTrafficSim
    virtual ITractZoneManagerApi* ZoneManager() = 0;                          // SL::spZoneManager

    // building development vslot 0x108 (get the growth developer of a developer type) -> 0x68/0x58:
    // the growth developer's desirability map is where the tract shift comes from.
    virtual IGrowthDeveloperApi* GrowthDeveloper(std::uint32_t developerType) = 0;
    // The cLotSizesFilter the pass registers with the lot configuration manager.
    virtual bool LotConfigurationsByFilter(std::vector<cSC4LotConfiguration*>& out,
                                           cISC4LotConfigurationFilter* filter) = 0;   // manager vslot 0x48
    virtual void RegisterLotConfigurationFilter(cISC4LotConfigurationFilter* filter) = 0;   // manager vslot 0x90
    virtual void UnregisterLotConfigurationFilter(cISC4LotConfigurationFilter* filter) = 0; // manager vslot 0x98
    // Resource manager + the tract developer's ini exemplar (0xe7e2c2db 6534284a / 0xe8da7677).
    virtual bool ReadTunables(TractTunables& io) = 0;   // false: the exemplar is missing (the original then skips the style step)
    virtual std::int32_t SimulatorDay() = 0;                                  // simulator clock vslot 0x30
    virtual std::uint32_t RandomBelow(std::uint32_t range) = 0;               // cRZRandom::RandomUint32UniformBelow
    // cRZRandomIterator over one tract's city cells (rect [tract<<shift .. (tract+1)<<shift - 1]);
    // see the note on ListExistingLots.
    virtual void ForEachCellInTract(int tractX, int tractZ, int shift,
                                    const std::function<void(int, int)>& fn) = 0;

    // The cell queries of the growth predicates, one per original call chain.
    virtual ITractNetworkManagerApi* NetworkManager() = 0;                    // SL::spCity vslot 0x140 GetTrafficNetwork
    virtual std::uint8_t LandValueAt(int x, int z) = 0;                       // land value sim vslot 0x30 GetLandValue
    virtual bool IsCellWatered(int x, int z) = 0;                             // plumbing sim vslot 0x58
    virtual std::uint8_t PoweredAt(int x, int z) = 0;                         // power sim vslot 0x120 GetPoweredGrid -> grid vslot 0x38
    virtual bool FireProtectedAt(int x, int z) = 0;                           // SL::spFireProtectionSim vslot 0x28 -> grid vslot 0x38
    virtual std::int16_t ParkEffectAt(int x, int z) = 0;                      // SL::spAuraSim vslot 0x70 GetParkMap -> grid vslot 0x38
    virtual bool HasTripLengthAt(int x, int z) = 0;                           // traffic sim vslot 0x68 GetTripLengthMap -> grid vslot 0x38
    // SL::spDemolition vslot 0x38 DemolishLots(true, lots, privilege 0, flags, false, null, null, null).
    virtual void DemolishLots(const std::list<cRZAutoRefCount<cSC4Lot>>& lots, std::uint32_t flags) = 0;
    // SL::spRM vslot 0x18 GetResource(key, 0xa52160f5 exemplar) -> cSCPropertyHelp::GetPropertyValue(bool).
    virtual void BuildingExemplarBool(const cGZPersistResourceKey& key, std::uint32_t property, bool& value) = 0;
};
ITractDeveloperHost* TractDeveloperHost();
void SetTractDeveloperHost(ITractDeveloperHost* host);

// ---------------------------------------------------------------------------------------------------
// cISC4TractDeveloper: the interface of the primary COM view (20 slots, vtable 0x100a75be0).
// ---------------------------------------------------------------------------------------------------
class cISC4TractDeveloper : public cIGZUnknown {
public:
    virtual bool Init() = 0;                                                          // [3]  0x18
    virtual bool Shutdown() = 0;                                                      // [4]  0x20
    virtual int Grow(const GrowthProfile& profile, std::list<int>& outcomes, bool& declined) = 0;  // [5] 0x28
    virtual int Decline(const GrowthProfile& profile) = 0;                            // [6]  0x30
    virtual void UpdatePopulation(const GrowthProfile& profile) = 0;                  // [7]  0x38
    virtual std::uint32_t GetCurrentStyle() const = 0;                                // [8]  0x40
    virtual const std::vector<std::uint32_t>& GetActiveStyles() const = 0;            // [9]  0x48
    virtual void SetActiveStyles(const std::vector<std::uint32_t>& styles) = 0;       // [10] 0x50
    virtual bool IsUsingAllStylesAtOnce() const = 0;                                  // [11] 0x58
    virtual void UseAllStylesAtOnce(bool on) = 0;                                     // [12] 0x60
    virtual std::int32_t GetYearsBetweenStyleChanges() const = 0;                      // [13] 0x68
    virtual void SetYearsBetweenStyleChanges(int years) = 0;                          // [14] 0x70
    virtual ~cISC4TractDeveloper() = default;                                         // [15]/[16]
    virtual bool DoMessage(cIGZMessage2* message) = 0;                                // [17] 0x88
    virtual bool Write(cIGZOStream& stream) const = 0;                                // [18] 0x90
    virtual bool Read(cIGZIStream& stream) = 0;                                       // [19] 0x98
    virtual std::uint32_t GetGZCLSID() const = 0;                                     // [20] 0xa0
};

// ---------------------------------------------------------------------------------------------------
// The records the class keeps.
// ---------------------------------------------------------------------------------------------------

// cSC4TractDeveloper::CandidateLot (0xf0 bytes): one lot the growth pass could build on. `kind` says
// where it came from (1 take over an abandoned lot, 2 an existing lot, 3 aggregation, 4 subdivision).
struct TractCandidateLot {
    std::int32_t kind = 0;                          // +0x00 1 take-over, 2 existing, 3 aggregation, 4 subdivision
    std::int32_t reserved04 = 0;                    // +0x04
    cSC4Lot* lot = nullptr;                         // +0x08 borrowed
    std::int32_t x = 0;                             // +0x10
    std::int32_t z = 0;                             // +0x14
    std::int32_t facing = 0;                        // +0x18
    std::uint8_t sizeX = 0;                         // +0x1c
    std::uint8_t sizeZ = 0;                         // +0x1d
    std::uint16_t reserved1e = 0;                   // +0x1e
    std::set<cSC4Lot*> neighbours;                  // +0x20 lots the aggregation/subdivision touches
    std::int32_t existingCapacity = -1;             // +0x38 (-1 = not measured yet)
    std::int32_t abandonedCapacity = -1;            // +0x3c
    std::int32_t maxGrowthStage = 0;                // +0x40
    std::int32_t maxWealth = 0;                     // +0x44 (the building profile's +4, i.e. wealth)
    float score = 0;                                // +0x48 (compared by CompareCandidateLots)
    std::int32_t rectX0 = -1;                       // +0x4c (GetBoundingRect fills these; -1 = not set)
    std::int32_t rectZ0 = -1;                       // +0x50
    std::int32_t rectX1 = -1;                       // +0x54
    std::int32_t rectZ1 = -1;                       // +0x58
    std::int32_t reserved5c = 0;                    // +0x5c
    std::uint8_t reserved60[0x20] = {};             // +0x60..0x7f (never read by the recovered code)
    std::uint8_t ratio = 0;                         // +0x80 subdivison ratio
    std::uint8_t reserved81[7] = {};                // +0x81
    // FindNetworks' counts per edge (0 north/-z, 1 east/+x, 2 south/+z, 3 west/-x) of the network types next to the lot.
    std::vector<std::int32_t> streets = std::vector<std::int32_t>(4, 0);   // +0x88 street (3) / dirt road (11)
    std::vector<std::int32_t> roads = std::vector<std::int32_t>(4, 0);     // +0xa0 road (0) / one-way road (10)
    std::vector<std::int32_t> avenues = std::vector<std::int32_t>(4, 0);   // +0xb8 avenue (6)
    std::vector<std::int32_t> rails = std::vector<std::int32_t>(4, 0);     // +0xd0 rail (1)
    std::uint8_t needsRoad = 0;                     // +0xe8 set when a configuration with required roads fits
    std::uint8_t reservedE9[7] = {};                // +0xe9
};
static_assert(sizeof(TractCandidateLot) == 0xf0, "CandidateLot must stay 0xf0 bytes");

// cSC4TractDeveloper::CandidateLot's original name; kept short for the member signatures below.
using CandidateLot = TractCandidateLot;

// cSC4TractDeveloper::BuildAttemptRecord (0x58 bytes): one snapshot of a build attempt.
struct TractBuildAttemptRecord {
    std::uint8_t attempted = 0;                            // +0x00 (this+0x114)
    std::uint8_t reserved01[7] = {};                       // +0x01
    std::set<cSC4LotConfiguration*> configurations;         // +0x08 suitable configurations
    std::vector<CandidateLot*> candidates;                  // +0x20 owned copies (FreeBuildAttempt frees them)
    std::uint64_t configurationData = 0;                    // +0x38 (this+0x160)
    std::int32_t configurationId = 0;                       // +0x40 (this+0x168)
    std::int32_t outcome0 = 0;                              // +0x44 (this+0x16c)
    std::int32_t outcome1 = 0;                              // +0x48 (this+0x170)
    std::int32_t outcome2 = 0;                              // +0x4c (this+0x174)
    std::int32_t outcomeCount = 0;                          // +0x50 size of the caller's outcome list
    std::int32_t reserved54 = 0;                            // +0x54
};
static_assert(sizeof(TractBuildAttemptRecord) == 0x58, "BuildAttemptRecord must stay 0x58 bytes");

using BuildAttemptRecord = TractBuildAttemptRecord;

// cSC4TractDeveloper::GrowthAttemptRecord (0x70 bytes): the state of one growth pass. The class member
// at +0x1e0 is the pass being run; RecordGrowthAttempt copies it into the growth database.
struct TractGrowthAttemptRecord {
    std::int32_t reserved0[2] = {0, 0};                     // +0x00 never written by the original
    std::int32_t day = 0;                                   // +0x08 julian day of the pass
    std::uint32_t reserved0c = 0;                           // +0x0c
    IGrowthDeveloperApi* developer = nullptr;               // +0x10 borrowed
    std::int32_t tractX = 0;                                // +0x18
    std::int32_t tractZ = 0;                                // +0x1c
    std::int32_t population = 0;                            // +0x20
    std::int32_t reserved24 = 0;                            // +0x24
    std::int32_t capacity = 0;                              // +0x28
    std::int32_t maxCapacity = 0;                           // +0x2c
    std::int32_t desirability = 0;                          // +0x30
    std::int32_t maximumBuilds = 0;                         // +0x34
    std::uint16_t objectId = 0;                             // +0x38
    std::uint16_t reserved3a = 0;                           // +0x3a
    std::int32_t reserved3c = 0;                            // +0x3c
    std::list<BuildAttemptRecord> attempts;                 // +0x40
    std::vector<std::vector<std::uint32_t>> histograms;     // +0x58 13 x 28 outcome counters
};
static_assert(sizeof(TractGrowthAttemptRecord) == 0x70, "GrowthAttemptRecord must stay 0x70 bytes");

// cSC4TractDeveloper::GrowthRecord (0xe0 bytes): the *grown* pass at +0x00 and the *failed* pass at
// +0x70 of the same tract (see RecordGrowthAttempt).
struct TractGrowthRecord {
    TractGrowthAttemptRecord grown;     // +0x00
    TractGrowthAttemptRecord failed;    // +0x70
};
static_assert(sizeof(TractGrowthRecord) == 0xe0, "GrowthRecord must stay 0xe0 bytes");

using GrowthAttemptRecord = TractGrowthAttemptRecord;
using GrowthRecord = TractGrowthRecord;

// The 13 histogram buckets of a growth attempt (index = outer vector; 28 outcomes each).
enum TractHistogramBucket : std::size_t {
    kHistogramOverall = 0,          // Grow's own outcome (+0x00)
    kHistogramTakeOver = 1,         // TakeOverAbandoned (+0x18)
    kHistogramConfigurations = 2,   // ListPossibleLotConfigurations (+0x30)
    kHistogramExisting = 3,         // ListCandidateLots_Existing (+0x48)
    kHistogramAggregation = 4,      // ListCandidateLots_Aggregation (+0x60)
    kHistogramSubdivision = 5,      // ListCandidateLots_Subdivision (+0x78)
    kHistogramBuild = 6,            // Build (+0x90)
    kHistogramSuitable = 7,         // IsLotConfigurationSuitable (+0xa8)
    kHistogramLotSuitable = 8,      // IsLotSuitable (+0xc0)
    kHistogramStyle = 10,           // PickBuilding's style filter (+0xf0)
    kHistogramCompatible = 11,      // IsLotConfigurationCompatible (+0x108)
    kHistogramBuilding = 12,        // IsBuildingCompatible (+0x120)
    kHistogramBucketCount = 13,
};
constexpr std::size_t kTractOutcomeSlots = 28;

// ---------------------------------------------------------------------------------------------------
// The object.
// ---------------------------------------------------------------------------------------------------
class cSC4TractDeveloper final : public cISC4TractDeveloper,
                                 public cIGZMessageTarget2,
                                 public cIGZSerializable {
public:
    cSC4TractDeveloper();
    ~cSC4TractDeveloper() override;

    // cIGZUnknown (one shared count at +0x1c)
    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override;
    std::uint32_t Release() override;

    // cISC4TractDeveloper
    bool Init() override;
    bool Shutdown() override;
    int Grow(const GrowthProfile& profile, std::list<int>& outcomes, bool& declined) override;
    int Decline(const GrowthProfile& profile) override;
    void UpdatePopulation(const GrowthProfile& profile) override;
    std::uint32_t GetCurrentStyle() const override;                                    // @ 0x10013c166
    const std::vector<std::uint32_t>& GetActiveStyles() const override { return activeStyles_; }  // @ 0x10013c18a
    void SetActiveStyles(const std::vector<std::uint32_t>& styles) override;           // @ 0x10013c0fa
    bool IsUsingAllStylesAtOnce() const override { return !singleStyle_; }             // @ 0x10013c198
    void UseAllStylesAtOnce(bool on) override { singleStyle_ = !on; }                  // @ 0x10013c1a8
    std::int32_t GetYearsBetweenStyleChanges() const override { return yearsBetweenStyleChanges_; }  // @ 0x10013c1ba
    void SetYearsBetweenStyleChanges(int years) override { yearsBetweenStyleChanges_ = years; }  // @ 0x10013c1c6

    // cIGZMessageTarget2 / cIGZSerializable
    bool DoMessage(cIGZMessage2* message) override;                                     // @ 0x100135fe4
    bool Write(cIGZOStream& stream) const override;                                     // @ 0x100135b62
    bool Read(cIGZIStream& stream) override;                                            // @ 0x100135c32
    std::uint32_t GetGZCLSID() const override { return kTractDeveloperClassId; }        // @ 0x10013c1d2

    // Non-virtual helpers of the original (recovered unless marked PARTIAL).
    void ReadSimulatorTunableValues();                     // @ 0x1001353a6
    void FreeGrowthDatabase();                             // @ 0x100135a12
    void ClearCandidateList(std::vector<CandidateLot*>& list);   // @ 0x10013667c
    void ListExistingLots();                               // @ 0x1001366f6
    void RecordBuildAttempt();                             // @ 0x100137018
    void RecordGrowthAttempt();                            // @ 0x1001372b4
    int ListPossibleLotConfigurations();                   // @ 0x100137588
    int ScoreCandidateLots();                              // @ 0x100138296
    int FindExistingCapacity(CandidateLot* candidate);     // @ 0x1001390aa
    int IsLotConfigurationSuitable(cSC4LotConfiguration* configuration);  // @ 0x10013a43c
    CandidateLot* NewCandidateLot();                       // @ 0x100138eae (static pool)
    void DeleteCandidateLot(CandidateLot* candidate);      // @ 0x1001392b6 (static pool)
    static bool CompareCandidateLots(const CandidateLot* a, const CandidateLot* b);   // @ 0x10013b64e
    void GetBoundingRect(CandidateLot* candidate);         // @ 0x10013b130
    void FreeBuildAttempt(BuildAttemptRecord& record);     // @ 0x10013b6bc
    void FreeGrowthAttempt(GrowthAttemptRecord& record);   // @ 0x10013b77c
    const GrowthRecord* GetLastSuccessfulBuild(int x, int z,
                                               const std::list<std::uint32_t>& developerTypes) const;  // @ 0x10013b9d4
    const GrowthAttemptRecord* GetLastFailedBuild(int x, int z,
                                                  std::uint32_t developerType) const;  // @ 0x10013baf2

    int ListCandidateLots_Existing();                      // @ 0x1001377a0
    int Build(CandidateLot* candidate);                    // @ 0x1001388c4
    int IsLotSuitable(cSC4Lot* lot, bool useCache);        // @ 0x100138b10
    int PickBuilding(CandidateLot* candidate);             // @ 0x1001392fa
    void FindNetworks(CandidateLot* candidate);            // @ 0x10013a5f2
    int IsLotConfigurationCompatible(cSC4LotConfiguration* configuration, CandidateLot* candidate,
                                     bool needsRoad);      // @ 0x10013aa5c
    static bool TestForCornerLot(cSC4LotConfiguration* configuration, CandidateLot* candidate);  // @ 0x10013b318
    int IsBuildingCompatible(std::uint32_t buildingType, CandidateLot* candidate);       // @ 0x10013b436

    int TakeOverAbandoned();                               // @ 0x1001369ae (Mac i386 0x000e9d9c)
    // PARTIAL: not recovered (see the header note and notes/ledger_pending/cSC4TractDeveloper.txt).
    int ListCandidateLots_Aggregation();                   // @ 0x100137b04
    int ListCandidateLots_Subdivision();                   // @ 0x100137f44
    int BuildFarm(CandidateLot* candidate);                // @ 0x10013845e
    void PerformAggregation(CandidateLot* candidate, cSC4Lot*& out);   // @ 0x100139dd2
    void PerformSubdivision(CandidateLot* candidate);      // @ 0x10013a04e
    bool IsAggregationLegal(CandidateLot* candidate);      // @ 0x10013ae4a

    // Accessors the test and the neighbours use.
    bool IsInitialized() const { return initialized_; }
    std::uint32_t RefCount() const { return refCount_; }
    const GrowthAttemptRecord& CurrentAttempt() const { return currentAttempt_; }
    GrowthAttemptRecord& CurrentAttempt() { return currentAttempt_; }
    const std::map<std::uint32_t, std::map<std::pair<int, int>, GrowthRecord>>& GrowthDatabase() const {
        return growthDatabase_;
    }
    const std::list<BuildAttemptRecord>& BuildAttempts() const { return currentAttempt_.attempts; }  // +0x220
    const std::vector<CandidateLot*>& Candidates() const { return candidates_; }
    std::vector<CandidateLot*>& Candidates() { return candidates_; }
    // The +0x10c update Build performs after a successful construction (0x1001388c4).
    void AccumulateBuiltCapacity(std::int32_t capacity) { builtCount_ += capacity; }
    std::int32_t BuiltCapacity() const { return builtCount_; }
    bool SecondAttempt() const { return attempted_ != 0; }
    void SetSecondAttempt(bool second) { attempted_ = second ? 1 : 0; }   // test hook: Grow leaves its last pass's value
    const std::set<cSC4Lot*>& ExistingLots() const { return existingLots_; }
    const std::set<cSC4LotConfiguration*>& SuitableConfigurations() const { return configurations_; }
    const std::vector<std::vector<std::uint32_t>>& Histograms() const { return currentAttempt_.histograms; }
    std::vector<std::vector<std::uint32_t>>& Histograms() { return currentAttempt_.histograms; }
    std::size_t CandidatePoolSize() const;
    static void ClearCandidatePool();          // test hook: frees the class-wide pool
    const cISC4LotConfigurationFilter* CachedFilter(int wealth, int purpose) const;
    std::size_t CachedFilterCount() const { return filterCache_.size(); }

    // Original field offsets, asserted by the test.
    static constexpr std::uint32_t kOffsetCity = 0x28;
    static constexpr std::uint32_t kOffsetBuildingDevelopment = 0x30;
    static constexpr std::uint32_t kOffsetLotConfigurationManager = 0x40;
    static constexpr std::uint32_t kOffsetLotDeveloper = 0x48;
    static constexpr std::uint32_t kOffsetLotManager = 0x50;
    static constexpr std::uint32_t kOffsetExistingLots = 0x118;
    static constexpr std::uint32_t kOffsetConfigurations = 0x130;
    static constexpr std::uint32_t kOffsetCandidates = 0x148;
    static constexpr std::uint32_t kOffsetActiveStyles = 0x1a0;
    static constexpr std::uint32_t kOffsetFilterCache = 0x1c8;
    static constexpr std::uint32_t kOffsetCurrentAttempt = 0x1e0;
    static constexpr std::uint32_t kOffsetGrowthDatabase = 0x250;
    static constexpr std::uint32_t kClassSize = 0x268;

private:
    // The cLotSizesFilter the pass hands to the lot configuration manager (0x10013c4ca).
    class LotSizesFilter;

    void AcquireServices();
    void ReleaseServices();

    // +0x18
    bool initialized_ = false;
    std::uint32_t refCount_ = 0;                       // +0x1c
    cIGZMessageServer2* messageServer_ = nullptr;      // +0x20
    ITractCityApi* city_ = nullptr;                    // +0x28
    ITractBuildingDevelopmentApi* buildingDevelopment_ = nullptr;   // +0x30
    ITractDeveloperService* landValue_ = nullptr;      // +0x38
    cSC4LotConfigurationManager* lotConfigurationManager_ = nullptr;  // +0x40
    ITractLotDeveloperApi* lotDeveloper_ = nullptr;    // +0x48
    ITractLotManagerApi* lotManager_ = nullptr;        // +0x50
    ITractDeveloperService* plumbing_ = nullptr;       // +0x58
    ITractDeveloperService* power_ = nullptr;          // +0x60
    ITractDeveloperService* traffic_ = nullptr;        // +0x68
    ITractZoneManagerApi* zoneManager_ = nullptr;      // +0x70
    [[maybe_unused]] std::int32_t reserved74_ = 0;     // +0x74

    float capacityFactor_ = 1.5f;                      // +0x78
    float declineFactor_ = 0.0f;                       // +0x7c
    float buildBias_ = 1.2f;                           // +0x80
    float capacityFactor2_ = 1.5f;                     // +0x84
    float existingCapacityWeight_ = 1.0f;              // +0x88
    float abandonedCapacityWeight_ = 0.2f;             // +0x8c
    float aggregationPenalty_ = 10.0f;                 // +0x90
    float subdivisionPenalty_ = 5.0f;                  // +0x94
    float populationJitter_ = 0.2f;                    // +0x98
    float declineJitterLow_ = 0.01f;                   // +0x9c
    float declineJitterHigh_ = 0.1f;                   // +0xa0
    bool allowTakeOver_ = true;                        // +0xa4
    std::vector<std::uint32_t> tunableStyles_;         // +0xa8
    std::int32_t yearsBetweenStyleChanges_ = 0;        // +0xc0
    std::uint32_t reservedC4_ = 0;                     // +0xc4

    IGrowthDeveloperApi* growthDeveloper_ = nullptr;   // +0xc8
    std::int32_t tractX_ = 0;                          // +0xd0
    std::int32_t tractZ_ = 0;                          // +0xd4
    std::uint8_t profileFlag_ = 0;                     // +0xd8
    ConstructionProfile construction_;                 // +0xdc
    std::uint8_t reservedF0_[0x10] = {};               // +0xf0

    std::int32_t capacity_ = 0;                        // +0x100
    std::int32_t maximumBuildAttempts_ = 0;            // +0x104
    std::int32_t secondAttemptLimit_ = 0;              // +0x108
    std::int32_t builtCount_ = 0;                      // +0x10c
    std::int32_t reserved110_ = 0;                     // +0x110
    std::uint8_t attempted_ = 0;                       // +0x114
    std::uint8_t reserved115_[3] = {};                 // +0x115
    std::set<cSC4Lot*> existingLots_;                  // +0x118
    std::set<cSC4LotConfiguration*> configurations_;   // +0x130
    std::vector<CandidateLot*> candidates_;            // +0x148
    cSC4LotConfiguration* configuration_ = nullptr;    // +0x160 the configuration PickBuilding chose
    std::uint32_t buildingType_ = 0;                   // +0x168 the building type PickBuilding chose
    std::int32_t outcome0_ = 0;                        // +0x16c
    std::int32_t outcome1_ = 0;                        // +0x170 PickBuilding's minimum capacity
    std::int32_t outcome2_ = 0;                        // +0x174 PickBuilding's maximum capacity
    std::list<int>* outcomeList_ = nullptr;            // +0x178 borrowed
    std::uint8_t noGrowth_ = 0;                        // +0x180
    std::uint8_t reserved181_[7] = {};                 // +0x181
    std::map<cSC4Lot*, int> lotOutcomes_;              // +0x188
    std::vector<std::uint32_t> activeStyles_;          // +0x1a0
    std::int32_t styleIndex_ = 0;                      // +0x1b8
    std::int32_t styleYears_ = 0;                      // +0x1bc
    std::uint8_t singleStyle_ = 1;                     // +0x1c0 (IsUsingAllStylesAtOnce returns !this)
    std::uint8_t reserved1c1_[7] = {};                 // +0x1c1
    std::map<std::pair<int, int>, cISC4LotConfigurationFilter*> filterCache_;   // +0x1c8
    // +0x1e0: its attempts list is +0x220 and its histograms vector +0x238 (the record member the
    // original names mAttempt and copies into the growth database).
    GrowthAttemptRecord currentAttempt_;               // +0x1e0
    std::map<std::uint32_t, std::map<std::pair<int, int>, GrowthRecord>> growthDatabase_;  // +0x250
};

}  // namespace sc4
