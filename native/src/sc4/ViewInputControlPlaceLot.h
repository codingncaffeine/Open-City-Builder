// cSC4ViewInputControlPlaceLot — the city-view input tool that plops civic buildings, landmarks and
// rewards by dragging their footprint over the city (31 functions / 35 symbols including the thunks and
// destructor aliases, 0x1003bb6ba..0x1003c1d4a).
//
// The tool is a cSC4ViewInputControl (view id 0xc6bb8345) with a cIGZMessageTarget2 sub-object at +0x40
// (iid 0x452294aa). Its tool id is 0x88f154fb and its cursor 0x88e70b76 (`Init` writes the 64-bit
// constant 0x88e70b7688f154fb at +0x10; the valid cursor is 0x417ee36b). Two flavours share the class:
//   * toolKind_ == 0: the user picks a lot configuration (a civic / landmark plop); the configuration
//     list is filtered by a cSC4LotConfigurationFilter and `DoUserLotConfigurationTypePick` shows it.
//     The toll-booth configuration 0xebf9a440 switches on `tollBoothMode_`, which snaps the footprint to
//     a road and picks the facing/cell coverage from the road's connections.
//   * toolKind_ != 0: the tool was started for one building type (`buildingType_`), as the reward and
//     landmark buttons do; the configuration is looked up by building type.
//
// Original layout of the object (0x220 bytes). The cSC4ViewInputControl base owns +0x08..+0x38:
//   +0x40 cIGZMessageTarget2 vptr
//   +0x48 int tool kind (civic-lot mode 0 / building-type mode != 0)
//   +0x4c uint lot configuration id            +0x50 uint building type
//   +0x54 int initial facing (0, written only by the constructor)   +0x58 int facing
//   +0x5c int facing the current feedback was built for (-1 = none)
//   +0x60 / +0x64 int last mouse position
//   +0x68 cSC4LotConfigurationManager (referenced)   +0x70 bool is-reward flag
//   +0x78 unordered_set<uint> lot configuration ids the filter accepted
//   +0xa0 cSC4BuildingDevelopmentSimulator (referenced)   +0xa8 vector<uint> building types
//   +0xc0..+0xcc int world-space rectangle of the lot just placed (0,0,-1,-1 = none)
//   +0xd0/+0xd4/+0xd8 float preview centre      +0xdc valid feedback   +0xdd secondary feedback
//   +0xe0 cRZString error / cursor text ("can't place" reason)
//   +0x108 cSC4LocalGridHelper (8 bytes; the footprint grid helper)
//   +0x110 cRZString coverage effect name       +0x138 cRZString placement effect name
//   +0x160 coverage radius effect   +0x168 footprint effect   +0x170 direction arrow effect
//   +0x178 preview building prop occupant
//   +0x180 vector<cRZAutoRefCount<cISC4VisualEffect>> red selection rectangles
//   +0x198 same for the blue rectangles
//   +0x1b0 cS3DTextureBinding (footprint grid texture)   +0x1b8 float grid size
//   +0x1bc u8 building rotation   +0x1c0/+0x1c4/+0x1c8 float preview model centre
//   +0x1cc u8 coverage effect active   +0x1d0 float coverage radius   +0x1d4 float ring radius (32)
//   +0x1d8 u8 coverage categories (1 police, 2 fire, 4 health, 8 education)
//   +0x1e0 long long building price (-100000 = unknown)   +0x1f0 long long lot cost (-100000 = unknown)
//   +0x1f8 uint global effect id (10 or 0xf2)   +0x200 vector<PreviewProp> preview props (0x18 bytes each)
//   +0x218 bool toll-booth mode   +0x21c int placements left (1000; decremented per placement)
//
// Quirks kept (each asserted in scripts/tests/sc4_viewinputcontrolplacelot_test.cpp):
//  * The initial facing at +0x54 is written once by the constructor (0) and never again, so the
//    automatic-facing search always starts from north and the facing is only kept in +0x58.
//  * CanPlaceLot only validates while the preview centre is *outside* the lot placed by the last click
//    (+0xc0..+0xcc, shrunk by two world units). While the cursor stays over that footprint the tool keeps
//    reporting "not placeable" (0x1003c05cc).
//  * CanPlaceLot returns 8 when the existing lot must be demolished first, then adds the demolition cost
//    the demolition simulator wrote; every other non-zero answer carries a localized error string
//    (group 0xa419226) and answer 0 has the default string 0xca711914 with no placement.
//  * The footprint effect is created once per validity change with the name "building_footprint_ok" /
//    "building_footprint_notok"; the direction arrow is "Lot_Direction_Arrow".
//  * The coverage-radius effect name is the last "PlopMode_<category>_Inactive" written by
//    GetBuildingPreviewModel (police / fire / health / education), so a tool without a category creates
//    an effect from an empty name in Init.
//  * DoMessage always answers false, including for the effect-finished messages it handles
//    (0x49d5b83f clears the coverage-active flag and recalculates the coverage rings).
//  * SetVisualUserFeedback recreates the footprint effect whenever the validity flag changed but only
//    *moves* it otherwise; the direction arrow is rebuilt when the facing changed (0x5c vs 0x54).
//  * PlaceLot refuses to place when the city's mission hook is active and the target rectangle differs
//    (the tutorial's "build it here" restriction).
//  * OnMouseDownL ends the input after the placement counter drops below 2 (it decrements first, so the
//    surviving counter can go negative) and plays the failure sound 0xaa62c9d5 on every failed click.
//
// Everything outside the object (the lot configuration manager, the building development simulator, the
// lot/prop/occupant/demolition/zone managers, the renderer-side effects, the coverage helper, the local
// grid helper, the budget, the city, the traffic network map, the terrain and the two pick dialogs) goes
// through IPlaceLotHost / IPlaceLotView, one method per original call with its vtable slot in the
// comment. docs/SEAMS.md is generated, so its hook-up table is updated by the lead agent.
#pragma once
#include <cstdint>
#include <unordered_set>
#include <vector>

