// cSC4BuildingDevelopmentSimulator — owns the catalogue of building types (profiles loaded from building
// exemplars), the developers that grow lots, and the construction resources that bound how much can be
// under construction at once. 113 functions at 0x10014bddc..0x10015392a. This port covers the catalogue
// (exemplar scan, profiles, families, names, zone capacity map), the developer / wealth type mapping,
// construction resources, growth marks, cheats and the persisted state; the growth planner
// (PlanGrowth/Grow/ExecuteGrowthPlan/PlanDecline, tract and lot developers) remains unrecovered.
// Object layout: +0x80 tract developer map, +0xb8 developers (map type -> growth developer),
// +0xe0 construction resource counter, +0x178 stage population multiplier, +0x188 day of last successful
// build per developer type, +0x1b0 stage multiplier table, +0x1c9..0x1cd cheat bools.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <unordered_set>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

#include "gz/cGZPersistResourceKey.h"
#include "gz/cIGZMessage2.h"
#include "rz/cRZString.h"
#include "sc4/RecordTape.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/SCProperty.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4BuildingDevelopmentSimulator = 0x8990be7a;
constexpr std::uint32_t kExemplarTypeBuilding = 2;
constexpr std::uint32_t kExemplarTypeFoundation = 0x11;
constexpr std::uint32_t kExemplarTypeLotRetainingWall = 0x15;
constexpr std::uint32_t kMessageBuildingTypesUpdated = 0xa783ebc8;

// building exemplar property ids
constexpr std::uint32_t kBuildingPropWealth = 0x27812832;
constexpr std::uint32_t kBuildingPropPurpose = 0x27812833;
constexpr std::uint32_t kBuildingPropCapacities = 0x27812834;
constexpr std::uint32_t kBuildingPropPartialCapacity = 0x27812836;
constexpr std::uint32_t kBuildingPropGrowthStage = 0x499afa38;
constexpr std::uint32_t kBuildingPropCost = 0x49cac341;
constexpr std::uint32_t kBuildingPropName = 0x8a416a99;
constexpr std::uint32_t kBuildingPropCustomName = 0x899afbad;
constexpr std::uint32_t kBuildingPropFamilies = 0x27812870;
constexpr std::uint32_t kBuildingPropSize = 0x27812810;
constexpr std::uint32_t kBuildingPropGroups = 0xaa1dd396;

// cISC4BuildingDevelopmentSimulator::DeveloperType values (purpose x wealth)
enum DeveloperTypeValue : std::uint32_t {
    kDevResidentialLow = 0x1010, kDevResidentialMedium = 0x1020, kDevResidentialHigh = 0x1030,
    kDevCommercialServiceLow = 0x3110, kDevCommercialServiceMedium = 0x3120, kDevCommercialServiceHigh = 0x3130,
    kDevCommercialOfficeMedium = 0x3320, kDevCommercialOfficeHigh = 0x3330,
    kDevIndustrialAgriculture = 0x4100, kDevIndustrialDirty = 0x4200, kDevIndustrialManufacturing = 0x4300,
    kDevIndustrialHighTech = 0x4400,
};

class IConstructionResources {
public:
    virtual ~IConstructionResources() = default;
    virtual float Total() const = 0;                         // vslot 0x40
    virtual float Used() const = 0;                          // vslot 0x48
    virtual void Add(float amount) = 0;                      // vslot 0x88
    virtual void Update() = 0;                               // vslot 0xf0
    // Init (vslot 0x18) and the two setters Init / DoMonthlySimulation use (cSC4Demand vslots 0x90 / 0x98).
    virtual void Init() {}
    virtual void SetTotal(float total) { (void)total; }      // vslot 0x90 SetSupplyValue
    virtual void SetUsed(float used) { (void)used; }         // vslot 0x98 SetDemandValue
};

