// cSC4ModelMaker — the factory that turns building exemplars into renderable models (meshes, LOD choices,
// textures). 62 symbols at 0x10030c5e8..0x100314e2e: one object that every occupant's models go through, plus
// the two compiler thunks of every virtual and the deferred-operation record helper.
//
// What it does
//   * owns the model cache decisions: which occupants' models to load for the current zoom / rotation / view
//     filter, the load rectangle around the visible area (optionally expanded by the background loader) and the
//     eviction of models outside it (UnloadUnnecessaryModels / RemoveModelsInappropriateForRotation);
//   * creates model instances from exemplar instance ids: AddModel looks the S3D model resource up, falls back to
//     a placeholder instance whose id is chosen from the footprint size, computes the placement transform
//     (FindModelTransform) and adds the instance to the occupant's placeable object;
//   * the per-occupant state effects: lighting (DoLightingChanged + RebuildDynamicTexture, five tinted dynamic
//     textures), lot condition (ApplyLotStateEffect), highlight (DoOccupantHilighted), building completion
//     (DoBuildingCompletionChanged), terrain hiding on the cells an occupant covers (DoTerrainHidingFor*);
//   * the message subscription and dispatch (DoMessage): every occupant / lot / view / lighting message is
//     turned into the matching Do* handler, optionally deferred while EnableDeferredOccupantOperations is on.
//
// Port note: cIGZUnknown's destructor is declared non-virtual in src/gz/cIGZUnknown.h, so this port's vtable has no
// destructor slots (the original's slots 0x20 / 0x28); every other slot keeps its original offset.
//
// Original layout (0x180 bytes; the GZCOM class table also says 0x180):
//   +0x000 cIGZMessageTarget2 vtable (cISC4ModelMaker's primary group: QueryInterface, AddRef, Release, DoMessage,
//          the two destructors, Write, Read, GetGZCLSID)
//   +0x008 cIGZSerializable vtable (the second group)
//   +0x010 u8   initialized (Init / Shutdown guard)
//   +0x011 u8   cityInitialized (DoPostCityInit has registered the message targets)
//   +0x012 u8   readFromSave (Read ran; makes Init skip the work it already restored)
//   +0x014 u32  reference count (both groups share it)
//   +0x018 cIGZPersistResourceManager* resourceManager (SL::spRM)
//   +0x020 cIGZUnknown* renderer (the view's renderer, view vslot 0x20)
//   +0x028 SC4ViewUtilities* viewUtilities (renderer vslot 0x40)
//   +0x030 cIGZUnknown* animationTimer (SL::ATM())
//   +0x038 i32  zoom                      +0x03c i32  rotation
//   +0x040 i32  city size x               +0x044 i32  city size z
//   +0x048 SC4Rect<int> cityRect
//   +0x058 u32  streamed (meaning not recovered)
//   +0x05c u8   "defer the view update" (set from a streamed flag; DoMessage drops view updates while it is set)
//   +0x060 i32  pending zoom             +0x064 i32  pending rotation
//   +0x068 u8   inside a view-filter iteration (DoOccupantDestroyed etc. skip terrain hiding then)
//   +0x070 cISC4OccupantManager*         +0x078 cISC4OccupantFilter* current view filter
//   +0x080 cSC4OccupantTypesFilterHashed* model filter (the 11 model-bearing occupant types)
//   +0x088 cSC4OccupantTypesFilterHashed* terrain-hiding filter (the 5 types that hide terrain)
//   +0x090 cISC4OccupantFilter* zoom/rotation model-change filter
//   +0x098 cISC4LotManager*              +0x0a0 cIGZUnknown* building development simulator
//   +0x0a8 cIGZUnknown* terrain
//   +0x0b0 IModelAnimationCoordinator*
//   +0x0b8 i32  load radius [5] (per zoom level, DetermineLoadRadiusAroundVisibleArea)
//   +0x0cc SC4Rect<int> load rectangle (cells)   +0x0dc i32 focus x   +0x0e0 i32 focus z
//   +0x0e4 u8   background load task running
//   +0x0e8 i32  focus x at the last background load start   +0x0ec i32 focus z
//   +0x0f0 u32  zoom mask (bits 0..4)    +0x0f4 u32  rotation mask (bits 0..3)
//   +0x0f8 u8   background loading enabled
//   +0x100 IModelBackgroundLoader*
//   +0x108 u8   city "loaded" flag (message 0x26c63343 sets it, 0x26c63345 clears it)
//   +0x109 u8   ignore missing models   +0x10a u8 unload-unnecessary-models pending
//   +0x10b u8   background loading suspended
//   +0x10c float lighting colour r, +0x110 g, +0x114 b
//   +0x118 cS3DTextureBinding* dynamic texture [5] (slots 0..4, the RebuildDynamicTexture ids)
//   +0x140 std::vector<DeferredOccupantOperation> (0x18 bytes: begin / end / capacity)
//   +0x158 i32  deferred-operation nesting depth (EnableDeferredOccupantOperations)
//   +0x15c float camera yaw, +0x160 rotation angle, +0x164 pitch, +0x168 screen pitch
//   +0x16c i32 previous zoom, +0x170 previous rotation, +0x174 current zoom, +0x178 current rotation
//
// Quirks kept (addresses are the original functions)
//  * QueryInterface answers 0xa9c741b3 (the class id) as well as 0xe4fda3d4 (cIGZSerializable) on the *second*
//    group, and 1 / 0x452294aa (cIGZMessageTarget2) on the primary group (0x10030c87e).
//  * Release destroys the object while the reference count is 1 or less - the count starts at 0, so the first
//    Release deletes it; the deleting destructor (vslot 0x28) is what runs (0x10030c8e6).
//  * Shutdown clears the five dynamic texture bindings with a loop that stops after five of the six iterations
//    it looks like it wants (0x10030ca8e).
//  * Init keeps the *loader's* radius: if the renderer reports fewer than 3 detail levels the LOD offset is
//    that count, and 0 when it reports none (0x10030ce61) - see cSC4ModelMakerUtility::nModelLODOffset.
//  * DoOccupantChanged drops the model without re-creating it when the message says the occupant did not change
//    type (0x10030e098, the second branch).
//  * DoOccupantHilighted only acts when old != new highlight, and stores the two highlight values as *bytes*
//    in the deferred record (0x10030e5ba).
//  * DoOccupantMoved only rebuilds models for the two occupant types 0x74758926 / 0xa823821e and then bumps the
//    transform change counter and sets flag 0x100 on every instance (0x10030e218).
//  * ChangeModels returns false when the occupant has no placeable object, and uses layer 3 for a zero layer
//    argument (0x100310ea8).
//  * CommitDeferredOccupantOperations merges a "created" + "destroyed" pair and a "changed" + "shown" pair in
//    place, and a "destroyed" record cancels an earlier "created" record (0x100311bc2).
//  * DoTerrainHidingForOccupantCells would read `local_38` uninitialised when the terrain-hiding filter accepts an
//    occupant whose QueryInterface fails; the port keeps the "no foundation" answer (0x1003115f2).
//  * DetermineLoadRadiusAroundVisibleArea rounds with (int)(v - 0.5 + 12582912) - 12582912 and clamps to
//    {8, 8, 6, 6, 6}, with {0.5, 0.25, 0.125, 0.0625, 0.03125} cells per MB of memory (0x10030d0fa).
//
// Seams (see docs/SEAMS.md): the view / renderer (SL::spView3D, cISC4Renderer), the view utilities, the lighting
// manager, the terrain cell flags, the city cell query, the simulator ini, the model maker utility
// (cSC4ModelMakerUtility) and the three helper objects (cSC4ModelBackgroundLoader, cSC4AnimationCoordinator,
// cSC4ZoomRotationModelChangeOccupantFilter). Every seam method mirrors one original call and carries its slot /
// address in the comment.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "gz/StreamOps.h"
#include "gz/cGZPersistResourceKey.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZPersistResourceManager.h"
#include "gz/cIGZSerializable.h"
#include "s3d/ModelInstance.h"
#include "s3d/ModelInstanceAnimatedSprite.h"
#include "sc4/MiscTypes.h"
#include "sc4/Occupant.h"
#include "sc4/OccupantFilters2.h"
#include "sc4/OccupantManager.h"
#include "sc4/ZoneDeveloper.h"

