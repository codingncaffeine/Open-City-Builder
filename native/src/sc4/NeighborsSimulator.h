// cSC4NeighborsSimulator — the regional coupling simulator: it owns this city's cross-border connections and the
// trade deals (power / water / garbage) with the neighbouring region cities, runs the deal state machine once a
// simulated month and pushes the resulting commodity statistics into the region.
//
// 65 symbols at 0x10006e61e..0x100075eb8 (44 of them virtual, primary vtable 0x100a6c400). Object layout
// (0xa8 bytes):
//   +0x00 cISC4NeighborsSimulator vtable (iid 0x0773fef0)
//   +0x08 cIGZMessageTarget2 vtable (iid 0x452294aa)
//   +0x10 cIGZSerializable vtable (iid 0xe4fda3d4)
//   +0x18 bool city established (0x6ac284f3 sets it; simulate_deals does nothing before)
//   +0x19 bool initialized
//   +0x1c int  refcount
//   +0x20 cISC4City* city (SL::spCity; AddRef'd by Init, released by Shutdown only — the destructor leaks it)
//   +0x28 cIGZMessageServer2* message server (SL::spMS2; AddRef'd by Init, released by Shutdown only)
//   +0x2c u32  unused (zeroed by the constructor; never read)
//   +0x30 std::list<cRZAutoRefCount<cISC4NeighborConnection>> connections
//   +0x48 std::list<cRZAutoRefCount<cISC4NeighborDeal>>       deals
//   +0x60 std::list<deal-list iterator>                        deals to erase after the simulation pass
//   +0x70 the size of that last list (the std::list's own __size_)
//   +0x78 cISCResExemplar* tuning exemplar {0x7a4a8458, 0x6534284a, 0x6a469021}, iid 0xa52160f5 (released by
//        Shutdown and by the destructor)
//   +0x80 cSC4ResponseCurve (24-byte std::vector)
//   +0xa0 bool deals dirty     (simulate_deals / AddDeal / MarkDealsDirty set it)
//   +0xa1 bool connections dirty (the deal-state messages set it)
//   +0xa2 bool UI disabled (SetUIDisableFlag; simulate_deals skips the deal pass while set)
//   +0xa3 bool inside simulate_deals (re-entrancy guard: a nested call posts message 0xea418d8f data1 8)
//
// Per-tick behaviour (DoMessage): 0x66956814 (day) and 0x6695681a (idle time) flush the dirty flags to the
// region through update_deals_in_regional_cities; 0x66956816 (month) and 0xea418d8f (a deal status report)
// run simulate_deals; 0x26d31ec4 / 0x26d31ec5 clear the deals and flush; 0x278e6003 moves the connection
// occupants after a terrain change; 0x6ac284f3 marks the city established; 0xaa7a014d and 0x99ef1143 delete
// an occupant's connection.
//
// Connection types (0..12; only the values the code treats specially are documented):
//   3, 7, 11 get +15.5 altitude for their occupant (mask 0x888)
//   5, 6 also get occupant properties 0xcaa45672 / 0xcaa45671 (0xcaa45670 in both cases)
//   9 has no connection occupant at all
//   0, 4, 5 do not propagate city adjacency (mask 0x1fce)
// Deal types are buy/sell pairs: 0x4773ffe0/e1 power, 0x4773ffe2/e3 water, 0x4773ffe4/e5 garbage; the even
// member buys (imports) and the odd member sells (exports).
//
// All engine services are reached through INeighborsHost (one method per original call, original vtable slot in
// the comment). Quirks kept:
//   * the constructor writes 1 at +0xa0 as a 32-bit store, so "deals dirty" starts true;
//   * the destructor does not release the city or the message server (Shutdown does, but a plain delete leaks);
//   * simulate_deals spins forever on a deal with the silent flag (0x80) in its list — the original has no
//     iterator advance in that loop;
//   * deal renewal (flag 0x40 set) sets the expiration date to (today - today) + sim date = the sim date;
//   * the lock/freeze durations read from the exemplar are added to a temporary "today" date and never to the
//     suspension date the comparison uses, so a locked/frozen deal is released on the next tick;
//   * UpdateCommodityStats stores Price/Produced/Imported/Exported per commodity after multiplying costs by
//     (float) before widening to double.
#pragma once
#include <cstdint>
#include <list>
#include <vector>