#include "gz/StreamOps.h"
#include "gz/WinInterfaces.h"
#include "gz/cIGZMessage2.h"
#include "rz/cRZString.h"
#include "sc4/LotConfiguration.h"
#include "sc4/ViewInputControl.h"
#include "sc4/ZoneDeveloper.h"   // SC4Rect<int> (the original's cell rectangle)
#include "s3d/ModelInstance.h"   // SC4DrawAttributes

namespace sc4 {

class cSC4Lot;   // sc4/Lot.h

constexpr std::uint32_t kPlaceLotToolId = 0x88f154fb;                 // +0x10
constexpr std::uint32_t kPlaceLotCursorId = 0x88e70b76;               // +0x14
constexpr std::uint32_t kPlaceLotCursorValid = 0x417ee36b;
constexpr std::uint32_t kPlaceLotMessageTargetId = 0x452294aa;        // iid of the +0x40 sub-object
constexpr std::uint32_t kTollBoothLotConfiguration = 0xebf9a440;

// Init registers these with SL::spMS2 (vslot 0x28); Shutdown removes them (0x30).
constexpr std::uint32_t kPlaceLotMessages[] = {0x277a9364, 0x99ef1142, 0x99ef1143,
                                               0x49d5b83f, 0xa6b79602, 0xc9f775bb};
constexpr std::uint32_t kPlaceLotErrorStringGroup = 0x0a419226;       // cSLString group of every message
constexpr std::uint32_t kPlaceLotCursorTextId = 0xe9c877fa;           // SetCursorText / ClearCursorText id
constexpr std::uint32_t kPlaceLotSoundGroup = 0xea451903;
constexpr std::uint32_t kPlaceLotSoundFailure = 0xaa62c9d5;           // OnMouseDownL when nothing was placed

// ---------------------------------------------------------------------------------------------------
// Renderer-side seams. The original stores cISC4VisualEffect objects (VisualEffect.cpp) in the three
// effect slots and two std::vector<cRZAutoRefCount<cISC4VisualEffect>> selection lists; only these slots
// are called by this class.
// ---------------------------------------------------------------------------------------------------
class IPlaceLotEffect : public cIGZUnknown {
public:
    virtual void Start(int transition) = 0;                                     // vslot 0x18
    virtual void Stop(int transition) = 0;                                      // vslot 0x20
    virtual bool IsStarted() = 0;                                               // vslot 0x28
    virtual void SetEffectTransform(const cS3DTransform& transform) = 0;        // vslot 0x40
    virtual void SetFinishMessage(std::uint32_t message, std::uint32_t data) = 0;   // vslot 0x50
    // vslot 0x60 SetVectorParams(type, values, count); the selection rectangles use type 0xc with four
    // floats (x0, z0, x1, z1).
    virtual void SetVectorParams(std::uint32_t type, const float* values, int count) = 0;
    // vslot 0x68 SetVoidParams(type, values, count); the footprint effect hands over the grid texture
    // binding (type 6 with one pointer).
    virtual void SetVoidParams(std::uint32_t type, void** values, int count) = 0;
};

// SL::spEffectsManager vslot 0x30: CreateVisualEffect(name, &out), one reference on success.
class IPlaceLotEffects {
public:
    virtual ~IPlaceLotEffects() = default;
    virtual IPlaceLotEffect* Create(const char* name) = 0;
};

// cSC4CoverageRadiusEffectHelper (view vslot 0x188).
class IPlaceLotCoverageHelper {
public:
    virtual ~IPlaceLotCoverageHelper() = default;
    virtual void SetCategories(std::uint32_t categories) = 0;   // @ 0x100770f80
    virtual void RecalcCoverage() = 0;                          // @ 0x100770fd2
};

// cSC4LocalGridHelper (+0x108): Init @ 0x1000a98ce, Shutdown @ 0x1000a98f8, SetPosition @ 0x1000a9940,
// ShowEffect @ 0x1000a9aac.
class IPlaceLotLocalGrid {
public:
    virtual ~IPlaceLotLocalGrid() = default;
    virtual void Init() = 0;
    virtual void Shutdown() = 0;
    virtual void SetPosition(const cS3DVector3& position) = 0;
    virtual void ShowEffect(bool show) = 0;
};

// The 3D view (cSC4View3DWin) as this tool uses it. EndInput (0xd0) and TopControl (0xc0) come from
// IViewInputView; every other slot is listed with its original offset.
class IPlaceLotView : public IViewInputView {
public:
    virtual bool PickTerrain(int x, int y, float out[3], bool flag) = 0;                       // 0x120
    virtual cIGZWinKeyAccelerator* KeyAccelerator() = 0;                                       // 0xf8
    // 0x140 SetCursorText(id, 0, 0, text, 1)
    virtual void SetCursorText(std::uint32_t id, std::uint32_t arg2, std::uint32_t arg3,
                               const cIGZString* text, int flags) = 0;
    virtual void ClearCursorText(std::uint32_t id) = 0;                                        // 0x148
    virtual void SetCostIndicator(std::uint32_t cost) = 0;                                     // 0x150
    virtual void ClearCostIndicator(bool force) = 0;                                           // 0x160
    virtual IPlaceLotCoverageHelper* CoverageHelper() = 0;                                     // 0x188
    virtual std::uint32_t LastSelectedBuilding() = 0;                                          // 0x198
    virtual std::uint32_t LastSelectedLot() = 0;                                               // 0x1a0
    virtual void SetLastSelectedBuilding(std::uint32_t buildingType) = 0;                      // 0x1a8
    virtual void SetLastSelectedLot(std::uint32_t lotConfigurationId) = 0;                     // 0x1b0
    virtual void AddGlobalEffect(std::uint32_t effectId) = 0;      // SL::ViewManager vslot 0x20
    virtual void RemoveGlobalEffect(std::uint32_t effectId) = 0;   // SL::ViewManager vslot 0x28
};

// A prop the preview shows (cSC4Prop / its cISC43DPlaceableObject). The original reaches the occupant
// through prop->0x18 and then the prop manager (0xa0 AddProp / 0xe8 RemoveProp), the occupant manager
// (0xc0 AddOccupant) and the model instances (0x70 SetCategory(5) + 0x80 SetDrawAttributes); the seam
// folds those into the prop object because none of those interfaces is recovered.
class IPlaceLotProp : public cIGZUnknown {
public:
    virtual int Orientation() = 0;                                         // vslot 0x48
    virtual void SetOrientation(int orientation) = 0;                      // vslot 0x50
    virtual bool IsPlaced() = 0;                                           // vslot 0x100(1)
    // The occupant's property 0x4a89fcf3: only props that carry it become preview props (0x1003bd1xx).
    virtual bool ShowInPreview() = 0;
    virtual void Show(const cS3DVector3& position, const s3d::SC4DrawAttributes& attributes) = 0;
    virtual void Hide() = 0;
};

// SL::spPropManager
class IPlaceLotPropManager {
public:
    virtual ~IPlaceLotPropManager() = default;
    virtual bool PropTypeExists(std::uint32_t propId) = 0;                 // vslot 0x40
    virtual IPlaceLotProp* CreateProp(std::uint32_t propId) = 0;           // vslot 0x78, one reference
    // vslot 0x80 CreateProp(building profile, &out): the preview building itself.
    virtual IPlaceLotProp* CreateBuildingProp(std::uint32_t buildingType) = 0;
};

// cISC4Terrain as the tool uses it (SL::spTerrain).
class IPlaceLotTerrainQuery {
public:
    virtual ~IPlaceLotTerrainQuery() = default;
    virtual bool IsWater(float x, float z) = 0;          // terrain vslot 0x210 -> query object 0xa0
};
class IPlaceLotTerrain {
public:
    virtual ~IPlaceLotTerrain() = default;
    virtual float CellSize() = 0;                        // vslot 0x30
    virtual int CellCountX() = 0;                        // vslot 0x38
    virtual int CellCountZ() = 0;                        // vslot 0x40
    virtual IPlaceLotTerrainQuery* Query() = 0;          // vslot 0x210
};

// One cell of the traffic network map: cISC4NetworkOccupant's flags. city vslot 0x140 and
// SL::spTrafficNetworkMap both answer the map object; its vslot 0x50 is GetNetworkOccupantAt.
class IPlaceLotNetworkCell : public cIGZUnknown {
public:
    virtual bool HasAnyFlag(std::uint32_t flags) = 0;      // vslot 0x48
    virtual std::uint32_t Flags() = 0;                     // vslot 0x60
    virtual std::uint32_t EdgeTypes(std::uint32_t networkType) = 0;   // vslot 0x130
    virtual std::uint8_t EdgeFlags() = 0;                  // vslot 0x140
};
class IPlaceLotNetworkMap {
public:
    virtual ~IPlaceLotNetworkMap() = default;
    virtual IPlaceLotNetworkCell* GetCell(int cellX, int cellZ, std::uint32_t type, int flag) = 0;  // 0x50
};

// The city's mission hook (SL::spCity vslot 0x290): while active, PlaceLot only accepts the target
// rectangle (the tutorial's guided placement).
class IPlaceLotMissionTarget {
public:
    virtual ~IPlaceLotMissionTarget() = default;
    virtual const std::int32_t* Rect() = 0;                 // vslot 0x28 (x0, z0, x1, z1)
};
class IPlaceLotMission {
public:
    virtual ~IPlaceLotMission() = default;
    virtual bool IsActive() = 0;                            // vslot 0x40
    virtual IPlaceLotMissionTarget* Target() = 0;           // vslot 0x50
};

// SL::spCity
class IPlaceLotCity {
public:
    virtual ~IPlaceLotCity() = default;
    virtual void PositionToCell(float x, float z, int& cellX, int& cellZ) = 0;   // vslot 0x2f8
    virtual void CellToPosition(int cellX, int cellZ, float& x, float& z) = 0;   // vslot 0x308
    virtual IPlaceLotNetworkMap* NetworkMap() = 0;                               // vslot 0x140
    virtual IPlaceLotMission* Mission() = 0;                                     // vslot 0x290
};

// cISC4LotDeveloper slots the commit path uses.
class IPlaceLotLotDeveloper {
public:
    virtual ~IPlaceLotLotDeveloper() = default;
    virtual std::int64_t LotPrice(const SC4Rect<int>& rect) = 0;                // vslot 0x100
    // vslot 0x38 ConstructDefaultLot(lot, configuration, buildingType, arg4)
    virtual bool ConstructDefaultLot(cSC4Lot* lot, cSC4LotConfiguration* configuration,
                                     std::uint32_t buildingType, bool arg4) = 0;
};

// cSC4BuildingDevelopmentSimulator. The building profile query replaces the original's property reads
// on the preview occupant (the port does not build the preview occupant; see GetBuildingPreviewModel).
class IPlaceLotBuildingDevelopment {
public:
    virtual ~IPlaceLotBuildingDevelopment() = default;
    virtual void GetAllBuildingTypes(std::vector<std::uint32_t>& out) = 0;       // vslot 0x58
    virtual bool BuildingTypeName(std::uint32_t type, cIGZString& out) = 0;     // vslot 0x128
    virtual std::int64_t BuildingPrice(std::uint32_t type) = 0;                 // vslot 0x140 -> record +0x40
    virtual IPlaceLotLotDeveloper* LotDeveloper() = 0;                          // vslot 0x120
    // vslot 0x88(types, family) for every id + vslot 0x68(first type): the building type the preview
    // model is built from. The original keeps the cISC4LotConfigurationObject ids at +0x28.
    virtual bool ResolveBuildingType(const std::vector<std::uint32_t>& ids, std::uint32_t& out) = 0;
    // The police / fire simulators' "is this building a station" answers (spPoliceSim 0xb0,
    // spFireProtectionSim 0x38) used to pick the coverage effect.
    virtual bool BuildingIsPoliceStation(std::uint32_t buildingType) = 0;
    virtual bool BuildingIsFireStation(std::uint32_t buildingType) = 0;
    // spPoliceSim / spFireProtectionSim / spResidentialSim 0xa0|0xa8|0x168: (occupant, fraction,
    // &radius, &ringRadius).
    virtual bool BuildingCoverageRadius(std::uint32_t buildingType, int category, float fraction,
                                        float& radius, float& ringRadius) = 0;
};

// cISC4BudgetSimulator: 0x30 current funds, 0x38 deposit, 0x40 withdraw (cSC4BudgetSimulator).
class IPlaceLotBudget {
public:
    virtual ~IPlaceLotBudget() = default;
    virtual std::int64_t Funds() = 0;                    // vslot 0x30
    virtual void Deposit(std::int64_t amount) = 0;       // vslot 0x38 (refund when construction fails)
    virtual void Withdraw(std::int64_t amount) = 0;      // vslot 0x40
};

// SL::spLotManager: 0xa8 CanCreatePloppedLot, 0xb0 the lot-creation call the commit path makes.
class IPlaceLotLotManager {
public:
    virtual ~IPlaceLotLotManager() = default;
    // vslot 0xa8: 0 = the configuration does not fit, 1..7 = a "cannot place" reason, 8 = it fits but
    // an existing lot has to be demolished first.
    virtual int CanCreatePloppedLot(const SC4Rect<int>& rect, int facing,
                                    cSC4LotConfiguration* configuration) = 0;
    // vslot 0xb0 CreateLotIfPossible(x, z, sizeX, sizeZ, facing, &out, 1, playEffects, configuration)
    virtual bool CreateLotIfPossible(int x, int z, int sizeX, int sizeZ, int facing, bool arg7,
                                     bool arg8, cSC4LotConfiguration* configuration,
                                     cSC4Lot*& out) = 0;
};

// SL::spZoneManager vslot 0x58 PlaceZone(region, zoneType, ...); the tool always passes the same five
// booleans (1, lotCost == 0, 1, 1, arity) and three null trailing pointers.
class IPlaceLotZoneManager {
public:
    virtual ~IPlaceLotZoneManager() = default;
    virtual bool PlaceZone(const SC4Rect<int>& rect, int zoneType, bool arg2, bool arg3, bool arg4,
                           bool arg5, bool arg6) = 0;
};

// SL::spDemolition vslot 0x30 DemolishRegion(commit, cells, privilege 1, flags, false, filter, cost, ...): the arguments that vary
// are whether it is done or only priced (CanDemolishLot), the rectangle, the flags and the cost out pointer. The clearing passes
// the tool's cSC4PlaceLotDemolishFilter (power poles and power lines stay), folded into the host.
class IPlaceLotDemolition {
public:
    virtual ~IPlaceLotDemolition() = default;
    virtual bool DemolishRegion(bool commit, const SC4Rect<int>& rect, std::uint32_t flags, std::int64_t* cost) = 0;
};

// The filter the lot configuration manager applies (cSC4LotConfigurationFilter subclasses the original
// builds in Init). The manager itself is reached through the host.
class IPlaceLotFilter : public cIGZUnknown {
public:
    virtual bool Matches(cSC4LotConfiguration* configuration) = 0;
};

// The two modal pick dialogs (cGZWinGen + cGZWinGrid built by DoUser*Pick). Both functions are PARTIAL:
// the dialogs are not rebuilt, so the port asks the host, which owns the real dialog.
class IPlaceLotChooser {
public:
    virtual ~IPlaceLotChooser() = default;
    virtual std::uint32_t PickLotConfiguration(std::uint32_t current,
                                               const std::unordered_set<std::uint32_t>& ids) = 0;
    virtual std::uint32_t PickBuildingType(std::uint32_t current,
                                           const std::vector<std::uint32_t>& types) = 0;
};

// Everything outside the object. One method per original call, the global / slot in the comment.
class IPlaceLotHost {
public:
    virtual ~IPlaceLotHost() = default;

