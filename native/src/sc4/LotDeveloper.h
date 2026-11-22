// cSC4LotDeveloper — builds, lights, levels and demolishes the buildings of zoned lots (124 functions,
// 0x10013eb1e..0x100148eda, class id 0xa990bfe0). This port covers the terrain analysis the developer uses to
// decide whether a lot is buildable (altitude extremes, averages, flatness, slope, levelling), the tunables
// loaded from the simulator exemplar and the development-cost estimate. Construction / demolition,
// foundations, props, base textures and the Test* gates need the lot, occupant and business-unit interfaces and
// stay unrecovered.
//
// Original layout used here: +0x30 city/cell converter, +0x38 terrain leveller, +0x70 terrain, +0x78 vertices
// per row, +0x7c/+0x80 city extent in metres, +0x118 u16 condition threshold, +0x11c/+0x120/+0x124 floats,
// +0x128 flat tolerance, +0x12c float, +0x130/+0x134 u32, +0x138/+0x139 bool, +0x13c level tolerance,
// +0x140 maximum levelling amount, +0x148 development-cost response curve.
#pragma once
#include <deque>
#include <list>
#include <map>
#include <vector>
#include <cstdint>
#include <functional>

#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZSerializable.h"
#include "sc4/CommonIIDs.h"
#include "sc4/SCProperty.h"
#include "sc4/cSC4ResponseCurve.h"

