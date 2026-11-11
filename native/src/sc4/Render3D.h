// cSC43DRender — the city view's 3D renderer (0x10075f3a8..0x10076a1e1): the object cSC4App::Init creates through the
// GZCOM factory (class id 0xe9c622d8), which owns the 3D camera, the orthographic UI camera, the view utilities
// (SC4ViewUtilities), the draw context (SC4DrawContext), the quad grid that indexes visible models, four view-object
// lists (static / dynamic / overlay / foreground), the render-property set, the dirty-rectangle queue, the model change
// queue and the screen-shake list. It is a cISC43DRender (primary vtable, vslots 3..74), a cIGZMessageTarget2 (the
// camera / city / property notifications, +8) and a cIGZSerializable (viewport size, draw frustum, camera control,
// projection state, +0x10), like every GZCOM service.
//
// Original layout (0x428 bytes, the size GZCOM_CreatecSC43DRender allocates; all writes from the constructor at
// 0x10075f4fe):
//   +0x000 primary vptr (cISC43DRender)          +0x008 message-target vptr (cIGZMessageTarget2)
//   +0x010 serializable vptr (cIGZSerializable)  +0x018 bool initialised
//   +0x01c refcount (primary)                    +0x014 refcount (message target)
//   +0x00c refcount (serializable)               +0x020 graphic-system object (QI 0x73283c of +0x28)
//   +0x028 cIGZGraphicSystem2*                   +0x030/+0x058 embedded graphic-port interfaces
//   +0x038 std::string graphic-system name A (data +0x048)
//   +0x060 std::string graphic-system name B (data +0x070)
//   +0x088 cSC4RenderProperties* (referenced)    +0x090 bool render system borrowed
//   +0x091 bool background picture enabled       +0x098 cIGZBuffer* background picture (referenced)
//   +0x0a0 background texture id                 +0x0a4 / +0x0a8 its width / height
//   +0x0ac bool background picture visible       +0x0ad unused
//   +0x0ae bool transparent background           +0x0af bool driver has the buffer-region extension
//   +0x0b0 bool partial region writes allowed    +0x0b1 bool backing store valid
//   +0x0b4 / +0x0b8 viewport width / height      +0x0bc..+0x0cb draw-frustum rectangle (4 ints)
//   +0x0cc / +0x0d0 subpixel image bias X / Y    +0x0d8 cS3DCamera* 3D camera (referenced)
//   +0x0e0 cSC4CameraControl* (referenced)       +0x0e8 / +0x0ec projection / view state stamps
//   +0x0f0 projection matrix (4x4 floats)        +0x130 cS3DCamera* orthographic UI camera (referenced)
//   +0x138 camera view (0 none, 1 2D, 2 3D, 3 UI) +0x13c / +0x140 view offsets
//   +0x144 bool lighting enabled                 +0x145 bool fog enabled
//   +0x148..+0x15b float[5] light levels (1, 1, 1, 10000, 15000)
//   +0x160 float (15000)                         +0x164 bool, +0x165 bool (dirty-rect capture suppressed)
//   +0x168 / +0x16c city size X / Z              +0x170 SC4ViewUtilities* (owned)
//   +0x178 cSC4LightingManager* (owned)          +0x180 SC4DrawContext* (owned)
//   +0x188 SC4QuadGrid* (owned)                  +0x1f8 int, +0x1fc int change counter
//   +0x200 bool dirty-rect capture enabled       +0x214 bool
//   +0x228 / +0x240 / +0x258 / +0x270 std::list<ViewObjectInfo> (priority ordered)
//   +0x288 cISC4ViewObject3D* background view object (referenced)
//   +0x290 terrain view (referenced; see the seam)   +0x298 bool clear colour valid
//   +0x2a0 float[4] viewport clear colour        +0x2b0 float, +0x2b4 bool, +0x2b8 int
//   +0x2c0 std::vector<ModelChangeQueueItem> (stride 0x38)   +0x2d8 bool walking the queue
//   +0x2e0 std::list<DirtyRectangleQueueItem>    +0x2f0 its count
//   +0x2f8 int item count, +0x300 / +0x304 accumulated / maximum area
//   +0x308 std::vector<...> scratch              +0x328 / +0x329 bools
//   +0x32c..+0x33c ints                          +0x340 cRZTimer (units 4) +0x358 int (10000)
//   +0x360 std::list<cSC4Shake>                  +0x378 int shake serial
//   +0x380 cRZTimer (units 5)                    +0x398 / +0x39c screen-shake X / Y
//   +0x3a0 float                                 +0x3a4 int map scale factor
//   +0x3a8 / +0x3b8 float[4] fixed-view -> container offsets
//   +0x3c8..+0x41c performance counters (16 ints, only the two stats strings read them)
//
// What is in this file: the object's lifecycle, its driver / property / camera / view-object plumbing, the small
// services the window layer, the camera control and the terrain view call every frame, the per-frame draw pass
// (Draw / Draw*View / the partial / translated updates) and — since RENDER3D-2 — the model change queue
// (InsertModel / RemoveModel / MoveModel / ChangeModel / ProcessModelChangeQueue) and the quad-grid rebuild. Still
// missing: the camera / city messages (DoMessage) and picking; see notes/ledger_pending/cSC43DRender.txt.
//
// The model change queue in one paragraph, because it is what feeds DrawStaticView: InsertModel builds an
// SC43DGridElement, stores it as the model's custom data (cIS3DModelInstance vslot 0x28), inserts it into the quad grid
// and, when the model belongs in the backing store, appends a change-0 item. RemoveModel / MoveModel / ChangeModel only
// queue an item for an element that is in the backing store (flag 0x8); a dynamic element is changed in place. Draw
// drains the queue before the static view pass (0x100765a93), and the drain sets the 0x8 / 0x10 flags and queues the
// dirty rectangles the redraw needs. The element flags the queue maintains: 0x1 never drawn dynamically, 0x4 set by
// InsertModel once the element is in the grid, 0x8 static (backing store), 0x10 in the change queue, 0x20 / 0x40 / 0x80
// the model-property and draw-attribute bits, 0x100 draw behind the dynamic models, 0x200 the dirty rectangle is a
// partial update, 0x400 / 0x800 / 0x1000 / 0x2000 the zoom-threshold groups.
//
// Quirks kept (with their addresses):
//  * Release() tears the object down when the primary count is below 2: it calls Shutdown (vslot 0x20) and then the
//    deleting destructor (vslot 0x260, 0x10075fe36), so the last release frees the object. Heap-allocate a renderer
//    before releasing it to zero. The three sub-object refcounts are independent, so a Release through the
//    message-target or serializable vtable only decrements its own count.
//  * QueryInterface answers 1 (the primary), 0xe9c622d8 (the class id, primary), 0x681bd529 and 0x452294aa
//    (cIGZMessageTarget2) and 0xe4fda3d4 (cIGZSerializable); a failed query returns 0 and writes nothing.
//  * Init is idempotent: the second call returns 1 without touching anything (0x10076002a).
//  * SetBackingStoreEnabled cannot enable the store when the driver has no buffer-region extension
//    (0x1007699b0); the flag is therefore read-only in practice until StaticSetGDriver found the extension.
//  * GetProjectionMatrixEntries / GetViewMatrixEntries return null in this build (0x10076a1ba, 0x10076a1c2), even
//    though the projection matrix is kept in +0x0f0.
//  * SetViewportClearColor also clears the "clear colour valid" byte +0x298 (0x1007693a0).
//  * The constructor always allocates the view utilities, the draw context and the quad grid, even when no driver has
//    been set yet, so a renderer can be inspected before StaticSetGDriver (0x10075f4fe).
//  * Init calls ViewUtilities::UpdateCameraPosition with a possibly null camera control; that path dereferences it in
//    the original. The port only updates when a control is attached (documented deviation, same visible behaviour for
//    every caller that sets the control first).
//  * Screen shake: the first StartScreenShake restarts the clock and clears +0x3a0; every shake gets a serial number
//    from +0x378 (0x100769064). StopScreenShake with a non-zero falloff shortens the shake to now + the remaining
//    falloff instead of removing it (0x10076929a).
//  * GetStatsString1 reads a counter set the draw path normally fills; the format strings are kept verbatim.
//  * InsertModel on a model that already has an element with the 0x10 (queued) flag sets: if the queue holds an item
//    for it, InsertModel drains the queue and then builds a *second* element anyway, overwriting the model's custom
//    data and leaking the element the drain left in the grid (0x100761ecd jumps straight into the create path). The
//    port keeps the jump; the second element is what the model then points at.
//  * A full model change queue (0x2bc0 bytes = 200 items) forces a full static-view redraw, reason 4
//    (0x100762049 / 0x10076203e).
//  * ChangeModel(4) forwards to MoveModel, and ChangeModel(1) only toggles the 0x80 draw-attribute bit (plus the
//    quadrant flag) without any dirty rectangle (0x100762968).
//  * ClearModelChangeQueue calls the virtual RemoveModel for every queued model (0x10076113a), and IsModelInserted
//    drains the queue once it grows past eight items and an element is queued (0x10076180e).
//  * RebuildQuadGrid sets the new elements' grid cells with the *old* grid's cell size, because SetGridRectForElement
//    still runs on +0x188 before the new grid is stored; if AddElement then fails it retries the cell rectangle
//    (0,0)-(1,1) and, if that fails too, leaves the model with no element without releasing its reference
//    (0x100765592).
//  * MoveModel / MoveGridElement drop the model's element when the grid rejects the move (SetCustomData(null)) while
//    the model keeps the reference the grid took (0x1007628d6 / 0x100762458).
//  * WorldToContainerRect always widens the projected box by 2 units per side, clamps it into [0.5, gridSize-0.5],
//    then pulls a degenerate corner back by 0.1 and caps a box wider / taller than half the grid at one unit
//    (0x1007620c4). The four rotations pick the two corners and swap in the isometric diagonal offsets
//    (0.35355339 = 1/(2*sqrt(2)) and 0.14644661 = (1-1/sqrt(2))/2).
//
// Seams (unrecovered neighbours; original slot / address in the comment):
//  * IRender3DHost        — SL::spMS2 (message server), SL::spRM property group 0x4a639ef2, SL::spSC4 static-shutdown
//                           registration (+0x218), the cSC4LightingManager factory, SL::spTerrain / SL::spCity.
//  * IRender3DSettings    — cSC4RenderProperties: the threshold and capability fields the renderer reads at +0x2c0,
//                           +0x308, +0x350 (zoom thresholds), +0x278, +0x7b8 (partial writes), +0x638 (clip),
//                           +0xb8 (static-view pass) of its inner cProperties object.
//  * IRender3DGraphicSystem — cIGZGraphicSystem2, the GZ engine graphics system (+0x20 / +0x28); only the port query and
//                           the two graphics ports at vslot 0xe8 are needed.
//  * IBackingStore / cIGZGDriver — the Metal driver services (see render/gd/).
#pragma once
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "gz/StreamOps.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZSerializable.h"
#include "gz/cIGZString.h"
#include "render/gd/IBackingStore.h"
#include "render/gd/IGZGDriver.h"
#include "rz/cRZTimer.h"
#include "s3d/Camera.h"
#include "s3d/S3DMath.h"
#include "sc4/DrawContext.h"
#include "sc4/EffectDescriptions.h"
#include "sc4/QuadGrid.h"
#include "sc4/SCProperty.h"
#include "sc4/ViewUtilities.h"

