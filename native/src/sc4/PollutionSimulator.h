// cSC4PollutionSimulator — air / water / garbage / radiation pollution grids and the city's garbage economy
// (117 functions, 0x10009469e..0x10009afde, class id 0x8990c065, interfaces 0x452294aa message target,
// 0x89c498be, 0xe4fda3d4 serializable). This port covers the value grids and their accessors, the tunables
// loaded from the simulator exemplar, the per-building capacity helpers, the garbage disposal priority chain
// (eatGarbage), the recycling-centre effect and the end-of-month roll-over. The daily diffusion, monthly
// budget and visual-effect passes need the traffic / building / effects simulators and stay unrecovered.
//
// Original layout used here: +0x78 four per-type value grids (tract size 2), +0x98 four per-type "source" grids
// (tract size 2, garbage 1), +0x90 is the value grid of type 3 (radiation), lists +0xc0 water treatment,
// +0xd8 incinerators, +0xf0 waste-to-energy plants, +0x108 recycling centres, counts at +0xd0/+0xe8/+0x100/
// +0x118, averages +0x20c/+0x210/+0x234, month accumulators +0x214.., recycling effect +0x240, garbage
// counters +0x260..+0x2cc, efficiency +0x348, supply +0x34c, capacity +0x350.
#pragma once
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <vector>

#include "gz/cIGZSerializable.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "sc4/RetainedReference.h"
#include "sc4/RandomIterator.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/cSC4ResponseCurve.h"
#include "sc4/cSC4SimGrid.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4PollutionSimulator = 0x8990c065;
constexpr std::uint32_t kPollutionRequesterSatisfactionId = 0xe9a4300b;
constexpr std::uint32_t kPollutionGridInstanceValueBase = 0x89d5bbf0;     // + type (-0x762a4410)
constexpr std::uint32_t kPollutionGridInstanceSourceBase = 0x49d5bc40;    // + type

enum PollutionType : std::uint32_t { kPollutionAir = 0, kPollutionWater = 1, kPollutionGarbage = 2, kPollutionRadiation = 3 };

// exemplar {type 0x6534284a, group 0xe7e2c2db, instance 0xc911e35b}
constexpr std::uint32_t kPollutionPropTrafficAirFactor = 0x69501944;
constexpr std::uint32_t kPollutionPropMaxGarbageForUI = 0x49134bd4;
constexpr std::uint32_t kPollutionPropMaxAirForUI = 0x49134bdd;
constexpr std::uint32_t kPollutionPropMaxWaterForUI = 0x09134be2;
constexpr std::uint32_t kPollutionPropMaxRadiationForUI = 0x89135044;
constexpr std::uint32_t kPollutionPropMaxAirDelta = 0x6a406201;
constexpr std::uint32_t kPollutionPropMaxWaterDelta = 0x8a40620b;
constexpr std::uint32_t kPollutionPropGarbageThreshold = 0x0913519b;
constexpr std::uint32_t kPollutionPropAirThreshold = 0xa91351a6;
constexpr std::uint32_t kPollutionPropWaterThreshold = 0x091351ae;
constexpr std::uint32_t kPollutionPropRadiationThreshold = 0x291351b5;
constexpr std::uint32_t kPollutionPropAbductionAmount = 0x09135479;
constexpr std::uint32_t kPollutionPropGarbageScaling = 0x09135482;
constexpr std::uint32_t kPollutionPropFloat2e0 = 0xaa1f8ab7;
constexpr std::uint32_t kPollutionPropMaxGarbageOnValve = 0x4a652679;
constexpr std::uint32_t kPollutionPropLandfillTileCapacity = 0x4a65267a;

