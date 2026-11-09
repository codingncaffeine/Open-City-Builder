// cSC4ViewInputControlQuery — the query / lot-inspection input tool of the 3D view (222 symbols,
// 0x1002c1dca..0x1002dc5a2, class id 0xc6bb8345 on the primary interface and 0x452294aa message target on the
// sub-object). The user hovers a cell or an occupant, the tool keeps the hovered occupant highlighted, gathers the
// text the tip shows and, on a left click, opens the query dialog (a script window) or the neighbour-connection
// dialog. Two flavours share the class (EQueryType): kQueryBuilding (0) inspects buildings, kQueryNetwork (1)
// inspects network pieces and draws their routes into a cSC4PathView.
//
// Object layout of the original (the offsets this port documents in its fields):
//   +0x00 cSC4ViewInputControl vptr            +0x40 message-target (cIGZMessageTarget2) vptr
//   +0x48 hovered occupant (cISC4Occupant*, referenced)   +0x50 SC4Rect<int> city-cell rect of the pick
//   +0x60 previous highlight value                        +0x68 callout box (cISC4WinCalloutBox*)
//   +0x70 / +0x78 minimised / expanded legend windows (network mode)
//   +0x80 cRZCallbackTimer (0x38 bytes; the "mouse tip" timer)
//   +0xb8 tip enabled   +0xbc/+0xc0/+0xc4 last pick position (x, y, z)   +0xc8 hover valid   +0xc9 mouse down
//   +0xcc EQueryType   +0xd0 tip timer period   +0xd4 show coordinates   +0xd8 = 3 (written, never read)
//   +0xe0 cSC4PathView* (network mode)
// Class statics: the query dialog window / its enum callback, and the three tip strings gsQueryString(1..3).
//
// Everything the class needs from an unrecovered neighbour goes through the seams below. Slot numbers are the
// original vtable offsets; where the original reached a global (SL::spPowerSim, ...) the comment names it.
//
// PARTIAL (see notes/ledger_pending/cSC4ViewInputControlQuery.txt): the tip/describe text builders
// (Get*SummaryInfo, GetBuildingOccupantTipInfo, GetBuildingOccupantTrafficInfo, GetNetworkOccupantTrafficInfo,
// DescribePick/DescribeCell/DescribeBuilding/DescribeDemand/DescribePopulation, WriteDetailedDebugInfo and the
// headline/trip-volume/path helpers) and the row handlers for the simulators that are not recovered yet (the RCI building rows and the air and water pollution volumes) are declared but not
// implemented. The fire, police/jail, park/occupant-cost and neighbour-connection rows — everything whose simulator
// (cSC4FireProtectionSimulator, cSC4PoliceSimulator, cSC4BudgetSimulator, cSC4NeighborsSimulator) is recovered — are
// ported, as are the school, hospital, library, landfill, incinerator, recycling, waste-to-energy, power plant, water building, landmark and transit station rows, the airport and seaport rows (usage, capacity, traffic meter, maintenance), the dialog enumeration callbacks (EnumFunc*) and the recovered control-id -> row handler table.
#pragma once
#include <cstdint>
#include <map>
#include <vector>

#include "gz/WinInterfaces.h"
#include "gz/WinProc.h"
#include "gz/cIGZMessage2.h"
#include "rz/cRZCallbackTimer.h"
#include "rz/cRZString.h"
#include "sc4/ISC4WinCalloutBox.h"
#include "sc4/Occupant.h"
#include "sc4/ViewInputControl.h"
#include "sc4/cSC4ResponseCurve.h"