#include "gz/cGZDate.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZSerializable.h"
#include "rz/cRZAutoRefCount.h"
#include "sc4/Exemplar.h"
#include "sc4/ISC4City.h"
#include "sc4/ISC4RegionalCity.h"
#include "sc4/MiscTypes.h"
#include "sc4/NeighborConnection.h"
#include "sc4/NeighborDeal.h"
#include "sc4/cSC4ResponseCurve.h"

class cISCPropertyHolder;
class cIGZString;

namespace sc4 {

class cISC4Occupant;

constexpr std::uint32_t kGZCLSID_cSC4NeighborsSimulator = 0xc990c0a4;
constexpr std::uint32_t kGZIID_cISC4NeighborsSimulator = 0x0773fef0;

// Messages (Init registers the seven target-side ids; the last two are only posted).
constexpr std::uint32_t kNeighborsMessageOccupantGone = 0xaa7a014d;      // DeleteOccupantConnection(occupant)
constexpr std::uint32_t kNeighborsMessageTerrainChanged = 0x278e6003;    // DoTerrainChanged(rect)
constexpr std::uint32_t kNeighborsMessageDealStatus = 0xea418d8f;        // data1: status (0/8 = connections)
constexpr std::uint32_t kNeighborsMessageCityEstablished = 0x6ac284f3;
constexpr std::uint32_t kNeighborsMessageOccupantRemoved = 0x99ef1143;   // the 0x2890d4de path
constexpr std::uint32_t kNeighborsMessageCityReset = 0x26d31ec4;
constexpr std::uint32_t kNeighborsMessageCityReset2 = 0x26d31ec5;
constexpr std::uint32_t kNeighborsMessageDay = 0x66956814;
constexpr std::uint32_t kNeighborsMessageMonth = 0x66956816;
constexpr std::uint32_t kNeighborsMessageIdleTime = 0x6695681a;
constexpr std::uint32_t kNeighborsMessageConnectionCreated = 0x8a0d15da;  // data1 type, data2 x, data3 z
constexpr std::uint32_t kNeighborsMessageConnectionDeleted = 0xea0d163e;  // data1 type, data2 x, data3 z

// Connection types.
constexpr std::uint32_t kConnectionTypeMax = 12;
constexpr std::uint32_t kConnectionTypeNoOccupant = 9;
constexpr std::uint32_t kConnectionTypePowerLine = 5;      // adds 0xcaa45672
constexpr std::uint32_t kConnectionTypePipe = 6;          // adds 0xcaa45671
// type < 0xc and bit set -> the connection occupant sits 15.5 above the terrain
constexpr std::uint32_t kRaisedConnectionTypeMask = 0x888;
// type < 0xd and bit set -> the connection propagates city adjacency / connected-city traversal
constexpr std::uint32_t kConnectedCityTypeMask = 0x1fce;

// Deal types (buy/sell pairs).
constexpr std::uint32_t kDealTypePowerBuy = 0x4773ffe0, kDealTypePowerSell = 0x4773ffe1;
constexpr std::uint32_t kDealTypeWaterBuy = 0x4773ffe2, kDealTypeWaterSell = 0x4773ffe3;
constexpr std::uint32_t kDealTypeGarbageBuy = 0x4773ffe4, kDealTypeGarbageSell = 0x4773ffe5;
// extra deal flag used by the deal pass (on top of the flags in NeighborDeal.h)
constexpr std::uint32_t kDealFlagExpired = 0x40, kDealFlagBudgetExceeded = 0x20;
constexpr std::uint32_t kDealFlagFrozenBit = 0x200;   // the frozen state bit (NeighborDeal's 0x300 is the mask)

// Exemplar property ids read by the simulator.
constexpr std::uint32_t kNeighborsPropertyResponseCurve = 0xa469353;   // simulator response curve
constexpr std::uint32_t kNeighborsPropertyDealYears = 0xea46934f;      // years per deal term (x12 = months)
constexpr std::uint32_t kNeighborsPropertyMinDealYears = 0xaa46934b;
constexpr std::uint32_t kNeighborsPropertyPowerPrice = 0xea46932c;
constexpr std::uint32_t kNeighborsPropertyWaterPrice = 0x6a469337;
constexpr std::uint32_t kNeighborsPropertyGarbagePrice = 0x8a46933d;
constexpr std::uint32_t kNeighborsPropertyDealCostScale = 0xea4ad2ff;
constexpr std::uint32_t kNeighborsPropertyDealCostFactor = 0xca469346;
constexpr std::uint32_t kNeighborsPropertyDealCostFactor2 = 0x6a469342;
constexpr std::uint32_t kNeighborsPropertyFreezeDays = 0x8b58c59e;
constexpr std::uint32_t kNeighborsPropertySuspendDays = 0x6b588fad;
constexpr std::uint32_t kNeighborsPropertyGarbageCurve = 0x4b61ffc9;
constexpr std::uint32_t kNeighborsPropertyMinBudget = 0xea4ae9ef;
constexpr std::uint32_t kNeighborsPropertyBudgetCostFactor = 0xca46931f;

// Properties the connection occupant carries.
constexpr std::uint32_t kNeighborsPropertyConnectionFlag = 0xcaa46595;
constexpr std::uint32_t kNeighborsPropertyConnectionPower = 0xcaa45670;
constexpr std::uint32_t kNeighborsPropertyConnectionPipe = 0xcaa45671;
constexpr std::uint32_t kNeighborsPropertyConnectionPowerLine = 0xcaa45672;

// Budget line items / history series used by UpdateCommodityStats.
constexpr std::uint32_t kNeighborsBudgetPower = 0x8910bc8a;
constexpr std::uint32_t kNeighborsBudgetPowerSub = 0x1f470000;
constexpr std::uint32_t kNeighborsBudgetWater = 0xaa4014b4;
constexpr std::uint32_t kNeighborsBudgetGarbage = 0x6a42a8f5;
constexpr std::uint32_t kNeighborsHistoryPowerImported = 0xaa11f837;
constexpr std::uint32_t kNeighborsHistoryPowerExported = 0xaa11f83a;
constexpr std::uint32_t kNeighborsHistoryWaterImported = 0x0a14ebb5;
constexpr std::uint32_t kNeighborsHistoryWaterExported = 0xca14ebc6;
constexpr std::uint32_t kNeighborsHistoryGarbageExported = 0xea11017b;
constexpr std::uint32_t kNeighborsHistoryGarbageImported = 0xea11017e;
constexpr std::uint32_t kNeighborsHistoryGarbageDemanded = 0xeb54f32e;
constexpr std::uint32_t kNeighborsHistoryGarbageProduced = 0x4b54f333;

// The occupant-prop type CreateConnection asks the property manager for.
constexpr std::uint32_t kConnectionOccupantPropType = 0x29f10000;
// 0x2890d4de is the power-pole occupant type; its connection type is 4.
constexpr std::uint32_t kOccupantTypePowerPole = 0x2890d4de;
constexpr std::uint32_t kOccupantTypeNetworkOccupant = 0xc772bf98;

using ConnectionList = std::list<cRZAutoRefCount<cISC4NeighborConnection>>;
using DealList = std::list<cRZAutoRefCount<cISC4NeighborDeal>>;

// Everything the simulator calls outside itself. One method per original call; the vtable slot or free-function
// address is in the comment. Assembled the same way as IRegionalCityHost / ITrafficHost.
class INeighborsHost {
public:
    virtual ~INeighborsHost() = default;

