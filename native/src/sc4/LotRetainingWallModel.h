// cSC4LotRetainingWallModel — one wall segment of a lot: the mesh that hides the terrain step along one edge of a
// locked lot whose ground is cut into a slope (0x1001671ac..0x1001688ca, 31 symbols, object 0xc8 bytes).
//
// The model is a cS3DModelInstanceBasic (+8) with the cISC4LotRetainingWallModel interface on top (iid 0xa9e2831e,
// the only method is GetWallModel @ vslot 0x18 -> `this`). cSC4LotRetainingWallOccupant creates one model per lot
// edge, hands it the lot's bounding box and the edge index (Init), and adds it to the placeable object and the
// renderer. The geometry is an unindexed triangle list of 24-byte sVF_V3F_C4UB_T2F vertices (format 10), one or two
// triangles per cell of the edge, with the terrain heights on the two vertices of the cell and the lot's own altitude
// as the top of the wall.
//
// Object layout (0xc8; sLotRetainingWallModelAllocator = 0xc8, operator new @ 0x1001673c4):
//   +0x00 cISC4LotRetainingWallModel vptr      +0x08 cS3DModelInstanceBasic (0x38, ends at +0x3f)
//   +0x3c bool built (written by Init, never read back by any recovered function)
//   +0x40 cS3DBoundingBox local bounds, relative to the wall's first cell vertex (min {1,1,1} / max {0,0,0})
//   +0x58 cS3DBoundingBox world bounds (min {1,1,1} / max {0,0,0}; what GetWorldBounds answers)
//   +0x70 u8 zoom level (0xff = not set yet)   +0x71 u8 view rotation (0xff)   +0x72 u8 segments built
//   +0x74 SC4Point<int> wall start cell        +0x7c SC4Point<int> wall end cell (exclusive)
//   +0x84 s8 wall edge 0..3 (0xff = none)      +0x88 float texture scale (16.0)
//   +0x8c float uv scale (0.0625 = 1/16, never written; the port uses kRetainingWallUvScale) +0x90 u32 ids[5]
//   +0xa8 cS3DTextureBinding* (one reference)  +0xb0 std::vector<sVF_V3F_C4UB_T2F> vertices
//
// The edge/cell convention (SetupVerticesFromBBoxAndWallEdge @ 0x10016872a): the model's box is converted from metres
// to city cells with (metres * 0.0625 + 0.1 + 2^23) - 2^23 for the minima and (metres * 0.0625 - 0.9 + 2^23) - 2^23
// for the maxima, i.e. rounding to nearest after a 0.1 / -0.9 bias. Edge 0 is the wall along x at z0, edge 1 along z
// at x1+1, edge 2 along x at z1+1 and edge 3 along z at x0.
//
// The neighbours the original reached through SL::sp* globals are behind ILotRetainingWallModelHost (one method per
// original call, address in the comment); cSC4Lot itself is a concrete class in this port (sc4/Lot.h).
//
// Quirks kept:
//  * A cell is only walled when the terrain is *above* the lot altitude (the lot is cut into the slope); the segment
//    test uses the lot altitude, the two branches below it use altitude + 0.002.
//  * SetupVertices (true, ...) writes the bounding box on every call but only reports true through the `changed`
//    out-parameter when one of the six bounds moved by at least 0.0001 (so Init's second call reports true).
//  * The along-axis range is shrunk by 0.001 on both ends before it becomes the bounding box, and the fixed axis is
//    0.001..0.002 thick (a wall is a plane, and a zero-thickness box would break the culling).
//  * SetupVertices (false, true, ...) is the recolor-only path Draw uses when the view rotation changed: it does not
//    touch the geometry and returns 0 without writing `changed`. Every other early-out (no lot at the wall cell, no
//    engine pointer) also returns 0 and leaves `changed` alone: only the build path's closing store writes it
//    (0x1001684d7).
//  * SetupPackedRenderState calls SL::spRenderer vslot 0x50 (cSC43DRender::GetDrawContext) and throws the answer
//    away (0x1001676a5..0x1001676ab).
//  * SetupTextures, when the texture id of the new zoom level fails to bind, adds 0x695c00c6 to the id and retries
//    once (the ids are generated per zoom level by the lot builder, see 0x100167793).
//  * GetSortValue answers 0xc9862a86 when there is no binding (0x1001674e0); with one it ORs 0x2a86 into the
//    binding's texture id.
//  * Draw refuses the model for the two edges that face away from the camera: with the view rotation r only
//    edges (r+3)%4 and r%4 are drawn (0x10016758d..0x1001675b7).
//  * Draw's recolour path hands SetupVertices no lighting manager even though the original passes the draw
//    context's (+0x68): cSC43DRender::Init stores the same object there and in SL::spLightingManager, so the
//    host's answer is the same one, but the port never reads the context field.
#pragma once
#include <cstdint>
#include <vector>