    virtual cIGZMessageServer2* MessageServer() = 0;                     // SL::spMS2
    virtual IPlaceLotView* View() = 0;                                   // SL::spView3D (the base's +0x28 object)
    virtual IPlaceLotTerrain* Terrain() = 0;                             // SL::spTerrain
    virtual IPlaceLotCity* City() = 0;                                   // SL::spCity
    virtual IPlaceLotNetworkMap* NetworkMap() = 0;                       // SL::spTrafficNetworkMap (city 0x140)
    virtual IPlaceLotBuildingDevelopment* BuildingDevelopment() = 0;     // SL::spBuildingDevelopmentSim
    virtual IPlaceLotBudget* Budget() = 0;                               // SL::spBudgetSim
    virtual IPlaceLotLotManager* LotManager() = 0;                       // SL::spLotManager
    virtual IPlaceLotZoneManager* ZoneManager() = 0;                     // SL::spZoneManager
    virtual IPlaceLotDemolition* Demolition() = 0;                       // SL::spDemolition
    virtual IPlaceLotPropManager* PropManager() = 0;                     // SL::spPropManager
    virtual IPlaceLotEffects* Effects() = 0;                             // SL::spEffectsManager
    virtual IPlaceLotLocalGrid* CreateLocalGrid() = 0;                   // the cSC4LocalGridHelper at +0x108 (host-owned)
    // The footprint texture of GetBuildingPreviewModel: an RZ buffer of size x size 32-bit texels (clear, a white rectangle
    // inset by the margins, its inside 2 texels in at alpha 0xaa) made a cS3DTextureBinding by the texture-binding factory
    // (vslot 0x18). One reference for the caller (+0x1b0); ReleaseFootprintTexture is cS3DTextureBinding::Release.
    virtual void* CreateFootprintTexture(int, int, int) { return nullptr; }
    virtual void ReleaseFootprintTexture(void*) {}