namespace sc4 {

class cSC4Lot;
class cISC4BusinessUnit;
class cISC4Occupant;
struct BuildingProfile;
constexpr std::uint32_t kGZCLSID_cSC4LotDeveloper = 0xa990bfe0;
constexpr std::uint32_t kGZIID_cISC4LotDeveloper = 0x4818e5d2;
constexpr std::uint32_t kLotDeveloperTunablesKeyType = 0x6534284a;     // exemplar key {type, group, instance} (Mac i386 0x000eef60)
constexpr std::uint32_t kLotDeveloperTunablesKeyGroup = 0xe7e2c2db;
constexpr std::uint32_t kLotDeveloperTunablesKeyInstance = 0xe8da7677;

// tunable property ids
constexpr std::uint32_t kLotDevPropConditionThreshold = 0x47e2c400;    // u16 -> +0x118
constexpr std::uint32_t kLotDevPropConditionRatios = 0x47e2c401;       // float[3] -> +0x124, +0x120, +0x11c
constexpr std::uint32_t kLotDevPropFlatTolerance = 0xe900ea24;         // -> +0x128
constexpr std::uint32_t kLotDevPropFloat12c = 0xe900ea25;
constexpr std::uint32_t kLotDevPropU32_130 = 0xe900ea26;   // the default lot foundation type
constexpr std::uint32_t kLotDevPropU32_134 = 0x6916516b;   // the default building foundation type
constexpr std::uint32_t kLotDevPropBool138 = 0xe900ea27;
constexpr std::uint32_t kLotDevPropBool139 = 0xe900ea28;
constexpr std::uint32_t kLotDevPropLevelTolerance = 0xe900ea29;        // -> +0x13c
constexpr std::uint32_t kLotDevPropMaxLevelling = 0xe900ea2a;          // -> +0x140
constexpr std::uint32_t kLotDevPropNightAccumulator = 0xe900ea2b;   // -> the night-light accumulator (i386 +0xa4)
constexpr std::uint32_t kLotDevPropNightBatch = 0xe900ea2c;         // -> buildings lit per batch (i386 +0xa0)
constexpr std::uint32_t kLotDevPropCostCurve = 0x2a2d7824;             // interleaved x,y floats

constexpr float kLotMaxLevelling = 3.4028235e+38f;                     // "use the tunable" sentinel

struct LotCellRect { int x0 = 0, z0 = 0, x1 = 0, z1 = 0; };            // inclusive cells (SC4Rect<int>)
struct LotMetreRect { float x0 = 0, z0 = 0, x1 = 0, z1 = 0; };         // SC4Rect<float>

struct LotDeveloperTunables {
    std::uint16_t conditionThreshold = 0;   // +0x118
    float ratioC = 0.6f;                    // +0x11c
    float ratioB = 0.4f;                    // +0x120
    float ratioA = 0.2f;                    // +0x124
    float flatTolerance = 0;                // +0x128
    float value12c = 0;                     // +0x12c
    std::uint32_t lotFoundationType = 0;      // +0x130 (i386 +0xd8): the foundation of a steep lot, unless its configuration names one
    std::uint32_t buildingFoundationType = 0; // +0x134 (i386 +0xdc): the foundation of a steep building, unless it names one
    bool flag138 = false, flag139 = false;
    float levelTolerance = 0;               // +0x13c
    float maxLevelling = 0;                 // +0x140
    // The night lights (lot-developer.md §8): the constructor sets 200 and 8; the tunables overwrite them when present. The
    // accumulator is the running animation time of the batches (the 200 ms threshold itself is fixed).
    std::uint32_t nightAccumulator = 200;   // i386 +0xa4
    std::uint32_t nightBatch = 8;           // i386 +0xa0
};

// The building development simulator calls the Test* gates make (this+0x28 in the original).
class ILotGateServices {
public:
    virtual ~ILotGateServices() = default;
    virtual const BuildingProfile* ProfileOfBuilding(std::uint32_t buildingType) = 0;     // vslot 0x138 (borrowed)
    virtual std::uint32_t WealthOfDeveloperType(std::uint32_t developerType) = 0;          // vslot 0xf8 GetWealth (i386 0x7c)
};
constexpr std::uint32_t kLotPropFailureCounter = 0xe999b11f;       // lot property: consecutive failed power tests
constexpr std::uint32_t kLotPropDesirabilityCounter = 0xc999e206;  // ... failed desirability tests
constexpr std::uint32_t kLotPropWaterCounter = 0x8999b20b;         // ... failed water tests
constexpr std::uint32_t kLotPropTravelCounter = 0x8999b10b;        // ... failed road / commute tests
constexpr std::uint32_t kLotPropZot = 0xc999c45e;                  // the zot the lot shows (4 power, 5 water, 6 road, 7 commute)
constexpr std::uint32_t kPropPropZot = 0x48e95539;                 // on a zot prop: which zot it is

// What the daily pass (lot-developer.md §4.2, §5.1-5.6) asks of the city: the building development simulator (+0x18), power
// (+0x34), plumbing (+0x2c), traffic (+0x54), the city (+0x1c), the prop manager (+0x30) and the message server.
class ILotDailyServices {
public:
    virtual ~ILotDailyServices() = default;
    // BDS vslot 0x84 GetDeveloper(vslot 0x78 GetDeveloperType(purpose, wealth)): its purpose (vslot 0x1c) and desirability map
    // cell (vslot 0x34, grid 0x1c); false when there is no developer.
    virtual bool DeveloperDesirability(std::uint32_t purpose, std::uint32_t wealth, int x, int z, std::int32_t& developerPurpose,
                                       std::int16_t& desirability) = 0;
    // BDS vslot 0x80 GetDeveloper(purpose, wealth) -> vslot 0x50 GetWaterSupplyCap; false when there is no developer
    virtual bool DeveloperWaterSupplyCap(std::uint32_t purpose, std::uint32_t wealth, std::uint8_t& cap) = 0;
    // lot vslot 0x24 GetLotConfiguration -> vslot 0x2c GetGrowthStage; false without a configuration
    virtual bool LotGrowthStage(const cSC4Lot& lot, std::uint8_t& stage) = 0;
    virtual bool CheatIgnorePower() = 0;                    // BDS vslot 0xdc
    virtual bool CheatIgnorePlumbing() = 0;                 // BDS vslot 0xd8
    virtual bool IsPowered(int x, int z) = 0;               // power simulator vslot 0x94
    virtual bool IsCellWatered(int x, int z) = 0;           // plumbing simulator vslot 0x2c
    virtual std::uint8_t TripLengthAt(int x, int z) = 0;    // traffic vslot 0x34 GetTripLengthMap -> grid vslot 0x1c
    virtual void CellWidths(float& x, float& z) = 0;        // city vslots 0x16c / 0x170
    virtual std::int32_t SimDateNumber() = 0;               // simulator vslot 0x40
    virtual bool CityHasJobs() = 0;                         // demand (0x2000, 0x20000) QueryDemandValue > 0 (0x69247dc7)
    // the lot's building occupant (lot vslot 0xc0) and its bounding box (occupant vslot 0x28); false without one
    virtual bool BuildingBounds(const cSC4Lot& lot, float lo[3], float hi[3]) = 0;
    // prop manager vslot 0x64 AddLotProp(type, position, the view rotation SC4ViewUtilities +0x10); the manager keeps it
    virtual cISC4Occupant* AddZotProp(std::uint32_t type, const float position[3]) = 0;
    // RemoveLotProp (0x006C9780): every prop of the lot whose kPropPropZot is `zot`; true when one was removed
    virtual bool RemoveLotPropsWithZot(cSC4Lot& lot, std::uint32_t zot) = 0;
    virtual void PostLotNotification(cSC4Lot& lot, std::uint32_t data1) = 0;   // SC4PostMessage2(0x4c04de69, data1, 0, 0, 0, lot)
};

class IBaseLotDeveloperHost {
public:
    virtual ~IBaseLotDeveloperHost() = default;
    // terrain model (this+0x70)
    virtual float VertexHeight(int vertexIndex) = 0;                   // vslot 0x68
    virtual float VertexWaterHeight(int vertexIndex) = 0;              // vslot 0xf0
    virtual float HeightAt(float x, float z) = 0;                      // vslot 0x60
    virtual float WaterHeightAt(float x, float z) = 0;                 // vslot 0xf8
    virtual bool VertexIsLevelled(int vx, int vz) = 0;                 // vslot 0x128
    virtual int VerticesPerRow() = 0;                                  // +0x78
    virtual float CityWidthMetres() = 0;                               // +0x7c
    virtual float CityDepthMetres() = 0;                               // +0x80
    // terrain leveller (this+0x38) vslot 0x48; returns the number of vertices changed
    virtual int LevelVertices(float height, bool a, bool b, int x0, int z0, int x1, int z1, void* lot, int mode,
                              int extra) = 0;
    // city / cell converter (this+0x30) vslot 0x2f8: metres -> cell coordinates
    virtual void CellOfPosition(float x, float z, int& cellX, int& cellZ) = 0;
    // occupant bounding box (occupant vslot 0x50)
    virtual void OccupantBounds(void* occupant, float lo[3], float hi[3]) = 0;
};

class cISC4OccupantManager;
class cISC4OccupantFilter;
class cISC4LotBaseTextureOccupant;
// What the base-texture functions reach outside the class: SL::spZoneDeveloper (+0x40), SL::spOccupantManager (+0x48),
// SL::spZoneManager, SL::spCity and the occupant's class factory.
class ILotBaseTextureServices {
public:
    virtual ~ILotBaseTextureServices() = default;
    virtual cSC4Lot* LotAtCell(int x, int z) = 0;                       // zone developer vslot 0x28 GetLot(x, z, false)
    virtual cISC4OccupantManager* OccupantManager() = 0;                // +0x48 (borrowed)
    virtual std::uint32_t ZoneDragColor(int zoneType) = 0;              // zone manager vslot 0x78 (RRGGBBAA)
    virtual std::uint8_t ZoneDisplayAlpha() = 0;                        // zone manager vslot 0x80
    virtual int CityCellsX() = 0;                                       // city vslot 0x2e8 (also +0x88)
    virtual int CityCellsZ() = 0;                                       // city vslot 0x2f0 (also +0x84)
    // GZCOM()->CreateInstance(0xc97f987c, 0x97f6d98) or new cSC4LotBaseTextureOccupant; the caller's reference
    virtual cISC4LotBaseTextureOccupant* CreateBaseTextureOccupant() = 0;
};

class cSC4LotConfiguration;
struct cSC4LotConfigurationObject;
class cISC4LotConfigurationRef;
class cISC4BuildingOccupant;
class cISC4Occupant;
// What StartConstruction and the building placement reach outside the class: the building development simulator (+0x18), the
// city (+0x1c), the occupant manager (+0x28) with the blocking-lot filter (+0x58) and the demolition utility (+0x20), the terrain
// (+0x3c), and the lot configuration manager behind the lot's configuration reference.
class ILotConstructionServices {
public:
    virtual ~ILotConstructionServices() = default;
    // The lot keeps its configuration as the manager's COM reference (lot vslot 0x28 / 0x24).
    virtual cISC4LotConfigurationRef* ConfigurationRef(cSC4LotConfiguration* configuration) = 0;
    virtual cSC4LotConfiguration* Configuration(cISC4LotConfigurationRef* reference) = 0;
    virtual std::int32_t BuildingPurpose(std::uint32_t buildingType) = 0;                    // +0x18 vslot 0x9c -> profile +0
    virtual std::uint32_t BuildingWealth(std::uint32_t buildingType) = 0;                    // +0x18 vslot 0x9c -> profile +4
    // +0x18 vslot 0x1c AddNewBuildingOccupant(type, position, rotation): the inserted building (borrowed) or null.
    virtual cISC4BuildingOccupant* AddNewBuildingOccupant(std::uint32_t type, float position[3], int rotation) = 0;
    virtual void CellCornerToPosition(int cellX, int cellZ, float& x, float& z) = 0;          // +0x1c vslot 0x180
    virtual float TerrainAltitude(float x, float z) = 0;                                      // +0x3c vslot 0x30
    // +0x28 vslot 0x94 GetOccupantsByBBox(min, max, filter: the +0x58 blocking filter), each demolished by +0x20 vslot 0x20(1, occupant,
    // 0, 1, 0, 0, 0).
    virtual void DemolishBlockingOccupants(const float minimum[3], const float maximum[3], cISC4OccupantFilter* filter) = 0;
    // GZCOM cSC4ConstructionOccupant (0xa97f909e / 0x496e7636), its SetLot(lot) and +0x28 vslot 0x50 InsertOccupant(it, 0).
    virtual void StartConstructionOccupant(cSC4Lot* lot) = 0;
    virtual cSC4Lot* LotAt(int x, int z) = 0;                                                 // +0x24 vslot 0x14 GetLot(x, z, true)
    virtual std::uint32_t RandomBelow(std::uint32_t range) = 0;                               // cRZRandom::RandomUint32Uniform
    virtual bool LotTextureExists(std::uint32_t instance) = 0;                                // spRM vslot 0x38 {FSH 0x7ab50e44, 0x0986135e}
    // spLotConfigurationManager vslot 0x6c: the prop types an object may become (its families resolved).
    virtual void ObjectPropTypes(const cSC4LotConfigurationObject& object, std::vector<std::uint32_t>& out) = 0;
    // +0x30 spPropManager vslot 0x64 AddLotProp(type, position, rotation): the inserted prop (borrowed) or null.
    virtual cISC4Occupant* AddLotProp(std::uint32_t type, const float position[3], std::int32_t rotation) = 0;
    virtual bool FloraTypeValid(std::uint32_t type) = 0;                                      // +0x38 spFloraSim vslot 0x2c
    virtual void FloraTypesInFamily(std::vector<std::uint32_t>& out, std::uint32_t family) = 0;  // vslot 0x38 (hash-set order)
    virtual cISC4Occupant* AddFlora(std::uint32_t type, float position[3]) = 0;              // vslot 0x48 (type, position, false)
    virtual bool BuildingTypeValid(std::uint32_t type) { (void)type; return false; }         // +0x18 vslot 0x38 IsBuildingTypeValid
    // +0x18 vslot 0x44 GetBuildingTypesByFamily(set, family): the family's building types in the order they are added.
    virtual void BuildingTypesInFamily(std::vector<std::uint32_t>& out, std::uint32_t family) { (void)out; (void)family; }
    // The demolition slots (lot-developer.md §7):
    // GetFirstBuilding: +0x28 slot 41 GetOccupantsByStandardCityCells(lot rect, 0x278128a0, 1), the first entry (borrowed) or null.
    virtual cISC4Occupant* FirstBuildingOnLot(cSC4Lot* lot) { (void)lot; return nullptr; }
    virtual void RemoveBuildingProps(cISC4Occupant* building) { (void)building; }              // +0x30 prop manager slot 38
    virtual void RemoveLotProps(cSC4Lot* lot) { (void)lot; }                                   // +0x30 prop manager slot 28
    // Rubble (AddLotRubbleProps): the global generator's Gaussian range, and one rubble prop: +0x30 prop manager vslot 0x3c
    // CreateProp(type), Init, flag 0x40, the rotation, the position (moved so that the prop's box stays inside boxMin..boxMax on
    // x and z), then vslot 0x68 AddLotProp(lot, prop).
    virtual double RandomGaussianRange(double lo, double hi) { return (lo + hi) * 0.5; }
    virtual void AddRubbleProp(cSC4Lot* lot, std::uint32_t type, std::uint32_t rotation, const float position[3], const float* boxMin,
                               const float* boxMax) { (void)lot; (void)type; (void)rotation; (void)position; (void)boxMin; (void)boxMax; }
    virtual void RemoveLotFlora(cSC4Lot* lot) { (void)lot; }                                   // flora slot 20 DemolishFloraInCellArea(rect, 0)
    virtual void RemoveOccupant(cISC4Occupant* occupant, std::uint32_t flags) { (void)occupant; (void)flags; }   // +0x28 slot 21 (o, 1, flags)
    virtual void DeleteLot(cSC4Lot* lot) { (void)lot; }                                        // +0x24 lot manager slot 24
    virtual void MoveOccupant(cISC4Occupant* occupant, bool post) { (void)occupant; (void)post; }   // +0x28 slot 24 MoveOccupant
    // ---- the foundations (lot-developer foundation routines)
    // +0x18 BDS vslot 0x5c AddNewFoundationOccupant(type, usage, position, width, depth, rotation, bottom)
    virtual bool AddNewFoundationOccupant(std::uint32_t type, int usage, float position[3], float width, float depth, int rotation,
                                          float bottom) {
        (void)type; (void)usage; (void)position; (void)width; (void)depth; (void)rotation; (void)bottom;
        return false;
    }
    // GetFoundationByStandardCityCell(usage, x, z) (vslot 0xc4): the foundation occupant (type 0x68fd0c69) of a usage on a cell
    // (1 building, 2 lot, 3 prop, 4 flora), borrowed; null without one
    virtual cISC4Occupant* FoundationAt(int usage, int x, int z) { (void)usage; (void)x; (void)z; return nullptr; }
    // GetFoundationByPosition (vslot 0xc8): +0x28 slot 36 GetOccupantsByBBox(the vertical line at (x - 0.05, z + 0.05), 0x68fd0c69),
    // the first foundation of the usage (borrowed); null without one
    virtual cISC4Occupant* FoundationAtPosition(int usage, float x, float z) { (void)usage; (void)x; (void)z; return nullptr; }
    virtual std::uint32_t FoundationTypeOf(cISC4Occupant* foundation) { (void)foundation; return 0; }   // foundation vslot 0x10
    virtual void RemoveFoundation(cISC4Occupant* foundation) { (void)foundation; }   // +0x28 vslot 0x54 (occupant, true, 0)
    // +0x24 vslot 0x20 GetLotsSurroundingLot(lot, list, 0xf): the lots touching `lot`, borrowed
    virtual void LotsAround(cSC4Lot* lot, std::vector<cSC4Lot*>& out) { (void)lot; (void)out; }
    // +0x24 lot manager i386 vslot 0x28 GetLotsSurroundingRect(rect, list, edge): appends the lots along one edge (1, 2, 4, 8), borrowed
    virtual void LotsSurroundingRect(const std::int32_t rect[4], int edge, std::vector<cSC4Lot*>& out) { (void)rect; (void)edge; (void)out; }
    virtual bool CellHasWater(int x, int z) { (void)x; (void)z; return false; }        // spTerrain vslot 0x88
    virtual float WaterLevelAt(float x, float z) { (void)x; (void)z; return 0.0f; }    // spTerrain vslot 0x7c
    virtual void CellCenterToPosition(int cellX, int cellZ, float& x, float& z) {     // +0x1c vslot 0x184
        x = static_cast<float>(cellX) * 16.0f + 8.0f;
        z = static_cast<float>(cellZ) * 16.0f + 8.0f;
    }
    // cSC4LotRetainingWallOccupant::AddRetainingWallOccupant(lot, create) (create false removes the lot's wall occupant and clears
    // lot flag 4) / UpdateSurroundingRetainingWalls(lot)
    virtual void AddRetainingWallOccupant(cSC4Lot* lot, bool create) { (void)lot; (void)create; }
    virtual void UpdateRetainingWalls(cSC4Lot* lot) { (void)lot; }                    // cSC4LotRetainingWallOccupant::UpdateRetainingWalls
    virtual void UpdateSurroundingRetainingWalls(cSC4Lot* lot) { (void)lot; }
};

// What Init, Shutdown and the message handlers (lot-developer.md §1.3, §1.4, §3.2, §8, §9) reach outside the class: the services
// Init takes from the city (Mac i386 +0x14 message server, +0x18 building development simulator, +0x1c city, +0x20 demolition,
// +0x24 lot manager, +0x28 occupant manager, +0x2c plumbing, +0x30 prop manager, +0x34 power, +0x38 flora, +0x3c terrain, +0x54
// traffic), the simulator's agents, the animation-tick manager and the 24-hour clock.
class ILotDeveloperCityServices {
public:
    virtual ~ILotDeveloperCityServices() = default;
    // Init takes each of SL::spCity, spLotManager, spOccupantManager, spPropManager, spBuildingDevelopmentSim, spPowerSim,
    // spPlumbingSim, spFloraSim, spTerrain and spTrafficSim and fails when one is null (spDemolition is taken unchecked): the
    // name of the first one missing, null when all are there.
    virtual const char* MissingService() = 0;
    virtual void CityCellCounts(int& x, int& z) = 0;                       // city vslots 0x174 / 0x178
    virtual ILotGateServices* GateServices() = 0;                          // the building development simulator (+0x18)
    // ReadSimulatorTunableValues: the exemplar {0x6534284a, 0xe7e2c2db, 0xe8da7677} (null when absent), released after reading
    virtual const cISCPropertyHolder* AcquireTunables() = 0;
    virtual void ReleaseTunables(const cISCPropertyHolder* tunables) = 0;
    // SL::spSimulator vslot 0x4c AddAgent(target, level, name, flags) / city vslot 200 (the simulator) vslot 0x54 RemoveAgent;
    // false when there is no simulator
    virtual bool AddSimulatorAgent(cIGZMessageTarget2* target, int level, const char* name, std::uint32_t flags) = 0;
    virtual void RemoveSimulatorAgent(cIGZMessageTarget2* target) = 0;
    // SL::spMS2 vslots 0x14 / 0x18; MessageServerPresent: SL::spMS2 is not null
    virtual bool MessageServerPresent() = 0;
    virtual void AddNotification(cIGZMessageTarget2* target, std::uint32_t type) = 0;
    virtual void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) = 0;
    virtual void PostMessage(std::uint32_t type) = 0;                      // a new standard message of that type, MessagePost(m, false)
    // SL::ATM() vslot 0x14 AddTickableObject / vslot 0x18 RemoveTickableObject (target, priority)
    virtual void AddTickable(cIGZMessageTarget2* target, std::uint32_t priority) = 0;
    virtual void RemoveTickable(cIGZMessageTarget2* target, std::uint32_t priority) = 0;
    virtual int VisualTimeMode() = 0;                                      // sp24HourClock vslot 0x18 (0 real, 1 noon, 2 midnight)
    virtual cSC4Lot* LotAt(int x, int z, bool exact) = 0;                  // lot manager (+0x24) vslot 0x14 GetLot (borrowed)
    // lot manager vslot 0x1c GetLotsInCellRect(list, x1, z1, x2, z2, true): the lots touching the rectangle, borrowed
    virtual void LotsInCellRect(int x1, int z1, int x2, int z2, std::vector<cSC4Lot*>& out) = 0;
    virtual cISC4OccupantManager* OccupantManager() = 0;                   // +0x28 (borrowed)
};