class cISC4OccupantFilter;
class cISC4ViewObject3D;
class cIGZBuffer;
class SC43DGridElement;
namespace sc4 { class cSC4CameraControl; class cSC4RenderProperties; }
namespace s3d { class cIS3DModelInstance; }

// ---------------------------------------------------------------------------------------------
// Seams
// ---------------------------------------------------------------------------------------------

// The cSC4RenderProperties fields the renderer reads, named after what the renderer does with them. The original
// reaches them as SL::spRenderProperties (+0x18 = the inner cProperties) + offset; the offsets are in the comments.
class IRender3DSettings {
public:
    virtual ~IRender3DSettings() = default;
    // +0x7b8: buffer-region writes are unavailable -> partial writes stay off (read at 0x1007601b2, 0x100762ee2).
    virtual bool DisablePartialRegionWrites() const = 0;
    // +0x638: the driver honours clip rectangles, else SetClipRect does nothing (0x10076160e).
    virtual bool ClipRectSupported() const = 0;
    // +0x278: zoom above which the backing store is disabled (0x100762e??).
    virtual int BackingStoreMaxZoom() const = 0;
    // +0x2c0 / +0x308 / +0x350: per-flag zoom thresholds for the three static-view flag groups (0x100762538).
    virtual int StaticViewMaxZoomA() const = 0;
    virtual int StaticViewMaxZoomB() const = 0;
    virtual int StaticViewMaxZoomC() const = 0;
    // +0xb8: redraw the static view inside UpdateViewportRect / Draw (0x100764096, 0x1007659f6).
    virtual bool StaticViewRedrawOnViewportChange() const = 0;
    // +0x78: Draw runs the dynamic view pass and the two view-object lists around it (0x100765ec5, 0x100763ac6).
    virtual bool DrawDynamicViewEnabled() const = 0;
    // +0x938: Draw calls DoViewCleanup while a translated update is still pending (0x1007659f6, 0x1007660ee).
    virtual bool ViewCleanupEnabled() const = 0;
    // +0x738: ClearViewport disables lighting for the background pass and restores it afterwards (0x100766d5e).
    virtual bool ClearViewportPreservesLighting() const = 0;
    // +0x438: DoTranslatedViewUpdate rebuilds the draw frustum from the viewport rectangle (0x100763ac6).
    virtual bool TranslatedUpdateRebuildsDrawFrustum() const = 0;
    // +0x5b8: Draw applies the screen-shake offsets (0x1007659f6).
    virtual bool ScreenShakeEnabled() const = 0;
    // The shake drift per frame, read from the inner +0x30 property block at +0x80 / +0x38 (0x1007659f6).
    virtual int ScreenShakeOffsetX() const = 0;
    virtual int ScreenShakeOffsetY() const = 0;
    // +0x548 of the inner +0x30 block: how many frames a queued dirty rectangle keeps the queue alive (0x1007659f6).
    virtual int DirtyRectangleMaxAgeFrames() const = 0;
};
// Everything cSC43DRender reaches outside itself. One host for the whole renderer (the game keeps these as engine
// globals; a fresh port which has not recovered the city / terrain / property system yet supplies them).
class IRender3DHost {
public:
    virtual ~IRender3DHost() = default;
    // SL::spSC4 vslot 0x218: called by StaticInit to register StaticShutdown (0x10075ff58).
    virtual void RegisterStaticShutdown(bool (*function)(void*), void* context) = 0;
    // SL::spMS2 (cIGZMessageServer2): the camera and city notifications (0x1007602xx, 0x100760dbc).
    virtual cIGZMessageServer2* MessageServer() = 0;
    // SL::spRM property group 0x4a639ef2 -> cISCPropertyHolder (0x100760546).
    virtual sc4::cISCPropertyHolder* RenderPropertyHolder() = 0;
    // new cSC4LightingManager (0x240 bytes) for Init / +0x178 (0x1007602f6). The renderer takes ownership:
    // Shutdown deletes it (the original Releases it and its own Shutdown runs first).
    virtual IS4LightingManager* CreateLightingManager() = 0;
    // SL::spTerrain (+0x290) and SL::spCity (city size in +0x168 / +0x16c), only used by DoMessage.
    virtual cIGZUnknown* TerrainView() = 0;
    virtual cIGZUnknown* City() = 0;
    // The render-property thresholds / capabilities (see IRender3DSettings).
    virtual IRender3DSettings* Settings() = 0;
    // SL::spRenderProperties (+0x88 of the renderer): the tunable set Init copies in (0x1007601e2).
    virtual sc4::cSC4RenderProperties* RenderProperties() = 0;
    // The terrain view's pick (i386 +0x198: QI 0x6771477d vslot 0xc, else the terrain's vslot 0x10): where the ray meets the
    // terrain (with the water surface when `includeWater`). False without a terrain view.
    virtual bool PickTerrainView(const float* origin, const float* direction, float* out, bool includeWater) {
        (void)origin; (void)direction; (void)out; (void)includeWater;
        return false;
    }
    // The city's cell counts (i386 +0x19c vslots 0x1c / 0x20, PickTerrainAlways's clip box). False without a city.
    virtual bool CityCellCounts(int& x, int& z) { (void)x; (void)z; return false; }
};
namespace sc4 {
IRender3DHost* Render3DHost();
void SetRender3DHost(IRender3DHost* host);
}