    // --- framework services -------------------------------------------------------------------------
    virtual cIGZMessageServer2* MessageServer() = 0;                                  // SL::spMS2
    virtual cISC4City* City() = 0;                                                    // SL::spCity
    virtual cISC4RegionalCity* CurrentRegionalCity() = 0;                             // SL::spSC4 vslot 0x120
    // SL::spRM vslot 0x18: GetResource({0x7a4a8458, 0x6534284a, 0x6a469021}, iid 0xa52160f5); one reference.
    virtual cISCResExemplar* TuningExemplar() = 0;
    // SL::spSimulator vslot 0x98 / 0xa8. The three Init calls use intervals 6, 4 and 10, flags 2.
    virtual void AddSimulatorAgent(cIGZMessageTarget2* agent, int interval, const char* name) = 0;
    virtual void RemoveSimulatorAgent(cIGZMessageTarget2* agent) = 0;
    // SL::spSimulator vslot 0x30: the current simulated date.
    virtual void SimDate(cIGZDate& out) = 0;
    // SL::spMS2 vslot 0x20 (MessagePost) with a fresh cRZMessage2Standard; SC4PostMessage2 @ 0x10003a5ba.
    virtual void PostMessage(std::uint32_t type, std::uint32_t data1, std::uint32_t data2,
                             std::uint32_t data3, std::uint32_t data4, cIGZUnknown* unknown) = 0;
    // cRZMessage2Standard posted through SL::spMS2: used by sReportDealStatus<>. `unknown` may be null.
    virtual void PostDealStatusMessage(std::uint32_t status, cIGZUnknown* unknown) = 0;
    // The region (SL::spSC4 vslot 0x118) vslot 0x58 (GetCity) and 0x78 (ReloadCity).
    virtual cISC4RegionalCity* RegionCityAt(int x, int z) = 0;
    virtual void ReloadCityAt(int x, int z) = 0;