    // SL::spLotConfigurationManager
    virtual cSC4LotConfiguration* GetLotConfiguration(std::uint32_t id) = 0;               // vslot 0x40
    // vslot 0x68 GetLotConfigurationIDsByFilter(set, filter); the filter is released afterwards.
    virtual int GetLotConfigurationIDsByFilter(std::unordered_set<std::uint32_t>& out,
                                               IPlaceLotFilter* filter) = 0;
    // The two cSC4LotConfigurationFilter subclasses Init builds (vtable 0x100b073b8 / 0x100b07418).
    virtual IPlaceLotFilter* CreatePlopFilter(bool isReward, std::uint32_t buildingType) = 0;
    // vslot 0xd8 GetObjectIDArray(object, out)
    virtual void GetObjectIDArray(const cSC4LotConfigurationObject* object,
                                  std::vector<std::uint32_t>& out) = 0;

    // cSLString(group, instance, true) for the "can't place" reasons.
    virtual void LoadString(std::uint32_t group, std::uint32_t instance, cIGZString& out) = 0;
    virtual void PlaySound(std::uint32_t group, std::uint32_t id) = 0;   // sound system vslot 0x50
    // The lot's utility connections (the original: lot 0x180 -> 0x18 -> 0x30 property holder,
    // property 0x88ed3303 -> a cSC4PipeConnectionOccupant; SL::spNetworkManager 0x48 -> a
    // cSC4SubwayConnectionOccupant; both are unrecovered, so the adapter owns them).
    virtual void ConnectLotUtilities(cSC4Lot* lot, const cS3DVector3& position) = 0;
    // RZ::WM()->vslot 0x98, the window manager's end-of-mouse-move housekeeping.
    virtual void MouseMoved() = 0;