class IBuildingPlacementServices;
class ITractDeveloperRef;
class cSC4BuildingDevelopmentSimulator;
class IBuildingDevelopmentSimHost {
public:
    virtual ~IBuildingDevelopmentSimHost() = default;
    virtual void ForEachExemplarKey(const std::function<void(const cGZPersistResourceKey&)>& fn) = 0;
    virtual cISCPropertyHolder* AcquireExemplar(const cGZPersistResourceKey& key) = 0;     // borrowed
    virtual void ReleaseExemplar(cISCPropertyHolder* holder) = 0;
    virtual std::int64_t MaintenanceCost(cISCPropertyHolder* exemplar) = 0;                // budget sim vslot 0x190
    virtual bool LoadLocalizedName(cISCPropertyHolder* exemplar, std::uint32_t property, cRZString& out) = 0;
    virtual void CityCellSize(float& x, float& z) = 0;                                      // city vslot 0x2d8/0x2e0
    virtual void PostBuildingTypesUpdated() = 0;
    virtual std::int32_t SimulatorDay() = 0;
    // The lot configurations the zone capacity map is computed from.
    struct LotConfigurationInfo {
        std::uint32_t sizeX = 0, sizeZ = 0;
        std::vector<std::uint32_t> buildingIds;               // primary building object ids (types or families)
        float objectX = 0, objectZ = 0;                       // primary building object position
        std::uint8_t objectOrientation = 0;
        std::vector<bool> zoneCompatibility;                  // 16 zone types
    };
    virtual void LotConfigurations(std::vector<LotConfigurationInfo>& out) = 0;
    // ---- what Init / DoMessage reach (0x10014ce64, 0x10014e84c)
    // The simulator's message target (+0x10): SL::spMS2 notifications 0xe8bda2e7, 0x278e6003, 0x99ef1142, 0x99ef1143,
    // 0xb99ae81 and SL::spSimulator vslot 0x98 agents 0, 4, 6, 10 ("cSC4BuildingDevelopmentSimulator", flags 2). The host owns the
    // cIGZMessageTarget2 that forwards to DoMessage (the port's class has no COM base).
    virtual void RegisterMessageTarget(cSC4BuildingDevelopmentSimulator* simulator) { (void)simulator; }
    virtual void UnregisterMessageTarget(cSC4BuildingDevelopmentSimulator* simulator) { (void)simulator; }
    virtual IBuildingPlacementServices* PlacementServices() { return nullptr; }
    virtual IConstructionResources* CreateConstructionResources() { return nullptr; }   // new cSC4Demand, one reference
    // SL::spDemandSim vslot 0x30 GetDemand(type, 0) -> vslot 0x40: a demand channel's supply value
    virtual float DemandChannelSupply(std::uint32_t type) { (void)type; return 0.0f; }
    virtual ITractDeveloperRef* TractDeveloper() { return nullptr; }       // SL::spTractDeveloper (+0xd0)
    virtual std::uint32_t RandomBelow(std::uint32_t range) { (void)range; return 0; }   // sRandom.RandomUint32UniformBelow
    virtual std::uint64_t BeginGrowSlice() { return 0; }                  // cSC4DynamicLimitTimer(3 ms)
    virtual bool GrowSliceExpired(std::uint64_t slice) { (void)slice; return true; }
};
// cISC4BuildingDevelopmentSimulator::ConstructionProfile (the fields IsSuitableForConstruction reads) and its failure codes.
struct ConstructionProfile {
    std::uint32_t purpose = 0;               // +0x00 building purpose / zone family (7..9 skip the land value comparison)
    std::uint32_t reserved04 = 0;            // +0x04
    std::int32_t landValueClass = 0;         // +0x08 compared with the land value simulator's class at the cell
    std::uint32_t reserved0c = 0;            // +0x0c
    std::uint8_t minLandValue = 0;           // +0x10
    std::uint8_t maxLandValue = 0;           // +0x11 (both 0 = compare the class instead of the range)
    std::uint8_t reserved12[2] = {};         // +0x12
    std::uint32_t buildingType = 0;          // +0x14 building type PlopBuilding places
    std::int32_t rotation = 0;               // +0x18
    std::int32_t x = 0;                      // +0x1c
    std::int32_t z = 0;                      // +0x20
};
static_assert(offsetof(ConstructionProfile, landValueClass) == 0x08 && offsetof(ConstructionProfile, minLandValue) == 0x10 &&
              offsetof(ConstructionProfile, buildingType) == 0x14 && offsetof(ConstructionProfile, rotation) == 0x18 &&
              offsetof(ConstructionProfile, x) == 0x1c && offsetof(ConstructionProfile, z) == 0x20);