// The lot developer's own interface (class id 0xa990bfe0, iid 0x4818e5d2). Only the COM identity is
// recovered; the concrete methods are called directly, so the interface carries no extra slots.
class cISC4LotDeveloper : public cIGZUnknown {
public:
    virtual ~cISC4LotDeveloper() = default;
};

class cSC4LotDeveloper : public cISC4LotDeveloper, public cIGZMessageTarget2, public cIGZSerializable {
public:
    explicit cSC4LotDeveloper(IBaseLotDeveloperHost* host) : host_(host) {}

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    // Mac i386 0x000f1c6c / 0x000f5034 (lot-developer.md §1.3, §1.4): the services, the three agents, the nine subscriptions, the
    // animation tick, the boundary rules and a refresh of every lot's base texture; Shutdown undoes them.
    static void SetCityServices(ILotDeveloperCityServices* services);
    bool Init();
    bool Shutdown();
    bool IsInitialized() const { return initialized_; }
    // Mac i386 0x000f5d6a (§3.2): the day agent, the occupant, lot, power, clock, city-established, traffic-map and animation
    // messages. 0x278e6003 (terrain heights) reads its data and does nothing; the level 0 and level 6 agents fall to the default.
    bool DoMessage(cIGZMessage2* message) override;
    // ---- the night lights (§8; Mac i386 0x000f2dd0 / 0x000f5ac8 / 0x000f5726 / 0x000f57f0 / 0x000f1398)
    void DoOccupantInserted(cISC4Occupant* occupant);
    void DoOccupantRemoved(cISC4Occupant* occupant);
    void DoAnimationTick(std::uint32_t milliseconds);
    void SwitchNightChangeBuildings(int mode);                  // eNightChangeMode: 1 day, 2 night
    void LightBuildingAndProps(cISC4BuildingOccupant* building, bool night);
    // §9 (Mac i386 0x000f323e): the lots of a rectangle whose power changed
    void DoPowerChanged(const std::int32_t rect[4]);
    // slot 47 (Mac i386 0x000f0196): a changed flag walks every lot (empty textures when forced, else the configuration's)
    void SetForceEmptyLotBaseTextures(bool force);
    // slot 42 (Mac i386 0x000f1bee / 0x000ef524): the rule keyed by the four sides (0 own lot, 1 other, 2 the front), else key 0
    void InitBoundaryRules();
    void GetBoundaryLotTexture(const int sides[4], std::uint32_t& texture, std::uint8_t& rotation, bool& flag) const;
    // Read-only views for the native diagnostics: the night deque, its mode and cursor, and how many of each message DoMessage saw
    // (a port counter, not in the original).
    std::size_t NightBuildingCount() const { return nightBuildings_.size(); }
    std::uint32_t NightMode() const { return nightMode_; }
    std::uint32_t NightCursor() const { return nightCursor_; }
    bool LitFlag() const { return litFlag_; }
    const std::map<std::uint32_t, std::uint32_t>& MessageCounts() const { return messageCounts_; }
    std::uint32_t PowerChangedLots() const { return powerChangedLots_; }
    // lot-developer.md §4.1 (0x006CD770): perDay = lots / 28 + 1 lots a day, in the lot manager's list order; false = not done today.
    bool DoDailySimulation();
    // §4.2 (0x006CD3A0, Mac i386 0x000f4116): the network refresh, the state tests (desirability, power, water, travel), the
    // condition and the zots, then the business-unit step. PARTIAL: TestCoffeeBar (a cheat) is not ported.
    bool UpdateLot(cSC4Lot* lot);
    // Business units (Mac i386 0x000f3dbc, vslots 0x8c / 0x90 / 0x94 = 0x0064ff94 / 0x000f4dec / 0x000ee72e): an industrial lot
    // joins an adjoining lot of its purpose that reaches a road (a farm field joins a lot with a building); the unit's travel
    // populations gather on its designate, a member with road access.
    void PickUnitToJoin(cSC4Lot* lot);
    // slot 36 (Mac i386 0x000f39ae, lot-developer.md 5.7): an industrial lot (purpose 6..8) on `rect` {x0, z0, x1, z1} needs an anchor
    // building unless a lot on the rect is already one (occupant group 0x3000) or a lot along its edges has a building of the same
    // purpose under construction or later (or in a business unit).
    bool NeedsAnchorBuilding(const std::int32_t rect[4], std::int32_t purpose) const;
    void RemoveLotFromItsBusinessUnit(cSC4Lot* lot);
    void UpdateTravelDesignates(cISC4BusinessUnit*& unit);
    void UpdateTravelPopulations(cISC4BusinessUnit* unit);
    static void SetDailyServices(ILotDailyServices* services);
    // §5.1-5.4 (Mac i386 0x000ed514 / 0x000f0de8 / 0x000f0c2c / 0x000f0950); TestPower: 2 powered, 1 within grace, 0 failed
    bool TestDesirability(cSC4Lot& lot, int x, int z);
    int TestPower(cSC4Lot& lot, int x, int z);
    bool TestWater(cSC4Lot& lot, int x, int z);
    bool TestTravel(cSC4Lot& lot, const BuildingProfile& profile, int x, int z);
    // §5.6 (Mac i386 0x000f0752 / 0x000f0684)
    void AddBuildingZotProp(cSC4Lot& lot, std::uint32_t propType, std::uint32_t zot);
    void RemoveBuildingZotProp(cSC4Lot& lot, std::uint32_t zot);
    static bool IsEmptyLotSuitableForZot(const cSC4Lot& lot, int x, int z);
    // The lot manager's lot list (the zone developer's), for the daily pass.
    static void SetLotListProvider(const std::list<cSC4Lot*>* (*provider)());
    bool Write(cIGZOStream& stream) const override;   // @ 0x10014009a
    bool Read(cIGZIStream& stream) override;          // @ 0x100140242
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4LotDeveloper; }

    // tunables (ReadSimulatorTunableValues): reads from the simulator exemplar when it exists
    void ReadSimulatorTunableValues();                                  // through the city services (Init)
    void ReadSimulatorTunableValues(const cISCPropertyHolder* exemplar);
    LotDeveloperTunables& Tunables() { return tunables_; }
    const LotDeveloperTunables& Tunables() const { return tunables_; }

    float GetTerrainAltitudeByVertex(int vx, int vz, bool includeWater) const;
    float GetLowestOrHighestAreaAltitude(const LotCellRect& area, bool lowest, bool includeWater) const;
    float GetLowestOrHighestAreaAltitude(const LotMetreRect& area, bool lowest, bool includeWater) const;
    float GetAverageAreaAltitude(const LotCellRect& area, bool includeWater) const;
    int LevelAreaInternalVertices(const LotCellRect& area, float height, bool skipLevelled, bool force,
                                  void* lot);
    int LevelBorderingVertices(const LotCellRect& area, float height, void* lot);
    bool AttemptAreaLevelling(const LotCellRect& area, float unusedHeight, bool includeWater, bool flag, float maxSlope,
                              void* lot);
    void GetOccupantCellCoverage(void* occupant, LotCellRect& out) const;
    bool IsAreaDeveloped(const LotCellRect&) const { return false; }        // @ 0x1001476bc (the original returns 0)
    // The lot forms: each reads the lot's cell rectangle (lot vslot 0x70 -> bytes +0x40..+0x43) and calls the area version.
    static LotCellRect RectOfLot(const cSC4Lot& lot);
    bool IsLotDeveloped(const cSC4Lot& lot) const;                                   // @ 0x1001476c4
    bool IsLotFlat(const cSC4Lot& lot, bool includeWater) const;                     // @ 0x1001477ac
    float GetLotAltitudeRange(const cSC4Lot& lot, bool includeWater) const;          // @ 0x1001478a6
    float GetLotSlope(const cSC4Lot& lot, bool includeWater) const;                  // @ 0x100147a92
    float GetOccupantSlope(void* occupant, bool includeWater) const;                 // @ 0x100147ad4
    bool IsAreaFlat(const LotCellRect& area, bool includeWater) const;
    float GetAreaAltitudeRange(const LotCellRect& area, bool includeWater) const;
    float GetAreaSlope(const LotCellRect& area, bool includeWater) const;
    std::int64_t GetLotDevelopmentCost(const LotCellRect& area) const;

    // ---- lot condition gate (@ 0x1001483c8): the condition grade the lot should have now. An abandoned lot (state 4) that has
    // stood long enough steps down one grade through {1, 1, 2, 3}; a lot in state 3 steps by the gap between the building's
    // wealth and its occupants' and by its occupancy; everything else keeps its condition.
    void SetGateServices(ILotGateServices* services) { gates_ = services; }
    std::uint32_t TestCondition(cSC4Lot& lot) const;
    // ---- abandonment (vslots 0x40 / 0x48, i386 0x000ec736 / 0x000ec764): the tract developer's decline abandons a building
    // (state 4 with the reason) and its take-over moves a developer back in (state 3 with the developer's wealth, a condition
    // from the power failures and the wealth gap, the developer's population at the lot's free capacity).
    void AbandonBuilding(cSC4Lot* lot, std::uint8_t vacateReason);
    void ReoccupyBuilding(cSC4Lot* lot, std::uint32_t developerType);

    // ---- lot base textures (0x100145e4c..0x100148ec2)
    static void SetBaseTextureServices(ILotBaseTextureServices* services);
    bool GetLotBaseTextureOccupant(cSC4Lot* lot, cISC4LotBaseTextureOccupant*& out, bool create);   // @ 0x100145e4c
    // The terrain under a developed lot changed (the building development simulator's DoTerrainChanged): the lot is refitted.
    bool UpdateLotElements(cSC4Lot* lot, std::uint32_t reason);                      // vslot 0x94 @ i386 0x000f2b1c
    void UpdateLotObjectAltitudes(cSC4Lot* lot);                                    // @ i386 0x000f28ce ("Alitudes" in the original)
    void UpdateLotBaseTextureOccupant(cSC4Lot* lot, bool force);                     // vslot 0xb4 @ i386 0x000ecfd0
    // cSC4Lot::GetLotConfigurationObject (i386 0x0011e53e): the configuration object whose position is the occupant's (x, z within 0.1).
    const cSC4LotConfigurationObject* LotConfigurationObjectOf(cSC4Lot* lot, cSC4LotConfiguration* configuration, cISC4Occupant* occupant);
    void SetEmptyLotBaseTexture(cSC4Lot* lot);                                                    // @ 0x100146066
    void RemoveLotBaseTexture(cSC4Lot* lot);                                                      // @ 0x10014652e
    // Demolition (lot-developer.md §7; i386 0x000f1b84 / 0x000f392c / 0x000ec5f6 / 0x000f12cc / 0x000f385c).
    bool StartDemolishLot(cSC4Lot* lot);                                                          // slot 12
    void EndDemolishLot(cSC4Lot* lot);                                                            // slot 13
    void ExplodeLot(cSC4Lot* lot, bool burnt);                                                    // slot 14
    // Mac i386 0x000ee31e: rubble piles on a destroyed lot, inside the box the building stood in (burnt: the charred kinds).
    void AddLotRubbleProps(cSC4Lot* lot, const float* boxMin, const float* boxMax, bool burnt);
    cISC4Occupant* GetFirstBuilding(cSC4Lot* lot);                                                // slot 38
    bool ClearLotBuildings(cSC4Lot* lot, std::uint32_t state, bool removeFoundation, std::uint32_t flags);
    void ClearLot(cSC4Lot* lot);
    bool GetForceEmptyLotBaseTextures() const { return forceEmptyLotBaseTextures_; }             // @ 0x100148ec2

    // ---- construction (i386 0x000f48d0 and the placement it uses)
    static void SetConstructionServices(ILotConstructionServices* services);
    bool StartConstruction(cSC4Lot* lot, cSC4LotConfiguration* configuration, std::uint32_t buildingType, bool construct);   // vslot 0x18
    bool ConstructDefaultLot(cSC4Lot* lot, cSC4LotConfiguration* configuration, std::uint32_t buildingType, bool construct);  // i386 0x000f30f0
    // ConstructDefaultLot's pick: the first id of a __gnu_cxx::hash_set holding `ids` (a building type as is, a family as its
    // member types, in that order): identity hash over 193 buckets, a new id at the head of its bucket. 0 when the set is empty.
    static std::uint32_t FirstBuildingTypeOf(const std::vector<std::uint32_t>& ids, const std::function<bool(std::uint32_t)>& isType,
                                             const std::function<void(std::vector<std::uint32_t>&, std::uint32_t)>& familyMembers);
    void ClearLotBlockingObjects(cSC4Lot* lot);                                                                       // i386 0x000f044a
    // The lot-state-changed message (0xe8bda2e7) for the lot whose snapshot it carries: a lot reaching state 3 or 4 gets its
    // configuration's base textures, one finishing construction (2 -> 3) its props, flora and network nodes (DoMessage branch).
    void DoLotStateChanged(const cSC4Lot& snapshot, std::uint32_t oldState, std::uint32_t newState);
    void SetNormalLotBaseTexture(cSC4Lot* lot, cSC4LotConfiguration* configuration);                                 // vslot 0xb0
    void AddLotProps(cSC4Lot* lot, cSC4LotConfiguration* configuration);                                             // i386 0x000ef4c0
    cISC4Occupant* SelectAndCreateLotProp(cSC4Lot* lot, std::uint32_t objectIndex);                                 // i386 0x000ef38a
    cISC4Occupant* CreateLotProp(cSC4Lot* lot, std::uint32_t objectIndex, std::uint32_t propType);                 // i386 0x000ee79c (slot 63)
    void AddLotFlora(cSC4Lot* lot, cSC4LotConfiguration* configuration);                                             // i386 0x000f2ffe
    std::uint32_t PickFloraTypeForLot(const std::vector<std::uint32_t>& types);                                      // i386 0x000f2efa
    std::uint32_t PickLotBaseTextureForLot(cSC4Lot* lot, const std::vector<std::uint32_t>& textures);               // i386 0x000ef182
    void GetPositionForBuilding(cSC4Lot* lot, float position[3], std::int32_t& rotation);                            // vslot 0xa0
    void GetPositionForLotConfigurationObject(cSC4Lot* lot, cSC4LotConfiguration* configuration,
                                              const cSC4LotConfigurationObject* object, float position[3],
                                              std::int32_t& rotation);                                                // vslot 0x9c


    // ---- foundations
    bool AddLotFoundation(cSC4Lot* lot, cSC4LotConfiguration* configuration, bool fixedAltitude, bool level, bool walls);   // vslot 0xdc @ i386 0x000ef2d0
    bool AddLotFoundation(cSC4Lot* lot, std::uint32_t type, bool fixedAltitude, bool level, bool walls);   // vslot 0xd8 @ i386 0x000f363a
    bool AddBuildingFoundation(cISC4BuildingOccupant* building, cSC4LotConfiguration* configuration);   // vslot 0xec @ i386 0x000ecdc2
    bool AddBuildingFoundation(cISC4BuildingOccupant* building, std::uint32_t type);                 // vslot 0xe8 @ i386 0x000eccde
    cISC4Occupant* GetLotFoundation(cSC4Lot* lot);                     // vslot 0xcc @ i386 0x000ec8da (borrowed)
    bool RemoveLotFoundation(int x, int z);                            // vslot 0xd4 @ i386 0x000ecaca
    bool RemoveLotFoundation(cSC4Lot* lot);                            // @ i386 0x000ecb8c
    void UpdateLotFoundation(cSC4Lot* lot);                            // vslot 0xf4 @ i386 0x000ecbee
    // GetFoundationByPosition: the foundation of a usage on the vertical line at (x - 0.05, z + 0.05) (borrowed, null without one)
    cISC4Occupant* GetFoundationByPosition(int usage, float x, float z);   // vslot 0xc8 @ i386 0x000f10a6
    bool RemoveFoundation(int usage, float x, float z);                    // vslot 0xd0 @ i386 0x000ec9c8
    bool RemoveBuildingFoundation(cISC4Occupant* building);                // vslot 0xf0 @ i386 0x000ece44
    cISC4Occupant* GetFoundationByOccupant(cISC4Occupant* occupant);       // vslot 0xc0 @ i386 0x000ec876 (borrowed)
    bool IsLotObjectFoundationRequired(cISC4Occupant* occupant);           // @ i386 0x000ecca2 (PC slot 56)
    void UpdateLotRetainingWalls(cSC4Lot* lot);                            // vslot 0xf8 @ i386 0x000ece8c
    void UpdateSurroundingLotFoundations(cSC4Lot* lot);                // @ i386 0x000f2e7c
    float GetLotFoundationBottomAltitude(cSC4Lot* lot);                // @ i386 0x000f34de
    float PickLotFoundationAltitude(cSC4Lot* lot, const float corners[4]);   // @ i386 (the bottom / highest arguments are unused)
    void GetCornerAltitudes(const LotCellRect& rect, float out[4], bool includeWater) const;   // @ i386 GetCornerAltitudes