namespace sc4 {

class cSC4ModelMaker;

constexpr std::uint32_t kGZCLSID_cSC4ModelMaker = 0xa9c741b3;
constexpr std::uint32_t kGZIID_cISC4ModelMaker = 0xa9c741b3;
// kGZIID_cIGZSerializable (0xe4fda3d4) comes from sc4/DBSegment.h.

// The occupant types the model maker reacts to (CreateOccupantFilters, 0x10030cebc).
constexpr std::uint32_t kModelOccupantTypeBuilding = 0x278128a0;
constexpr std::uint32_t kModelOccupantTypeFoundation = 0x68fd0c69;
constexpr std::uint32_t kModelOccupantTypeRetainingWall = 0x895d1169;
constexpr std::uint32_t kModelOccupantTypeFlora = 0x74758926;
constexpr std::uint32_t kModelOccupantTypeProp = 0xa823821e;
constexpr std::uint32_t kModelOccupantTypeLotBaseTexture = 0x497f6d9d;
constexpr std::uint32_t kModelOccupantTypeNetwork = 0xc772bf98;
constexpr std::uint32_t kModelOccupantTypeNetworkShadow = 0x2890d4de;
constexpr std::uint32_t kModelOccupantTypeUnderground = 0x2a183167;
constexpr std::uint32_t kModelOccupantTypeMiscA = 0x8a13918a;
constexpr std::uint32_t kModelOccupantTypeMiscB = 0x6a11bd2b;

// The message types DoMessage dispatches (the occupant and lot ones are in sc4/Occupant.h and sc4/Lot.h; the
// numbers are the original's, several computed as negative switch cases).
constexpr std::uint32_t kMessageOccupantShown = 0x99ef1144;      // DoOccupantShown (0x99ef1146 is "moved")
constexpr std::uint32_t kMessageOccupantMoved = 0x99ef1146;
constexpr std::uint32_t kMessageViewZoomRotation = 0xa6b79601;     // void1 / void2: two SC4Point<int> payloads
constexpr std::uint32_t kMessageViewZoomRotationDeferred = 0xa6b79602;
constexpr std::uint32_t kMessageViewTranslation = 0xa6b79611;
constexpr std::uint32_t kMessageViewTranslationDeferred = 0xa6b79620;
constexpr std::uint32_t kMessageViewFilterChange = 0x6cdb65b;
constexpr std::uint32_t kMessageCityLoaded = 0x26c63343;
constexpr std::uint32_t kMessageCityUnloaded = 0x26c63345;
constexpr std::uint32_t kMessageCityPostInit = 0x26d31ec1;
constexpr std::uint32_t kMessageLightingChanged = 0xc9da96ea;
constexpr std::uint32_t kMessageRenderDetailLevelChanged = 0xc9f775bb;   // recompute nModelLODOffset
constexpr std::uint32_t kMessagePropStateChangedM = 0xe9c86ddb;
constexpr std::uint32_t kMessageLotStateChangedM = 0x9d8e0b6;
constexpr std::uint32_t kMessageBuildingCompletionChanged = 0x2977ae2f;
constexpr std::uint32_t kMessageDeferredOccupantOperations = 0x49d968c4;
constexpr std::uint32_t kMessageUnloadModels = 0xba44eb51;
constexpr std::uint32_t kMessageSuspendLoading = 0xcad61e00;
constexpr std::uint32_t kMessageResumeLoading = 0xead61e16;
constexpr std::uint32_t kMessageBackgroundLoaderCommunication = 0x86ad10ee;

// The three detail levels the view can run at (render properties property 0x16), and the LOD offset the original
// keeps in a static of cSC4ModelMakerUtility.
extern int nModelLODOffset;

// ---- seam: cSC4ModelMakerUtility (unrecovered, 0x1002c03c8..0x1002c0e82) -----------------------------------
class IModelMakerUtility {
public:
    virtual ~IModelMakerUtility() = default;
    virtual int GetOccupantModelClass(cISC4Occupant* occupant) = 0;                     // 0x1002c0e82
    virtual void GetOccupantOrientationAndState(cISC4Occupant* occupant, std::uint32_t& orientation,
                                                int& state) = 0;                        // 0x1002c0d70
    // One instance id per model and, in `transforms`, three floats per id (offset xyz; all zero = no offset).
    virtual void GetAllModelInstanceIDs(std::vector<std::uint32_t>& ids, std::vector<float>* transforms,
                                        cISC4Occupant* occupant, int zoom, int rotation, int state, bool lit,
                                        std::uint32_t orientation, std::uint32_t* outA,
                                        std::uint32_t* outB) = 0;                       // 0x1002c04ac
    virtual bool OccupantUsesBuildingStyleModels(cISC4Occupant* occupant) = 0;          // 0x1002c0ce0
    virtual bool FoundationOccupantUsesBuildingStyleModels(cISC4Occupant* occupant) = 0;   // 0x1002c0ca4
    virtual std::uint32_t GetModelProperty1Instance(std::uint32_t baseId, int rotation, int index) = 0;   // 0x1002c03c8
    virtual bool GetAnimatedModelFromResourceKey(const cGZPersistResourceKey& key,
                                                 s3d::cIS3DModelInstanceAnimated** out) = 0;   // 0x1001a6fb7
};
IModelMakerUtility* ModelMakerUtility();
void SetModelMakerUtility(IModelMakerUtility* utility);

// The model instance a network occupant hands out from GetModelInstances (cISC4NetworkOccupant vslot 0x108, the
// occupant's own cS3DModelInstanceBasic subobject in the original; see sc4/NetworkOccupantModel.h). One reference.
using NetworkModelFactory = s3d::cIS3DModelInstance* (*)(cISC4Occupant* occupant);
void SetNetworkModelFactory(NetworkModelFactory factory);

// ---- seam: cSC4ModelBackgroundLoader (unrecovered owner of the background load tasks) -----------------------
class IModelBackgroundLoader : public cIGZUnknown {
public:
    virtual void Shutdown() = 0;                                                     // 0x1003623a0
    virtual bool Init(cSC4ModelMaker* owner) = 0;                                    // 0x10036249a
    virtual void ExpandLoadRect(SC4RectT<int>& rect, int cells) = 0;                 // 0x100363da8
    virtual void StartLoadingForZoomAndRotation(int zoom, int rotation, const SC4RectT<int>& rect,
                                                const SC4Point<int>& focus) = 0;     // 0x100363274
    virtual void RefocusLoadToNewViewLocation(const SC4RectT<int>& rect, const SC4Point<int>& focus) = 0;   // 0x100363436
    virtual void CancelAllLoadTasks() = 0;                                           // 0x100363592
    virtual void SuspendLoading() = 0;                                               // 0x100363604
    virtual void ResumeLoading() = 0;                                                // 0x10036360e
    virtual void ProcessBackgroundThreadToMainThreadCommunications() = 0;            // 0x1003639ee
};

// ---- seam: cSC4AnimationCoordinator (unrecovered owner of the occupant animations) --------------------------
class IModelAnimationCoordinator : public cIGZUnknown {
public:
    virtual void Shutdown() = 0;                                                     // 0x1003588c4
    virtual bool Init() = 0;                                                         // 0x100358a56
    virtual void SetOccupantAnimation(cISC4Occupant* occupant, bool on) = 0;         // 0x1003592fa
};

// ---- seam: the view, the renderer, the view utilities, the lighting manager, the terrain and the city --------
// cSTEOverlayManager::AddShadow (vslot 0x58) as the model maker calls it; `texture` is the model material's texture binding.
class IShadowOverlay {
public:
    virtual ~IShadowOverlay() = default;
    virtual std::uint32_t AddShadow(std::uint64_t texture, int pointCount, const cS3DVector3* points, const cS3DVector2* texCoords,
                                    const cS3DBoundingBox& box, const cS3DTransform& transform) = 0;   // 0xffffffff when nothing was added
};

class IModelMakerHost {
public:
    virtual ~IModelMakerHost() = default;
    // SL::spView3D: the renderer it hands out (view vslot 0x20).
    virtual cIGZUnknown* Renderer() = 0;
    // The message server (SL::spMS2) and the animation tick manager (SL::ATM(), notifications on channel 0xc0).
    virtual cIGZMessageServer2* MessageServer() = 0;
    virtual cIGZMessageServer2* AnimationTickManager() = 0;
    // The managers DoPostCityInit takes (SL::spOccupantManager / spLotManager / spBuildingDevelopmentSim / spTerrain).
    virtual cISC4OccupantManager* OccupantManager() = 0;
    virtual cISC4LotManager* LotManager() = 0;
    virtual cIGZUnknown* BuildingDevelopmentSim() = 0;
    virtual cIGZUnknown* Terrain() = 0;
    // The current view filter (view vslot 0x90 -> cSC4ViewFilterInformation::filter); returns one reference.
    virtual cISC4OccupantFilter* CurrentViewFilter() = 0;
    // SL::spView3D vslot 0x90 called with its result discarded (0x10030f6d6); the original refreshes the view
    // filter information here.
    virtual void RefreshViewFilter() = 0;
    // The renderer (SL::spRenderer is the view's renderer): vslot 0xd8 Has, 0xf0 update, 0xf8 add(instance, layer).
    virtual bool RendererHasModelInstance(s3d::cIS3DModelInstance* instance) = 0;
    virtual void RendererUpdateModelInstance(s3d::cIS3DModelInstance* instance) = 0;
    // renderer vslot +0x7c (i386): ChangeModel(instance, code) for a model the renderer already holds (1 highlight, 2 / 3 a
    // layer refresh, 5 draw properties). Not an insert: inserting again built a second grid element that outlived the model's
    // removal, so a zoom change left the old zoom's model drawing over the new one.
    virtual void RendererChangeModelInstance(s3d::cIS3DModelInstance* instance, int code) = 0;
    // SL::spViewUtilities (the object is borrowed; the model maker only reads the fields below through the host).
    virtual cIGZUnknown* ViewUtilities() = 0;
    // SL::spViewUtilities: +0x14 zoom, +0x18 rotation, +0x24 rotation angle, +0x15c viewport units per world unit.
    virtual int ViewZoom() = 0;
    virtual int ViewRotation() = 0;
    virtual float ViewRotationAngle() = 0;
    virtual float ViewportUnitsPerWorldUnit() = 0;
    // +0x3e4 / +0x3ec focus point and +0x3f4..+0x400 the visible ground rectangle (x0, z0, x1, z1), world units.
    virtual bool ViewGroundRectangle(float& focusX, float& focusZ, float rect[4]) = 0;
    // SL::spViewUtilities +0x3f4 (the model-cull threshold scale at SL::spRenderProperties +0x48 / +0x2c0 is the
    // other factor; see AddModel).
    virtual float ModelSizeThreshold() = 0;
    // SL::spRenderProperties +0x48 -> +0x80: the alpha ApplyLotStateEffect gives a tinted dynamic texture.
    virtual float TextureTintAlpha() = 0;
    // SL::spRenderProperties vslot 0x38(0x16): the render detail level, 0..2 (the model LOD offset is min(level, 2)).
    virtual int RenderDetailLevel() = 0;
    // SL::spLightingManager vslot 0x50 (a float4 out; the fourth is unused).
    virtual void LightingColour(float out[4]) = 0;
    // SL::spTerrain: vslot 0x38 size x, 0x40 size z, 0x290 cell flags, 0x288 set the cell's interior flags,
    // 0x228 refresh the overlay for a rectangle, 0x268 "cell has water", 0xd0 ground normal.
    virtual int TerrainSizeX() = 0;
    virtual int TerrainSizeZ() = 0;
    virtual std::uint32_t TerrainCellFlags(int x, int z) = 0;
    virtual void TerrainSetCellFlags(int x, int z) = 0;
    virtual void TerrainRefreshCell(int x, int z) = 0;
    virtual bool TerrainCellHasWater(int x, int z) = 0;
    virtual void TerrainNormalAt(float x, float z, float out[3]) = 0;
    // SL::spTrafficNetworkMap vslot 0x58 network-at(x, z, 0x1f4f, 1); false when the cell has no network.
    virtual bool NetworkAt(float x, float z) = 0;
    // SL::spCity vslot 0x2f8: a world position to a standard city cell.
    virtual void PositionToCityCell(float x, float z, int& cx, int& cz) = 0;
    // The simulator ini (framework vslot 0xa8, then vslot 0x58 GetValue(key, value, 1)).
    virtual bool ReadConfigOption(const char* key, std::string& value) = 0;
    // Factories for the three helper objects (all unrecovered owners, one reference each).
    virtual IModelBackgroundLoader* CreateBackgroundLoader() = 0;                     // new(0x280) + Init(this)
    virtual IModelAnimationCoordinator* CreateAnimationCoordinator() = 0;              // new(0x88) + Init()
    // GetAnimatedModelFromResourceKey (i386 0x00139f80): key type 0x29a5d1ec -> GetSpriteAnimFromResourceKey, 0x5ad0e817 ->
    // GetMeshAnimFromResourceKey, else nothing. One reference; null when there is no animation.
    virtual s3d::cIS3DModelInstanceAnimated* CreateAnimatedModel(const cGZPersistResourceKey& key) { (void)key; return nullptr; }
    virtual cISC4OccupantFilter* CreateZoomRotationModelChangeOccupantFilter() = 0;    // 0x1002c119a
    virtual void SetZoomRotationFilter(cISC4OccupantFilter* filter, int zoom, int rotation) = 0;   // 0x1002c11d6
    // cSC4ModelMakerUtility::GetModelInstanceID(occupant, zoom, rotation, rotationAndFlip, flag, &a, &b) @ 0x1002c03da
    virtual std::uint32_t GetModelInstanceID(cISC4Occupant* occupant, int zoom, int rotation, int rotationAndFlip,
                                             bool flag, std::uint32_t* outA, std::uint32_t* outB) = 0;
    // cSC4NetworkManager::MapShadowTexture @ 0x10016ece4 / the two terrain overlay managers of the shadow decal.
    virtual std::uint32_t MapShadowTexture(std::uint32_t instance, std::uint8_t& rotation, std::uint32_t networkFlags,
                                           std::uint32_t raisedEdges) = 0;
    // cSTEOverlayManager::AddShadowDecal @ 0x1007292c6 (vslot 0x50) on the land (0) or water (1) overlay manager.
    virtual std::uint32_t AddShadowDecal(int overlay, const cGZPersistResourceKey& key, float x, float z,
                                         float height, float rotation, bool flip) = 0;
    // ---- what CreateOccupantShadow (0x100313a00) reaches; the defaults make shadows a no-op until a host provides them ----
    // SL::spRenderProperties + 0x30 -> +0x398: the shadow quality (props cast shadows from 3, the box passed on changes at 5).
    virtual int ShadowRenderQuality() { return 0; }
    // SL::spRenderProperties + 0x48 -> +0x230: a model whose height times the view scale is below this casts no shadow.
    virtual float ShadowMinimumHeight() { return 0.0f; }
    // The model maker's +0x28 object's float at +0x15c: the scale of a model's height on the screen.
    virtual float ShadowViewScale() { return 1.0f; }
    // The terrain's overlay manager `index` (terrain vslot 0x210 -> vslot 0x90): the land (0) overlay that receives shadows.
    virtual IShadowOverlay* ShadowOverlay(int index) { (void)index; return nullptr; }
};
IModelMakerHost* ModelMakerHost();
void SetModelMakerHost(IModelMakerHost* host);

// ---- the deferred occupant operation (0x100314e90) ----------------------------------------------------------
struct DeferredOccupantOperation {
    // The operation type stored at +0x08; CommitDeferredOccupantOperations switches on it.
    enum OperationType : std::uint8_t {
        kCreated = 1, kDestroyed = 2, kShown = 3, kChanged = 4, kMoved = 5, kHilighted = 6,
        kInserted = 7, kRemoved = 8,
    };