enum ReasonForFailure : std::uint32_t {
    kReasonNone = 0,              // the building does not fit the zone type at the cell
    kReasonLandValueRange = 1,    // the land value is outside [min, max]
    kReasonLandValueClass = 2,    // the land value class differs from the profile's
    kReasonNoLot = 3,             // the cell has no lot
    kReasonLotNotReady = 4,       // the lot's state (vslot 0x108) is not 1
    kReasonNoPower = 5,           // the cell has no power (and the ignore-power cheat is off)
};

// The five city services IsSuitableForConstruction asks, one original call each (this+0x48..0x60 of the simulator).
class IConstructionLot {
public:
    virtual ~IConstructionLot() = default;
    virtual std::int32_t ConstructionState() = 0;                           // lot vslot 0x108
};
class IConstructionSuitabilityHost {
public:
    virtual ~IConstructionSuitabilityHost() = default;
    virtual std::int8_t ZoneTypeAt(int x, int z) = 0;                       // SL::spZoneManager vslot 0x60
    virtual IConstructionLot* LotAt(int x, int z) = 0;                      // SL::spLotManager vslot 0x28 (x, z, 1); borrowed
    virtual std::int32_t LandValueClassAt(int x, int z) = 0;                // SL::spLandValueSim vslot 0x28
    virtual std::uint8_t LandValueAt(int x, int z) = 0;                     // SL::spLandValueSim vslot 0x30
    virtual bool HasPowerAt(int x, int z) = 0;                              // SL::spPowerSim vslot 0x98
};