// occupant property ids
constexpr std::uint32_t kPollutionOccGarbageCapacity = 0x48ee7a44;
constexpr std::uint32_t kPollutionOccRecyclingMax = 0x88ed1059;
constexpr std::uint32_t kPollutionOccIncinerator = 0xa8ebcf0c;
constexpr std::uint32_t kPollutionOccWasteToEnergy = 0x48ebd0b1;
constexpr std::uint32_t kPollutionOccWaterTreatment = 0xc8ebd131;
constexpr std::uint32_t kPollutionOccWaterTreatmentUsed = 0x28ecf510;
constexpr std::uint32_t kPollutionOccIncineratorUsed = 0xe8ecf641;
constexpr std::uint32_t kPollutionOccWasteToEnergyUsed = 0x88ecf76c;
constexpr std::uint32_t kPollutionOccAge = 0xc910b4cf;
constexpr std::uint32_t kPollutionOccAgeCurve = 0x8a1e07ee;
constexpr std::uint32_t kPollutionOccMaintenanceBase = 0x2a58d350;
constexpr std::uint32_t kPollutionOccMaintenanceCurve = 0x0911e2e7;
constexpr std::uint32_t kPollutionOccRecyclingFactor = 0x68ed1736;
constexpr std::uint32_t kPollutionOccWasteToEnergyRate = 0x08fd1613;
constexpr std::uint32_t kPollutionOccWasteToEnergyProduced = 0x27812852;
constexpr std::uint32_t kPollutionPropFundingEfficiencyCurve = 0x6911e2df;

struct PollutionTunables {
    float trafficAirFactor = 8.0f;
    std::uint32_t maxGarbageForUI = 27000, maxAirForUI = 27000, maxWaterForUI = 32000, maxRadiationForUI = 32000;
    std::uint32_t maxAirDelta = 50, maxWaterDelta = 50;
    std::uint32_t airThreshold = 16000, waterThreshold = 16000, garbageThreshold = 16000, radiationThreshold = 1;
    std::uint32_t abductionAmount = 5000;
    float garbageScalingFactor = 0.125f;          // 0x3e000000
    std::uint32_t landfillTileCapacity = 0x7fff;
    std::uint32_t maxGarbageOnValve = 0x9ffec;
    float value2e0 = 0.0f;
};

class IPollutionOccupant {
public:
    virtual ~IPollutionOccupant() = default;
    virtual cISCPropertyHolder* PropertyHolder() = 0;                  // vslot 0x30
    virtual void Position(float xyz[3]) = 0;                           // vslot 0x40
};

// City objects the occupant passes reach (lot manager, budget simulator). Optional: set with SetServices.
class IPollutionLot {
public:
    virtual ~IPollutionLot() = default;
    virtual void SetState(std::uint32_t state) = 0;                    // lot vslot 0x120 (2 = warning, 4 = abandon)
};
struct PollutionBudgetRecord {                                         // the 32-byte record budget sim vslot 0x198 fills
    std::uint8_t opaque[16] = {};
    std::int64_t cost = 0;                                             // +0x10 the cost currently booked
    std::int32_t id = 0;                                               // +0x18 (passed back to SetBudgetLineItem)
    std::int32_t zero = 0;                                             // +0x1c
};
class IPollutionDepartment {
public:
    virtual ~IPollutionDepartment() = default;
    virtual void AddCost(std::int64_t delta) = 0;                      // department vslot 0x68
};
class IPollutionServices {
public:
    virtual ~IPollutionServices() = default;
    virtual IPollutionLot* LotAtPosition(const float xyz[3]) = 0;      // spCity vslot 0x2f8 -> spLotManager vslot 0x28(x, z, 0)
    virtual bool FindBudgetLineItem(cISCPropertyHolder* holder, std::uint32_t id, PollutionBudgetRecord& record) = 0;   // spBudgetSim vslot 0x198
    virtual IPollutionDepartment* DepartmentFor(const PollutionBudgetRecord& record) = 0;                              // spBudgetSim vslot 0x1c8
    virtual void SetBudgetLineItem(cISCPropertyHolder* holder, std::int32_t id, std::int64_t cost) = 0;               // spBudgetSim vslot 0x1a8
};