    cISC4Occupant* occupant = nullptr;   // +0x00 (one reference)
    std::uint8_t operation = 0;          // +0x08
    std::uint8_t arg1 = 0;               // +0x09 (highlight from / "inserted" flag)
    std::uint8_t arg2 = 0;               // +0x0a (highlight to)
    std::uint8_t pad[5] = {};

    DeferredOccupantOperation() = default;
    DeferredOccupantOperation(cISC4Occupant* value, OperationType type);   // 0x100314e90
    DeferredOccupantOperation(const DeferredOccupantOperation& other);      // takes a reference
    DeferredOccupantOperation& operator=(const DeferredOccupantOperation& other);
    ~DeferredOccupantOperation();                                          // drops the reference
};
static_assert(sizeof(DeferredOccupantOperation) == 0x10, "DeferredOccupantOperation is 0x10 bytes");

// The model-instance array the original takes from a cRZStackAllocator<cIS3DModelInstance*,10,1>: ten entries on
// the stack, a heap array beyond that.
class ModelInstanceBuffer {
public:
    explicit ModelInstanceBuffer(std::uint32_t count);
    ~ModelInstanceBuffer();
    ModelInstanceBuffer(const ModelInstanceBuffer&) = delete;
    ModelInstanceBuffer& operator=(const ModelInstanceBuffer&) = delete;
    s3d::cIS3DModelInstance** data() { return data_; }

private:
    s3d::cIS3DModelInstance* small_[10] = {};
    s3d::cIS3DModelInstance** heap_ = nullptr;
    s3d::cIS3DModelInstance** data_ = nullptr;
};

// ---- the class -----------------------------------------------------------------------------------------------
class cSC4ModelMaker final : public cIGZMessageTarget2, public cIGZSerializable {
public:
    cSC4ModelMaker();                                   // 0x10030c5fe (C2), 0x10030c5e8 (C1 alias)
    ~cSC4ModelMaker();                                  // 0x10030c78c (D2), 0x10030c846 (D1), 0x10030c850 (D0)