namespace sc4 {

constexpr std::uint32_t kGZIID_cSC4ViewInputControlQuery = 0x452294aa;   // cIGZMessageTarget2 sub-object

// --- the query tool's seams ---------------------------------------------------------------------------------

// cISC4View3DWin as the query tool uses it. The two slots the base tool already uses (EndInput 0xd0, TopControl
// 0xc0) come from IViewInputView.
class IQueryView : public IViewInputView {
public:
    virtual cIGZWin* ViewWindow() = 0;                                                    // vslot 0x18
    virtual bool PickTerrain(int x, int y, float out[3], bool flag) = 0;                  // 0x120
    virtual bool ShowCoordinates() = 0;                                                   // 0x138
    // 0x140: SetTip(id, 0, title, text, 0)
    virtual void SetTip(std::uint32_t id, int unused, const cIGZString* title, const cIGZString* text, int flags) = 0;
    virtual void ClearTip(std::uint32_t id) = 0;                                          // 0x148
    // 0x188: the coverage-radius effect helper, then cSC4CoverageRadiusEffectHelper::RecalcCoverage.
    virtual void RecalcCoverage() = 0;
};

// cSC4OccupantFilter: the picking filter. The original builds one per query (a cSC4OccupantFilter subclass with the
// right virtual IsValid), hands it to the renderer and releases it afterwards.
class IOccupantFilter : public cIGZUnknown {
public:
    virtual bool IsValid(const cISC4Occupant* occupant) = 0;
};

// The renderer's pick result: the original keeps the returned object alive, asks it for the picked object
// (vslot 0x18) and queries that for the interface it wants.
class IPickResult : public cIGZUnknown {
public:
    virtual cIGZUnknown* GetObject() = 0;                                                 // vslot 0x18
};

// SL::spRenderer
class IQueryRenderer {
public:
    virtual ~IQueryRenderer() = default;
    virtual IOccupantFilter* CreateFilter(int kind) = 0;      // cSC4OccupantFilter subclass (vtables 0x100b05448..)
    virtual IPickResult* Pick(int x, int y, IOccupantFilter* filter) = 0;                 // vslot 0x200
    virtual void AddLayer(cIGZUnknown* layer) = 0;                                        // vslot 0x100
    virtual void RemoveLayer(cIGZUnknown* layer) = 0;                                     // vslot 0x108
};

// cSC4PathView (0x10077c6fc Shutdown, 0x10077cf3c ClearPaths).
class IQueryPathView : public cIGZUnknown {
public:
    virtual void ClearPaths() = 0;
    virtual bool ShutdownView() = 0;
};

// SL::spCity
class IQueryCity {
public:
    virtual ~IQueryCity() = default;
    // 0x2f8: terrain position -> city cell (the original: (float x, float z, this, int* cellX, int* cellZ)).
    virtual void PositionToCell(float x, float z, int& cellX, int& cellZ) = 0;
    virtual int CellCountX() = 0;                              // vslot 0x2e8
    virtual int CellCountZ() = 0;                              // vslot 0x2f0
    virtual void FollowAutomaton(cIGZUnknown* automaton) = 0;   // 0x158 -> automaton manager vslot 0x110
    virtual void SelectStoplightProp(cIGZUnknown* prop) = 0;    // 0x140 -> prop manager vslot 0x128
};

// cISC4Lot as the query reads it.
class IQueryLot : public cIGZUnknown {
public:
    virtual void GetOccupants(std::vector<cISC4Occupant*>& out) = 0;                  // vslot 0x188
    virtual void GetCellRect(SC4CellRect& rect) = 0;                                  // 0x70
    virtual int GetState() = 0;                                                       // 0x108 (4 = abandoned)
    virtual bool IsHistorical() = 0;                                                  // 0x158
    virtual void SetHistorical(bool on) = 0;                                          // 0x160
    virtual int GetAbandonReason() = 0;                                               // 0x168 (1..6)
    // The RCI rows (defaults answer "nothing"):
    virtual int GetOccupantWealth() { return 0; }                                     // 0x148
    virtual std::uint32_t GetBuildingType(bool flag) { (void)flag; return 0; }        // 0x170
    virtual std::uint16_t GetCapacity(int wealth) { (void)wealth; return 0; }         // 0x190
    virtual float GetOccupancy() { return 0.0f; }                                     // 0x1e8
    virtual void GetCenter(float* xyz) { xyz[0] = xyz[1] = xyz[2] = 0.0f; }           // 0x78
    virtual bool GetNeedsPower() { return false; }                                    // 0x208
    virtual bool GetHasPower() { return false; }                                      // 0x210
    virtual cISCPropertyHolder* AsPropertyHolder() { return nullptr; }                // 0x18
    // vslot 0xc0 GetBuilding, then the building's AsOccupant (vslot 0xc): the lot's building occupant (borrowed), or null.
    virtual cISC4Occupant* GetBuildingOccupant() { return nullptr; }
};

// The growth developer the rating rows read (cSC4GrowthDeveloper, recovered): the rating thresholds (vslot 0xf0 with a kind
// 0..6) and the response curves it keeps (+0x88[0..3] pollution, +0xd0 school, +0xd8 hospital, +0xe0 crime, +0xe8 trip length,
// +0xf0 traffic); a missing curve is null.
class IQueryGrowthDeveloper {
public:
    virtual ~IQueryGrowthDeveloper() = default;
    virtual void GetRatingThresholds(int kind, int& low, int& high) = 0;
    virtual const cSC4ResponseCurve* PollutionCurve(int index) = 0;
    virtual const cSC4ResponseCurve* SchoolCurve() = 0;
    virtual const cSC4ResponseCurve* HospitalCurve() = 0;
    virtual const cSC4ResponseCurve* CrimeCurve() = 0;
    virtual const cSC4ResponseCurve* TripLengthCurve() = 0;
    virtual const cSC4ResponseCurve* TrafficCurve() = 0;
};

// cISC4BuildingOccupant as SetOrGetName uses it (interface 0x87dda3a9): vslot 0x90 SetName.
class IQueryBuildingOccupant : public cIGZUnknown {
public:
    virtual void SetName(cIGZString& name) = 0;   // vslot 0x90
    virtual bool GetName(cRZString& name) = 0;    // 0x98
    virtual std::uint16_t GetProfileCapacity() { return 0; }   // 0x58 GetBuildingProfile() -> the profile's capacity (+8)
};

// SL::spAuraSim: the per-cell aura value (a signed byte) the mayor rating averages over a lot.
class IQueryAuraSim {
public:
    virtual ~IQueryAuraSim() = default;
    virtual std::int8_t GetAura(int cellX, int cellZ) = 0;   // vslot 0x28
    virtual std::int8_t GetMayorRating() { return 0; }       // vslot 0x40 (the goofy stat rows feed it to a curve)
};
// cISC4NetworkOccupant (iid 0xa821ef94) as the network tip reads it: vslot 0xb0 IsOfType(network type).
class IQueryNetworkOccupant : public cIGZUnknown {
public:
    virtual bool IsOfType(std::uint32_t networkType) const = 0;
};

// SL::spLandValueSim vslot 0x28 GetLandValueType(x, z): 0..3.
class IQueryLandValueSim {
public:
    virtual ~IQueryLandValueSim() = default;
    virtual int GetLandValueType(int cellX, int cellZ) { (void)cellX; (void)cellZ; return 0; }
};

// SL::spLotManager
class IQueryLotManager {
public:
    virtual ~IQueryLotManager() = default;
    virtual IQueryLot* GetLotForOccupant(cISC4Occupant* occupant) = 0;   // vslot 0x70
    virtual IQueryLot* GetLotAtCell(int cellX, int cellZ, int unused) = 0;   // vslot 0x28
};

// SL::spPowerSim / SL::spPlumbingSim booleans used by the "is powered" / "is watered" rows.
class IQueryPowerSim {
public:
    virtual ~IQueryPowerSim() = default;
    virtual bool IsPowered(cISC4Occupant* occupant) = 0;   // vslot 0xf8
    // The power plant rows (cSC4PowerSimulator, recovered). Defaults answer "nothing" so a host only overrides what it has.
    virtual void GetOccupantPowerProduced(cISC4Occupant*, std::uint32_t& capacity, std::uint32_t& used) { capacity = used = 0; }   // 0x108
    virtual void GetOccupantMaxPowerProduced(cISC4Occupant*, std::uint32_t& out) { out = 0; }                                   // 0x110
    virtual float GetCurrentPlantEfficiencyPercent(cISC4Occupant*) { return 0.0f; }                                              // 0x168
    virtual float GetCurrentPlantConditionPercent(cISC4Occupant*) { return 0.0f; }                                               // 0x170
    virtual bool GetMonthlyPowerGenerationCost(cISC4Occupant*, std::int64_t&) { return false; }                                  // 0x188
    virtual bool GetPowerPlantFundingPercentage(cISC4Occupant*, float&) { return false; }                                        // 0x190
    virtual void SetPowerPlantFundingPercentage(cISC4Occupant*, float) {}                                                        // 0x198
};

// SL::spPollutionSim (cSC4PollutionSimulator, recovered): garbage, recycling and incinerator figures.
class IQueryPollutionSim {
public:
    virtual ~IQueryPollutionSim() = default;
    virtual std::int64_t GarbageProducedPrevMonth() { return 0; }        // 0x118
    virtual std::int64_t GarbageRecycledPrevMonth() { return 0; }        // 0x130
    virtual std::int64_t GarbageLandfilledPrevMonth() { return 0; }      // 0x148
    virtual int RecyclingCenterCount() { return 0; }                     // 0x160
    virtual float GarbageScalingFactor() { return 0.0f; }                // 0x180
    virtual void RecyclingCenterMaxCapacity(cISC4Occupant*, float& out) { out = 0.0f; }      // 0x1b0
    virtual void RecyclingCenterCurrentCapacity(cISC4Occupant*, float& out) { out = 0.0f; }  // 0x1b8
    virtual bool CurrentOccupantEfficiency(cISC4Occupant*, float& out) { out = 0.0f; return false; }   // 0x1c8
    virtual void IncineratorUsedCapacity(cISC4Occupant*, std::uint32_t& out) { out = 0; }          // 0x1d0
    virtual void WasteToEnergyUsedCapacity(cISC4Occupant*, std::uint32_t& out) { out = 0; }        // 0x1d8
    virtual void IncineratorCurrentCapacity(cISC4Occupant*, std::uint32_t& out) { out = 0; }       // 0x1e0
};

// SL::spLandfillDeveloper (also SL::spCity slot 0x270).
class IQueryLandfillDeveloper {
public:
    virtual ~IQueryLandfillDeveloper() = default;
    virtual void GetLandfillUsage(std::uint32_t& usage, std::uint32_t& capacity) { usage = capacity = 0; }   // 0x50
    virtual std::int64_t GetCurrentBudget() { return 0; }                                                    // 0x40
};
class IQueryPlumbingSim {
public:
    virtual ~IQueryPlumbingSim() = default;
    virtual bool IsWatered(cISC4Occupant* occupant) = 0;   // vslot 0x48
    virtual int GetNumPipes() { return 0; }                // vslot 0x90
    virtual int GetNumDistressedPipes() { return 0; }      // vslot 0x98
    // The water building rows (cSC4PlumbingSimulator, recovered). Defaults answer "no data".
    // vslot 0xc0 GetWaterProducerInfo(occupant, current, maximum, efficiency, pumpWorking, used): a null out is skipped.
    virtual bool GetWaterProducerInfo(cISC4Occupant*, std::uint32_t* current, std::uint32_t* maximum, float* efficiency, bool* pumpWorking,
                                      std::uint32_t* used) { (void)current; (void)maximum; (void)efficiency; (void)pumpWorking; (void)used; return false; }
    virtual bool GetWaterProducerConditionPercent(cISC4Occupant*, float&) { return false; }       // vslot 0xc8
    virtual bool GetOccupantMonthlyUpkeepCost(cISC4Occupant*, std::int64_t&) { return false; }    // vslot 0xd0
};

// SL::spSimulator (the frame counter that gates the periodic dialog refresh) and SL::spResidentialSim.
class IQuerySimulator {
public:
    virtual ~IQuerySimulator() = default;
    virtual std::uint32_t FrameCount() = 0;                                     // 0x30 -> simulator vslot 0xd0
};
// What GetSchoolQueryData / GetHospitalQueryData (vslots 0xd0 / 0xd8) fill in for one school or hospital. The original record is a
// 0x50-byte tQueryData that starts with the building name; the rows only read these fields.
struct QueryEducationRecord {
    std::uint32_t count = 0;        // +0x38 students / patients / books borrowed
    float grade = 0.0f;             // +0x3c the meter value
    float staffFunding = 0.0f;      // +0x40 fraction
    std::int32_t staffCost = 0;     // +0x44 monthly cost at full funding
    float secondFunding = 0.0f;     // +0x48 buses (school) / ambulances (hospital)
    std::int32_t secondCost = 0;    // +0x4c
};
class IQueryResidentialSim {
public:
    virtual ~IQueryResidentialSim() = default;
    virtual void SetQueryCell(int cellX4, int cellZ4) = 0;                      // vslot 0x1d8
    virtual bool GetSchoolQueryData(cISC4Occupant*, QueryEducationRecord&) { return false; }     // vslot 0xd0
    virtual bool GetHospitalQueryData(cISC4Occupant*, QueryEducationRecord&) { return false; }   // vslot 0xd8
    // vslot 0xe0 EstimateCurrentOccupantCapacity: desks / beds / books in the collection / tickets in `first`, teachers / doctors in `second`.
    virtual void EstimateCurrentOccupantCapacity(cISC4Occupant*, std::uint32_t& first, std::uint32_t& second) { first = second = 0; }
    // vslot 0x98 SetOccupantFundingPercentages(occupant, staff, second, education): staff and bus/ambulance fractions.
    virtual void SetOccupantFundingPercentages(cISC4Occupant*, float, float, bool) {}
};

// SL::spFireProtectionSim (cSC4FireProtectionSimulator, recovered: FireProtectionSimulator.h). The row handlers only
// use these four station queries; the original addresses each through the simulator's own vslot.
class IQueryFireSim {
public:
    virtual ~IQueryFireSim() = default;
    virtual float StationEffectiveness(cISC4Occupant* station) = 0;                 // vslot 0x50 GetStationEfficiency
    // vslot 0x40: GetStationFunding(station, int* cost, float* fraction), the share returned through `fraction`.
    virtual bool StationFunding(cISC4Occupant* station, int* cost, float* fraction) = 0;
    virtual void SetStationFunding(cISC4Occupant* station, float fraction) = 0;     // vslot 0x48
    // vslot 0x38 of the QueryInterface(0xc9c00435) view (the dispatch interface): GetDispatchesAtStation.
    virtual bool DispatchInfo(cISC4Occupant* station, std::uint32_t* availability, std::uint32_t* baseCall,
                              std::uint32_t* count) = 0;
};

// SL::spPoliceSim (cSC4PoliceSimulator, recovered: PoliceSimulator.h).
class IQueryPoliceSim {
public:
    virtual ~IQueryPoliceSim() = default;
    virtual float StationEffectiveness(cISC4Occupant* station) = 0;                 // vslot 0xc8
    virtual int ArrestCount(cISC4Occupant* station) = 0;                            // vslot 0x158
    virtual int CrimeCount(cISC4Occupant* station) = 0;                             // vslot 0x160
    virtual bool StationFunding(cISC4Occupant* station, int* cost, float* fraction) = 0;   // vslot 0x38
    virtual void SetStationFunding(cISC4Occupant* station, float fraction) = 0;     // vslot 0x40
    virtual bool DispatchInfo(cISC4Occupant* station, std::uint32_t* availability, std::uint32_t* baseCall,
                              std::uint32_t* count) = 0;                            // QueryInterface(0xc9c00435) 0x38
    virtual float JailEfficiency() = 0;                                             // vslot 0x120 (no occupant)
    virtual int JailCapacity(cISC4Occupant* jail) = 0;                              // vslot 0x118
    virtual std::int64_t JailMaintenance(cISC4Occupant* jail) = 0;                  // vslot 0x128
};

// SL::spBudgetSim (cSC4BudgetSimulator, recovered: BudgetSimulator.h): the monthly cost of one budget value.
class IQueryBudgetSim {
public:
    virtual ~IQueryBudgetSim() = default;
    // vslot 0x198: (property holder, budget value id, out) -> out is a 0x18-byte record; its +0x10 qword is the cost.
    virtual bool OccupantValueCost(cISCPropertyHolder* holder, std::uint32_t value, std::int64_t& cost) = 0;
    // vslot 0x198 GetBudgetItemForPurpose(holder, purpose, item): the item's department (+0) and monthly cost (+0x10).
    struct Item { std::uint32_t department = 0; std::int64_t cost = 0; };
    virtual bool GetBudgetItemForPurpose(cISCPropertyHolder*, std::uint32_t purpose, Item& item) { (void)purpose; item = Item{}; return false; }
    // vslot 0xd8 GetDepartmentBudget(department) -> vslot 0x50: the department's funding fraction.
    virtual bool DepartmentFundingFraction(std::uint32_t department, float& fraction) { (void)department; fraction = 0.0f; return false; }
};

// cISC4RegionalCity as the neighbour-connection rows read it (RegionalCity.h: vslots 0x108 / 0x118).
class IQueryNeighborCity : public cIGZUnknown {
public:
    virtual void GetCityName(cIGZString& out) const = 0;    // vslot 0x108
    virtual void GetMayorName(cIGZString& out) const = 0;   // vslot 0x118
};

// SL::spNeighborsSim (cSC4NeighborsSimulator, recovered: NeighborsSimulator.h and
// SL::spTrafficSim (cSC4TrafficSimulator) for the congestion row.
class IQueryNeighborsSim {
public:
    virtual ~IQueryNeighborsSim() = default;
    virtual bool IsValidConnectionCell(int x, int z) = 0;   // static IsValidConnectionCell(x, z, nullptr)
    virtual bool FindConnection(std::uint32_t type, int x, int z) = 0;                 // vslot 0x58
    virtual IQueryNeighborCity* FindCityOnOtherSide(int x, int z) = 0;                 // vslot 0x60
};
class IQueryTrafficSim {
public:
    virtual ~IQueryTrafficSim() = default;
    // vslot 0x50 GetCongestionMap(), then the map's 0x38 cell getter (a byte).
    virtual std::uint8_t CongestionCell(int x, int z) = 0;
    // vslot 0x118 GetTransitSwitchQueryData(x, z, record): the station at a cell (cSC4TrafficSimulator::GetTransitSwitchQueryData).
    struct TransitData {
        std::int32_t served = 0, capacity = 0;   // +0, +4
        float percent = 0.0f;                    // +8 (not read by the rows)
        std::int32_t usage = 0, maintenance = 0; // +0xc, +0x10
    };
    virtual bool GetTransitSwitchQueryData(int x, int z, TransitData& out) { (void)x; (void)z; out = TransitData{}; return false; }
};

// SL::spCity slot 0x268 (the airport developer) and slot 0x260 (the seaport developer): the rows only ask a lot's usage and
// capacity (vslot 0x60) and its monthly maintenance cost (vslot 0x70).
class IQueryPortDeveloper {
public:
    virtual ~IQueryPortDeveloper() = default;
    virtual bool GetUsageAndCapacity(IQueryLot* lot, std::uint32_t& usage, std::uint32_t& capacity) = 0;   // vslot 0x60
    virtual std::int64_t GetMaintenanceCost(IQueryLot* lot) = 0;                                          // vslot 0x70
};

// Everything else: one adapter implements this for the game, a fake for the test.
class IQueryHost {
public:
    virtual ~IQueryHost() = default;
    virtual cIGZMessageServer2* MessageServer() = 0;          // SL::spMS2
    virtual IQueryView* QueryView() = 0;                      // SL::spView3D (same object as IViewInputHost::View3D)
    virtual IQueryRenderer* Renderer() = 0;                   // SL::spRenderer
    virtual IQueryCity* City() = 0;                           // SL::spCity
    virtual IQueryLotManager* LotManager() = 0;               // SL::spLotManager
    virtual IQueryPowerSim* PowerSim() = 0;                   // SL::spPowerSim
    virtual IQueryPlumbingSim* PlumbingSim() = 0;             // SL::spPlumbingSim
    virtual IQueryAuraSim* AuraSim() = 0;                     // SL::spAuraSim
    virtual IQuerySimulator* Simulator() = 0;                 // SL::spSimulator
    virtual IQueryResidentialSim* ResidentialSim() = 0;       // SL::spResidentialSim
    virtual IQueryFireSim* FireSim() = 0;                     // SL::spFireProtectionSim
    virtual IQueryPoliceSim* PoliceSim() = 0;                 // SL::spPoliceSim
    virtual IQueryBudgetSim* BudgetSim() = 0;                 // SL::spBudgetSim
    virtual IQueryNeighborsSim* NeighborsSim() = 0;           // SL::spNeighborsSim
    virtual IQueryTrafficSim* TrafficSim() = 0;               // SL::spTrafficSim
    // The per-cell maps the rating rows sample (each the simulator's map object, then its vslot 0x38 getter).
    virtual std::uint16_t PollutionMapValue(int map, int x, int z) { (void)map; (void)x; (void)z; return 0; }   // SL::spPollutionSim 0x28(map)
    virtual float SchoolGradeValue(int x, int z) { (void)x; (void)z; return 0.0f; }                            // SL::spResidentialSim 0x138
    virtual float HospitalGradeValue(int x, int z) { (void)x; (void)z; return 0.0f; }                          // 0x140
    virtual std::uint8_t CrimeValue(int x, int z) { (void)x; (void)z; return 0; }                              // SL::spCrimeSim 0x28
    virtual std::uint8_t TrafficValue(int x, int z) { (void)x; (void)z; return 0; }                            // SL::spTrafficSim 0x48
    virtual std::uint8_t TripLengthValue(int x, int z) { (void)x; (void)z; return 0; }                         // SL::spTrafficSim 0x68
    // FindBestDeveloper(lot) 0x1002cf948: the growth developer whose building type or cell suits the lot; null when none.
    virtual IQueryGrowthDeveloper* FindBestDeveloper(IQueryLot* lot) { (void)lot; return nullptr; }
    // SL::spBuildingDevelopmentSim vslot 0x138 GetBuildingProfile(type): the profile's wealth (+4).
    virtual std::uint32_t BuildingTypeWealth(std::uint32_t buildingType) { (void)buildingType; return 0; }
    virtual bool TerrainCellHasWater(int x, int z) { (void)x; (void)z; return false; }   // SL::spCity vslot 0x2a0 (terrain) -> vslot 0x110
    virtual std::uint32_t ZoneTypeAt(int x, int z) { (void)x; (void)z; return 0xff; }     // SL::spZoneManager vslot 0x60
    // SC4LoadExemplarString @ 0x100121132: the localized string the property names; false when it has none.
    virtual bool LoadExemplarString(cIGZString& out, cISCPropertyHolder* holder, std::uint32_t property) { (void)out; (void)holder; (void)property; return false; }
    virtual IQueryLandValueSim* LandValueSim() { return nullptr; }                // SL::spLandValueSim
    // SL::spRM vslot 0x18 GetResource(key, 0xa52160f5): the exemplar's property holder, null when the resource is missing.
    virtual cISCPropertyHolder* GetTuningProperties(const cGZPersistResourceKey& key) { (void)key; return nullptr; }
    virtual IQueryPollutionSim* PollutionSim() { return nullptr; }                // SL::spPollutionSim
    virtual IQueryLandfillDeveloper* LandfillDeveloper() { return nullptr; }       // SL::spLandfillDeveloper
    virtual IQueryPortDeveloper* AirportDeveloper() { return nullptr; }   // SL::spCity slot 0x268
    virtual IQueryPortDeveloper* SeaportDeveloper() { return nullptr; }   // SL::spCity slot 0x260
    // QueryInterface(0xa821ef94): the occupant as a network piece (one reference), or null when it is not one. The default
    // casts the interface the occupant answers; the game's network occupants do not implement IQueryNetworkOccupant, so the
    // game's host wraps them.
    virtual IQueryNetworkOccupant* NetworkOccupant(cISC4Occupant* occupant) {
        void* raw = nullptr;
        return occupant && occupant->QueryInterface(0xa821ef94, &raw) && raw ? static_cast<IQueryNetworkOccupant*>(raw) : nullptr;
    }
    // The query dialogs (DoQueryDialog / DoZoneBasedQueryDialog / DoNeighborConnectionQueryDialog):
    //   RZ::WM() vslot 0xa4 DoModalWin: runs the dialog until EndDialog; answers its result (0 = OK).
    virtual int RunModalDialog(cIGZWin* dialog) { (void)dialog; return -1; }
    //   RZ::WM() vslot 0x24 (0xf, window, result): ends the modal loop of `dialog` with `result`.
    virtual void EndModalDialog(cIGZWin* dialog, std::uint32_t result) { (void)dialog; (void)result; }
    //   SL::spWinLocationSaver vslot 0x24 (window, 1) before the modal loop / vslot 0x14 after it: place the dialog where
    //   it was last time (first time: centered) and remember where the user left it.
    virtual void RestoreDialogPlace(cIGZWin* dialog) { (void)dialog; }
    virtual void SaveDialogPlace(cIGZWin* dialog) { (void)dialog; }
    //   RZ::WM() vslot 0x5c DestroyWindow(dialog), then vslot 0x94 SetFocus(the 3D view).
    virtual void CloseDialog(cIGZWin* dialog) { (void)dialog; }
    virtual IQueryPathView* CreatePathView() = 0;             // new cSC4PathView + Init
    virtual cISC4WinCalloutBox* CreateCalloutBox() = 0;       // new cSC4WinCalloutBox (0x1003c5f5c)
    // SC4CreateWindowFromScript(instance, parent, id, iid, out) (@ 0x100039bf7)
    virtual cIGZWin* CreateWindowFromScript(std::uint32_t instance, cIGZWin* parent, std::uint32_t id) = 0;
    // new cSC4QueryDialogWinProc(this) + cIGZWinGen::SetWinProc (vslot 0x38) on the legend windows.
    virtual void AttachLegendWinProc(cIGZWin* window) = 0;
    virtual void DetachWinProc(cIGZWin* window) = 0;          // SetWinProc(nullptr)
    virtual bool IsKeyDown(std::uint32_t virtualKey) = 0;     // cIGZKeyboard vslot 0x18
    virtual bool IsDebugMode() = 0;                           // SL::spSC4 vslot 0xa8
    virtual void FormatMoneyFloat(double value, cRZString& out) = 0;   // RZ::LM()->0x128()->0x98
    // SC4LoadString(group, instance, out) @ 0x100120e10; the tool's strings all live in group 0xea5524eb.
    virtual void LoadString(std::uint32_t group, std::uint32_t instance, cIGZString& out) = 0;
};
IQueryHost* QueryHost();
void SetQueryHost(IQueryHost* host);

// cSC4QueryDialogWinProc (i386 0x00218e3c..): the query dialogs' window proc. OK / Cancel (0x287259f6 with id 0 or 1) end
// the modal loop with that id; a funding slider (0x887113a4) re-captions its money text (id + 0x100) from the slider's
// amount parameter, calls the row callback the slider carries and refreshes the rows its id list names.
class cSC4QueryDialogWinProc final : public gz::cIGZWinProc {
public:
    explicit cSC4QueryDialogWinProc(cIGZWin* dialog);   // the original takes the dialog's cIGZWinGen (one reference)
    ~cSC4QueryDialogWinProc();
    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override;
    bool SetOccupant(cISC4Occupant* occupant);                                         // 0x002080ee
    void EndDialog(std::uint32_t result);                                              // 0x0020a56e
    bool DoWinProcMessage(cIGZWin*, cGZMessage&) override { return false; }
    bool DoWinMsg(cIGZWin* window, std::uint32_t type, std::uint64_t p1, std::uint64_t p2, std::uint64_t p3) override;   // 0x002182da

private:
    std::uint32_t refs_ = 0;
    cIGZWin* dialog_ = nullptr;          // +0x10
    cISC4Occupant* occupant_ = nullptr;  // +0x14
};

// cSC4QueryMessageFilter (i386 0x0020c3d8): put on every row of a query dialog; on a draw message (0x16) a performance
// meter (a bitmap carrying parameter 0xea563ab0) paints its bar.
//   PARTIAL: the bar painting (DrawPerformanceMeter, i386 0x0020c23c) is not ported; the meters show their bitmap only.
class cSC4QueryMessageFilter final : public cIGZWinMessageFilter {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }    // the original's: static lifetime semantics
    std::uint32_t Release() override { return 0; }
    bool DoMessage(cIGZWin*, cGZMessage&) override { return false; }
};