    // --- the current city object --------------------------------------------------------------------
    virtual int CityCellCountX() = 0;                                                 // city vslot 0x2e8
    virtual int CityCellCountZ() = 0;                                                 // city vslot 0x2f0
    virtual void CityCellCenterToPosition(int x, int z, float& outX, float& outZ) = 0;  // city vslot 0x308
    virtual void CityPositionToCell(float x, float z, int& outX, int& outZ) = 0;      // city vslot 0x2f8
    virtual float TerrainAltitudeAtCell(int x, int z) = 0;                            // SL::spTerrain vslot 0x90
    virtual float TerrainAltitudeAtPosition(float x, float z) = 0;                    // SL::spTerrain vslot 0x60
    virtual void RecordHistory(std::uint32_t series, int value) = 0;                  // city 0x100 -> history 0x60
    // The budget simulator (city 0x1a0): total vslot 0x30, income vslot 0x80, line item 0xd8 then 0x78/subs 0xa0/0x50.
    virtual double BudgetTotal() = 0;
    virtual double BudgetIncome() = 0;
    // false when the line item does not exist (the original tests the object pointer, not the amount).
    virtual bool BudgetLineItemAmount(std::uint32_t lineId, double& out) = 0;
    virtual bool BudgetSubLineItemAmount(std::uint32_t lineId, std::uint32_t subId, double& out) = 0;
    virtual int PowerCapacity() = 0;                                                  // SL::spPowerSim 0x60
    virtual int PowerDemand() = 0;                                                    // SL::spPowerSim 0x58
    virtual int WaterCapacity() = 0;                                                  // SL::spPlumbingSim 0x70
    virtual int WaterDemand() = 0;                                                    // SL::spPlumbingSim 0xb8
    virtual void LandfillRange(int& start, int& end) = 0;                             // SL::spLandfillDeveloper 0x50
    // SL::spPollutionSim: 0xf8, 0x100, 0x130, 0x110 and 0x118.
    virtual int PollutionValue(int which) = 0;

    // --- occupants and props (DoTerrainChanged / CreateConnection / DeleteConnection) -----------------
    // SL::spPropManager vslot 0x88: occupants intersecting the cell rectangle (original returns references).
    virtual void OccupantsInRect(int x0, int z0, int x1, int z1, std::vector<cISC4Occupant*>& out) = 0;
    // SL::spPropManager vslot 0xe8: delete the prop.
    virtual void DeleteProp(cISC4Occupant* occupant) = 0;
    // SL::spPropManager vslot 0x78 (create, one reference) and 0xa0 (drop the manager's reference).
    virtual bool CreateProp(std::uint32_t propType, void** out) = 0;
    virtual void FinishProp(void* prop) = 0;
    // The created prop's interface: 0x18 -> the placeable object, 0x48 SetPosition, 0x50 edge flags,
    // 0x18 -> 0x30 -> 0x60 AddProperty.
    virtual void SetPropPosition(void* prop, const float xyz[3]) = 0;
    virtual void SetPropEdgeFlags(void* prop, std::uint32_t flags) = 0;
    virtual void AddPropConnectionProperties(void* prop, std::uint32_t connType) = 0;
    virtual void ReleaseProp(void* prop) = 0;
    // SL::spOccupantManager vslot 0x180: release a list returned by OccupantsInRect.
    virtual void ReleaseOccupantList(std::vector<cISC4Occupant*>& list) = 0;
    // SL::spOccupantManager vslot 0xc0: deactivate / hide the occupant (arg 1).
    virtual void DeactivateOccupant(cISC4Occupant* occupant) = 0;
    virtual const cISCPropertyHolder* OccupantProperties(cISC4Occupant* occupant) = 0;   // occupant 0x30
    virtual void OccupantPosition(cISC4Occupant* occupant, float& x, float& z) = 0;      // occupant 0x40
    virtual void SetOccupantPosition(cISC4Occupant* occupant, const float xyz[3]) = 0;   // occupant 0x48
    virtual std::uint32_t OccupantType(cISC4Occupant* occupant) = 0;                     // occupant 0x38
    // cISC4NetworkOccupant (QI 0xa821ef94) vslot 0xb0: does the occupant carry this network type?
    virtual bool OccupantHasNetworkType(cISC4Occupant* occupant, std::uint32_t networkType) = 0;
    // QI 0xa8c86142 then vslot 0x68: true while the occupant is still referenced (the original dereferences
    // the interface without checking the QI result).
    virtual bool OccupantRetained(cISC4Occupant* occupant) = 0;