    // The preview occupant's property holder is not built by the port; these answer the same properties
    // for the building type of the chosen configuration. (0x91b42a2 health, 0x691b42b3 education,
    // 0xea2e078b / 0xea3209f8 placement count, 0xbd86fd3 reward use count, 0x1401 / 0x1302 groups.)
    virtual bool BuildingPropertyBool(std::uint32_t buildingType, std::uint32_t property) = 0;
    virtual bool BuildingPropertyUint32(std::uint32_t buildingType, std::uint32_t property,
                                        std::uint32_t& out) = 0;
    virtual bool OccupantGroup(std::uint32_t buildingType, std::uint32_t group) = 0;
    // SL::spTrafficSim vslot 0x100: the network type of a reward category (16 - this = placements left).
    virtual int RewardNetworkType(std::uint8_t category) = 0;

    virtual IPlaceLotChooser* Chooser() = 0;
};
IPlaceLotHost* PlaceLotHost();
void SetPlaceLotHost(IPlaceLotHost* host);

class cSC4ViewInputControlPlaceLot final : public cSC4ViewInputControl, public cIGZMessageTarget2 {
public:
    struct PreviewProp {                       // 0x18 bytes
        std::uint8_t orientation = 0;          // +0
        float x = 0, y = 0, z = 0;             // +4, +8, +0xc
        IPlaceLotProp* prop = nullptr;         // +0x10 (referenced)
    };