// A garbage deal with a neighbour (cISC4NeighborDeal): its monthly amount and how much of it was used so far.
class IPollutionDeal {
public:
    virtual ~IPollutionDeal() = default;
    virtual int Amount() = 0;                                          // vslot 0x28
    virtual int Used() = 0;                                            // vslot 0x50
    virtual void SetUsed(int used) = 0;                                // vslot 0x54
};
// An occupant a 0x99ef1142 / 0x99ef1143 message names, as DoOccupantChanged reads it: its property holder, the city cells its
// bounding box covers (spCity vslot 0x17c of both corners) and its position.
struct PollutionOccupantFootprint {
    IPollutionOccupant* occupant = nullptr;                            // stable for the occupant's life (the lists keep it)
    cISCPropertyHolder* holder = nullptr;
    int cellX0 = 0, cellZ0 = 0, cellX1 = -1, cellZ1 = -1;
    float position[3] = {};
};

class IPollutionHost {
public:
    virtual ~IPollutionHost() = default;
    // Init's services: the simulator's agents (SL::spSimulator vslot 0x4c), notifications (SL::spMS2), the tuning exemplar,
    // the history series length (history warehouse vslot 0x68).
    virtual void AddAgent(cIGZMessageTarget2* target, int interval, const char* name, std::uint32_t flags) = 0;
    virtual void RemoveAgent(cIGZMessageTarget2* target) = 0;
    virtual void AddNotification(cIGZMessageTarget2* target, std::uint32_t message) = 0;
    virtual void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t message) = 0;
    virtual const cISCPropertyHolder* SimulatorExemplar() = 0;         // {0x6534284a, 0xe7e2c2db, 0xc911e35b}
    virtual void CreateHistory(std::uint32_t historyId, std::uint32_t length) = 0;
    // The daily pass: ordinance factors (ordinance simulator vslot 0x64), the zone of a cell (zone manager vslot 0x30), the traffic
    // simulator's air contribution at a cell (its grid vslot 0x1c), the neighbour garbage deals (neighbours simulator vslot 0x74).
    virtual float OrdinanceFactor(std::uint32_t property, std::uint32_t index) = 0;
    virtual int ZoneTypeAt(int cellX, int cellZ) = 0;
    virtual std::uint8_t TrafficAirAt(int cellX, int cellZ) = 0;
    virtual void GarbageDeals(std::uint32_t kind, std::vector<IPollutionDeal*>& out) = 0;   // 0x4773ffe5 import, 0x4773ffe4 export
    virtual void SetSanitationLineItemCost(int item, std::int64_t cost) = 0;               // the sanitation department's line item
    virtual bool OccupantFootprint(void* occupant, PollutionOccupantFootprint& out) = 0;    // false: not a city occupant
    virtual bool PowerAvailable() = 0;                                 // power simulator vslot 0xf8
    virtual bool OccupantPowered(IPollutionOccupant* occupant) = 0;    // this+0x60 vslot 0xf8
    virtual float SanitationFunding() = 0;                             // budget department +0x50 -> float
    virtual void LandfillRange(int& low, int& high) = 0;               // landfill developer vslot 0x50
    virtual int LandfillTransportLoad() = 0;                           // landfill developer vslot 0x58
    virtual void PostMessage(std::uint32_t message, std::uint32_t arg) = 0;   // SC4PostMessage2
    virtual void AddHistory(std::uint32_t historyId, std::int64_t value) = 0; // history warehouse vslot 0x60
    virtual float RecyclingCapacitySlots() = 0;                        // this+0x50 vslot 0x80
};

class cSC4PollutionSimulator final : public cIGZSerializable, public cIGZMessageTarget2 {
public:
    explicit cSC4PollutionSimulator(IPollutionHost* host);
    ~cSC4PollutionSimulator() = default;