    // --- a regional city (the cISC4RegionalCity virtuals the partial interface does not declare) ------
    virtual void CityPosition(const cISC4RegionalCity* city, int& x, int& z) = 0;                 // 0x28
    virtual void CitySize(const cISC4RegionalCity* city, int& width, int& height) = 0;            // 0x38
    virtual bool CityEstablished(const cISC4RegionalCity* city) = 0;                              // 0x158
    virtual void CityName(const cISC4RegionalCity* city, cIGZString& out) = 0;                    // 0x108
    virtual double CityGetBudget(const cISC4RegionalCity* city) = 0;                              // 0x190
    virtual void CitySetBudget(cISC4RegionalCity* city, double value) = 0;                        // 0x198
    virtual void CitySetIncome(cISC4RegionalCity* city, double value) = 0;                        // 0x1a8
    virtual cISC4NeighborConnection* CityFindConnection(const cISC4RegionalCity* city, int type, int x, int z) = 0;  // 0xa0
    virtual void CityAllConnections(const cISC4RegionalCity* city, ConnectionList& out) = 0;      // 0xa8
    virtual void CityChangeSymmetricConnection(cISC4RegionalCity* city, cISC4NeighborConnection* connection, bool add) = 0;  // 0xb0
    virtual void CityUpdateLocalDeals(cISC4RegionalCity* city) = 0;                               // 0x220
    virtual void CitySetLocalDeals(cISC4RegionalCity* city, const DealList& deals) = 0;           // 0x228
    virtual void CityGetLocalDeals(cISC4RegionalCity* city, DealList& out) = 0;                   // 0x230
    virtual void CityUpdateImportExport(cISC4RegionalCity* city) = 0;                             // 0x238
    virtual double CityGetExported(const cISC4RegionalCity* city, int commodity) = 0;             // 0x1b0
    virtual void CitySetExported(cISC4RegionalCity* city, int commodity, double value) = 0;       // 0x1b8
    virtual double CityGetImported(const cISC4RegionalCity* city, int commodity) = 0;             // 0x1c0
    virtual void CitySetImported(cISC4RegionalCity* city, int commodity, double value) = 0;       // 0x1c8
    virtual double CityGetProduced(const cISC4RegionalCity* city, int commodity) = 0;             // 0x1d0
    virtual void CitySetProduced(cISC4RegionalCity* city, int commodity, double value) = 0;       // 0x1d8
    virtual double CityGetDemanded(const cISC4RegionalCity* city, int commodity) = 0;             // 0x1e0
    virtual void CitySetDemanded(cISC4RegionalCity* city, int commodity, double value) = 0;       // 0x1e8
    virtual double CityGetCostPerUnit(const cISC4RegionalCity* city, int commodity) = 0;          // 0x1f0
    virtual void CitySetCostPerUnit(cISC4RegionalCity* city, int commodity, double value) = 0;    // 0x1f8