    bool QueryInterface(std::uint32_t iid, void** out) override;    // 0x10030c87e
    std::uint32_t AddRef() override;                                // 0x10030c8ca
    std::uint32_t Release() override;                               // 0x10030c8e6
    bool DoMessage(cIGZMessage2* message) override;                 // 0x10030d75a
    bool Write(cIGZOStream& stream) const override;                 // 0x10030d1d2
    bool Read(cIGZIStream& stream) override;                        // 0x10030d36e
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4ModelMaker; }   // 0x100314e0c

    bool Init();                                        // 0x10030cd76
    bool Shutdown();                                    // 0x10030c958

    // The background loader calls this when its zoom/rotation load task has finished (0x100314df4).
    void LoadForZoomLevelCompleted(int zoom) { loadTaskActive_ = false; (void)zoom; }

    // The handlers DoMessage dispatches to (all non-virtual in the original).
    void CreateOccupantFilters();                                       // 0x10030cebc
    void DetermineLoadRadiusAroundVisibleArea();                        // 0x10030d0fa
    void DoOccupantCreated(cISC4Occupant* occupant, bool flag, std::uint32_t type);      // 0x10030de94
    void DoOccupantDestroyed(cISC4Occupant* occupant, bool flag, std::uint32_t type);    // 0x10030dfb4
    void DoOccupantChanged(cISC4Occupant* occupant, bool flag, std::uint32_t type);      // 0x10030e098
    void DoOccupantMoved(cISC4Occupant* occupant, bool flag, std::uint32_t type);        // 0x10030e218
    void DoLotStateChanged(class cSC4Lot* lot, std::uint32_t oldState, std::uint32_t newState);   // 0x10030e4b2
    void DoOccupantHilighted(cISC4Occupant* occupant, int from, int to);                 // 0x10030e5ba
    void UpdateModelLoadRect();                                          // 0x10030e844
    void DoZoomAndRotationChange(int oldZoom, int oldRotation, int zoom, int rotation);  // 0x10030ea18
    void DoViewTranslationChange();                                      // 0x10030ed04
    void DoViewFilterChange(void* oldInfo, void* newInfo);               // 0x10030eea0
    void DoOccupantInserted(cISC4Occupant* occupant, bool flag, std::uint32_t type);     // 0x10030efde
    void DoOccupantRemoved(cISC4Occupant* occupant, bool flag, std::uint32_t type);      // 0x10030f0e0
    void DoOccupantShown(cISC4Occupant* occupant, bool shown, std::uint32_t type);       // 0x10030f1ce
    void DoPostCityInit();                                               // 0x10030f36c
    void DoBuildingCompletionChanged(cISC4BuildingOccupant* building);   // 0x10030faba
    void ApplyLotStateEffect(cISC4Occupant* occupant, std::uint32_t type, std::uint32_t condition);   // 0x10030fe50
    void DoPropStateChanged(cISC4Occupant* prop, std::uint8_t state);   // 0x1003108b0
    void EnableDeferredOccupantOperations(bool enable);                  // 0x10031093a
    void DoLightingChanged();                                            // 0x100310972
    void UnloadUnnecessaryModels();                                      // 0x100310bfe
    bool ChangeModels(cISC4Occupant* occupant, std::uint32_t type, int layer);   // 0x100310ea8
    bool RemoveModels(cISC4Occupant* occupant, std::uint32_t type);     // 0x100311028
    bool AddModels(cISC4Occupant* occupant, int zoom, int rotation, std::uint32_t type, bool visible);   // 0x1003110be
    bool DoTerrainHidingForOccupantCells(cISC4Occupant* occupant, std::uint32_t type, bool hide);   // 0x1003115f2
    void RebuildDynamicTexture(int slot);                                // 0x1003116d8
    void CommitDeferredOccupantOperations();                             // 0x100311bc2
    bool OccupantFilterIterationFunction(cISC4Occupant* occupant);       // 0x100311ee4
    bool DoTerrainHidingForCells(const SC4RectT<int>& rect, bool hide);  // 0x10031200e
    void RemoveModelsInappropriateForRotation(int rotation);             // 0x1003127ec
    void LoadOccupantModelsForZoomAndRotation(cISC4Occupant* occupant, int zoom, int rotation);   // 0x1003128c2
    void UpdateFocusOfBackgroundLoad();                                  // 0x100312eba
    bool GetOccupantLotCondition(cISC4Occupant* occupant, std::uint32_t& condition);   // 0x100312f2e
    bool LoadBuildingModels(cISC4Occupant* occupant, int zoom, int rotation, std::uint32_t type);   // 0x10031300c
    bool LoadFoundationModels(cISC4Occupant* occupant, int zoom, int rotation, std::uint32_t type);   // 0x10031304c
    bool ApplyStateEffectToModels(cISC4Occupant* occupant, std::uint32_t type, std::uint32_t condition);   // 0x100313254
    bool LoadFloraModels(cISC4Occupant* occupant, int zoom, int rotation, std::uint32_t type);   // 0x10031332c
    bool LoadModelsCommon(cISC4Occupant* occupant, int zoom, int rotation, std::uint32_t type);   // 0x10031336c
    bool LoadPropModels(cISC4Occupant* occupant, int zoom, int rotation, std::uint32_t type);   // 0x1003137f4
    bool LoadLotBaseTextureModels(cISC4Occupant* occupant, int zoom, int rotation, std::uint32_t type);   // 0x100313858
    bool LoadNetworkModels(cISC4Occupant* occupant, int zoom, int rotation);   // 0x1003138c4
    bool CreateOccupantShadow(cISC4Occupant* occupant, int zoom, int rotation);   // 0x100313a00
    bool LoadUndergroundConnectionModels(cISC4Occupant* occupant, int zoom, int rotation);   // 0x100314220
    s3d::cIS3DModelInstance* AddModel(cISC4Occupant* occupant, const cGZPersistResourceKey& key, int rotation,
                                      int state, bool lit, const float* position, const float* offset);   // 0x100314328
    s3d::cIS3DModelInstance* CreatePlaceHolderModelInstance(cISC4Occupant* occupant, int zoom, int rotation,
                                                            int facing, bool lit);   // 0x10031470e
    void FindModelTransform(cISCPropertyHolder* holder, bool objectRotation, int rotation, const float* position,
                            const float* offset, cS3DTransform& transform);   // 0x100314822

