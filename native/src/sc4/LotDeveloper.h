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
#include <vector>
#include <cstdint>

#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZSerializable.h"
#include "sc4/CommonIIDs.h"
#include "sc4/SCProperty.h"
#include "sc4/cSC4ResponseCurve.h"

namespace sc4 {

class cSC4Lot;
struct BuildingProfile;
constexpr std::uint32_t kGZCLSID_cSC4LotDeveloper = 0xa990bfe0;
constexpr std::uint32_t kGZIID_cISC4LotDeveloper = 0x4818e5d2;
constexpr std::uint32_t kLotDeveloperTunablesKeyType = 0xe7e2c2db;     // exemplar key {type, group, instance}
constexpr std::uint32_t kLotDeveloperTunablesKeyGroup = 0x6534284a;
constexpr std::uint32_t kLotDeveloperTunablesKeyInstance = 0xe8da7677;

// tunable property ids
constexpr std::uint32_t kLotDevPropConditionThreshold = 0x47e2c400;    // u16 -> +0x118
constexpr std::uint32_t kLotDevPropConditionRatios = 0x47e2c401;       // float[3] -> +0x124, +0x120, +0x11c
constexpr std::uint32_t kLotDevPropFlatTolerance = 0xe900ea24;         // -> +0x128
constexpr std::uint32_t kLotDevPropFloat12c = 0xe900ea25;
constexpr std::uint32_t kLotDevPropU32_130 = 0xe900ea26;
constexpr std::uint32_t kLotDevPropU32_134 = 0x6916516b;
constexpr std::uint32_t kLotDevPropBool138 = 0xe900ea27;
constexpr std::uint32_t kLotDevPropBool139 = 0xe900ea28;
constexpr std::uint32_t kLotDevPropLevelTolerance = 0xe900ea29;        // -> +0x13c
constexpr std::uint32_t kLotDevPropMaxLevelling = 0xe900ea2a;          // -> +0x140
constexpr std::uint32_t kLotDevPropU32_0f0 = 0xe900ea2b;
constexpr std::uint32_t kLotDevPropU32_0ec = 0xe900ea2c;
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
    std::uint32_t value130 = 0, value134 = 0;
    bool flag138 = false, flag139 = false;
    float levelTolerance = 0;               // +0x13c
    float maxLevelling = 0;                 // +0x140
    std::uint32_t value0f0 = 0, value0ec = 0;
};