// cIGZGraphicSystem2 (GZ engine, unrecovered). Only the port query and the two graphic ports are used.
class IRender3DGraphicSystem {
public:
    virtual ~IRender3DGraphicSystem() = default;
    virtual bool QueryInterface(std::uint32_t iid, void** out) = 0;          // [0]
    virtual std::uint32_t AddRef() = 0;                                       // [1]
    virtual std::uint32_t Release() = 0;                                      // [2]
    // vslot 0xe8: the two embedded graphic ports (+0x30 / +0x58 of the renderer in the original).
    virtual bool GetGraphicPorts(void** primary, void** secondary) = 0;
};

// cISC4ViewObject3D (unrecovered): the background / terrain redraw hooks the renderer keeps in its four lists. Only the
// reference protocol and the draw call the viewport update makes are needed (vslot 0x18 of the original object).
class cISC4ViewObject3D : public cIGZUnknown {
public:
    virtual void DrawViewObject(SC4DrawContext& context) = 0;   // [3] +0x18, called at 0x100764096 / 0x100764c54
};

// SC43DGridElement: the SC4GridElement the renderer inserts into the quad grid for one model. The original derives it
// from SC4GridElement (src/sc4/QuadGrid.h) and stores the model at +0x20, the per-model screen bounds at +0x28..+0x3c
// (three 1.0 scale floats, then zeros) and the object flags in the inherited +0xa word (0x8 = static/backing store,
// 0x40 = static, 0x100 = always dynamic, 0x400/0x1800/0x2000 = the zoom-threshold groups).
class SC43DGridElement : public SC4GridElement {
public:
    ~SC43DGridElement() override = default;
    s3d::cIS3DModelInstance* model = nullptr;   // +0x20
    // The element's bounds in the container space the sort keys project (the constructor writes 1, 1, 1, 0, 0, 0, i.e.
    // an empty box until InsertModel fills it). The three pairs are (min, max) of x, y and z.
    float boundsMinX = 1.0f;                    // +0x28
    float boundsMinY = 1.0f;                    // +0x2c
    float boundsMinZ = 1.0f;                    // +0x30
    float boundsMaxX = 0.0f;                    // +0x34
    float boundsMaxY = 0.0f;                    // +0x38
    float boundsMaxZ = 0.0f;                    // +0x3c
};

// cSC4ModelSortElement: the 16-byte record the static / dynamic passes sort (0x10076a60c is its vector's slow path).
// The key is a 32-bit fixed-point depth built by the viewport Z projection plus the model's own sort value; the passes
// sort it with their own heapsort (front-to-back for the first list, back-to-front for the other two).
struct cSC4ModelSortElement {
    s3d::cIS3DModelInstance* model = nullptr;   // +0
    std::uint32_t key = 0;                      // +8
};
static_assert(sizeof(cSC4ModelSortElement) == 16, "the original record is 16 bytes");

// ---------------------------------------------------------------------------------------------
// cISC43DRender: the base interface, vtable slots 3..74 of the binary (slots 0..2 are cIGZUnknown).
// ---------------------------------------------------------------------------------------------
class cISC43DRender : public cIGZUnknown {
public:
    // Which update the renderer is asked to do. Only the values this port uses are named; the rest are plain ints in
    // the binary (the enumeration was not exported).
    enum ViewUpdateReason : std::uint32_t {
        kViewUpdateNone = 0,                // Draw clears the reason before it returns
        kViewUpdateTranslated = 3,          // AttemptTranslatedViewUpdate's fallback (0x1007658be)
        kViewUpdateDirtyRectOverflow = 4,   // AddDirtyRectangleInternal falls back to a full update
        kViewUpdateDirtyRectMerged = 6,     // a dirty rectangle absorbed another one
        kViewUpdateDirtyRectPartial = 7,    // ProcessDirtyRectangleQueue, kind >= 2
        kViewUpdateDirtyRectModel = 8,      // ProcessDirtyRectangleQueue kind 1 / DoSingleModelStaticDrawAddition
        kViewUpdateViewCleanup = 9,         // DoViewCleanup's trickle band
        kViewUpdateForceFullRedraw = 10,    // ForceFullRedraw (0x1007658ae)
    };
    // The frustum IsModelVisible tests against: 2 = the view frustum, anything else = the draw frustum.
    enum FrustumType : int { kFrustumDraw = 0, kFrustumView = 2 };