    // --- accessors the tests and the seams use ---------------------------------------------------------------
    cISC4OccupantManager* OccupantManager() const { return occupantManager_; }
    cISC4LotManager* LotManager() const { return lotManager_; }
    cISC4OccupantFilter* ModelFilter() const { return modelFilter_; }
    cISC4OccupantFilter* TerrainHidingFilter() const { return terrainHidingFilter_; }
    cISC4OccupantFilter* ZoomRotationFilter() const { return zoomRotationFilter_; }
    IModelBackgroundLoader* BackgroundLoader() const { return backgroundLoader_; }
    IModelAnimationCoordinator* AnimationCoordinator() const { return animationCoordinator_; }
    bool IsInitialized() const { return initialized_; }
    bool BackgroundLoadingEnabled() const { return backgroundLoading_; }
    bool IgnoreMissingModels() const { return ignoreMissingModels_; }
    int DeferredDepth() const { return deferredDepth_; }
    const std::vector<DeferredOccupantOperation>& DeferredOperations() const { return deferred_; }
    const SC4RectT<int>& LoadRect() const { return loadRect_; }
    const SC4RectT<int>& CityRect() const { return cityRect_; }
    int LoadRadius(int zoom) const { return zoom >= 0 && zoom < 5 ? loadRadius_[zoom] : 0; }
    std::uint32_t ZoomMask() const { return zoomMask_; }
    std::uint32_t RotationMask() const { return rotationMask_; }
    int Zoom() const { return currentZoom_; }
    int Rotation() const { return currentRotation_; }
    cIGZUnknown* DynamicTexture(int slot) const {
        return slot >= 0 && slot < 5 ? dynamicTexture_[slot] : nullptr;
    }

private:
    void PushDeferred(cISC4Occupant* occupant, DeferredOccupantOperation::OperationType type);   // 0x100314e90 caller
    // One occupant-manager query (vslot 0x138 without rectangles / 0x148 with) whose result the caller walks; the
    // list entries each hold one reference and are released here.
    using OccupantVisitor = bool (cSC4ModelMaker::*)(cISC4Occupant*);
    void VisitOccupants(const SC4RectT<int>* rect, cISC4OccupantFilter* filter, OccupantVisitor visitor);
    void ReleaseViewFilter();