// The building development simulator calls the Test* gates make (this+0x28 in the original).
class ILotGateServices {
public:
    virtual ~ILotGateServices() = default;
    virtual const BuildingProfile* ProfileOfBuilding(std::uint32_t buildingType) = 0;     // vslot 0x138 (borrowed)
};
constexpr std::uint32_t kLotPropFailureCounter = 0xe999b11f;       // lot property: consecutive failed power tests

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
    // this vslot 0x150 GetBoundaryLotTexture: the lot developer's rule map (+0x170) holds the same 47 rules as the zone
    // developer's (both InitBoundaryRules build the identical table), so the host answers from the recovered one.
    virtual void BoundaryLotTexture(const int sides[4], std::uint32_t& texture, std::uint8_t& rotation, bool& flag) = 0;
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
    // +0x28 vslot 0x94 GetOccupantsByBBox(min, max, +0x58 blocking filter), each demolished by +0x20 vslot 0x20(1, occupant, 0, 1, 0, 0, 0).
    virtual void DemolishBlockingOccupants(const float minimum[3], const float maximum[3]) = 0;
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
    bool DoMessage(cIGZMessage2* message) override;   // unrecovered
    bool Write(cIGZOStream& stream) const override;   // @ 0x10014009a
    bool Read(cIGZIStream& stream) override;          // @ 0x100140242
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4LotDeveloper; }

    // tunables (ReadSimulatorTunableValues): reads from the simulator exemplar when it exists
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

    // ---- lot base textures (0x100145e4c..0x100148ec2)
    static void SetBaseTextureServices(ILotBaseTextureServices* services);
    bool GetLotBaseTextureOccupant(cSC4Lot* lot, cISC4LotBaseTextureOccupant*& out, bool create);   // @ 0x100145e4c
    void SetEmptyLotBaseTexture(cSC4Lot* lot);                                                    // @ 0x100146066
    void RemoveLotBaseTexture(cSC4Lot* lot);                                                      // @ 0x10014652e
    bool GetForceEmptyLotBaseTextures() const { return forceEmptyLotBaseTextures_; }             // @ 0x100148ec2

    // ---- construction (i386 0x000f48d0 and the placement it uses)
    static void SetConstructionServices(ILotConstructionServices* services);
    bool StartConstruction(cSC4Lot* lot, cSC4LotConfiguration* configuration, std::uint32_t buildingType, bool construct);   // vslot 0x18
    void ClearLotBlockingObjects(cSC4Lot* lot);                                                                       // i386 0x000f044a
    // The lot-state-changed message (0xe8bda2e7) for the lot whose snapshot it carries: a lot reaching state 3 or 4 gets its
    // configuration's base textures, one finishing construction (2 -> 3) its props, flora and network nodes (DoMessage branch).
    void DoLotStateChanged(const cSC4Lot& snapshot, std::uint32_t oldState, std::uint32_t newState);
    void SetNormalLotBaseTexture(cSC4Lot* lot, cSC4LotConfiguration* configuration);                                 // vslot 0xb0
    void AddLotProps(cSC4Lot* lot, cSC4LotConfiguration* configuration);                                             // i386 0x000ef4c0
    cISC4Occupant* SelectAndCreateLotProp(cSC4Lot* lot, std::uint32_t objectIndex);                                 // i386 0x000ef38a
    void AddLotFlora(cSC4Lot* lot, cSC4LotConfiguration* configuration);                                             // i386 0x000f2ffe
    std::uint32_t PickFloraTypeForLot(const std::vector<std::uint32_t>& types);                                      // i386 0x000f2efa
    std::uint32_t PickLotBaseTextureForLot(cSC4Lot* lot, const std::vector<std::uint32_t>& textures);               // i386 0x000ef182
    void GetPositionForBuilding(cSC4Lot* lot, float position[3], std::int32_t& rotation);                            // vslot 0xa0
    void GetPositionForLotConfigurationObject(cSC4Lot* lot, cSC4LotConfiguration* configuration,
                                              const cSC4LotConfigurationObject* object, float position[3],
                                              std::int32_t& rotation);                                                // vslot 0x9c


private:
    bool AddLotFoundation(cSC4Lot* lot, cSC4LotConfiguration* configuration);                    // vslot 0xdc (PARTIAL)
    void AddBuildingFoundation(cISC4BuildingOccupant* building, cSC4LotConfiguration* configuration);   // vslot 0xec (PARTIAL)
    IBaseLotDeveloperHost* host_;
    ILotGateServices* gates_ = nullptr;
    std::uint32_t refCount_ = 0;               // COM refcount (shared by the three sub-objects)
    LotDeveloperTunables tunables_;
    cSC4ResponseCurve developmentCostCurve_;                           // +0x148
    // What the city file keeps of the developer (stream version 1): the extent it was working on and the queue of buildings it
    // was constructing with the position of the one in progress.
    std::int32_t streamVerticesPerRow_ = 0;                            // +0x78
    float streamCityWidth_ = 0.0f, streamCityDepth_ = 0.0f;            // +0x7c / +0x80
    std::deque<cIGZUnknown*> constructionQueue_;                       // +0xa8 (one reference each)
    std::uint32_t queueCursor_ = 0;                                    // +0xd8/+0xe0: the iterator, as an index into the queue
    std::uint32_t stage_ = 0;                                          // +0xe8
    bool forceEmptyLotBaseTextures_ = false;                           // +0xa0
    bool flagF4_ = false;                                              // +0xf4
public:
    ~cSC4LotDeveloper() { for (cIGZUnknown* occupant : constructionQueue_) if (occupant) occupant->Release(); }
private:
};

}  // namespace sc4