    // --- the confirmation dialog --------------------------------------------------------------------
    // nSC4UI::GenericConfirmationDialog @ 0x10030b655 (no OK / cancel text) with the cSLStrings (group 0x6a231eaa) of the
    // title and the format; the format (0x0a8e152d) is Sprintf'd with `name`, or, when null (no established city on the
    // other side), with the default name string `defaultNameId` (i386 0x0004ce32: the city name overwrites that string).
    virtual bool ConfirmationDialog(std::uint32_t titleId, std::uint32_t formatId, std::uint32_t defaultNameId,
                                    const cIGZString* name) = 0;
};

INeighborsHost* NeighborsHost();
void SetNeighborsHost(INeighborsHost* host);

// cISC4NeighborsSimulator — the interface in the original's slot order (primary vtable 0x100a6c400).
class cISC4NeighborsSimulator : public cIGZUnknown {
public:
    virtual bool Init() = 0;                                                                       // [3]  0x18
    virtual bool Shutdown() = 0;                                                                   // [4]  0x20
    virtual bool RequestCreateConnection(std::uint32_t type, int x, int z, bool fromOther) = 0;    // [5]  0x28
    virtual void CreateConnection(std::uint32_t type, int x, int z, bool fromOther) = 0;           // [6]  0x30
    virtual void DeleteConnection(std::uint32_t type, int x, int z) = 0;                           // [7]  0x38
    virtual void DeleteOccupantConnection(cISC4Occupant* occupant) = 0;                            // [8]  0x40
    virtual void DeleteAllConnectionsOfType(std::uint32_t type) = 0;                               // [9]  0x48
    virtual bool HasConnection(std::uint32_t type, int x, int z) const = 0;                        // [10] 0x50
    virtual cISC4NeighborConnection* FindConnection(std::uint32_t type, int x, int z) = 0;         // [11] 0x58
    virtual cISC4RegionalCity* FindCityOnOtherSideOfConnection(int x, int z) const = 0;            // [12] 0x60
    virtual cISC4NeighborConnection* FindConnectionOnOtherSide(std::uint32_t type, int x, int z) = 0;  // [13] 0x68
    virtual cISC4NeighborConnection* FindConnectionOnOtherSide(cISC4NeighborConnection* connection) = 0;  // [14] 0x70
    virtual cISC4NeighborConnection* FindUnvisitedConnection(std::uint32_t type, const cISC4RegionalCity* city) = 0;  // [15] 0x78
    virtual void GetAllConnections(ConnectionList& out) const = 0;                                 // [16] 0x80
    virtual void GetConnectionsOfType(std::uint32_t type, ConnectionList& out) const = 0;          // [17] 0x88
    virtual void GetConnectionsOfTypeToCity(std::uint32_t type, const cISC4RegionalCity* city, ConnectionList& out) const = 0;  // [18] 0x90
    virtual void GetConnectionsToCity(const cISC4RegionalCity* city, ConnectionList& out) const = 0;  // [19] 0x98
    virtual void GetAdjacentCities(std::list<cRZAutoRefCount<cISC4RegionalCity>>& out, bool includeUnestablished) const = 0;  // [20] 0xa0
    virtual void GetConnectedCities(std::list<cRZAutoRefCount<cISC4RegionalCity>>& out, bool includeUnestablished) const = 0;  // [21] 0xa8
    virtual void GetAllConnectedCities(std::list<cRZAutoRefCount<cISC4RegionalCity>>& out, bool includeUnestablished) const = 0;  // [22] 0xb0
    virtual cISC4NeighborConnection* FindConnectionSuitableForDeal(std::uint32_t dealType, const cISC4RegionalCity* city) = 0;  // [23] 0xb8
    virtual bool ProposeDeal(std::uint32_t dealType, const cISC4RegionalCity* city, cISC4NeighborDeal** out) = 0;  // [24] 0xc0
    virtual bool ProposeDeals(DealList& out, bool fromUI) = 0;                                     // [25] 0xc8
    virtual bool AddDeal(cISC4NeighborDeal* deal) = 0;                                             // [26] 0xd0
    virtual void SetUIDisableFlag(bool disabled) = 0;                                              // [27] 0xd8
    virtual void FindDealsAll(DealList& out) = 0;                                                  // [28] 0xe0
    virtual void FindDeals(std::uint32_t dealType, DealList& out) = 0;                             // [29] 0xe8
    virtual void FindDeals(std::uint32_t dealType, const cISC4RegionalCity* city, DealList& out) = 0;  // [30] 0xf0
    virtual void MarkDealsDirty() = 0;                                                             // [31] 0xf8
    virtual std::int64_t GetTotalMonthlyNeighborDealIncome() = 0;                                  // [32] 0x100
    virtual std::int64_t GetTotalMonthlyNeighborDealExpense() = 0;                                 // [33] 0x108
    virtual void UpdateCommodityStats(cISC4City* city) = 0;                                        // [34] 0x110
    virtual double GetDealCost(std::uint32_t dealType, double amount, const cISC4RegionalCity* city) const = 0;  // [35] 0x118
    virtual double GetDealMaxAmount(std::uint32_t dealType, const cISC4RegionalCity* city) const = 0;  // [36] 0x120
    virtual cISCPropertyHolder* GetTuningExemplar() const = 0;                                     // [37] 0x128
    // [38]/[39] 0x130/0x138 are the complete and deleting destructors of the original vtable; the
    // non-virtual DeleteConnection(cISC4NeighborConnection*, bool) is the out-of-line function below.
    virtual ~cISC4NeighborsSimulator() = default;
    virtual bool Write(cIGZOStream& stream) const = 0;                                             // [40] 0x140
    virtual bool Read(cIGZIStream& stream) = 0;                                                    // [41] 0x148
    virtual std::uint32_t GetGZCLSID() const = 0;                                                  // [42] 0x150
    virtual bool DoMessage(cIGZMessage2* message) = 0;                                             // [43] 0x158
};

class cSC4NeighborsSimulator : public cISC4NeighborsSimulator,
                               public cIGZMessageTarget2,
                               public cIGZSerializable {
public:
    explicit cSC4NeighborsSimulator(INeighborsHost* host = nullptr);
    ~cSC4NeighborsSimulator() override;

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;

    bool Init() override;
    bool Shutdown() override;

    // --- connections ---
    bool RequestCreateConnection(std::uint32_t type, int x, int z, bool fromOther) override;
    void CreateConnection(std::uint32_t type, int x, int z, bool fromOther) override;
    void DeleteConnection(std::uint32_t type, int x, int z) override;
    void DeleteOccupantConnection(cISC4Occupant* occupant) override;
    void DeleteAllConnectionsOfType(std::uint32_t type) override;
    bool HasConnection(std::uint32_t type, int x, int z) const override;
    cISC4NeighborConnection* FindConnection(std::uint32_t type, int x, int z) override;
    cISC4RegionalCity* FindCityOnOtherSideOfConnection(int x, int z) const override;
    cISC4NeighborConnection* FindConnectionOnOtherSide(std::uint32_t type, int x, int z) override;
    cISC4NeighborConnection* FindConnectionOnOtherSide(cISC4NeighborConnection* connection) override;
    cISC4NeighborConnection* FindUnvisitedConnection(std::uint32_t type, const cISC4RegionalCity* city) override;
    void GetAllConnections(ConnectionList& out) const override;
    void GetConnectionsOfType(std::uint32_t type, ConnectionList& out) const override;
    void GetConnectionsOfTypeToCity(std::uint32_t type, const cISC4RegionalCity* city, ConnectionList& out) const override;
    void GetConnectionsToCity(const cISC4RegionalCity* city, ConnectionList& out) const override;
    void DeleteConnection(cISC4NeighborConnection* connection, bool removeFromList);   // @ 0x100071546

    // The three static cell conversions (0x100070670 / 0x1000706fa / 0x1000707f4 / 0x100070a70).
    static bool IsValidConnectionCell(int x, int z, cISC4RegionalCity* city);
    static void ConvertConnectionCellToNeighboringRegionCell(int x, int z, int& outX, int& outZ, cISC4RegionalCity* city);
    static bool ConvertConnectionCellToNeighborCell(int x, int z, int& outCellX, int& outCellZ,
                                                    cISC4RegionalCity** city, int& neighborCellX, int& neighborCellZ);
    static bool ConvertNeighborCellToConnectionCell(cISC4RegionalCity* city, int x, int z, int& outX, int& outZ);

    // --- adjacent / connected cities ---
    void GetAdjacentCities(std::list<cRZAutoRefCount<cISC4RegionalCity>>& out, bool includeUnestablished) const override;
    void GetConnectedCities(std::list<cRZAutoRefCount<cISC4RegionalCity>>& out, bool includeUnestablished) const override;
    void GetAllConnectedCities(std::list<cRZAutoRefCount<cISC4RegionalCity>>& out, bool includeUnestablished) const override;

    // --- deals ---
    cISC4NeighborConnection* FindConnectionSuitableForDeal(std::uint32_t dealType, const cISC4RegionalCity* city) override;
    bool ProposeDeal(std::uint32_t dealType, const cISC4RegionalCity* city, cISC4NeighborDeal** out) override;
    bool ProposeDeals(DealList& out, bool fromUI) override;
    bool AddDeal(cISC4NeighborDeal* deal) override;
    void SetUIDisableFlag(bool disabled) override { uiDisableFlag_ = disabled; }                    // @ 0x1000735d6
    void FindDealsAll(DealList& out) override;
    void FindDeals(std::uint32_t dealType, DealList& out) override;
    void FindDeals(std::uint32_t dealType, const cISC4RegionalCity* city, DealList& out) override;
    void MarkDealsDirty() override { dealsDirty_ = true; }                                           // @ 0x100075e8c
    std::int64_t GetTotalMonthlyNeighborDealIncome() override;
    std::int64_t GetTotalMonthlyNeighborDealExpense() override;
    void UpdateCommodityStats(cISC4City* city) override;
    double GetDealCost(std::uint32_t dealType, double amount, const cISC4RegionalCity* city) const override;
    double GetDealMaxAmount(std::uint32_t dealType, const cISC4RegionalCity* city) const override;
    cISCPropertyHolder* GetTuningExemplar() const override;

    // --- messages / streams / simulation ---
    bool DoMessage(cIGZMessage2* message) override;
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4NeighborsSimulator; }