    virtual bool Init() = 0;                                                          // [3]  +0x18
    virtual bool Shutdown() = 0;                                                      // [4]  +0x20
    virtual void SetMapScaleFactor(int factor) = 0;                                   // [5]  +0x28
    virtual void SetGraphicSystem(IRender3DGraphicSystem* system) = 0;                // [6]  +0x30
    virtual void SetGDriver(gd::cIGZGDriver* driver) = 0;                             // [7]  +0x38
    virtual SC4ViewUtilities* GetViewUtilities() const = 0;                           // [8]  +0x40
    virtual IS4LightingManager* GetLightingManager() const = 0;                       // [9]  +0x48
    virtual SC4DrawContext* GetDrawContext() const = 0;                               // [10] +0x50
    virtual void GetViewportSize(std::uint32_t& width, std::uint32_t& height) const = 0;   // [11] +0x58
    virtual void SetViewportSize(std::uint32_t width, std::uint32_t height) = 0;      // [12] +0x60
    virtual void SetSubpixelImageBias(float x, float y) = 0;                          // [13] +0x68
    virtual s3d::cS3DCamera* GetCamera() = 0;                                         // [14] +0x70
    virtual void SetCamera(s3d::cS3DCamera* camera) = 0;                              // [15] +0x78
    virtual sc4::cSC4CameraControl* GetCameraControl() = 0;                           // [16] +0x80
    virtual void SetCameraControl(sc4::cSC4CameraControl* control) = 0;               // [17] +0x88
    virtual const float* GetProjectionMatrixEntries() const = 0;                      // [18] +0x90
    virtual const float* GetViewMatrixEntries() const = 0;                            // [19] +0x98
    virtual void ForceFullRedraw() = 0;                                               // [20] +0xa0
    virtual void Draw() = 0;                                                          // [21] +0xa8
    virtual bool TakeSnapshot(cIGZBuffer** out, bool a) = 0;                          // [22] +0xb0
    virtual bool TakeSnapshot(cIGZString& path, bool a) = 0;                          // [23] +0xb8
    virtual void Set3DCameraView() = 0;                                               // [24] +0xc0
    virtual void Set2DCameraView() = 0;                                               // [25] +0xc8
    virtual void SetUICameraView() = 0;                                               // [26] +0xd0
    virtual bool IsModelInserted(s3d::cIS3DModelInstance* model) = 0;                 // [27] +0xd8
    virtual bool InsertModel(s3d::cIS3DModelInstance* model) = 0;                     // [28] +0xe0
    virtual bool RemoveModel(s3d::cIS3DModelInstance* model) = 0;                     // [29] +0xe8
    virtual bool MoveModel(s3d::cIS3DModelInstance* model) = 0;                       // [30] +0xf0
    virtual bool ChangeModel(s3d::cIS3DModelInstance* model, int change) = 0;         // [31] +0xf8
    virtual bool AddViewObject(cISC4ViewObject3D* object, int list, std::uint32_t priority) = 0;   // [32] +0x100
    virtual bool RemoveViewObject(cISC4ViewObject3D* object) = 0;                     // [33] +0x108
    virtual bool FindViewObject(cISC4ViewObject3D* object) const = 0;                 // [34] +0x110
    virtual void SetZoom(int zoom) = 0;                                               // [35] +0x118
    virtual void ZoomIn() = 0;                                                        // [36] +0x120
    virtual void ZoomOut() = 0;                                                       // [37] +0x128
    virtual void SetRotation(int rotation) = 0;                                       // [38] +0x130
    virtual void RotateLeft() = 0;                                                    // [39] +0x138
    virtual void RotateRight() = 0;                                                   // [40] +0x140
    virtual void SetZoomAndRotation(int zoom, int rotation) = 0;                      // [41] +0x148
    virtual std::uint32_t StartScreenShake(float amplitude, float frequency) = 0;     // [42] +0x150
    virtual void StartScreenShake(const cSC4ShakeDescription& description,
                                  const cS3DVector2* position) = 0;                     // [43] +0x158
    virtual void StopScreenShake(std::uint32_t serial) = 0;                           // [44] +0x160
    virtual void GetShakeOffsets(float& x, float& y) = 0;                             // [45] +0x168
    virtual void SetViewportClearColor(const float* color) = 0;                       // [46] +0x170
    virtual void ClearViewport(bool clearDepth) = 0;                                  // [47] +0x178
    virtual void ClearViewportWithSolidColor(const float* color, bool a, bool b) = 0; // [48] +0x180
    virtual void DrawViewportRectangle(const cRZRect& rect, const float* color, bool a, bool b) = 0;   // [49] +0x188
    virtual void DrawTexturedViewportRectangle(const cRZRect& rect, std::uint32_t texture, float alpha,
                                               bool a, bool b) = 0;                   // [50] +0x190
    virtual void EnableBackgroundPicture(bool enable) = 0;                            // [51] +0x198
    virtual cIGZBuffer* GetBackgroundPicture(bool* visible) const = 0;                // [52] +0x1a0
    virtual bool SetBackgroundPicture(cIGZBuffer* buffer, bool visible) = 0;          // [53] +0x1a8
    virtual bool SetBackgroundPicture(std::uint32_t texture, int width, int height, bool visible) = 0;   // [54] +0x1b0
    virtual void EnableTransparentBackground(bool enable) = 0;                        // [55] +0x1b8
    virtual bool GetBackingStoreEnabled() = 0;                                        // [56] +0x1c0
    virtual void SetBackingStoreEnabled(bool enable) = 0;                             // [57] +0x1c8
    virtual bool BorrowRenderSystem() = 0;                                            // [58] +0x1d0
    virtual bool ReturnRenderSystem() = 0;                                            // [59] +0x1d8
    virtual bool PickTerrain(const float* origin, const float* direction, float* out, bool a, bool b) = 0;  // [60]
    virtual bool PickTerrain(int x, int y, float* out, bool a, bool b) = 0;           // [61] +0x1e8
    virtual bool PickTerrainAlways(const float* origin, const float* direction, float* out, bool a) = 0;    // [62]
    virtual bool PickTerrainAlways(int x, int y, float* out, bool a) = 0;             // [63] +0x1f8
    virtual bool Pick(int x, int y, cISC4OccupantFilter* filter, s3d::cIS3DModelInstance*& out) = 0;       // [64]
    virtual bool Pick(int x, int y, const float* origin, const float* direction,
                      cISC4OccupantFilter* filter, s3d::cIS3DModelInstance*& out) = 0;   // [65] +0x208
    virtual bool GetModelsInVolume(const cS3DBoundingBox& box, s3d::cIS3DModelInstance** out,
                                   std::uint32_t& capacity) = 0;                      // [66] +0x210
    virtual bool IsModelVisible(s3d::cIS3DModelInstance* model, FrustumType frustum) = 0;   // [67] +0x218
    virtual bool IsModelVisible(SC43DGridElement* element, FrustumType frustum) = 0;  // [68] +0x220
    virtual bool GetViewFogEnabled() const = 0;                                       // [69] +0x228
    virtual bool SetViewFogEnabled(bool enable) = 0;                                  // [70] +0x230
    virtual bool GetLightingEnabled() const = 0;                                      // [71] +0x238
    virtual bool SetLightingEnabled(bool enable) = 0;                                 // [72] +0x240
    virtual void GetStatsString1(cIGZString& out) = 0;                                // [73] +0x248
    virtual void GetStatsString2(cIGZString& out) = 0;                                // [74] +0x250
};