class cSC4ViewInputControlQuery : public cSC4ViewInputControl, public cIGZMessageTarget2 {
public:
    enum EQueryType : std::int32_t { kQueryBuilding = 0, kQueryNetwork = 1 };

    // The extra data the neighbour-connection / airport / seaport / landfill rows need: the cell or connection, the
    // control-id offset added to the base id, and the connection type (cISC4NeighborsSimulator::ConnectionType).
    struct tCellInfo {
        std::int32_t x = 0;                // [0]
        std::int32_t z = 0;                // [1]
        std::int32_t idOffset = 0;         // [2]
        std::int32_t connectionType = 0;   // [3]
    };
    using tSetOrGetFn = bool (*)(cIGZWin*, std::uint32_t, bool, cISC4Occupant*);

    cSC4ViewInputControlQuery(EQueryType type, std::uint32_t tipPeriod);   // @ 0x1002c1ec2
    ~cSC4ViewInputControlQuery() override;                                 // @ 0x1002dc524

    // cIGZUnknown / cIGZMessageTarget2
    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x1002c2036
    std::uint32_t AddRef() override;                               // @ 0x1002c1fd6
    std::uint32_t Release() override;                              // @ 0x1002c1ff2
    bool DoMessage(cIGZMessage2* message) override;                // @ 0x1002c2a0e