    cSC4ViewInputControlPlaceLot(int toolKind, std::uint32_t lotConfigurationId,
                                 std::uint32_t buildingType, bool isReward);   // @ 0x1003bb6ba
    ~cSC4ViewInputControlPlaceLot() override;                                  // @ 0x1003bb94a

    // cIGZUnknown (both bases): the original's own AddRef/Release/QueryInterface plus the +0x40 thunks.
    bool QueryInterface(std::uint32_t iid, void** out) override;               // @ 0x1003bdd50
    std::uint32_t AddRef() override;                                          // @ 0x1003bdcf0
    std::uint32_t Release() override;                                         // @ 0x1003bdd0c
    std::uint32_t AddRefFromMessageTarget();                                  // @ 0x1003bdcfe
    std::uint32_t ReleaseFromMessageTarget();                                 // @ 0x1003bdd2c
    bool QueryInterfaceFromMessageTarget(std::uint32_t iid, void** out);      // @ 0x1003bdd84

    // cIGZMessageTarget2
    bool DoMessage(cIGZMessage2* message) override;                           // @ 0x1003c1998
    bool DoMessageFromMessageTarget(cIGZMessage2* message);                   // @ 0x1003c1afc

    bool Init() override;                                                     // @ 0x1003bbbba
    bool Shutdown() override;                                                 // @ 0x1003bd89a
    bool OnKeyDown(std::uint32_t key, std::uint32_t modifiers) override;      // @ 0x1003be3f0
    bool OnMouseDownL(int x, int y, std::uint32_t modifiers) override;        // @ 0x1003bed24
    bool OnMouseMove(int x, int y, std::uint32_t modifiers) override;         // @ 0x1003c0ca4
    bool OnMouseExit() override;                                              // @ 0x1003c11bc