#include "sc4/Occupant.h"
#include "sc4/PackedRenderState.h"
#include "s3d/ModelInstance.h"

namespace sc4 {

class cSC4Lot;

// SC4Point<int>: two int32 (the original's cell point; cSC4ResponseCurve's global SC4Point is the float pair, hence
// the suffix, as in sc4/LotBaseTextureOccupant.h).
struct SC4PointInt {
    std::int32_t x = 0;
    std::int32_t z = 0;
};

constexpr std::uint32_t kGZIID_cISC4LotRetainingWallModel = 0xa9e2831e;
constexpr std::uint32_t kLotRetainingWallModelSize = 0xc8;

// {type, group, instance} of a generated texture binding (cS3DTextureBindingFactory::CreateTexture @ vslot 0x20).
// The wall textures are the FSH pair the lot builder writes: type 0x7ab50e44, group 0x891b0e1a, instance = texture id.
constexpr std::uint32_t kRetainingWallTextureType = 0x7ab50e44;
constexpr std::uint32_t kRetainingWallTextureGroup = 0x891b0e1a;
struct SC4TextureBindingKey {
    std::uint32_t type = 0;       // +0
    std::uint32_t group = 0;      // +4
    std::uint32_t instance = 0;   // +8
};
static_assert(sizeof(SC4TextureBindingKey) == 0x0c, "texture binding key");

// The retry step SetupTextures adds to a texture id that failed to bind (0x100167793).
constexpr std::uint32_t kRetainingWallTextureRetryStep = 0x695c00c6;
// The default sort value with no texture binding (0x1001674e0) and the OR mask with one (0x1001674d7).
constexpr std::uint32_t kRetainingWallSortValueNoTexture = 0xc9862a86;
constexpr std::uint32_t kRetainingWallSortValueMask = 0x2a86;
// Texture repeats per metre on the wall face: u and v are the position * 1/16 (0x1001676b9).
constexpr float kRetainingWallUvScale = 0.0625f;

// -------------------------------------------------------------------------------------------------
// cISC4LotRetainingWallModel (iid 0xa9e2831e): one method, at the primary vtable's slot 3.
// -------------------------------------------------------------------------------------------------
class cISC4LotRetainingWallModel : public cIGZUnknown {
public:
    virtual cISC4LotRetainingWallModel* GetWallModel() = 0;   // [3] 0x18 @ 0x100168850
    // PORT-ONLY: the parts of cSC4LotRetainingWallModel the wall occupant (cSC4LotRetainingWallOccupant) calls on the
    // model it finds through this interface; the original calls the concrete class directly.
    virtual std::int8_t WallEdge() const = 0;                                               // +0x84
    virtual void SetTextureData(const std::uint32_t* textureIds, float scale) = 0;          // @ 0x100168808
    virtual bool Init(const cS3DBoundingBox& bounds, int wallEdge, bool& builtOut) = 0;     // @ 0x1001686d4
    // the base at +8, as the placeable object and the occupant manager see a model (sc4::cIS3DModelInstance)
    virtual sc4::cIS3DModelInstance* AsModelInstance() = 0;
};

// sVF_V3F_C4UB_T2F, 0x18 bytes: Draw submits it as the interleaved format 10 (V3F_C4UB_T2F).
struct LotRetainingWallVertex {
    float x = 0.0f, y = 0.0f, z = 0.0f;                     // +0
    // +0xc: the geometry build leaves these at zero (the original's vector::resize value-initialises the POD
    // vertex); the lighting pass fills rgb and forces the alpha to 0xff.
    std::uint8_t color[4] = {0, 0, 0, 0};
    float u = 0.0f, v = 0.0f;                               // +0x10
};
static_assert(sizeof(LotRetainingWallVertex) == 0x18, "sVF_V3F_C4UB_T2F stride");

// -------------------------------------------------------------------------------------------------
// Seams: the SL::sp* globals and the two unrecovered neighbours (cISC4LotManager, cS3DTextureBinding).
// -------------------------------------------------------------------------------------------------

// cISC4LotManager as far as the model reads it.
class ILotRetainingWallLotManager {
public:
    virtual ~ILotRetainingWallLotManager() = default;
    // cISC4LotManager vslot 0x28 (x, z, 0): the lot covering a standard city cell (borrowed).
    virtual cSC4Lot* LotAtCell(int x, int z) = 0;
};

// cSTETerrain as far as the model reads it (sc4/cSTETerrain.h in the game side).
class ILotRetainingWallTerrain {
public:
    virtual ~ILotRetainingWallTerrain() = default;
    virtual int CellCountX() const = 0;                        // vslot 0x38
    virtual int CellCountZ() const = 0;                        // vslot 0x40
    virtual float AltitudeAtVertex(int x, int z) const = 0;    // vslot 0x70
};

// cISC4LightingManager vslot 0xc0 (GetColor(position, normal, r, g, b)); sc4/LightingManager.h in the game side.
class ILotRetainingWallLighting {
public:
    virtual ~ILotRetainingWallLighting() = default;
    virtual void GetColor(const cS3DVector3& position, const cS3DVector3& normal, std::uint8_t& r, std::uint8_t& g,
                          std::uint8_t& b) = 0;
};

// cS3DTextureBinding as far as the model reads it: the first word of the binding's texture is the texture id, and
// Release drops the model's reference (0x1000d9bfa).
class ILotRetainingWallTextureBinding {
public:
    virtual ~ILotRetainingWallTextureBinding() = default;
    virtual std::uint32_t TextureId() const = 0;
    virtual void Release() = 0;
};

// cIS3DTextureBindingFactory (RZ framework service 0x1ac0e11a / 0xfac0e219) vslot 0x20: CreateTexture(key, &out),
// answering one reference or null.
class ILotRetainingWallTextures {
public:
    virtual ~ILotRetainingWallTextures() = default;
    virtual ILotRetainingWallTextureBinding* CreateTexture(const SC4TextureBindingKey& key) = 0;
};

// The engine services of the original, one method per call site.
class ILotRetainingWallModelHost {
public:
    virtual ~ILotRetainingWallModelHost() = default;
    virtual ILotRetainingWallLotManager* LotManager() = 0;        // SL::spLotManager @ 0x100b214c8
    virtual ILotRetainingWallTerrain* Terrain() = 0;              // SL::spTerrain    @ 0x100b214c0
    virtual ILotRetainingWallLighting* Lighting() = 0;            // SL::spLightingManager
    virtual ILotRetainingWallTextures* Textures() = 0;            // RZGetFramework()->GetService(0x1ac0e11a, 0xfac0e219)
    // SL::spRenderer != 0 (0x100b214d0): the original calls vslot 0x50 (cSC43DRender::GetDrawContext) and drops the
    // result, in SetupPackedRenderState.
    virtual void TouchRendererDrawContext() = 0;
    // SC4ViewUtilities::GetVectorForEdge @ 0x10017723c (src/sc4/ViewUtilities.h): the outward normal of a lot edge.
    virtual void GetVectorForEdge(float* v, int edge) = 0;
};
ILotRetainingWallModelHost* LotRetainingWallModelHost();
void SetLotRetainingWallModelHost(ILotRetainingWallModelHost* host);

// The packed render state every wall model shares (cSC4LotRetainingWallModel::mPackedRenderState).
const cSC4PackedRenderState& LotRetainingWallPackedRenderState();
// cSC4LotRetainingWallModel::mbStaticInitialized: the one-shot flag SetupPackedRenderState is built behind.
bool& LotRetainingWallStaticInitialized();

// -------------------------------------------------------------------------------------------------
// The object.
// -------------------------------------------------------------------------------------------------
class cSC4LotRetainingWallModel final : public cISC4LotRetainingWallModel, public s3d::cS3DModelInstanceBasic {
public:
    cSC4LotRetainingWallModel();    // @ 0x1001671ac
    ~cSC4LotRetainingWallModel() override;   // @ 0x100167284 (D2), 0x100167338 (D1), 0x100167360 (D0)