    // non-virtual thunks into the message-target sub-object (0x1002c1fe4 / 0x1002c2012 / 0x1002c206a / 0x1002c5c8e)
    std::uint32_t AddRefFromMessageTarget();
    std::uint32_t ReleaseFromMessageTarget();
    bool QueryInterfaceFromMessageTarget(std::uint32_t iid, void** out);
    bool DoMessageFromMessageTarget(cIGZMessage2* message);

    bool Init() override;                                          // @ 0x1002c2078
    bool Shutdown() override;                                      // @ 0x1002c2642
    void Activate() override {}                                    // the original has no override
    bool ShouldStack() override;                                   // @ 0x1002d4ea8 (false)
    bool OnKeyDown(std::uint32_t key, std::uint32_t mods) override;   // @ 0x1002c5ca0 (false)
    bool OnMouseDownL(int x, int y, std::uint32_t mods) override;     // @ 0x1002d4342
    bool OnMouseUpL(int x, int y, std::uint32_t mods) override;       // @ 0x1002d4818
    bool OnMouseMove(int x, int y, std::uint32_t mods) override;      // @ 0x1002d4862
    bool OnMouseExit() override;                                      // @ 0x1002d4e2a

    // state machine
    void UpdatePaths();                                            // @ 0x1002c1dca
    void SetLegendMinimized(bool minimized);                       // @ 0x1002c1dfe
    void SetHighlightState(int state);                             // @ 0x1002c28f6
    void SetTip(const cIGZString* title, const cIGZString* text);  // @ 0x1002c5b68
    bool IsDebugQueryEnabled();                                    // @ 0x1002cf312
    void PlayOccupantQuerySound(cISC4Occupant* occupant);           // @ 0x1002cde0c