    // cIGZUnknown / cIGZSerializable identity (one shared count at +0x24; the original answers +0 for
    // iid 1 / 0x877181ec, +8 for the message target, +0x10 for 0x89c498be and +0x18 for the serializable).
    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    bool Write(cIGZOStream& stream) const override;
    bool ReadCityRecord(cIGZIStream& stream);   // @ 0x100096588
    bool WriteCityRecord(cIGZOStream& stream) const;   // the city-file layout ReadCityRecord reads
    bool Read(cIGZIStream& stream) override;
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4PollutionSimulator; }

    // the simulation (Init @ 0x0006f836, Shutdown, DoMessage @ 0x0006e7d4, the agents' daily / monthly passes)
    bool Init();
    bool Shutdown();
    bool DoMessage(cIGZMessage2* message) override;
    void SimulationBegin();
    void DoDailySimulation();
    void DoMonthlySimulation();
    void DoOccupantChanged(void* occupant, bool added);
    static constexpr std::uint32_t kUpdatePeriod = 30;                  // mUpdatePeriod (StaticInit): days for a whole-city sweep

    // tunables + curves (ReadSimulatorTunableValues)
    void ReadSimulatorTunableValues(const cISCPropertyHolder* exemplar);
    const PollutionTunables& Tunables() const { return tunables_; }
    cSC4ResponseCurve& PropRequestCurve() { return propRequestCurve_; }
    cSC4ResponseCurve& GarbageTransportCapacityCurve() { return garbageTransportCapacity_; }
    cSC4ResponseCurve& AgeCurve() { return ageCurve_; }                 // +0x1c8
    cSC4ResponseCurve& EfficiencyCurve() { return efficiencyCurve_; }   // +0x1e8
    cSC4ResponseCurve& FundingEfficiencyCurve() { return fundingEfficiencyCurve_; }   // property 0x6911e2df

    // grids
    void CreateGrids();
    cSC4SimGrid<std::int16_t>* GetPollutionGrid(PollutionType type) const;
    std::int16_t GetPollutionValue(PollutionType type, int x, int z) const;
    bool IsRadioactive(unsigned x, unsigned z) const;
    void GetAirValue(unsigned x, unsigned z, int& out) const;
    void GetWaterValue(unsigned x, unsigned z, int& out) const;
    void GetGarbageValue(unsigned x, unsigned z, int& out) const;
    bool IsAirPolluted(unsigned x, unsigned z) const;
    bool IsWaterPolluted(unsigned x, unsigned z) const;
    bool IsGarbageAccumulated(unsigned x, unsigned z) const;
    void SetIsRadioactive(unsigned x, unsigned z, bool on);
    void SetAirValue(unsigned x, unsigned z, int value);
    void SetWaterValue(unsigned x, unsigned z, int value);
    void SetGarbageValue(unsigned x, unsigned z, int value);
    bool IsActiveLandfill(unsigned x, unsigned z) const;
    void SetIsActiveLandfill(unsigned x, unsigned z, bool on, std::uint32_t zoneTypeAtCell);

    // summary accessors
    bool IsGarbageUncollected() const { return garbageUncollected_; }
    std::uint32_t GetTotalGarbageUncollected() const { return totalGarbageUncollected_; }
    std::int32_t GetAverageAirValue() const { return averageAir_; }
    std::int32_t GetAverageWaterValue() const { return averageWater_; }
    std::int16_t GetAverageGarbageValue() const { return averageGarbage_; }
    std::uint32_t GetMonthlyIncineratorCapacity() const { return monthlyIncineratorCapacity_; }
    std::uint32_t GetMonthlyWasteToEnergyCapacity() const { return monthlyWasteToEnergyCapacity_; }
    float GetRecyclingCenterEffect() const { return recyclingEffect_; }
    std::int32_t GetBuildingGarbageCapacity(cISCPropertyHolder* occupant) const;
    float GetGarbageScalingFactor() const { return tunables_.garbageScalingFactor; }
    void GarbagePutInLandfill(int amount) { landfilledThisMonth_ += amount; }
    void AddToGarbageSupply(int amount);
    int GetGarbageSupply();
    float GetEfficiency() const { return efficiency_; }
    void SetEfficiency(float value) { efficiency_ = value; }
    std::int64_t GetGarbageProducedPrevMonth() const { return producedPrev_; }
    std::int64_t GetGarbageImportedPrevMonth() const { return importedPrev_; }
    std::int64_t GetGarbageExportedPrevMonth() const { return exportedPrev_; }
    std::int64_t GetGarbageRecycledPrevMonth() const { return static_cast<std::int64_t>(recycledPrev_); }
    std::int64_t GetGarbageIncineratedPrevMonth() const { return incineratedPrev_; }
    std::int64_t GetGarbageConvertedToEnergyPrevMonth() const { return convertedPrev_; }
    std::int64_t GetGarbageLandfilledPrevMonth() const { return landfilledPrev_; }
    std::int32_t GetGarbageAbductedPrevMonth() const { return abductedPrev_; }
    int GetIncineratorCount() const { return static_cast<int>(incinerators_.size()) ; }
    int GetGarbageCapacity() const { return garbageCapacity_; }
    void SetGarbageCapacity(int value) { garbageCapacity_ = value; }
    std::uint32_t GetMaxAirPollutionLevelForUI() const { return tunables_.maxAirForUI; }
    std::uint32_t GetMaxWaterPollutionLevelForUI() const { return tunables_.maxWaterForUI; }
    std::uint32_t GetMaxGarbagePollutionLevelForUI() const { return tunables_.maxGarbageForUI; }
    std::uint32_t GetMaxRadiationPollutionLevelForUI() const { return tunables_.maxRadiationForUI; }
    std::uint32_t GetRequesterSatisfactionId() const { return kPollutionRequesterSatisfactionId; }
    bool UsesCustomPropSelector() const { return false; }
    int GetRequestedShare(int x0, int z0, int x1, int z1) const;

    // worst tracts (x, z, value)
    struct WorstTract { std::uint16_t x = 0, z = 0; std::int32_t value = 0; };
    WorstTract worstAir, worstWater, worstGarbage;
    void GetWorstAirTract(int& x, int& z, int& value) const { x = worstAir.x; z = worstAir.z; value = worstAir.value; }               // @ 0x10009ae46
    void GetWorstWaterTract(int& x, int& z, int& value) const { x = worstWater.x; z = worstWater.z; value = worstWater.value; }      // @ 0x10009ae66
    void GetWorstGarbageTract(int& x, int& z, int& value) const { x = worstGarbage.x; z = worstGarbage.z; value = worstGarbage.value; }   // @ 0x10009ae86
    void SetServices(IPollutionServices* services) { services_ = services; }
    // The pollution visual effect spawner (0x1000986d8, particle systems per 8x8 block) is not recovered; AddRadiationToCity
    // calls this hook where the original calls it.
    void SetVisualEffectUpdate(std::function<void()> hook) { visualEffectUpdate_ = std::move(hook); }
    void AgeOccupantList(std::list<IPollutionOccupant*>& list, std::uint32_t usedProperty, std::uint32_t capacityProperty);   // @ 0x100099c88
    void SetOccupantListUsedCapacity(std::list<IPollutionOccupant*>& list, std::uint32_t capacityProperty,
                                     std::uint32_t usedProperty, std::uint32_t total, std::uint32_t& remaining,
                                     std::uint32_t count);                                                                // @ 0x10009a004
    void CalcMaintenanceCosts(std::list<IPollutionOccupant*>& list);                                                       // @ 0x10009a1fe
    void AddRadiationToCity(const float position[3], float amount, float radius);                                          // @ 0x10009aaf4
    // @ 0x10009aea6 (the original reads a global that the tunables load fills)
    std::uint32_t GetLandfillTileCapacity() const { return tunables_.landfillTileCapacity; }
    // @ 0x10009ac48: appends the incinerators, then the waste-to-energy plants, then the recycling centres.
    void GetGarbageBuildings(std::list<IPollutionOccupant*>& out) const {
        for (IPollutionOccupant* o : incinerators_) out.push_back(o);
        for (IPollutionOccupant* o : wasteToEnergy_) out.push_back(o);
        for (IPollutionOccupant* o : recyclingCenters_) out.push_back(o);
    }

    // occupant lists
    std::list<IPollutionOccupant*>& WaterTreatmentList() { return waterTreatment_; }
    std::list<IPollutionOccupant*>& IncineratorList() { return incinerators_; }
    std::list<IPollutionOccupant*>& WasteToEnergyList() { return wasteToEnergy_; }
    std::list<IPollutionOccupant*>& RecyclingList() { return recyclingCenters_; }
    static bool AddOrRemoveOccupant(std::list<IPollutionOccupant*>& list, IPollutionOccupant* occupant, bool add);
    int GetWaterTreatmentPlantCount() const { return static_cast<int>(waterTreatment_.size()); }
    int GetWasteToEnergyBuildingCount() const { return static_cast<int>(wasteToEnergy_.size()); }
    int GetRecyclingCenterCount() const { return static_cast<int>(recyclingCenters_.size()); }
    std::uint32_t recyclingCenterCountSnapshot = 0;                     // +0x208 (copy of the list size at Init)

    // per-occupant capacity helpers (all read the occupant's exemplar)
    void GetRecyclingCenterMaxCapacity(IPollutionOccupant* occupant, float& percentage) const;
    void GetRecyclingCenterCurrentCapacity(IPollutionOccupant* occupant, float& percentage) const;
    void GetMaxCapacity(std::uint32_t property, IPollutionOccupant* occupant, std::uint32_t& out) const;
    bool GetCurrentCapacity(std::uint32_t property, IPollutionOccupant* occupant, std::uint32_t& out) const;
    void GetIncineratorCurrentCapacity(IPollutionOccupant* occupant, std::uint32_t& out) const;
    void GetWasteToEnergyCurrentCapacity(IPollutionOccupant* occupant, std::uint32_t& out) const;
    void GetWaterTreatmentMaxCapacity(IPollutionOccupant* occupant, std::uint32_t& out) const;
    void GetWaterTreatmentCurrentCapacity(IPollutionOccupant* occupant, std::uint32_t& out) const;
    void GetWaterTreatmentUsedCapacity(IPollutionOccupant* occupant, std::uint32_t& out) const;
    bool GetCurrentOccupantCondition(IPollutionOccupant* occupant, float& percentage) const;
    bool GetCurrentOccupantEfficiency(IPollutionOccupant* occupant, float& percentage) const;
    bool GetUnusedCapacity(std::uint32_t maxProperty, std::uint32_t usedProperty, IPollutionOccupant* occupant,
                           std::uint32_t& out) const;
    void GetIncineratorUsedCapacity(IPollutionOccupant* occupant, std::uint32_t& out) const;
    void GetWasteToEnergyUsedCapacity(IPollutionOccupant* occupant, std::uint32_t& out) const;
    bool CalcOccupantMaintenanceCost(IPollutionOccupant* occupant, std::int64_t& out) const;
    void ResetOccupantListCapacityUsedProperty(std::list<IPollutionOccupant*>& list, std::uint32_t currentProperty,
                                               std::uint32_t savedProperty) const;
    void ComputeWasteToEnergyListCurrentCapacity();
    void CalculateRecyclingCenterEffect();

    // garbage disposal chain: returns the amount actually consumed
    float eatGarbage(float amount, std::uint32_t& wasteToEnergyBudget, std::uint32_t& incineratorBudget);
    void EndOfCycle();

    // monthly counters
    std::int64_t recycledThisMonth_ = 0;      // +0x260
    std::int64_t wasteToEnergyThisMonth_ = 0; // +0x270
    std::int64_t incineratedThisMonth_ = 0;   // +0x278
    std::int64_t landfilledThisMonth_ = 0;    // +0x280
    std::int32_t recyclingAllowance_ = 0;     // +0x2c0 garbage the recycling centres can take this month
    std::uint32_t abductedThisMonth_ = 0;     // +0x2c4
    std::int32_t abductedPrev_ = 0;           // +0x2c8
    std::int32_t activeLandfillCount_ = 0;    // +0x2cc
    // end-of-cycle accumulators (filled by the daily sweep)
    std::uint32_t airSum = 0, waterSum = 0;                             // +0x214, +0x218
    std::uint32_t periodValue21c[2] = {}, periodValue224[2] = {};       // +0x21c/+0x220 -> +0x224/+0x228 at the end
    std::uint32_t cellCount = 0;                                        // +0x22c
    std::uint32_t radiationCount = 0;                                   // +0x230
    void SetTotalGarbageUncollected(std::uint32_t total) { totalGarbageUncollected_ = total; }
    bool dirtyRadiation = false, dirtyAir = false, dirtyWater = false, dirtyGarbage = false;   // +0x328..+0x32b