// ---------------------------------------------------------------------------------------------
// cSC43DRender
// ---------------------------------------------------------------------------------------------
namespace sc4 {

class cSC43DRender final : public cISC43DRender,
                           public cIGZMessageTarget2,
                           public cIGZSerializable {
public:
    // The view utilities reach the camera through their host pointer (+8); the original stores the renderer itself
    // and calls its vslot 0x70. The port keeps the renderer's three base vtables (primary / message target /
    // serializable) exactly as the binary has them, so the one-method host is a small adapter instead.
    struct ViewUtilitiesCameraHost final : IViewUtilitiesHost {
        cSC43DRender* owner = nullptr;
        s3d::cS3DCamera* GetCamera() override { return owner ? owner->camera_ : nullptr; }
    };

    // What one entry of a view-object list holds: the object (referenced) and its priority.
    struct ViewObjectInfo {
        cISC4ViewObject3D* object = nullptr;   // +8
        std::uint32_t priority = 0;            // +0x10
    };
    // One queued model change (+0x2c0, stride 0x38 in the original). `change` is the original byte: 0 insert,
    // 1 remove, 2 move, 3/4 change, 5 flags. The box at +0x1c is the element's box when the change was queued (an
    // empty box 1,1,1,0,0,0 for the changes that do not carry one); the move drain unions it with the new box.
    struct ModelChangeQueueItem {
        s3d::cIS3DModelInstance* model = nullptr;   // +0x00
        std::uint8_t change = 0;                    // +0x08
        // +0x09..+0x1b: never read. The original writes the stale stack words at +0x10 / +0x18 here (see the
        // disassembly of 0x100761ff6); the port keeps them zero.
        std::uint8_t reserved[0x13] = {};
        float boundsMin[3] = {1.0f, 1.0f, 1.0f};    // +0x1c
        float boundsMax[3] = {0.0f, 0.0f, 0.0f};    // +0x28
    };
    static_assert(sizeof(ModelChangeQueueItem) == 0x38, "the original queue item is 0x38 bytes");
    // One queued dirty rectangle (+0x2e0). `kind` is the original's +0x14 word.
    struct DirtyRectangleQueueItem {
        int kind = 0;                 // +0x14
        cRZRect rect;                 // +0x18
        s3d::cIS3DModelInstance* model = nullptr;   // +0x28 (referenced)
        std::uint32_t stamp = 0;      // +0x10
    };
    // One running screen shake (+0x360 list node, 0x70 bytes in the original).
    struct cSC4Shake {
        bool positioned = false;      // +0x18
        float positionX = 0;          // +0x1c
        float positionY = 0;          // +0x20
        float duration = 0;           // +0x10
        float elapsed = 0;            // +0x14
        float amplitude = 0;          // +0x28
        std::vector<float> curveA;    // +0x30
        std::vector<float> curveB;    // +0x48
        float frequency = 0;          // +0x60
        float falloff = 0;            // +0x2c
        std::uint32_t serial = 0;     // +0x68
    };

    cSC43DRender();     // @ 0x10075f4fe (C1 alias @ 0x10075f3a8)
    ~cSC43DRender();    // @ 0x10075faec (deleting @ 0x10075fdde)

    // cIGZUnknown (three independent reference counts, as in the original)
    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x10075feee
    std::uint32_t AddRef() override;                                // @ 0x10075fe0c
    std::uint32_t Release() override;                               // @ 0x10075fe36

    // Static driver / class plumbing
    static bool StaticInit(void* context);                          // @ 0x10075ff58
    static bool StaticShutdown(void* context);                      // @ 0x10075ff8c
    static void StaticSetGDriver(gd::cIGZGDriver* driver);          // @ 0x1007614c6

    // cISC43DRender
    bool Init() override;                                           // @ 0x10076002a
    bool Shutdown() override;                                       // @ 0x100760dbc
    void SetMapScaleFactor(int factor) override;                    // @ 0x10076a176
    void SetGraphicSystem(IRender3DGraphicSystem* system) override;  // @ 0x100761392 (partial)
    void SetGDriver(gd::cIGZGDriver* driver) override;              // @ 0x10076143c
    SC4ViewUtilities* GetViewUtilities() const override;            // @ 0x10076a182
    IS4LightingManager* GetLightingManager() const override;        // @ 0x1007615b6
    SC4DrawContext* GetDrawContext() const override;                // @ 0x10076a190
    void GetViewportSize(std::uint32_t& width, std::uint32_t& height) const override;   // @ 0x1007615c4
    void SetViewportSize(std::uint32_t width, std::uint32_t height) override;           // @ 0x1007615da
    void SetSubpixelImageBias(float x, float y) override;           // @ 0x1007615f8
    s3d::cS3DCamera* GetCamera() override;                          // @ 0x10076a19e
    void SetCamera(s3d::cS3DCamera* camera) override;               // @ 0x100761720
    cSC4CameraControl* GetCameraControl() override;                 // @ 0x10076a1ac
    void SetCameraControl(cSC4CameraControl* control) override;     // @ 0x1007617b0
    const float* GetProjectionMatrixEntries() const override;       // @ 0x10076a1ba
    const float* GetViewMatrixEntries() const override;             // @ 0x10076a1c2
    void ForceFullRedraw() override;                                // @ 0x1007658ae
    void Draw() override;                                           // @ 0x1007659f6
    bool TakeSnapshot(cIGZBuffer** out, bool a) override;           // @ 0x100769718 (PARTIAL)
    bool TakeSnapshot(cIGZString& path, bool a) override;           // @ 0x100769760 (PARTIAL)
    void Set3DCameraView() override;                                // @ 0x100766a22
    void Set2DCameraView() override;                                // @ 0x100766a6a
    void SetUICameraView() override;                                // @ 0x100766aa0
    bool IsModelInserted(s3d::cIS3DModelInstance* model) override;  // @ 0x10076180e
    bool InsertModel(s3d::cIS3DModelInstance* model) override;      // @ 0x100761e58
    bool RemoveModel(s3d::cIS3DModelInstance* model) override;      // @ 0x100762628
    bool MoveModel(s3d::cIS3DModelInstance* model) override;        // @ 0x10076279c
    bool ChangeModel(s3d::cIS3DModelInstance* model, int change) override;   // @ 0x100762968
    bool AddViewObject(cISC4ViewObject3D* object, int list, std::uint32_t priority) override;   // @ 0x1007693d0
    bool RemoveViewObject(cISC4ViewObject3D* object) override;      // @ 0x100769450
    bool FindViewObject(cISC4ViewObject3D* object) const override;  // @ 0x100769508
    void SetZoom(int zoom) override;                                // @ 0x100768e2e
    void ZoomIn() override;                                         // @ 0x100768e44
    void ZoomOut() override;                                        // @ 0x100768e56
    void SetRotation(int rotation) override;                        // @ 0x100768e68
    void RotateLeft() override;                                     // @ 0x100768e7a
    void RotateRight() override;                                    // @ 0x100768e8c
    void SetZoomAndRotation(int zoom, int rotation) override;       // @ 0x100768e9e
    std::uint32_t StartScreenShake(float amplitude, float frequency) override;             // @ 0x100768eb0
    void StartScreenShake(const cSC4ShakeDescription& description, const cS3DVector2* position) override;  // @ 0x100769064
    void StopScreenShake(std::uint32_t serial) override;            // @ 0x10076929a
    void GetShakeOffsets(float& x, float& y) override;              // @ 0x10076937e
    void SetViewportClearColor(const float* color) override;        // @ 0x1007693a0
    void ClearViewport(bool clearDepth) override;                   // @ 0x100766d5e (PARTIAL)
    void ClearViewportWithSolidColor(const float* color, bool a, bool b) override;   // @ 0x100767716 (PARTIAL)
    void DrawViewportRectangle(const cRZRect& rect, const float* color, bool a, bool b) override;   // @ 0x1007677da (PARTIAL)
    void DrawTexturedViewportRectangle(const cRZRect& rect, std::uint32_t texture, float alpha,
                                       bool a, bool b) override;    // @ 0x1007695a2 (PARTIAL)
    void EnableBackgroundPicture(bool enable) override;             // @ 0x1007697f6
    cIGZBuffer* GetBackgroundPicture(bool* visible) const override;  // @ 0x100769804
    bool SetBackgroundPicture(cIGZBuffer* buffer, bool visible) override;   // @ 0x10076981e (PARTIAL)
    bool SetBackgroundPicture(std::uint32_t texture, int width, int height, bool visible) override;   // @ 0x10076990a
    void EnableTransparentBackground(bool enable) override;         // @ 0x100769996
    bool GetBackingStoreEnabled() override;                         // @ 0x1007699a4
    void SetBackingStoreEnabled(bool enable) override;              // @ 0x1007699b0
    bool BorrowRenderSystem() override;                             // @ 0x100769ff6
    bool ReturnRenderSystem() override;                             // @ 0x10076a006
    bool PickTerrain(const float* origin, const float* direction, float* out, bool a, bool b) override;   // [60] @ 0x0050498a (i386)
    bool PickTerrain(int x, int y, float* out, bool a, bool b) override;    // [61] @ 0x00504360 (i386)
    bool PickTerrainAlways(const float* origin, const float* direction, float* out, bool a) override;     // [62] @ 0x0050679c (i386)
    bool PickTerrainAlways(int x, int y, float* out, bool a) override;      // [63] @ 0x005042b6 (i386)
    bool Pick(int x, int y, cISC4OccupantFilter* filter, s3d::cIS3DModelInstance*& out) override;        // [64] PARTIAL
    bool Pick(int x, int y, const float* origin, const float* direction, cISC4OccupantFilter* filter,
              s3d::cIS3DModelInstance*& out) override;              // [65] PARTIAL
    bool GetModelsInVolume(const cS3DBoundingBox& box, s3d::cIS3DModelInstance** out,
                           std::uint32_t& capacity) override;       // @ 0x100768d5c
    bool IsModelVisible(s3d::cIS3DModelInstance* model, FrustumType frustum) override;   // @ 0x100768d64
    bool IsModelVisible(SC43DGridElement* element, FrustumType frustum) override;        // @ 0x100768dca
    bool GetViewFogEnabled() const override;                        // @ 0x100769bce
    bool SetViewFogEnabled(bool enable) override;                   // @ 0x100769bda
    bool GetLightingEnabled() const override;                       // @ 0x100769c02
    bool SetLightingEnabled(bool enable) override;                  // @ 0x100769c0e
    void GetStatsString1(cIGZString& out) override;                 // @ 0x10076a084
    void GetStatsString2(cIGZString& out) override;                 // @ 0x10076a0f2

    // cIGZMessageTarget2
    bool DoMessage(cIGZMessage2* message) override;                 // @ 0x100762b36 (PARTIAL)

    // cIGZSerializable
    bool Write(cIGZOStream& stream) const override;                 // @ 0x1007611e2
    bool Read(cIGZIStream& stream) override;                        // @ 0x1007612a2
    std::uint32_t GetGZCLSID() const override;                      // @ 0x10076a1ca


    // ---- non-virtual services -------------------------------------------------------------------
    static bool HasBackingStore();                                  // @ 0x1007699d8
    static bool CreateBackingStore();                               // @ 0x1007699ee
    // The original's file-scope _g_MacJustZoomed: the zoom path sets it, Draw's first SaveBackingStore consumes it
    // (0x1007641b4). Nothing in the recovered code writes it yet.
    static void SetMacJustZoomed(bool zoomed) { mbMacJustZoomed = zoomed; }
    static bool MacJustZoomed() { return mbMacJustZoomed; }
    void ClearViewObjectList(std::list<ViewObjectInfo>& list);      // @ 0x10075f4ce
    void InsertIntoViewObjectList(std::list<ViewObjectInfo>& list, cISC4ViewObject3D* object,
                                  std::uint32_t priority);          // @ 0x10075f3e6
    bool RemoveFromViewObjectList(std::list<ViewObjectInfo>& list, cISC4ViewObject3D* object);   // @ 0x10075f474
    void ClearModelChangeQueue();                                   // @ 0x10076113a
    void ClearDirtyRectangleQueue();                                // @ 0x100763a56
    void DoFullViewUpdate(ViewUpdateReason reason);                 // @ 0x1007625c0
    void ReadProperties();                                          // @ 0x100760546
    SC4QuadGrid* CreateQuadGrid();                                  // @ 0x1007604c2
    void UpdateViewportRect(const cRZRect& rect);                   // @ 0x100764096
    void TurnOnLighting(bool enable);                               // @ 0x100764062
    void AddDirtyRectangle(int kind, const cRZRect& rect, s3d::cIS3DModelInstance* model, int extra);   // @ 0x100769c44
    void DoViewTranslationChange3();                                // @ i386 0x00504c0c
    void AddDirtyRectangleInternal(int kind, const cRZRect& rect, s3d::cIS3DModelInstance* model, int extra);   // @ 0x100769cca (PARTIAL)
    bool AttemptTranslatedViewUpdate(int dx, int dy);               // @ 0x1007658be
    void SetClipRect(const cRZRect& rect);                          // @ 0x10076160e
    void UnsetClipRect();                                           // @ 0x1007616de

    // ---- the per-frame draw pass (the 13 functions of RENDER3D-1) --------------------------------
    void DrawStaticView();                                          // @ 0x100764c54
    void DrawDynamicView();                                         // @ 0x100764240
    void DoSingleModelStaticDrawAddition(s3d::cIS3DModelInstance* model, const cRZRect& rect);   // @ 0x1007650d0
    void DoTranslatedViewUpdate(int dx, int dy);                    // @ 0x100763ac6
    void DoPartialViewUpdate(const cRZRect& rect, ViewUpdateReason reason);   // @ 0x100764a6c
    void DoViewCleanup();                                           // @ 0x1007660ee
    void ProcessDirtyRectangleQueue();                              // @ 0x1007661ba
    void GetVisibleContainerRect(SC4RectFloat& out, FrustumType frustum);        // @ 0x100765194
    void GetViewFrustumContainerRect(SC4RectFloat& out);            // @ 0x100766b22
    void GetDrawFrustumContainerRect(SC4RectFloat& out);            // @ 0x100766c30
    void GetViewportRayContainerRect(int x, int y, SC4RectFloat& out);   // @ 0x1007679b4
    bool SaveBackingStore();                                        // @ 0x1007641b4
    bool SaveBackingStore(int x, int y, int width, int height);     // @ 0x100764aec
    bool RestoreBackingStore();                                     // @ 0x10076605c
    bool RestoreBackingStore(int dstX, int dstY, int width, int height, int srcX, int srcY);   // @ 0x100769b1c
    void SetShakeOffsets();                                         // @ 0x1007663a8

    // ---- the model change queue and the quad grid (RENDER3D-2) ----------------------------------
    void ProcessModelChangeQueue();                                 // @ 0x100761896
    // (float const*, float const*, SC4Rect<float>&, int): the world box -> container-space rectangle of one element,
    // for the camera rotation (the original's 4th argument is the rotation, read from the view utilities).
    void WorldToContainerRect(const float* boxMin, const float* boxMax, SC4RectFloat& out, int rotation);   // @ 0x1007620c4
    void UpdateGridElementObjectFlags(s3d::cIS3DModelInstance* model, SC43DGridElement* element);   // @ 0x100762458
    bool ModelShouldGoIntoBackingStore(const SC43DGridElement* element);   // @ 0x100762538
    bool MoveGridElement(SC43DGridElement* element, s3d::cIS3DModelInstance* model);   // @ 0x1007628d6
    // (int oldZoom, int oldRotation, int newZoom, int newRotation): rebuilds the grid when either changed. The
    // original's 4th argument is the new rotation, not a city coordinate (0x1007657c6 passes it to
    // WorldToContainerRect).
    void RebuildQuadGrid(int oldZoom, int oldRotation, int newZoom, int newRotation);   // @ 0x100765592
    void ReevaluateBackingStoreObjectFlags();                       // @ 0x1007657c6
    // The two camera-change notifications the zoom / rotation message handler calls (DoMessage is still PARTIAL):
    // (oldZoom, oldRotation, newZoom, newRotation), each ignoring the axis that did not change.
    void DoViewRotationChange2(int oldZoom, int oldRotation, int newZoom, int newRotation);   // @ 0x10076385e
    void DoViewZoomChange3(int oldZoom, int oldRotation, int newZoom, int newRotation);       // @ 0x100763936

    // ---- inspection for the tests / the host ----------------------------------------------------
    bool IsInitialized() const { return initialized_; }
    bool RenderSystemBorrowed() const { return renderSystemBorrowed_; }
    bool PartialWritesAllowed() const { return partialWritesAllowed_; }
    bool BackingStoreValid() const { return backingStoreValid_; }
    bool BackgroundPictureEnabled() const { return backgroundPictureEnabled_; }
    bool BackgroundPictureVisible() const { return backgroundPictureVisible_; }
    bool TransparentBackground() const { return transparentBackground_; }
    std::uint32_t BackgroundTexture() const { return backgroundTexture_; }
    int BackgroundTextureWidth() const { return backgroundTextureWidth_; }
    int BackgroundTextureHeight() const { return backgroundTextureHeight_; }
    int MapScaleFactor() const { return mapScaleFactor_; }
    int CameraView() const { return cameraView_; }
    const float* ViewportClearColor() const { return viewportClearColor_; }
    bool ViewportClearColorValid() const { return viewportClearColorValid_; }
    const std::list<ViewObjectInfo>& ViewObjects(int list) const { return viewObjectLists_[list]; }
    const std::vector<ModelChangeQueueItem>& ModelChanges() const { return modelChanges_; }
    const std::list<DirtyRectangleQueueItem>& DirtyRectangles() const { return dirtyRectangles_; }
    std::size_t DirtyRectangleCount() const { return dirtyRectangles_.size(); }
    int DirtyRectangleAccumulatedArea() const { return dirtyAccumulatedArea_; }
    int DirtyRectangleMaxArea() const { return dirtyMaxArea_; }
    std::size_t ShakeCount() const { return shakes_.size(); }
    SC4QuadGrid* QuadGrid() const { return quadGrid_; }
    std::size_t ModelSortCount(int list) const {
        const std::vector<cSC4ModelSortElement>& v =
            list == 0 ? modelSortFront_ : (list == 1 ? modelSortBack_ : modelSortUnused_);
        return v.size();
    }
    const std::vector<SC43DGridElement*>& HighlightElements() const { return highlightElements_; }
    bool ViewCleanupPending() const { return viewCleanupPending_; }
    const std::int32_t* CleanupRect() const { return cleanupRect_; }
    ViewObjectInfo* FindViewObjectInfo(cISC4ViewObject3D* object);
    static IRender3DHost* Host() { return host_; }
    static void SetHost(IRender3DHost* host) { host_ = host; }
    static gd::cIGZGDriver* Driver() { return mpGDriver; }
    static bool StaticInitialized() { return mbStaticInitialized; }
    static gd::IBackingStore* BackingStoreService() { return mpBackingStoreExtension; }

private:
    // graphic ports (the original's embedded +0x30 / +0x58 objects)
    struct GraphicPort {
        IRender3DGraphicSystem* owner = nullptr;
        int slot = 0;
    };
    // +0x00..+0x1c
    [[maybe_unused]] std::uint32_t refCountMessageTarget_ = 0;    // +0x0c
    [[maybe_unused]] std::uint32_t refCountSerializable_ = 0;     // +0x10
    [[maybe_unused]] std::uint32_t refCount_ = 0;                 // +0x14
    std::uint32_t refCountPrimary_ = 0;          // +0x1c
    bool initialized_ = false;                   // +0x18
    [[maybe_unused]] std::uint8_t pad19_[3] = {};

    // +0x20..+0x87 graphic system
    IRender3DGraphicSystem* graphicSystem_ = nullptr;   // +0x28
    GraphicPort graphicPortA_;                          // +0x30
    GraphicPort graphicPortB_;                          // +0x58
    std::string graphicSystemNameA_;                    // +0x38
    std::string graphicSystemNameB_;                    // +0x60

    // +0x88..+0x147 render state
    sc4::cSC4RenderProperties* renderProperties_ = nullptr;   // +0x88
    bool renderSystemBorrowed_ = false;          // +0x90
    bool backgroundPictureEnabled_ = true;       // +0x91
    cIGZBuffer* backgroundPicture_ = nullptr;    // +0x98
    std::uint32_t backgroundTexture_ = 0;        // +0xa0
    int backgroundTextureWidth_ = 0;             // +0xa4
    int backgroundTextureHeight_ = 0;            // +0xa8
    bool backgroundPictureVisible_ = true;       // +0xac
    [[maybe_unused]] bool unusedAd_ = false;                      // +0xad
    bool transparentBackground_ = false;         // +0xae
    bool partialWritesSupported_ = true;         // +0xaf
    bool partialWritesAllowed_ = true;           // +0xb0
    bool backingStoreValid_ = false;             // +0xb1
    int viewportWidth_ = 0;                      // +0xb4
    int viewportHeight_ = 0;                     // +0xb8
    std::int32_t drawFrustumRect_[4] = {};       // +0xbc..+0xcb
    float subpixelImageBiasX_ = 0;               // +0xcc
    float subpixelImageBiasY_ = 0;               // +0xd0
    s3d::cS3DCamera* camera_ = nullptr;          // +0xd8
    sc4::cSC4CameraControl* cameraControl_ = nullptr;   // +0xe0
    int projectionState_ = -1;                   // +0xe8
    int viewState_ = -1;                         // +0xec
    float projectionMatrix_[16] = {};            // +0xf0
    s3d::cS3DCamera* uiCamera_ = nullptr;        // +0x130
    int cameraView_ = 0;                         // +0x138
    int viewOffsetX_ = 0;                        // +0x13c (DoViewTranslationChange3: the pixels the view moved since the last frame)
    int viewOffsetY_ = 0;                        // +0x140
    bool lightingEnabled_ = true;                // +0x144
    bool fogEnabled_ = false;                    // +0x145
    // +0x148..+0x15b: the five floats the constructor writes (1, 1, 1, 1, 1); the first three are the fog colour
    // Draw passes to SetFog. +0x15c / +0x160 are the fog start / end distances.
    float lightLevels_[5] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};   // +0x148
    float fogStart_ = 10000.0f;                  // +0x15c (0x461c4000 in the constructor)
    float fogEnd_ = 15000.0f;                    // +0x160 (0x466a6000)
    bool inDraw_ = false;                        // +0x164 (Draw is re-entrant only through the host)
    bool captureDirtyRectangles_ = false;        // +0x165 (suppresses AddDirtyRectangle when set)
    [[maybe_unused]] float citySizeX_ = 4096.0f;                  // +0x168
    [[maybe_unused]] float citySizeZ_ = 4096.0f;                  // +0x16c