    // Non-virtual helpers (the original's out-of-line functions).
    void update_deals_in_regional_cities(bool deals, bool connections);   // @ 0x10006f258
    void DoTerrainChanged(const SC4RectInt32& rect);                     // @ 0x10006f8fe
    void simulate_deals();                                               // @ 0x10006fb6a
    bool create_deal_proposition(std::uint32_t dealType, int amount, const cISC4RegionalCity* city,
                                 DealList& out, bool silent);         // @ 0x100072e2e

    // Test hooks.
    const ConnectionList& Connections() const { return connections_; }
    const DealList& Deals() const { return deals_; }
    bool DealsDirty() const { return dealsDirty_; }
    bool ConnectionsDirty() const { return connectionsDirty_; }
    bool UIDisabled() const { return uiDisableFlag_; }
    bool InsideSimulation() const { return inSimulateDeals_; }
    bool CityEstablished_() const { return cityEstablished_; }
    bool Initialized() const { return initialized_; }
    bool StaticInitializedFlag() const { return mbStaticInitialized; }
    float CurveValue(float x) const { return curve_.GetOutputValue(x); }
    static void ResetStaticInitialized() { mbStaticInitialized = false; }
    // @ 0x100b6ddd0: set by the first Init, never cleared.
    static inline bool mbStaticInitialized = false;

private:
    INeighborsHost* Host() const { return host_ ? host_ : NeighborsHost(); }

    INeighborsHost* host_ = nullptr;
    bool cityEstablished_ = false;                             // +0x18
    bool initialized_ = false;                                 // +0x19
    std::uint32_t refCount_ = 0;                               // +0x1c
    cISC4City* city_ = nullptr;                                // +0x20
    cIGZMessageServer2* messages_ = nullptr;                   // +0x28
    [[maybe_unused]] std::uint32_t unused2c_ = 0;              // +0x2c
    ConnectionList connections_;                               // +0x30
    DealList deals_;                                           // +0x48
    std::list<DealList::iterator> pendingRemovals_;            // +0x60
    cISCResExemplar* tuningExemplar_ = nullptr;                // +0x78
    cSC4ResponseCurve curve_;                                  // +0x80
    bool dealsDirty_ = true;                                   // +0xa0 (the constructor stores a 32-bit 1)
    bool connectionsDirty_ = false;                            // +0xa1
    bool uiDisableFlag_ = false;                               // +0xa2
    bool inSimulateDeals_ = false;                             // +0xa3
};

}  // namespace sc4