// ---- placement services (the city objects the original reaches through SL:: globals and this+0x30..0xd8) --------
class IBuildingTypeFilter {                                                 // cISC4BuildingTypeFilter
public:
    virtual ~IBuildingTypeFilter() = default;
    virtual bool Accepts(std::uint32_t buildingType) = 0;                   // vslot 0x18
};
class IPlacedOccupant {                                                     // a building / foundation occupant being added
public:
    virtual ~IPlacedOccupant() = default;
    virtual cISCPropertyHolder* PropertyHolder() = 0;                       // vslot 0x18 -> vslot 0x30
};
class IPlacedFoundation : public IPlacedOccupant {                          // cISC4FoundationOccupant
public:
    virtual std::uint32_t FoundationType() = 0;                             // vslot 0x20
    virtual void SetFoundationType(std::uint32_t type) = 0;                 // vslot 0x28
    virtual void SetHeight(float height) = 0;                               // vslot 0x38
    virtual void SetSize(const float size[3]) = 0;                          // vslot 0x40
    virtual void SetStyle(int style) = 0;                                   // vslot 0x50
    virtual void SetRotation(int rotation) = 0;                             // vslot 0x60
    virtual void SetPosition(const float position[3]) = 0;                  // vslot 0x98
    virtual void ApplyExemplar(cISCPropertyHolder* exemplar) = 0;           // the 0xac2b5f7 interface, vslot 0x18
    virtual void FinishPlacement() = 0;                                     // secondary base (+8) vslot 0x18
};
class IPlacedBuilding : public IPlacedOccupant {                            // cISC4BuildingOccupant slots AddNewBuildingOccupant uses
public:
    virtual void SetBuildingType(std::uint32_t type) = 0;                   // vslot 0x28
    virtual void SetBounds(const float minimum[3], const float maximum[3]) = 0;   // vslot 0x40
    virtual void SetRotation(int rotation) = 0;                             // vslot 0x78
    virtual void FinishPlacement() = 0;                                     // vslot 0xc8
    virtual void SetPosition(const float position[3]) = 0;                  // vslot 0xe0
    virtual void GetBounds(float minimum[3], float maximum[3]) = 0;         // vslot 0xe8
};
struct GrowthTract {                                                        // cISC4GrowthDeveloper::GrowthItem (0xc bytes)
    std::int32_t x = 0, z = 0;
    std::int32_t style = 1;                                                 // the byte Grow hands on
};
class IGrowthDeveloperRef {                                                 // cISC4GrowthDeveloper
public:
    virtual ~IGrowthDeveloperRef() = default;
    virtual void Init(cISCPropertyHolder* properties) = 0;                  // vslot 0x18
    virtual void StartMapUpdate() {}                                        // vslot 0xd8 (DoMonthlySimulation)
    virtual bool IsMapUpdateFinished() const { return true; }              // vslot 0xe0 (DoIdle)
    // what Grow reads (0x10014fb24)
    virtual std::uint32_t SubType() const { return 0; }                    // vslot 0x30
    virtual bool ListsValid() const { return false; }                      // vslot 0x50
    virtual std::vector<GrowthTract> GrowthList() const { return {}; }     // vslot 0x58
    virtual float ActiveDemand() const { return 0.0f; }                    // vslot 0x90 -> demand vslot 0x60
};
// One pending demolition of the decline plan (a node of the list at this+0x110).
class IDeclineSource {
public:
    virtual ~IDeclineSource() = default;
    virtual float Condition() = 0;                                          // vslot 0x90 -> vslot 0x60
};
struct DeclineEntry {
    std::uint32_t day = 0;                                                  // +0x10 first day the entry may run
    std::int32_t a = 0, b = 0;                                              // +0x14, +0x18
    IDeclineSource* source = nullptr;                                       // +0x20
    std::uint8_t flag = 0;                                                  // +0x28
};
struct TractOutcome {                                                       // cISC4TractDeveloper::Outcome argument (its inner list is empty)
    IDeclineSource* source = nullptr;
    std::int32_t a = 0, b = 0;
    std::uint8_t flag = 0;
};
class ITractDeveloperRef {                                                  // cISC4TractDeveloper
public:
    virtual ~ITractDeveloperRef() = default;
    virtual int ExecuteOutcome(const TractOutcome& outcome) = 0;            // vslot 0x30
    // vslot 0x28 Grow(GrowthProfile{developer, x, z, style}, outcomes, declined): > 0 when something was built
    virtual int Grow(IGrowthDeveloperRef* developer, int x, int z, std::uint8_t style) { (void)developer; (void)x; (void)z; (void)style; return 0; }
};
class IBuildingPlacementServices {
public:
    virtual ~IBuildingPlacementServices() = default;
    virtual IPlacedBuilding* CreateBuilding() = 0;                          // new cSC4BuildingOccupant, one reference held
    virtual void ReleaseBuilding(IPlacedBuilding* building) = 0;            // vslot 0x10
    virtual IPlacedFoundation* CreateFoundation() = 0;                      // new cSC4FoundationOccupant, one reference held
    virtual void ReleaseFoundation(IPlacedFoundation* foundation) = 0;
    virtual float FoundationSurfaceHeight(const int cells[4], int which) = 0;   // this+0xd8 vslot 0x80(cells, which, 1)
    virtual void LoadBuildingProperties(IPlacedBuilding& building, std::uint32_t type) = 0;
    virtual IGrowthDeveloperRef* CreateGrowthDeveloper() = 0;               // new cSC4GrowthDeveloper (reference-initialised)
    virtual float SurfaceHeightForFootprint(const float rect[4]) = 0;       // this+0xd8 vslot 0x88(rect, 0, 1)
    virtual void CellToPosition(int x, int z, float& px, float& pz) = 0;    // city (this+0x30) vslot 0x308
    virtual void PositionToCell(float x, float z, int& cx, int& cz) = 0;    // city vslot 0x2f8
    virtual float TerrainHeight(float x, float z) = 0;                      // this+0x68 vslot 0x60
    virtual bool OccupantAtCell(int cx, int cz, std::uint32_t kindMask) = 0;   // occupant manager (this+0x38) vslot 0x100
    virtual bool AddOccupant(IPlacedOccupant* occupant, bool flag) = 0;     // occupant manager vslot 0xa0
    virtual bool RemoveOccupant(IPlacedOccupant* occupant, bool a, bool b) = 0;   // occupant manager vslot 0xa8
};

IBuildingDevelopmentSimHost* BuildingDevelopmentSimHost();
void SetBuildingDevelopmentSimHost(IBuildingDevelopmentSimHost* host);