    // +0x170..+0x227 owned subsystems
    SC4ViewUtilities* viewUtilities_ = nullptr;   // +0x170
    IS4LightingManager* lightingManager_ = nullptr;   // +0x178
    SC4DrawContext* drawContext_ = nullptr;       // +0x180
    SC4QuadGrid* quadGrid_ = nullptr;             // +0x188
    float unknown190_ = 0.0f;                     // +0x190 (the quad-grid size cached by CreateQuadGrid)
    std::vector<SC43DGridElement*> highlightElements_;    // +0x198 (the 0x80-flagged models, drawn last with an offset)
    std::vector<cSC4ModelSortElement> modelSortFront_;    // +0x1b0 (sorted front-to-back, drawn first)
    std::vector<cSC4ModelSortElement> modelSortBack_;     // +0x1c8 (sorted back-to-front, drawn second)
    std::vector<cSC4ModelSortElement> modelSortUnused_;   // +0x1e0 (filled by DrawDynamicView, never drawn — quirk)
    int unknown1f8_ = 0;                          // +0x1f8 (the per-Draw serial the dirty-rect ages compare against)
    int changeCounter_ = 0;                       // +0x1fc
    bool viewContainerRectValid_ = false;         // +0x200 (the cached view-frustum container rect is valid)
    float viewContainerRect_[4] = {};             // +0x204..+0x213
    bool drawContainerRectValid_ = false;         // +0x214 (the cached draw-frustum container rect is valid)
    float drawContainerRect_[4] = {};             // +0x218..+0x227
    std::list<ViewObjectInfo> viewObjectLists_[4];   // +0x228, +0x240, +0x258, +0x270
    cISC4ViewObject3D* backgroundViewObject_ = nullptr;   // +0x288
    cIGZUnknown* terrainView_ = nullptr;          // +0x290
    bool viewportClearColorValid_ = false;        // +0x298
    float viewportClearColor_[4] = {0, 0, 0, 0};  // +0x2a0
    [[maybe_unused]] float unknown2b0_ = 1.0f;                     // +0x2b0
    [[maybe_unused]] bool unknown2b4_ = false;                     // +0x2b4
    [[maybe_unused]] int unknown2b8_ = 0;                          // +0x2b8