private:
    IPollutionHost* host_;
    std::uint32_t refCount_ = 0;                                        // +0x24
    PollutionTunables tunables_;
    SimGridPtr<std::int16_t> valueGrids_[4];          // +0x78
    // what a city file carries that the port does not model (written back as read)
    bool fromCityFile_ = false;
    bool loaded_ = false;                             // this[0x11]: Read filled the state, Init keeps it
    bool running_ = false;                            // this[0x10]: agents and notifications registered
    bool effectsShown_ = false;                       // +0x32c (this[0x21c]): the pollution view layer is on
    std::uint32_t cellX_ = 0, cellZ_ = 0, cellsPerDay_ = 0;
    std::int64_t file2d0_ = 0, sanitationCost_ = 0, producedThisMonth_ = 0, importedThisMonth_ = 0;
    RandomIterator walk_;
    cRetainedReference fileWaterGrid_;
    cRetainedReferences fileLists_[4];
    SimGridPtr<std::int16_t> sourceGrids_[4];         // +0x98
    cSC4ResponseCurve propRequestCurve_, garbageTransportCapacity_, ageCurve_, efficiencyCurve_, fundingEfficiencyCurve_;
    std::list<IPollutionOccupant*> waterTreatment_, incinerators_, wasteToEnergy_, recyclingCenters_;
    bool garbageUncollected_ = false;                                   // +0x244
    std::uint32_t totalGarbageUncollected_ = 0;                         // +0x248
    std::int32_t averageAir_ = 0, averageWater_ = 0;                    // +0x20c, +0x210
    std::int16_t averageGarbage_ = 0;                                   // +0x234
    std::uint32_t monthlyIncineratorCapacity_ = 0, monthlyWasteToEnergyCapacity_ = 0;   // +0x238, +0x23c
    float recyclingEffect_ = 0.0f;                                      // +0x240
    float efficiency_ = 1.0f;                                           // +0x348
    int garbageSupply_ = 0;                                             // +0x34c
    int garbageCapacity_ = 0;                                           // +0x350
    std::int64_t producedPrev_ = 0, importedPrev_ = 0, exportedPrev_ = 0;   // +0x288..+0x298
    double recyclingShare268_ = 0;                                      // +0x268 (read from the city record)
    IPollutionServices* services_ = nullptr;
    std::function<void()> visualEffectUpdate_;
    double recycledPrev_ = 0;                                           // +0x2a0
    std::int64_t incineratedPrev_ = 0, convertedPrev_ = 0, landfilledPrev_ = 0;   // +0x2a8..+0x2b8
};

}  // namespace sc4