class cSC4BuildingDevelopmentSimulator {
public:
    cSC4BuildingDevelopmentSimulator() = default;

    std::uint32_t AddRef() { return ++refCount_; }
    std::uint32_t Release();
    bool QueryInterface(std::uint32_t iid, void** out);
    bool Init();
    bool Shutdown();
    std::uint32_t GetGZCLSID() const { return kGZCLSID_cSC4BuildingDevelopmentSimulator; }

    // ---- the simulation (0x10014e84c..0x10014f222)
    bool DoMessage(cIGZMessage2* message);                                  // @ 0x10014e84c
    void DoIdle();                                                          // @ 0x10014ecba
    void DoDailySimulation();                                               // @ 0x10014edcc
    void DoMonthlySimulation();                                             // @ 0x10014ee44
    void ReadSimulatorTunableValues();                                      // @ 0x10014d71e
    static void InitDynamicDataFromExemplar(const cGZPersistResourceKey& key, void* context);   // @ 0x10014d666
    void Grow();                                                            // @ 0x10014fb24
    // Not recovered yet: PlanDecline 0x10015060c (the decline queue).
    void PlanDecline() {}
    int GrowthStage() const { return stage_; }

    // type / wealth mapping
    static std::uint32_t GetDeveloperType_Static(std::uint32_t purpose, std::int32_t wealth);
    static std::uint32_t GetWealth_Static(std::uint32_t developerType);
    std::uint32_t GetDeveloperType(std::uint32_t purpose, std::int32_t wealth) const { return GetDeveloperType_Static(purpose, wealth); }
    std::uint32_t GetWealth(std::uint32_t developerType) const { return GetWealth_Static(developerType); }
    std::uint8_t GetBuildingMapKey(std::uint32_t purpose, std::uint32_t wealth) const {
        return static_cast<std::uint8_t>(wealth + 1 + purpose * 4);
    }
    std::uint8_t GetBuildingMapKey(std::uint32_t buildingType) const;
    static void SetBuildingCategoryFlags(BuildingProfile& profile, const cISCPropertyHolder& exemplar);

    // catalogue
    static void InitStaticDataFromExemplar(const cGZPersistResourceKey& key, void* context);   // @ 0x10014c46a
    static void PostBuildingTypesUpdatedMessage();                                           // @ 0x10014c534
    static bool StaticInit();
    static bool StaticShutdown();
    static bool AddBuildingData(const cGZPersistResourceKey& key);
    static bool AddFoundationData(const cGZPersistResourceKey& key);
    static bool AddLotRetainingWallData(const cGZPersistResourceKey& key);
    static void LoadBuildingProperties(cISCPropertyHolder& exemplar, std::uint32_t type, bool firstLoad);
    static void CalculateMaxCapacity();
    void GetAllBuildingTypes(std::vector<std::uint32_t>& out) const;
    void GetAllBuildingFamilyTypes(std::vector<std::uint32_t>& out) const;
    bool GetBuildingKeyFromType(std::uint32_t type, cGZPersistResourceKey& key) const;
    bool IsBuildingTypeValid(std::uint32_t type) const;
    bool IsBuildingFamilyTypeValid(std::uint32_t type) const;
    int GetBuildingTypesByFamily(std::vector<std::uint32_t>& out, std::uint32_t family) const;
    bool IsBuildingInFamily(std::uint32_t type, std::uint32_t family) const;
    void GetAllFoundationTypes(std::vector<std::uint32_t>& out) const;
    bool GetFoundationKeyFromType(std::uint32_t type, cGZPersistResourceKey& key) const;
    void GetAllLotRetainingWallTypes(std::vector<std::uint32_t>& out) const;
    bool GetLotRetainingWallKeyFromType(std::uint32_t type, cGZPersistResourceKey& key) const;
    BuildingProfile& GetBuildingProfile(std::uint32_t type) const;
    const BuildingProfile* GetBuildingProfilePtr(std::uint32_t type) const;
    cRZString& GetBuildingName(std::uint32_t type) const;
    cRZString& GetExemplarName(std::uint32_t type) const;
    float GetMaxCapacity(int zoneType, std::uint32_t purpose, std::uint32_t wealth) const;   // @ 0x1001533ee: returns a float (the stored value truncated to int, then converted back)