private:
    IBaseLotDeveloperHost* host_;
    ILotGateServices* gates_ = nullptr;
    std::uint32_t refCount_ = 0;               // COM refcount (shared by the three sub-objects)
    LotDeveloperTunables tunables_;
    cSC4ResponseCurve developmentCostCurve_;                           // +0x148
    // What the city file keeps of the developer (stream version 1): the vertices per row and the city extent Init takes from the
    // terrain (Mac i386 +0x40 / +0x44 / +0x48), and the night-light deque (+0x64: every building, one reference each) with its
    // cursor (+0x8c, kept as an index), its mode (+0x9c: 0 idle, 1 switching to day, 2 switching to night) and the lit state new
    // buildings get (+0xa8).
    std::int32_t streamVerticesPerRow_ = 0;
    float streamCityWidth_ = 0.0f, streamCityDepth_ = 0.0f;
    std::deque<cISC4BuildingOccupant*> nightBuildings_;
    std::uint32_t nightCursor_ = 0;
    std::uint32_t nightMode_ = 0;
    bool forceEmptyLotBaseTextures_ = false;                           // i386 +0x5c
    bool litFlag_ = false;
    int dailyState_ = 0;                                               // §4.1: 0 / 3 start a cycle, 1 working
    std::uint32_t visited_ = 0;
    cSC4Lot* nextLot_ = nullptr;                                       // the iterator: the next lot to visit (one reference)
    // §2.2: the network / unit latches (set by network inserts and removals) and their cycle snapshots, the zot embargo after a
    // city is established and the commute check the traffic map enables
    bool netChanged_ = true, unitChanged_ = true;                      // i386 +0x5d / +0x5e
    bool netSnap_ = false, unitSnap_ = false, zotsAllowed_ = false;    // i386 +0xb8 / +0xb9 / +0xba
    std::uint32_t noZotUntilDay_ = 0;                                  // i386 +0x60
    bool canCheckCommute_ = false;                                     // i386 +0x5f
    // Init (i386 +0xc): the services it holds (+0x14 the message server; the others are the city services' to answer), the cell
    // counts (+0x50 / +0x4c), the blocking-lot filter (+0x58) and the boundary rules (+0xfc).
    bool initialized_ = false;
    bool messageServerHeld_ = false;
    int cellCountX_ = 0, cellCountZ_ = 0;
    cISC4OccupantFilter* blockingFilter_ = nullptr;
    struct BoundaryLotTextureRule {
        std::uint32_t texture = 0;
        std::uint8_t rotation = 0;
        bool flag = false;
    };
    std::map<int, BoundaryLotTextureRule> boundaryRules_;
    std::map<std::uint32_t, std::uint32_t> messageCounts_;
    std::uint32_t powerChangedLots_ = 0;
public:
    ~cSC4LotDeveloper();
};

}  // namespace sc4