    // picking
    bool PickQueriableOccupant(int x, int y, cISC4Occupant** out, IOccupantFilter* filter);   // @ 0x1002cdd7e
    bool PickAutomaton(int x, int y, cIGZUnknown** out);                                      // @ 0x1002d45fc
    bool PickStoplightProp(int x, int y, cIGZUnknown** out);                                   // @ 0x1002d470a

    // Controls of the query dialogs. The original compiles all of these (and every row handler below) as functions
    // that take the dialog window explicitly — no query-tool `this` is passed (0x1002d5094 reads its first argument
    // as the window's vtable) — so they are static here too and can be used as cIGZWin::EnumChildFn handlers.
    static bool SetEditControlText(cIGZWin* win, std::uint32_t id, cIGZString& text);                  // @ 0x1002d4f40
    static bool GetEditControlText(cIGZWin* win, std::uint32_t id, cIGZString& text, std::uint32_t& length);  // @ 0x1002d4fb2
    static bool SetStaticControlText(cIGZWin* win, std::uint32_t id, const cIGZString& text);          // @ 0x1002d5094
    static bool SetStaticControlNumeric(cIGZWin* win, std::uint32_t id, std::uint32_t value);          // @ 0x1002d50dc
    static bool SetStaticControlNumericPercent(cIGZWin* win, std::uint32_t id, std::uint32_t value);   // @ 0x1002d5168
    static bool SetStaticControlSimoleons(cIGZWin* win, std::uint32_t id, std::int64_t value);         // @ 0x1002d51f4
    static bool SetStaticControlSimoleonsFloat(cIGZWin* win, std::uint32_t id, double value);          // @ 0x1002d527a
    static bool SetSliderControlValue(cIGZWin* win, std::uint32_t id, int minimum, int maximum, int value,
                                      std::int64_t money, cISC4Occupant* occupant, tSetOrGetFn callback,
                                      std::uint32_t* array, std::uint32_t arraySize);                  // @ 0x1002d54a0
    static bool GetSliderControlValue(cIGZWin* win, std::uint32_t id, int& value);                      // @ 0x1002d5758
    static bool SetPerformanceMeterValue(cIGZWin* win, std::uint32_t id, float value);                  // @ 0x1002d5926