    // placement
    void SetPlacementServices(IBuildingPlacementServices* services) { placement_ = services; }
    void SetCityCells(int x, int z) { cityCellsX_ = x; cityCellsZ_ = z; }                       // +0x74, +0x78
    bool LocationIsOKForNewBuilding(std::uint32_t type, float x, float z);                     // @ 0x100150c2a (vslot 0x28)
    bool AddNewBuildingOccupant(IPlacedBuilding* building);                                     // @ 0x100150d3e (vslot 0x30)
    IPlacedBuilding* AddNewBuildingOccupant(std::uint32_t type, float* position, int rotation); // @ 0x100150d7a (vslot 0x38)
    void AddNewBuildingOccupant(std::uint32_t type, int x, int z, int rotation);                // @ 0x100151146 (vslot 0x40)
    void PlopBuilding(const ConstructionProfile& profile);                                      // @ 0x100150c0c (vslot 0x178)
    void GetStandardCityCellArea(const float rect[4], int out[4]) const;                        // @ 0x100150cc6
    bool AddNewFoundationOccupant(IPlacedOccupant* foundation);
    bool AddNewFoundationOccupant(std::uint32_t type, int style, float* position, float width, float depth,
                                  int rotation, float baseHeight);                              // @ 0x100151d84 (vslot 0xb8)
    bool LoadFoundationProperties(IPlacedFoundation* foundation, std::uint32_t type);           // @ 0x100151fc4 (vslot 0xd0)                                 // @ 0x100151d48 (vslot 0xb0)
    void DemolishFoundationOccupant(IPlacedOccupant* foundation, std::uint32_t reason);        // @ 0x1001522ce (vslot 0xc0)
    std::int64_t GetCostOfOccupantDemolition(IPlacedBuilding* building, std::uint32_t) const;   // @ 0x1001511da (vslot 0x48)
    std::int64_t GetCostOfOccupantDemolition(IPlacedFoundation*, std::uint32_t) const { return 0; }   // @ 0x100152302 (vslot 0xc8)
    int GetBuildingTypesByFilter(std::unordered_set<std::uint32_t>& out, IBuildingTypeFilter* filter) const;   // @ 0x1001513ca

    // developers
    void CreateDeveloper(cISCPropertyHolder* properties);                                       // @ 0x10014e664
    IGrowthDeveloperRef* GetDeveloper(std::uint32_t developerType) const;                       // @ 0x10015359a (vslot 0x108)
    IGrowthDeveloperRef* GetDeveloper(std::uint32_t purpose, std::int32_t wealth) const;        // @ 0x100153572 (vslot 0x100)
    const std::map<std::uint32_t, IGrowthDeveloperRef*>& GetDevelopers() const { return developers_; }   // +0xb8
    void SetDeveloperForTest(std::uint32_t type, IGrowthDeveloperRef* developer) {
        if (developer) developers_[type] = developer; else developers_.erase(type);
    }
    ITractDeveloperRef* GetTractDeveloper() const { return tractDeveloper_; }                   // +0xd0
    void SetTractDeveloper(ITractDeveloperRef* developer) { tractDeveloper_ = developer; }
    void* GetBuildingMap() const { return buildingMap_; }                                       // +0x80
    void SetBuildingMap(void* map) { buildingMap_ = map; }
    void AddDeclineEntry(const DeclineEntry& entry) { declinePlan_.push_back(entry); }          // list at +0x110
    void ExecuteDeclinePlan();                                                                  // @ 0x10015001a

    // construction resources (the counter object is owned by the city)
    void SetConstructionResources(IConstructionResources* resources) { resources_ = resources; }
    std::uint16_t GetFreeConstructionResources() const;
    std::uint16_t GetTotalConstructionResources() const;
    bool ClaimConstructionResources(std::uint16_t amount);
    void ReleaseConstructionResources(std::uint16_t amount);
    float GetStagePopulationMultiplier(int stage) const { return stageMultipliers_[stage]; }
    void SetStagePopulationMultipliers(const std::vector<float>& values) { stageMultipliers_ = values; }
    std::int32_t GetDayOfLastSuccessfulBuild(std::int32_t developerType) const;
    void NoteSuccessfulBuild(std::int32_t developerType, std::int32_t day) { lastBuildDay_[developerType] = day; }