    // ---- layout (offsets are the original's; see the header comment) ----
    bool initialized_ = false;                      // +0x10
    bool cityInitialized_ = false;                  // +0x11
    bool readFromSave_ = false;                     // +0x12
    std::uint8_t pad13_ = 0;                        // +0x13
    std::uint32_t refCount_ = 0;                    // +0x14
    cIGZPersistResourceManager* resourceManager_ = nullptr;   // +0x18
    cIGZUnknown* renderer_ = nullptr;               // +0x20
    cIGZUnknown* viewUtilities_ = nullptr;          // +0x28 (borrowed)
    cIGZUnknown* animationTimer_ = nullptr;         // +0x30
    int zoom_ = 0;                                  // +0x38
    int rotation_ = 0;                              // +0x3c
    int citySizeX_ = 0;                             // +0x40
    int citySizeZ_ = 0;                             // +0x44
    SC4RectT<int> cityRect_;                        // +0x48
    std::uint32_t stream58_ = 0;                    // +0x58
    bool deferViewUpdate_ = false;                  // +0x5c
    std::uint8_t pad5d_[3] = {};                    // +0x5d..+0x5f
    int pendingZoom_ = 0;                           // +0x60
    int pendingRotation_ = 0;                       // +0x64
    bool inViewIteration_ = false;                  // +0x68
    std::uint8_t pad69_[7] = {};
    cISC4OccupantManager* occupantManager_ = nullptr;   // +0x70
    cISC4OccupantFilter* viewFilter_ = nullptr;     // +0x78
    cSC4OccupantTypesFilterHashed* modelFilter_ = nullptr;       // +0x80
    cSC4OccupantTypesFilterHashed* terrainHidingFilter_ = nullptr;   // +0x88
    cISC4OccupantFilter* zoomRotationFilter_ = nullptr;          // +0x90
    cISC4LotManager* lotManager_ = nullptr;         // +0x98
    cIGZUnknown* buildingDevelopment_ = nullptr;    // +0xa0
    cIGZUnknown* terrain_ = nullptr;                // +0xa8
    IModelAnimationCoordinator* animationCoordinator_ = nullptr;    // +0xb0
    int loadRadius_[5] = {};                        // +0xb8
    SC4RectT<int> loadRect_;                        // +0xcc
    int focusX_ = 0;                                // +0xdc
    int focusZ_ = 0;                                // +0xe0
    bool loadTaskActive_ = false;                   // +0xe4
    std::uint8_t padE5_[3] = {};
    int loadFocusX_ = 0;                            // +0xe8
    int loadFocusZ_ = 0;                            // +0xec
    std::uint32_t zoomMask_ = 0;                    // +0xf0
    std::uint32_t rotationMask_ = 0;                // +0xf4
    bool backgroundLoading_ = false;                // +0xf8
    std::uint8_t padF9_[7] = {};
    IModelBackgroundLoader* backgroundLoader_ = nullptr;   // +0x100
    bool cityLoaded_ = false;                       // +0x108
    bool ignoreMissingModels_ = false;              // +0x109
    bool unloadPending_ = false;                    // +0x10a
    bool loadingSuspended_ = false;                 // +0x10b
    float lighting_[3] = {};                        // +0x10c / +0x110 / +0x114
    std::uint8_t pad118_[4] = {};
    cIGZUnknown* dynamicTexture_[5] = {};           // +0x118 (counted texture bindings)
    std::vector<DeferredOccupantOperation> deferred_;   // +0x140
    int deferredDepth_ = 0;                         // +0x158
    std::uint8_t pad15c_[4] = {};
    float yaw_ = 0;                                 // +0x15c
    float rotationAngle_ = 0;                       // +0x160
    float pitch_ = 0;                               // +0x164
    float screenPitch_ = 0;                         // +0x168
    int previousZoom_ = 0;                          // +0x16c
    int previousRotation_ = 0;                      // +0x170
    int currentZoom_ = 0;                           // +0x174
    int currentRotation_ = 0;                       // +0x178
};

}  // namespace sc4