    // rows of the query dialogs (only the ones that work without an unrecovered neighbour are implemented)
    static bool SetOrGetControlValue(cIGZWin* win, std::uint32_t id, bool set, tCellInfo* info);   // @ 0x1002daf96
    // The control-id -> row handler table the EnumFunc* callbacks dispatch through (@ 0x100a92150, 0x7f entries:
    // index = (id & 0xfff) - 0x1000). Entries the original has but this port does not implement yet are null.
    static tSetOrGetFn const kRowFunctions[0x7f];
    static bool SetOrGetName(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d5a0a
    static bool SetOrGetWhyOccupantAbandoned(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d5bc0
    static bool SetOrGetOccupantIsPowered(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);      // @ 0x1002dacd2
    static bool SetOrGetOccupantIsWatered(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);      // @ 0x1002dadd6
    static bool SetOrGetOccupantIsHistorical(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002daed6
    static bool SetOrGetResidentialBuildingMayorApproval(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002da692

    // fire / police / jail / park rows
    static bool SetOrGetFireStationEffectiveness(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);      // @ 0x1002d5de4
    static bool SetOrGetFireStationLocalFunding(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);      // @ 0x1002d5e1e
    static bool SetOrGetFireStationEngineCount(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);       // @ 0x1002d5f02
    static bool SetOrGetFireStationMaintenanceCost(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d5f82
    static bool SetOrGetPoliceStationEffectiveness(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d5fd8
    static bool SetOrGetPoliceStationArrestCount(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);     // @ 0x1002d6016
    static bool SetOrGetPoliceStationCrimeCount(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);      // @ 0x1002d6056
    static bool SetOrGetPoliceStationLocalFunding(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);    // @ 0x1002d6096
    static bool SetOrGetPoliceStationPatrolCarCount(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);  // @ 0x1002d617a
    static bool SetOrGetPoliceStationMaintenanceCost(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant); // @ 0x1002d61fa
    static bool SetOrGetJailCondition(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);                // @ 0x1002d6250
    static bool SetOrGetJailCellCount(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);                // @ 0x1002d631c
    static bool SetOrGetJailInmateCount(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);              // @ 0x1002d636a
    static bool SetOrGetJailMaintenanceCost(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);          // @ 0x1002d63e6
    static bool SetOrGetParkMaintenanceCost(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);          // @ 0x1002d6426
    static bool SetOrGetOccupantCost(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);                 // @ 0x1002d6434

    // neighbour-connection rows
    static IQueryNeighborCity* FindCityOnOtherSideOfConnection(int connectionType, int x, int z);   // @ 0x1002d906e
    // --- airport / seaport rows: the occupant form reads the occupant's first city cell and calls the cell form ---
    static bool SetOrGetAirportOccupantAirTrafficVolume(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d655a
    static bool SetOrGetAirportAirTrafficVolume(cIGZWin* win, std::uint32_t id, bool set, int x, int z);   // @ 0x1002d65ae
    static bool SetOrGetAirportOccupantMaintenanceCost(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d6a9e
    static bool SetOrGetAirportMaintenanceCost(cIGZWin* win, std::uint32_t id, bool set, int x, int z);   // @ 0x1002d6af2
    static bool SetOrGetAirportOccupantUsage(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d6b6e
    static bool SetOrGetAirportUsage(cIGZWin* win, std::uint32_t id, bool set, int x, int z);   // @ 0x1002d6bc2
    static bool SetOrGetAirportOccupantCapacity(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d6c4c
    static bool SetOrGetAirportCapacity(cIGZWin* win, std::uint32_t id, bool set, int x, int z);   // @ 0x1002d6ca0
    static bool SetOrGetSeaportOccupantUsage(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d6d2a
    static bool SetOrGetSeaportUsage(cIGZWin* win, std::uint32_t id, bool set, int x, int z);   // @ 0x1002d6d7e
    static bool SetOrGetSeaportOccupantCapacity(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d6e08
    static bool SetOrGetSeaportCapacity(cIGZWin* win, std::uint32_t id, bool set, int x, int z);   // @ 0x1002d6e5c
    static bool SetOrGetSeaportOccupantSeaTrafficVolume(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d6ee6
    static bool SetOrGetSeaportSeaTrafficVolume(cIGZWin* win, std::uint32_t id, bool set, int x, int z);   // @ 0x1002d6f3a
    static bool SetOrGetSeaportOccupantMaintenanceCost(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d7016
    static bool SetOrGetSeaportMaintenanceCost(cIGZWin* win, std::uint32_t id, bool set, int x, int z);   // @ 0x1002d706a
    // --- school / hospital / library rows (the residential simulator's query data) ---
    static bool SetOrGetSchoolGrade(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d7142
    static bool SetOrGetSchoolLocalStaffFunding(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d71fc
    static bool SetOrGetSchoolLocalBusFunding(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d7346
    static bool SetOrGetSchoolTeacherCount(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d748c
    static bool SetOrGetSchoolDeskCount(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d74de
    static bool SetOrGetSchoolStudentCount(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d7530
    static bool SetOrGetMuseumExhibitTicketsAvailable(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d75dc
    static bool SetOrGetLibraryBooksBorrowedCount(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d75ea
    static bool SetOrGetLibraryBooksInCollectionCount(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d7696
    static bool SetOrGetHospitalGrade(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d76e8
    static bool SetOrGetHospitalLocalStaffFunding(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d77a2
    static bool SetOrGetHospitalLocalAmbulanceFunding(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d78e8
    static bool SetOrGetHospitalDoctorCount(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d7a2c
    static bool SetOrGetHospitalBedCount(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d7a7e
    static bool SetOrGetHospitalPatientCount(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d7ad0
    // --- landfill / incinerator / recycling / power rows ---
    static bool SetOrGetLandfillUsage(cIGZWin* win, std::uint32_t id, bool set, int x, int z);   // @ 0x1002d7b7c
    static bool SetOrGetLandfillNetTrashThisMonth(cIGZWin* win, std::uint32_t id, bool set, int x, int z);   // @ 0x1002d7c22
    static bool SetOrGetIncineratorEfficiency(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d7c92
    static bool SetOrGetIncineratorPercentCapacity(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d7cfe
    static bool SetOrGetIncineratorTrashIncinerated(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d7d84
    static bool SetOrGetRecyclingCenterEfficiency(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d7e5c
    static bool SetOrGetRecyclingCenterPercentCapacity(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d7ef6
    static bool SetOrGetRecyclingCenterTrashRecycled(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d7fc8
    static bool SetOrGetWasteToEnergyEfficiency(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d8064
    static bool SetOrGetPowerPlantEfficiency(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d80a2
    static bool SetOrGetWasteToEnergyPercentCapacity(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d80e0
    static bool SetOrGetPowerPlantPercentCapacity(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d80ee
    static bool SetOrGetWasteToEnergyMaxPowerOutputCapacity(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d8162
    static bool SetOrGetPowerPlantMaxPowerOutputCapacity(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d81ae
    static bool SetOrGetWasteToEnergyCurrentPowerOutputCapacity(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d81fa
    static bool SetOrGetPowerPlantCurrentPowerOutputCapacity(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d8208
    static bool SetOrGetWasteToEnergyPowerUsed(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d8264
    static bool SetOrGetPowerPlantPowerUsed(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d8272
    static bool SetOrGetWasteToEnergyCostPerUnit(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d82ca
    static bool SetOrGetPowerPlantCostPerUnit(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d82d8
    static bool SetOrGetWasteToEnergyTrashIncinerated(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d8378
    static bool SetOrGetWasteToEnergyPercentCondition(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d83f4
    static bool SetOrGetPowerPlantPercentCondition(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d843e
    static bool SetOrGetPowerPlantLocalFunding(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d8488
    // --- water building rows ---
    static bool SetOrGetWaterBuildingEfficiency(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d859e
    static bool SetOrGetWaterTreatmentPlantEfficiency(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002daf82
    static bool SetOrGetWaterBuildingUsagePercent(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d860e
    static bool SetOrGetWaterBuildingMaxCapacity(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d868e
    static bool SetOrGetWaterBuildingCurrentCapacity(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d86f2
    static bool SetOrGetWaterBuildingWaterUsed(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d875a
    static bool SetOrGetWaterBuildingCostPerUnit(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d87c0
    static bool SetOrGetWaterBuildingPumpCondition(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d8860
    static bool SetOrGetWaterBuildingPercentCondition(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d894c
    // --- landmark rows and the transit station's global funding ---
    static bool SetOrGetTransitStationGlobalFunding(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d8e3c
    static bool SetOrGetLandmarkFunding(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);       // @ 0x1002d8e9e
    static bool SetOrGetLandmarkMaintenance(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d8f14
    static bool SetOrGetLandmarkHomage(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);        // @ 0x1002d8fc4
    // --- transit station rows (the traffic simulator's switch / station data under the occupant's cell) ---
    static bool SetOrGetTransitStationServiceQuality(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d89b0
    static bool SetOrGetTransitStationUsagePercent(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d8aae
    static bool SetOrGetTransitStationMaintenanceCost(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d8ba2
    static bool SetOrGetTransitStationActualUsage(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d8c80
    static bool SetOrGetTransitStationMaxCapacity(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d8d5e
    // --- RCI building rows (first batch: occupancy, customers, land value, the goofy stat) ---
    static std::uint32_t GetOccupantCapacity(cISC4Occupant* occupant);        // @ 0x1002d9baa
    static std::uint32_t GetCurrentOccupancyValue(cISC4Occupant* occupant);   // @ 0x1002d9a92
    static int GetLandValue(cISC4Occupant* occupant);                         // @ 0x1002da05e
    static bool SetOrGetGoofyStat(cIGZWin* win, std::uint32_t id, bool set, cISCPropertyHolder* holder, std::uint32_t propertyId);   // @ 0x1002d6944
    static bool SetOrGetOccupantGoofyStat(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002dac96
    static bool SetOrGetAirportLostLuggage(cIGZWin* win, std::uint32_t id, bool set, int x, int z);              // @ 0x1002d68bc
    static bool SetOrGetAirportOccupantLostLuggage(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d6876
    static bool SetOrGetRCIBuildingCurrentOccupancy(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002d9ca8
    static bool SetOrGetRCIBuildingMaxOccupancy(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);       // @ 0x1002d9d5e
    static bool SetOrGetRCIBuildingLandValue(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);          // @ 0x1002da28c
    static bool SetOrGetCommercialServiceBuildingCustomers(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002dac26
    // --- RCI rating rows: GetSubjective* helpers (growth developer + per-cell map -> a rating index 0..2) ---
    using tSubjectiveFn = std::uint32_t (*)(IQueryGrowthDeveloper*, int, int, int&);
    static std::uint32_t GetSubjectivePollution(IQueryGrowthDeveloper* developer, int x, int z, int& value);      // @ 0x1002cf38e
    static std::uint32_t GetSubjectiveGarbage(IQueryGrowthDeveloper* developer, int x, int z, int& value);       // @ 0x1002cf488
    static std::uint32_t GetSubjectiveSchoolGrade(IQueryGrowthDeveloper* developer, int x, int z, int& value);   // @ 0x1002cf544
    static std::uint32_t GetSubjectiveHospitalGrade(IQueryGrowthDeveloper* developer, int x, int z, int& value); // @ 0x1002cf5fe
    static std::uint32_t GetSubjectiveCrimeLevel(IQueryGrowthDeveloper* developer, int x, int z, int& value);    // @ 0x1002cf6b8
    static std::uint32_t GetSubjectiveTrafficLevel(IQueryGrowthDeveloper* developer, int x, int z, int& value);  // @ 0x1002cf776
    static std::uint32_t GetSubjectiveTripLength(IQueryGrowthDeveloper* developer, int x, int z, int& value);    // @ 0x1002cf87c
    static void GetSubjectiveOccupantRatingString(cISC4Occupant* occupant, tSubjectiveFn rating, const std::uint32_t* strings, cRZString& out);   // @ 0x1002d99be
    static bool SetOrGetRCIBuildingPollution(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002da0e2
    static bool SetOrGetRCIBuildingGarbage(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);     // @ 0x1002da170
    static bool SetOrGetRCIBuildingCrime(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);       // @ 0x1002da1fe
    static bool SetOrGetResidentialBuildingCommute(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);   // @ 0x1002da38a
    static bool SetOrGetRCIBuildingTraffic(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);     // @ 0x1002da462
    static bool SetOrGetResidentialBuildingEQ(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);  // @ 0x1002da724
    static bool SetOrGetResidentialBuildingHQ(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant);  // @ 0x1002da7b2
    static bool SetOrGetNeighborConnectionNeighborCityName(cIGZWin* win, std::uint32_t id, bool set, int x, int z,
                                                           int connectionType);                     // @ 0x1002d90d6
    static bool SetOrGetNeighborConnectionNeighborCityMayorName(cIGZWin* win, std::uint32_t id, bool set, int x,
                                                                int z, int connectionType);              // @ 0x1002d9184
    static bool SetOrGetNeighborConnectionCongestion(cIGZWin* win, std::uint32_t id, bool set, int x, int z);   // @ 0x1002d922e

    static void GetOccupantName(cISC4Occupant* occupant, cRZString& out);                  // @ 0x1002d2616
    static float GetMayorRatingPercent(cISC4Occupant* occupant);                           // @ 0x1002da4f0
    static void GetRatingIn3DegreesFromPercent(cRZString& out, float percent);             // @ 0x1002da640

    // static enum callbacks used by the dialog windows
    static bool EnumFuncSetFilter(cIGZWin* win, std::uint32_t id, void* child, void* context);       // @ 0x1002db37e
    static bool EnumFuncClearFilter(cIGZWin* win, std::uint32_t id, void* child, void* context);     // @ 0x1002db3bc
    static bool EnumFuncSetValue(cIGZWin* win, std::uint32_t id, void* child, void* context);        // @ 0x1002db566
    static bool EnumFuncUpdateValue(cIGZWin* win, std::uint32_t id, void* child, void* context);     // @ 0x1002db5d8
    static bool EnumFuncGetValue(cIGZWin* win, std::uint32_t id, void* child, void* context);        // @ 0x1002db69e
    static bool EnumFuncNeighborDialogSetValue(cIGZWin* win, std::uint32_t id, void* cellInfo, void* occupant);   // @ 0x1002db70e
    static bool EnumFuncNeighborDialogGetValue(cIGZWin* win, std::uint32_t id, void* cellInfo, void* occupant);   // @ 0x1002db748

    // --- PARTIAL: declared, not implemented (see the header comment and the ledger) ---
    void GetBuildingOccupantTrafficInfo(cRZString& text, cRZString& title, std::uint32_t& icon);
    void GetNetworkOccupantTrafficInfo(cRZString& text, cRZString& title);
    void GetBuildingOccupantTipInfo(cRZString& text, cRZString& title, std::uint32_t& icon, std::uint32_t& meter,
                                    float& meterValue);
    void GetNetworkOccupantSummaryInfo(cRZString& title, cRZString& text);
    void GetPropOccupantSummaryInfo(cRZString& title);
    void GetFloraOccupantSummaryInfo(cRZString& title);
    void GetTerrainSummaryInfo(cRZString& title, cRZString& text);
    // @ 0x1002cfb90 a lot's headline sentences keyed by importance
    static void GetLotHeadlines(IQueryLot* lot, std::multimap<int, cRZString>& headlines);
    static std::uint32_t GetBuildingWealth(IQueryLot* lot);   // @ 0x1002cfae0
    void DescribePick(int x, int y, cRZString& line1, cRZString& line2, cRZString& line3, bool detailed);   // @ 0x1002ccd38
    bool DoQueryDialog(cISC4Occupant* occupant);                                              // i386 0x00218ed0
    bool DoZoneBasedQueryDialog(std::uint32_t zoneType, std::uint32_t cellX, std::uint32_t cellZ);   // i386 0x00219306
    bool DoNeighborConnectionQueryDialog(cISC4Occupant* occupant);                            // i386 0x00219668
    void GetOccupantPaths();
    void ShowAllRoutesThatIntersectCityRect(const SC4CellRect& rect);
    void WriteDetailedDebugInfo(int cellX, int cellZ);

    // The static state the original keeps at file scope.
    static cIGZWin* QueryDialogWindow();
    static void SetQueryDialogWindow(cIGZWin* window);
    static cIGZWin::EnumChildFn EnumSetFunction();
    static void SetEnumSetFunction(cIGZWin::EnumChildFn fn, void* context1, void* context2);

    // inspection for tests
    EQueryType QueryType() const { return queryType_; }
    bool TipEnabled() const { return tipEnabled_; }
    bool HoverValid() const { return hoverValid_; }
    bool MouseDown() const { return mouseDown_; }
    cISC4Occupant* HoveredOccupant() const { return occupant_; }
    std::uint32_t TipPeriod() const { return tipPeriod_; }
    cRZCallbackTimer& TipTimer() { return tipTimer_; }
    cISC4WinCalloutBox* CalloutBox() const { return callout_; }
    void SetCityCellRect(const SC4CellRect& rect) { cityCellRect_ = rect; }
    void SetHoveredOccupantForTest(cISC4Occupant* occupant) { occupant_ = occupant; }   // no reference is taken
    void SetPickPositionForTest(float x, float y, float z) { pickX_ = x; pickY_ = y; pickZ_ = z; }

protected:
    // The two windows of the network-mode legend come from this script instance (original: resource group 0x96a006b0,
    // instance 0xabb0120f, created with window id 0x6bb92bcb).
    static constexpr std::uint32_t kLegendInstance = 0xabb0120f;
    static constexpr std::uint32_t kLegendWindowId = 0x6bb92bcb;

    void CreateLegendWindows();
    cIGZWin* ViewWindow();

public:
    // the child ids of the legend script window (public so the adapter / test can build it)
    static constexpr std::uint32_t kLegendMinimizedChild = 0x0bb0f5e7;
    static constexpr std::uint32_t kLegendExpandedChild = 0x6bb92bca;

protected:

    cISC4Occupant* occupant_ = nullptr;       // +0x48 hovered occupant (referenced)
    SC4CellRect cityCellRect_{};              // +0x50
    std::uint32_t occupantHighlight_ = 0;     // +0x60
    cISC4WinCalloutBox* callout_ = nullptr;   // +0x68
    cIGZWin* legendMinimized_ = nullptr;      // +0x70
    cIGZWin* legendExpanded_ = nullptr;       // +0x78
    cRZCallbackTimer tipTimer_;               // +0x80
    bool tipEnabled_ = true;                  // +0xb8
    float pickX_ = 0.0f;                      // +0xbc
    float pickY_ = 0.0f;                      // +0xc0
    float pickZ_ = 0.0f;                      // +0xc4
    bool hoverValid_ = false;                 // +0xc8
    bool mouseDown_ = false;                  // +0xc9
    EQueryType queryType_ = kQueryBuilding;   // +0xcc
    std::uint32_t tipPeriod_ = 3;             // +0xd0
    bool showCoordinates_ = false;            // +0xd4
    std::uint32_t unknownD8_ = 3;             // +0xd8 (written by the ctor, never read)
    IQueryPathView* pathView_ = nullptr;      // +0xe0

private:
    static void MouseTipCallback(void* self);      // @ 0x1002c2596
    static void CoverageRingCallback(void* self);  // @ 0x1002d4eb0
    static cIGZWin* sQueryDialog_;
    static cIGZWin::EnumChildFn sEnumSetFunction_;
    static void* sEnumContext1_;
    static void* sEnumContext2_;
};

}  // namespace sc4