    // Preview / feedback state machine.
    void GetBuildingPreviewModel();                                           // @ 0x1003bc9ac
    void ReleaseLotConfigurationEffects();                                    // @ 0x1003bda28
    void ChangeLotConfiguration(std::uint32_t lotConfigurationId);            // @ 0x1003c0c74
    void SetVisualUserFeedback(bool valid, bool secondary, const cS3DVector3& position);   // @ 0x1003be50c
    void SetConstraintFeedback(const cS3DVector3& position);                  // @ 0x1003c1220
    void SetPropOccupantDrawAttributes(const cS3DVector3& position, s3d::SC4DrawAttributes& attributes,
                                       bool show);                            // @ 0x1003bdd92

    // Footprint / validity checks.
    bool GetLotCenterAndCellCoverage(int facing, float x, float z, float& centerX, float& centerZ,
                                     SC4Rect<int>& rect);                     // @ 0x1003c0282
    int GetLotFacingStreetCount(const SC4Rect<int>& rect, cSC4LotConfiguration* configuration,
                                const cS3DVector3& center, int facing);       // @ 0x1003c0440
    bool CanDemolishLot(cSC4LotConfiguration* configuration, const cS3DVector3& position, int facing,
                        const SC4Rect<int>& rect, std::int64_t* cost);        // @ 0x1003bfe4e
    bool GetAutomaticFacingAndCellCoverage(cSC4LotConfiguration* configuration, cS3DVector3& position,
                                           int& facing, SC4Rect<int>& rect);  // @ 0x1003c00a0
    bool CanPlaceLot(const cS3DVector3& position, int facing, SC4Rect<int>& rect, bool& valid,
                     std::int64_t& cost, int& resultCode, cRZString& error);  // @ 0x1003c05cc

    // Commit path.
    bool PlaceLot(const cS3DVector3& position, int facing, bool playEffects,
                  SC4Rect<int>& rect);                                        // @ 0x1003beef8
    bool PickTollBoothLotAndOrientation(std::uint32_t& lotConfigurationId, int& facing,
                                        cS3DVector3& position, SC4Rect<int>& rect);   // @ 0x1003c084c
    bool SeekCompatibleNetworks(SC4Rect<int>& rect, int maxDepth, int networkType);   // @ 0x1003c1b0e

    // PARTIAL: the two modal pick dialogs (the dialogs themselves are not rebuilt, the host owns them).
    std::uint32_t DoUserLotConfigurationTypePick(std::uint32_t current);      // @ 0x1003bc070
    std::uint32_t DoUserBuildingTypePick(std::uint32_t current);              // @ 0x1003bc502

    // Inspection for the test / the adapter.
    int ToolKind() const { return toolKind_; }
    std::uint32_t LotConfigurationId() const { return lotConfigurationId_; }
    std::uint32_t BuildingType() const { return buildingType_; }
    std::uint32_t PreviewBuildingType() const { return previewBuildingType_; }
    int Facing() const { return facing_; }
    int InitialFacing() const { return initialFacing_; }
    int FeedbackFacing() const { return feedbackFacing_; }
    bool TollBoothMode() const { return tollBoothMode_; }
    bool FeedbackValid() const { return feedbackValid_; }
    bool FeedbackSecondary() const { return feedbackSecondary_; }
    float PreviewX() const { return previewX_; }
    float PreviewY() const { return previewY_; }
    float PreviewZ() const { return previewZ_; }
    int PlacementsRemaining() const { return placementsRemaining_; }
    const cRZString& ErrorMessage() const { return errorMessage_; }
    std::uint32_t GlobalEffectId() const { return globalEffectId_; }
    std::uint8_t CoverageCategories() const { return coverageCategories_; }
    bool CoverageActive() const { return coverageActive_ != 0; }
    float CoverageRingRadius() const { return coverageRingRadius_; }
    std::int64_t BuildingPrice() const { return buildingPrice_; }
    std::int64_t LotCost() const { return lotCost_; }
    float GridSize() const { return gridSize_; }
    const std::vector<PreviewProp>& PreviewProps() const { return previewProps_; }
    const std::unordered_set<std::uint32_t>& LotConfigurationIds() const { return lotConfigurationIds_; }
    std::size_t SelectionEffectCount(bool red) const {
        return red ? selectionRed_.size() : selectionBlue_.size();
    }
    IPlaceLotEffect* CoverageEffect() const { return coverageEffect_; }
    IPlaceLotEffect* FootprintEffect() const { return footprintEffect_; }
    IPlaceLotEffect* DirectionEffect() const { return directionEffect_; }
    IPlaceLotProp* PreviewPropObject() const { return previewProp_; }
    const cRZString& CoverageEffectName() const { return effectName_; }
    const cRZString& PlacementEffectName() const { return selectionEffectName_; }
    void SetScreenClearRect(int x0, int z0, int x1, int z1) {
        clearX0_ = x0; clearZ0_ = z0; clearX1_ = x1; clearZ1_ = z1;
    }
    SC4Rect<int> ScreenClearRect() const { return SC4Rect<int>{clearX0_, clearZ0_, clearX1_, clearZ1_}; }
    void SetPreviewForTest(float x, float y, float z) { previewX_ = x; previewY_ = y; previewZ_ = z; }
    // Original field offsets, asserted by the test.
    static constexpr std::uint32_t kOffsetLotConfigurationId = 0x4c;
    static constexpr std::uint32_t kOffsetPreview = 0xd0;
    static constexpr std::uint32_t kOffsetGlobalEffectId = 0x1f8;
    static constexpr std::uint32_t kOffsetPreviewProps = 0x200;
    static constexpr std::uint32_t kOffsetPlacements = 0x21c;
    static constexpr std::uint32_t kOffsetMessageTarget = 0x40;

private:
    IPlaceLotHost* Host() const { return PlaceLotHost(); }
    IPlaceLotView* View() const;
    IPlaceLotTerrain* Terrain() const;
    IPlaceLotCity* City() const;