    // +0x2c0..+0x3a7 queues and timers
    std::vector<ModelChangeQueueItem> modelChanges_;   // +0x2c0
    bool walkingModelChanges_ = false;            // +0x2d8
    std::list<DirtyRectangleQueueItem> dirtyRectangles_;   // +0x2e0
    int dirtyCount_ = 0;                          // +0x2f8
    int dirtyAccumulatedArea_ = 0;                // +0x300
    int dirtyMaxArea_ = 0x7fffffff;               // +0x304
    std::vector<std::uint8_t> dirtyScratch_;      // +0x308
    bool processingDirtyRectangles_ = false;      // +0x2fc (set while ProcessDirtyRectangleQueue walks the list)
    bool unknown328_ = true;                      // +0x328 (the cleanup trickle is enabled)
    bool viewCleanupPending_ = false;             // +0x329 (a translated update left a band to re-render)
    [[maybe_unused]] std::uint8_t pad32a_[2] = {};
    int unknown320_ = 0;                          // +0x320 (40,000 after Init)
    int unknown324_ = 0;                          // +0x324 (dirty-rectangle stamp base)
    std::int32_t cleanupRect_[4] = {};            // +0x32c left, +0x330 top, +0x334 right, +0x338 bottom
    std::int32_t cleanupStep_ = 0x40;             // +0x33c (the band width DoViewCleanup re-renders per call)
    cRZTimer frameTimer_{cRZTimer::kMilliseconds};   // +0x340 (units 4)
    int unknown358_ = 10000;                      // +0x358 (the cleanup trickle interval, ms)
    std::list<cSC4Shake> shakes_;                 // +0x360
    int shakeSerial_ = 0;                         // +0x378
    cRZTimer shakeTimer_{cRZTimer::kSeconds};        // +0x380 (units 5)
    int shakeX_ = 0;                              // +0x398
    int shakeY_ = 0;                              // +0x39c
    float unknown3a0_ = 0;                        // +0x3a0 (the shake clock reading of the previous frame)
    int mapScaleFactor_ = 1;                      // +0x3a4
    std::unique_ptr<IS4ViewUtilities> viewUtilitiesSeam_;   // the draw context's +0x28 view utilities seam
    ViewUtilitiesCameraHost viewUtilitiesCameraHost_;      // the +8 host the view utilities project through