    // cISC4LotRetainingWallModel
    cISC4LotRetainingWallModel* GetWallModel() override;   // @ 0x100168850

    // ---- cS3DModelInstanceBasic / cIS3DModelInstance overrides the wall supplies
    bool QueryInterface(std::uint32_t iid, void** out) override;      // @ 0x1001673d4
    std::uint32_t AddRef() override;                                  // @ 0x100168834 (the base's, +8)
    std::uint32_t Release() override;                                 // @ 0x100168842
    void GetWorldBounds(cS3DBoundingBox& out) override;               // @ 0x100167452
    bool Pick(cSC4PickContext* context) override;                     // @ 0x1001674ac (always false)
    void GetSortValue(std::uint32_t& out) const override;             // @ 0x1001674c4
    int Draw(SC4DrawContext* context, bool highlight) override;       // @ 0x100167510

    // ---- the wall itself
    // 0x1001686d4: store the edge, work out the wall's cell span and build the geometry.
    bool Init(const cS3DBoundingBox& bounds, int wallEdge, bool& changed) override;
    // 0x10016872a: bbox + edge -> the wall's start and end cell
    static void SetupVerticesFromBBoxAndWallEdge(const cS3DBoundingBox& bounds, int wallEdge, SC4PointInt& start,
                                                 SC4PointInt& end);
    // 0x1001677ea: build (rebuild = true) or recolor (rebuild = false) the vertex list; `changed` gets the
    // "bounds moved" answer of the build path. The three engine pointers override the host's services; the original
    // fills them from SL::sp* when they are null.
    bool SetupVertices(bool rebuild, bool recolor, bool& changed, ILotRetainingWallLotManager* lotManager,
                       ILotRetainingWallTerrain* terrain, ILotRetainingWallLighting* lighting);
    // 0x100168638: is the cell next to (x, z) on the other side of the wall a locked lot? (edge 0/3 look one cell
    // back in z / x)
    bool SegmentIsAdjacentToFlatLot(int x, int z, ILotRetainingWallLotManager* lotManager,
                                    ILotRetainingWallTerrain* terrain);
    // 0x100168808: five texture ids and the texture scale (float)
    void SetTextureData(const std::uint32_t* textureIds, float scale) override;
    // 0x10016762a: zoom level -> per-zoom texture binding (and the one-shot render state on the first call)
    void DoZoomChange(int oldZoom, int newZoom);
    // 0x1001676f8: bind the texture of the new zoom level
    void SetupTextures(int oldZoom, int newZoom);
    // 0x100167692: build the shared packed render state
    static void SetupPackedRenderState();