    // Effect helpers shared by the feedback paths.
    IPlaceLotEffect* CreateEffect(const char* name);
    void ReleaseEffect(IPlaceLotEffect*& effect);            // stop if started, then release
    void ClearEffectList(std::vector<IPlaceLotEffect*>& effects);
    void SetCoverageEffectTransform(const cS3DVector3& position);
    void SetFootprintEffectTransform(const cS3DVector3& position, int facing);
    void SetDirectionEffectTransform(const cS3DVector3& position, int facing, int sizeX, int sizeZ);
    void UpdateCoverageCategories(std::uint32_t configurationId);
    cSC4LotConfiguration* Configuration() const;

    int toolKind_ = 0;                                               // +0x48
    std::uint32_t lotConfigurationId_ = 0;                           // +0x4c
    std::uint32_t buildingType_ = 0;                                 // +0x50
    int initialFacing_ = 0;                                          // +0x54 (never written after the ctor)
    int facing_ = 0;                                                 // +0x58
    int feedbackFacing_ = -1;                                        // +0x5c
    int mouseX_ = 0;                                                 // +0x60
    int mouseY_ = 0;                                                 // +0x64
    bool isReward_ = false;                                          // +0x70
    // Derived, not part of the original layout: the building type of the preview prop (the original reads it
    // back off the preview occupant). It equals buildingType_ for the building-type tools and the resolved
    // type for a civic / landmark tool.
    std::uint32_t previewBuildingType_ = 0;
    std::unordered_set<std::uint32_t> lotConfigurationIds_;          // +0x78
    std::vector<std::uint32_t> buildingTypes_;                       // +0xa8
    int clearX0_ = 0, clearZ0_ = 0, clearX1_ = 0, clearZ1_ = 0;      // +0xc0..+0xcc
    float previewX_ = 0, previewY_ = 0, previewZ_ = 0;               // +0xd0/+0xd4/+0xd8
    bool feedbackValid_ = false;                                     // +0xdc
    bool feedbackSecondary_ = false;                                 // +0xdd
    cRZString errorMessage_;                                         // +0xe0
    IPlaceLotLocalGrid* localGrid_ = nullptr;                        // +0x108 (seam)
    cRZString effectName_;                                           // +0x110
    cRZString selectionEffectName_;                                  // +0x138
    IPlaceLotEffect* coverageEffect_ = nullptr;                      // +0x160
    IPlaceLotEffect* footprintEffect_ = nullptr;                     // +0x168
    IPlaceLotEffect* directionEffect_ = nullptr;                     // +0x170
    IPlaceLotProp* previewProp_ = nullptr;                           // +0x178
    std::vector<IPlaceLotEffect*> selectionBlue_;                    // +0x180
    std::vector<IPlaceLotEffect*> selectionRed_;                     // +0x198
    void* gridTexture_ = nullptr;                                    // +0x1b0 (cS3DTextureBinding)
    float gridSize_ = 0.0f;                                          // +0x1b8
    std::uint8_t buildingRotation_ = 0;                              // +0x1bc
    float modelCenterX_ = 0.0f, modelCenterY_ = 0.0f, modelCenterZ_ = 0.0f;   // +0x1c0/+0x1c4/+0x1c8
    std::uint8_t coverageActive_ = 0;                                // +0x1cc
    float coverageRadius_ = 0.0f;                                    // +0x1d0
    float coverageRingRadius_ = 32.0f;                               // +0x1d4
    std::uint8_t coverageCategories_ = 0;                            // +0x1d8
    std::int64_t buildingPrice_ = -100000;                           // +0x1e0
    std::int64_t lotCost_ = -100000;                                 // +0x1f0
    std::uint32_t globalEffectId_ = 0;                               // +0x1f8
    std::vector<PreviewProp> previewProps_;                          // +0x200
    bool tollBoothMode_ = false;                                     // +0x218
    int placementsRemaining_ = 1000;                                 // +0x21c
};

}  // namespace sc4