    // growth marks
    void MarkTract(std::uint32_t developerType, int x, int z) { growthMarks_[developerType].insert({x, z}); }
    bool IsTractUnmarked(std::uint32_t developerType, int x, int z) const;
    void ClearGrowthMarks() { growthMarks_.clear(); }

    // growth predicates
    static bool GrowthStageFitsInZone(int stage, std::uint32_t zoneType);                        // @ 0x100150a4c
    static bool BuildingFitsInZone(std::uint32_t buildingPurpose, std::uint32_t zoneType);       // @ 0x100150a86
    void SetSuitabilityHost(IConstructionSuitabilityHost* host) { suitability_ = host; }          // +0x48..0x60 services
    bool IsSuitableForConstruction(const ConstructionProfile& profile, ReasonForFailure& reason) const;   // @ 0x100150b0c (vslot 0x170)

    // cheats
    bool CheatIgnorePlumbing() const { return cheatPlumbing_; }
    bool CheatIgnorePower() const { return cheatPower_; }
    bool CheatIgnoreCaps() const { return cheatCaps_; }
    bool CheatDebugGrowth() const { return cheatDebug_; }
    bool CheatCoffeeBar() const { return cheatCoffee_; }
    void SetCheats(bool plumbing, bool power, bool caps) { cheatPlumbing_ = plumbing; cheatPower_ = power; cheatCaps_ = caps; }

    // persisted state: version 4 -- growth marks, last-build days, cheats (the developer objects are
    // serialized by the growth developer classes through the host)
    bool Write(cIGZOStream& stream) const;
    bool Read(cIGZIStream& stream);
    bool ReadCityRecord(cIGZIStream& stream);   // @ 0x10014e064

private:
    std::uint32_t refCount_ = 0;
    bool initialized_ = false;
    IConstructionResources* resources_ = nullptr;                              // +0xe0
    IBuildingPlacementServices* placement_ = nullptr;
    int cityCellsX_ = 0, cityCellsZ_ = 0;                                       // +0x74, +0x78
    std::map<std::uint32_t, IGrowthDeveloperRef*> developers_;                  // +0xb8
    ITractDeveloperRef* tractDeveloper_ = nullptr;                              // +0xd0
    void* buildingMap_ = nullptr;                                               // +0x80
    std::list<DeclineEntry> declinePlan_;                                       // +0x110
    IConstructionSuitabilityHost* suitability_ = nullptr;                      // +0x48..0x60 (zone, lot, land value, power)
    std::vector<float> stageMultipliers_ = std::vector<float>(8, 1.0f);        // +0x1b0
    std::map<std::int32_t, std::int32_t> lastBuildDay_;                         // +0x180
    std::map<std::uint32_t, std::set<std::pair<int, int>>> growthMarks_;       // mGrowthMarks
    // the city-file record's tables the port does not model (kept as read, saved back around the growth marks)
    bool fromCityFile_ = false;
    cRecordTape fileHead_, fileTail_;
    bool cheatPlumbing_ = false, cheatPower_ = false, cheatCaps_ = false;       // +0x1c9..0x1cb
    bool cheatDebug_ = false, cheatCoffee_ = false;                             // +0x1cc, +0x1cd
    bool plannedGrowth_ = false;                                                // +0x1c8 (never set: Grow, not PlanGrowth)
    int stage_ = 0;                                                             // +0xe8 the idle stage machine
    std::uint32_t idleCursor_ = 0;                                              // +0xf0 the developer DoIdle waits on (its type)
    bool idleCursorValid_ = false;
    std::uint32_t minResources_ = 100, maxResources_ = 2000;                    // +0x19c, +0x1a0
    float resourcesPerDemand_ = 0.5f;                                           // +0x1a4
    std::uint32_t developerWeightBonus_ = 10;                                   // +0x1a8
    std::list<cIGZUnknown*> civicOccupants_;                                    // +0x88 (families 0x150a / 0x150b; one reference each)
    bool messageTargetRegistered_ = false;
};

}  // namespace sc4