    // ---- what the recovered neighbours (cSC4LotRetainingWallOccupant) read
    // PORT-ONLY: the cIS3DModelInstance subobject of the cS3DModelInstanceBasic base (the occupant's
    // ILotRetainingWallModel::AsModelInstance). The original reaches it through the base at +8.
    sc4::cIS3DModelInstance* AsModelInstance() override {
        return reinterpret_cast<sc4::cIS3DModelInstance*>(static_cast<s3d::cIS3DModelInstance*>(this));
    }
    std::int8_t WallEdge() const override { return wallEdge_; }     // +0x84
    std::uint32_t TextureId() const;                                // the binding's texture id, 0 without a binding
    const std::vector<LotRetainingWallVertex>& Vertices() const { return vertices_; }   // +0xb0
    std::uint8_t SegmentsBuilt() const { return segmentsBuilt_; }   // +0x72
    std::uint8_t ZoomLevel() const { return zoom_; }                // +0x70
    std::uint8_t ViewRotation() const { return rotation_; }         // +0x71

private:
    void ApplyLighting(ILotRetainingWallLighting* lighting);        // the tail of SetupVertices

    cS3DVector3 localMin_;   // +0x40 (the port keeps the two boxes as members; the original stores raw floats)
    cS3DVector3 localMax_;   // +0x4c
    cS3DVector3 worldMin_;   // +0x58
    cS3DVector3 worldMax_;   // +0x64
    std::uint8_t zoom_ = 0xff;           // +0x70
    std::uint8_t rotation_ = 0xff;       // +0x71
    std::uint8_t segmentsBuilt_ = 0;     // +0x72
    std::uint8_t built_ = 0;             // +0x3c (kept apart; the original's order is stated above)
    SC4PointInt wallStart_{};          // +0x74
    SC4PointInt wallEnd_{};            // +0x7c
    std::int8_t wallEdge_ = -1;          // +0x84
    float textureScale_ = 16.0f;         // +0x88
    std::uint32_t textureIds_[5] = {0, 0, 0, 0, 0};   // +0x90
    ILotRetainingWallTextureBinding* binding_ = nullptr;   // +0xa8 (one reference)
    std::vector<LotRetainingWallVertex> vertices_;         // +0xb0
};

}  // namespace sc4