    // +0x3a8..+0x425 fixed-view offsets and performance counters
    float fixedViewToContainerX_[4] = {};         // +0x3a8
    float fixedViewToContainerY_[4] = {};         // +0x3b8
    std::int32_t statsSps_ = 0;                   // +0x3c4 (the original reads the engine global 0x100b73800)
    std::int32_t stats_[16] = {};                 // +0x3c8..+0x404 (only the two stats strings read them)
    std::int32_t stats2_[6] = {};                 // +0x408..+0x41c

    // ---- port-only helpers (the original inlines these at every call site) -----------------------
    // malloc(0x40) + the empty bounds box + reference count 1 (0x100761edf / 0x100765700 are the same seven
    // instructions).
    static SC43DGridElement* CreateGridElement();
    // model->GetWorldBounds into the eight floats at +0x28 of the element's block (the original passes the element
    // itself as the out box).
    static cS3DBoundingBox ElementBox(const SC43DGridElement* element);
    static void SetElementBox(SC43DGridElement* element, const cS3DBoundingBox& box);
    // The project + AddDirtyRectangle tail every ProcessModelChangeQueue case jumps to (0x100761a4e / 0x100761a63).
    void QueueDirtyRectangleForBox(int kind, const float* boxMin, const float* boxMax,
                                   s3d::cIS3DModelInstance* model, int extra);
    // The push_back + "queue full" check InsertModel / RemoveModel / MoveModel / ChangeModel repeat
    // (0x100761fdb..0x1007620a3 and its three copies).
    void QueueModelChange(SC43DGridElement* element, s3d::cIS3DModelInstance* model, std::uint8_t change,
                          const float* boxMin, const float* boxMax);

    // ---- statics (the original's file-scope globals) --------------------------------------------
public:
    static constexpr std::uint32_t kCLSID = 0xe9c622d8;   // kCLSID_cSC43DRender

private:
    static inline gd::cIGZGDriver* mpGDriver = nullptr;
    static inline gd::IBackingStore* mpBackingStoreExtension = nullptr;
    static inline void* mpDriverSnapshotExtension = nullptr;
    static inline bool mbStaticInitialized = false;
    static inline bool mbBackingStoreEnabled = true;
    static inline bool mbMacJustZoomed = false;   // the original's _g_MacJustZoomed
    static inline IRender3DHost* host_ = nullptr;
    static inline cSC43DRender* spInstance = nullptr;   // the original's single render object (its FBO is static)
};

}  // namespace sc4
