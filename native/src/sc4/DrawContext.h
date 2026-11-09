// SC4DrawContext — the 3-D draw context every model, terrain and overlay path renders through (58 symbols,
// 0x100771e8a..0x100774e5c). It owns the fixed-function state the 3-D pass needs (a packed render-state word it
// diffs against, two texture-stage records, lighting / shading / transparency / fog values, the model-view
// matrix, a render-state stack and the current index buffer) and forwards the actual drawing to cIGZGDriver.
//
// Original layout (0x288 bytes, all pointers are wired by cSC43DRender::Init at 0x100760354..0x1007603f0):
//   +0x00 bool initialised                      +0x04 int   (cleared by the renderer; unread here)
//   +0x08 int viewport width                    +0x0c int   viewport height
//   +0x10 float zoom level as float             +0x14 int   zoom          +0x18 int rotation
//   +0x1c/+0x20 ints cleared                    +0x24 int   (never initialised by the original)
//   +0x28 SC4ViewUtilities*                     +0x30/+0x38 renderer objects (unused here)
//   +0x40 cIGZGDriver*                          +0x48 cSC43DRender*
//   +0x50 cSC4RenderProperties*                 +0x58 cS3DCamera*
//   +0x60 cSC4CameraControl*                    +0x68 cSC4LightingManager*
//   +0x70/+0x78 cleared                         +0x80 int pass (terrain 0 / water 1, set by the terrain view)
//   +0x88 std::vector<cS3DVector4> 11 highlight colours
//   +0xa0/+0xb8 std::vector (renderer scratch, not touched by this class)
//   +0xd0 u8 / +0xd1 u8 capability bytes        +0xd4 int max texture stages (from the device)
//   +0xd8 u32 current render-state word          +0xdc reserved
//   +0xe0 cSC4PackedRenderState default state   +0xe8 bool lighting
//   +0xec/+0xf0/+0xf4 float ambient light        +0xf8 bool fog
//   +0xfc float[3] fog colour (+0x108 holds the same FLT_MAX pattern; not read here)
//   +0x10c float fog start                       +0x110 float fog end
//   +0x114 int depth offset                      +0x118 cGDCombiner default combiner
//   +0x124 cSC4PackedRenderState highlight state +0x12c cGDCombiner highlight combiner
//   +0x138 std::vector<cStageInfo> (stride 0x68) +0x150 int max stages
//   +0x154 bool                                  +0x158 cS3DTransform (zeroed, not touched here)
//   +0x190 std::vector (renderer scratch)        +0x1a8 bool model-view matrix is up to date
//   +0x1ac cS3DTransform model transform (+0x1ac counter, +0x1ad flags, +0x1b0 rotation, +0x1d4 translation,
//           +0x1e0 scale)                        +0x1e4 cS3DTransform composed view transform
//   +0x21c float[16] model-view matrix (column-major rotation, translation in the last column)
//   +0x260 std::vector<cSC4PackedRenderState> render-state stack
//   +0x278 bool shade override active            +0x280 const u16* current index buffer
//
// Quirks kept (see the tests):
//  * SetHighlightColor / SetTexture / SetTexCoord / SetTexWrapModes / SetTexFiltering index the stage array with
//    the caller's stage number without a bounds check.
//  * DrawPrimsIndexed(prim, first, count) offsets the index pointer by first * 2 bytes; the "first" argument is a
//    byte/vertex offset, not an index count.
//  * SetIndexBuffer stores the pointer and ignores the count.
//  * SetTexEnvMode / SetTexCombiner only touch texture stage 0 unless cSC4RenderProperties::kUseSecondStage is set.
//  * SetTexTransform4 stores the matrix only for stage 1 (stage 0 still gets TexStageMatrix).
//  * The two SetModelTransform overloads disagree on the matrix convention: the cS3DTransform one builds the
//    rotation transposed with the translation in the last column (like ResetModelViewTransform), the cS3DMatrix4
//    one builds the rotation as stored with the translation in m[3], m[7], m[11]. Kept as is.
//  * Init takes the stage count from the *video mode* record (dword +0x24 of the 0x34-byte mode), which is 0 in
//    this build, so the count is clamped up to 2. Bytes +0x29 / +0x2a of the same record land in +0xd0 / +0xd1.
//  * RenderModelInstance's small-model cull scans four floats starting at the z extent; two of them are other
//    stack slots (the boolean argument reinterpreted as float and an uninitialised one). This port takes the
//    maximum of the three real extents; see the comment at the call site.
//
// Seams (unrecovered neighbours, see docs/SEAMS.md):
//  * +0x28 IS4ViewUtilities        — SC4ViewUtilities (+0x15c viewport units per world unit)
//  * +0x68 IS4LightingManager      — cSC4LightingManager (vslots 0x50 / 0x58)
//  * DrawBoundingBox IS3DMeshBuilder / IMeshBuilderHost — cS3DMeshBuilder (0x1001c5c4c..0x1001c6139)
#pragma once
#include <cstdint>
#include <vector>

#include "gz/StreamOps.h"
#include "render/gd/IGZGDriver.h"
#include "render/gd/VideoModes.h"
#include "sc4/PackedRenderState.h"
#include "sc4/RenderProperties.h"
#include "s3d/Camera.h"
#include "s3d/MaterialManager.h"
#include "s3d/MeshAndMaterial.h"
#include "s3d/ModelInstance.h"

namespace sc4 {
class cSC4CameraControl;   // src/sc4/CameraControl.h (only stored here, +0x60)
}

namespace gd {
// cGDCombiner — the 12-byte texture-combiner record cIGZGDriver::SetCombiner takes (vslot 0x1f0). Only the
// driver decodes it; SC4DrawContext stores it in the stage records and hands it back verbatim.
struct cGDCombiner {
    std::uint32_t word0 = 0x01030001;
    std::uint32_t word1 = 0x01030001;
    std::uint32_t word2 = 0x00010000;
};
}  // namespace gd

// The combiner literals SC4DrawContext uses. The constructor fills both combiner fields with the first one; Init
// replaces +0x118 with the default and +0x12c with the highlight combiner:
//   ctor      (+0x118 / +0x12c): u16 {1, 0x103, 1, 0x103} → words 0x01030001, 0x01030001, 0
//   +0x118 (stages that ask for a combiner):                    → words 0x01010002, 0x01030100, 0
//   +0x12c (highlight pass, copied into stage 0):               → words 0x01000202, 0x01000201, 0
inline constexpr gd::cGDCombiner kSC4CtorTexCombiner{0x01030001u, 0x01030001u, 0x00000000u};
inline constexpr gd::cGDCombiner kSC4DefaultTexCombiner{0x01010002u, 0x01030100u, 0x00000000u};
inline constexpr gd::cGDCombiner kSC4HighlightTexCombiner{0x01000202u, 0x01000201u, 0x00000000u};

// cS3DMesh as RenderMesh walks it (0x10077441a): three records built by the (unrecovered) cS3DMeshBuilder.
struct SC4MeshVertexGroup {
    std::uint32_t format = 0;        // +0 eGDVertexFormat
    std::uint16_t reserved4 = 0;     // +4
    std::uint16_t count = 0;         // +6 vertex count (0 = nothing to draw)
    const void* data = nullptr;      // +8 interleaved vertices
};
struct SC4MeshDrawRecord {           // 0xc bytes
    std::uint32_t primitive;         // +0 eGDPrimType
    std::uint32_t start;             // +4 first vertex / first index
    std::uint32_t count;             // +8
};
// The draw-record container as the mesh refers to it: a header word followed by begin / end pointers at +8/+0x10.
struct SC4MeshDrawList {
    const void* reserved0 = nullptr;            // +0
    const SC4MeshDrawRecord* begin = nullptr;   // +8
    const SC4MeshDrawRecord* end = nullptr;     // +0x10
};
struct SC4Mesh {
    const SC4MeshVertexGroup* vertices = nullptr;   // +0
    const std::uint16_t* const* indexGroup = nullptr;   // +8 (the index buffer is *indexGroup)
    const SC4MeshDrawList* draws = nullptr;          // +0x10
};

// ---------------------------------------------------------------------------------------------
// Seams
// ---------------------------------------------------------------------------------------------

// SC4ViewUtilities (draw context +0x28, cSC43DRender +0x170). A recovery of the class is in progress
// (src/sc4/ViewUtilities.* was being written when this class was ported); the draw context only needs the one
// field, so it goes through this seam until that class is ready.
class IS4ViewUtilities {
public:
    virtual ~IS4ViewUtilities() = default;
    // +0x15c: viewport units per world unit, written by UpdateCameraPosition from cSC4CameraControl +0x140.
    virtual float ViewportUnitsPerWorld() const = 0;
};

// cSC4LightingManager (unrecovered; draw context +0x68, cSC43DRender +0x178, 0x240 bytes).
class IS4LightingManager {
public:
    virtual ~IS4LightingManager() = default;
    virtual void GetGlobalLight(cS3DVector3& out) = 0;                                // vslot 0x50 @ 0x100778042
    virtual bool GetModelLight(s3d::cIS3DModelInstance* instance, cS3DVector3& out) = 0;   // vslot 0x58 @ 0x100778062
};

// cS3DMeshBuilder (unrecovered; the debug box mesh DrawBoundingBox builds, 0x1001c5c4c..0x1001c6139).
class IS3DMeshBuilder {
public:
    virtual ~IS3DMeshBuilder() = default;
    virtual void Clear() = 0;                                        // 0x1001c5c8c
    virtual void SetDirLight(const cS3DVector3& direction) = 0;      // 0x1001c5fd0
    virtual void SetColor(const cS3DVector4& color) = 0;             // 0x1001c5fb0 (C)
    virtual void AddBoxMesh(const cS3DVector3 corners[8]) = 0;       // nS3D::AddBoxMesh @ 0x1001c6139
    virtual const SC4Mesh* Mesh() const = 0;                         // 0x1001c5fea
};
// The original's function-local static builder (0x1001c5c4c) lives in the S3D engine; the host supplies it.
class IMeshBuilderHost {
public:
    virtual ~IMeshBuilderHost() = default;
    virtual IS3DMeshBuilder* BoxMeshBuilder() = 0;
};
IMeshBuilderHost* MeshBuilderHost();
void SetMeshBuilderHost(IMeshBuilderHost* host);

// ---------------------------------------------------------------------------------------------
// The draw context
// ---------------------------------------------------------------------------------------------
class SC4DrawContext {
public:
    // One texture stage (0x68 bytes): the state SetTexture / SetTexCoord / SetTexWrapModes / SetTexFiltering /
    // SetTexCombiner / SetTexEnvMode / SetTexTransform4 cache so that repeated calls are dropped.
    struct cStageInfo {
        cStageInfo();                                  // @ 0x100771e8a
        std::int32_t texture = -1;                     // +0x00
        std::int32_t texCoord = -1;                    // +0x04 eGDTexCoordSource
        std::int32_t wrapU = -1;                       // +0x08
        std::int32_t wrapV = -1;                       // +0x0c
        std::int32_t magFilter = -1;                   // +0x10 (SetTexFiltering's third argument)
        std::int32_t minFilter = -1;                   // +0x14 (second argument)
        gd::cGDCombiner combiner;                      // +0x18
        bool transform = false;                        // +0x24 a tex transform is installed
        float texMatrix[16];                           // +0x28 (its first float is the texture scale)
    };

    SC4DrawContext();                                  // @ 0x100771f04 (0x1007722a6 is its C2 alias)
    ~SC4DrawContext();                                 // @ 0x1007722b0 (0x100772356 is its C2 alias)

    void Init();                                       // @ 0x100772360
    void Shutdown();                                   // @ 0x1007725ee
    void SetHighlightColor(int index, const cS3DVector4& color);   // @ 0x1007725c0
    void SetCamera(s3d::cS3DCamera* camera);           // @ 0x100772604
    void ResetModelViewTransform();                    // @ 0x1007726ee
    void GetModelViewMatrix(float* matrix) const;      // @ 0x100772908
    void SetModelViewTransformChanged();               // @ 0x100772966

    // render state
    void EnableColorMaskFlag(bool enable);             // @ 0x100772974
    void EnableAlphaTestFlag(bool enable);             // @ 0x1007729c2
    void EnableDepthTestFlag(bool enable);             // @ 0x100772a28
    void EnableDepthMaskFlag(bool enable);             // @ 0x100772a80
    void EnableTextureStateFlag(bool enable, int stage);   // @ 0x100772ad4
    void EnableBlendStateFlag(bool enable);            // @ 0x100772c4a
    void EnableCullFaceFlag(bool enable);              // @ 0x100772cb6
    void SetTexture(int texture, int stage);           // @ 0x100772d0e
    void SetDepthFunc(int function);                   // @ 0x100772d64
    void SetBlendFunc(int source, int destination);    // @ 0x100772da8
    void SetAlphaFunc(int function, float reference);  // @ 0x100772e20
    void SetTexEnvMode(int mode, int stage);           // @ 0x100772eca
    void SetTexCoord(int source, int stage);           // @ 0x100772fa4
    void SetTexWrapModes(int wrapU, int wrapV, int stage);   // @ 0x10077302a
    void SetTexFiltering(int minFilter, int magFilter, int stage);   // @ 0x100773108
    void SetTexCombiner(const gd::cGDCombiner& combiner, int stage);   // @ 0x100773212
    void SetTexColor(const cS3DVector4& color);        // @ 0x100773296
    void SetTexTransform4(const cS3DMatrix4& matrix, int stage);   // @ 0x1007732be
    void ClearTexTransform(int stage);                 // @ 0x1007733c6
    void SetVertexBuffer(int format, std::uint32_t count, const void* vertices);   // @ 0x100773440
    void SetIndexBuffer(std::uint32_t count, const std::uint16_t* indices);        // @ 0x100773454
    void DrawPrims(int primitive, int format, int count, const void* vertices);    // @ 0x100773462
    void DrawPrims(int primitive, int first, int count);                           // @ 0x1007734a8
    void DrawPrimsIndexed(int primitive, int first, int count);                    // @ 0x1007734ba
    void DrawPrimsIndexed(int primitive, int count, const std::uint16_t* indices); // @ 0x1007734e8

    // lighting / shading / fog
    void SetLighting(bool enable);                     // @ 0x100773502
    bool GetLighting() const;                          // @ 0x100773582
    void SetShade(const cS3DVector3& shade);           // @ 0x10077358e
    void SetSelfLitShade(const cS3DVector3& shade);    // @ 0x1007735d8
    void SetModelShade(s3d::cIS3DModelInstance* instance, const cS3DVector3* shade);   // @ 0x10077360a
    void ResetShade();                                 // @ 0x1007736be
    void SetTransparency(float alpha);                 // @ 0x100773704
    void ResetTransparency();                          // @ 0x10077371a
    void SetFog(bool enable, const float* color, float start, float end);   // @ 0x100773738

    // packed render states
    void SetDefaultRenderState();                      // @ 0x1007738da
    void SetRenderState(const cSC4PackedRenderState& state);   // @ 0x100773906
    void SetDefaultRenderStateUnilaterally();          // @ 0x100773c08
    void SetRenderState(const s3d::cS3DMaterial& material, const s3d::SC4DrawAttributes* attributes);   // @ 0x100773d50
    void SetRenderStateHighlight(const s3d::cS3DMaterial& material, const s3d::SC4DrawAttributes* attributes);   // @ 0x1007740a4
    void SetRenderStateHighlight(std::uint32_t type);  // @ 0x100774130
    void SetDepthOffset(int offset);                   // @ 0x1007741b8
    int GetDepthOffset() const { return depthOffset_; }   // the field (+0x114) callers read directly
    void SetEmulatedSecondStageRenderState();          // @ 0x10077428a

    // geometry
    void RenderMesh(const SC4Mesh& mesh);              // @ 0x10077441a
    void SetModelTransform(const cS3DTransform& transform);   // @ 0x100774512
    void SetModelTransform(const cS3DMatrix4& transform);     // @ 0x1007747fa
    void RenderModelInstance(s3d::cIS3DModelInstance* instance, s3d::cS3DMeshAndMaterial* meshes,
                            const s3d::SC4DrawAttributes* attributes, bool highlight);   // @ 0x100774ad0
    void DrawBoundingBox(const cS3DBoundingBox& box, const cS3DVector4& color);   // @ 0x100774e5c

    // ---- port wiring / inspection -----------------------------------------------------------------
    // The original sets these fields directly from cSC43DRender::Init (0x100760354..0x1007603f0).
    void SetDriver(gd::cIGZGDriver* driver) { driver_ = driver; }                       // +0x40
    gd::cIGZGDriver* Driver() const { return driver_; }
    s3d::cS3DCamera* Camera() const { return camera_; }
    void SetRenderProperties(sc4::cSC4RenderProperties* properties) { renderProperties_ = properties; }   // +0x50
    void SetViewUtilities(IS4ViewUtilities* utilities) { viewUtilities_ = utilities; }   // +0x28
    void SetLightingManager(IS4LightingManager* lighting) { lightingManager_ = lighting; }   // +0x68
    void SetCameraControl(sc4::cSC4CameraControl* control) { cameraControl_ = control; } // +0x60
    void SetViewportSize(std::int32_t width, std::int32_t height) { viewportWidth_ = width; viewportHeight_ = height; }
    // +0x04: the pass the renderer is drawing. cSC4UndergroundOccupant::Draw reads it (0 = trench and walls,
    // 1 = the piece model); the original sets it from cSC43DRender before each pass.
    std::int32_t RenderPass() const { return renderPass_; }
    void SetRenderPass(std::int32_t pass) { renderPass_ = pass; }
    // +0x20: the cISC43DRender::ViewUpdateReason the renderer is running (written by DoPartialViewUpdate,
    // DoTranslatedViewUpdate, DoSingleModelStaticDrawAddition and Draw; nothing in the recovered draw code reads it).
    std::int32_t ViewUpdateReason() const { return reserved20_; }
    void SetViewUpdateReason(std::int32_t reason) { reserved20_ = reason; }
    // +0x80: the view pass Draw / the two view draws are in (cISC43DRender sets 0..5 around the view-object lists).
    std::int32_t ViewPass() const { return pass_; }
    void SetViewPass(std::int32_t pass) { pass_ = pass; }
    void SetZoomAndRotation(std::int32_t zoom, std::int32_t rotation);                   // +0x10 / +0x14 / +0x18
    std::int32_t Zoom() const { return zoom_; }                                          // +0x14
    std::int32_t ViewRotation() const { return rotation_; }                              // +0x18
    bool IsInitialized() const { return initialized_; }
    std::uint32_t StateWord() const { return stateWord_; }
    const std::vector<cStageInfo>& Stages() const { return stages_; }
    const std::vector<cS3DVector4>& HighlightColors() const { return highlightColors_; }
    const cS3DTransform& ViewTransform() const { return viewTransform_; }
    const cS3DTransform& ModelTransform() const { return modelTransform_; }
    std::int32_t MaxTextureStages() const { return maxTextureStages_; }
    std::uint8_t DeviceCapability29() const { return deviceCap29_; }
    std::uint8_t DeviceCapability2a() const { return deviceCap2a_; }

private:
    bool DeviceSecondStage() const;       // cSC4RenderProperties::kUseSecondStage (bools[11], +0x2f8)
    float ViewportUnitsPerWorld() const;  // +0x28 -> SC4ViewUtilities +0x15c
    void UpdateViewTransformFromCamera(); // copy cS3DCamera::ViewXform into viewTransform_ (+0x1e4)

    bool initialized_ = false;             // +0x00
    std::int32_t renderPass_ = 0;          // +0x04 (0 trench/walls, 1 pieces; set by the renderer)
    std::int32_t viewportWidth_ = 0;       // +0x08
    std::int32_t viewportHeight_ = 0;      // +0x0c
    float zoomScale_ = 0.0f;               // +0x10
    std::int32_t zoom_ = 0;                // +0x14
    std::int32_t rotation_ = 0;            // +0x18
    [[maybe_unused]] std::int32_t reserved1c_ = 0;          // +0x1c
    [[maybe_unused]] std::int32_t reserved20_ = 0;          // +0x20
    [[maybe_unused]] std::int32_t reserved24_ = 0;          // +0x24 (never initialised by the original)
    IS4ViewUtilities* viewUtilities_ = nullptr;        // +0x28
    [[maybe_unused]] void* rendererObject30_ = nullptr;     // +0x30 (cSC43DRender +0x20; unused here)
    [[maybe_unused]] void* rendererObject38_ = nullptr;     // +0x38 (cSC43DRender +0x28; unused here)
    gd::cIGZGDriver* driver_ = nullptr;    // +0x40
    [[maybe_unused]] void* renderer_ = nullptr;             // +0x48 cSC43DRender* (unused here)
    sc4::cSC4RenderProperties* renderProperties_ = nullptr;   // +0x50
    s3d::cS3DCamera* camera_ = nullptr;    // +0x58
    sc4::cSC4CameraControl* cameraControl_ = nullptr;   // +0x60 (unused here)
    IS4LightingManager* lightingManager_ = nullptr;     // +0x68
    [[maybe_unused]] void* reserved70_ = nullptr;           // +0x70
    [[maybe_unused]] void* reserved78_ = nullptr;           // +0x78
    [[maybe_unused]] std::int32_t pass_ = 0;                // +0x80 (terrain 0 / water 1)
    [[maybe_unused]] std::int32_t reserved84_ = 0;          // +0x84
    std::vector<cS3DVector4> highlightColors_;   // +0x88
    [[maybe_unused]] std::vector<std::uint8_t> rendererScratchA0_;   // +0xa0 (the renderer's; not touched here)
    [[maybe_unused]] std::vector<std::uint8_t> rendererScratchB8_;   // +0xb8
    std::uint8_t deviceCap29_ = 0;         // +0xd0
    std::uint8_t deviceCap2a_ = 0;         // +0xd1
    [[maybe_unused]] std::uint16_t reservedD2_ = 0;         // +0xd2
    std::int32_t maxTextureStages_ = 1;    // +0xd4
    std::uint32_t stateWord_ = 0;          // +0xd8
    [[maybe_unused]] std::uint32_t reservedDc_ = 0;         // +0xdc
    cSC4PackedRenderState defaultState_;   // +0xe0
    bool lighting_ = false;                // +0xe8
    float ambientLight_[3] = {1.0f, 1.0f, 1.0f};   // +0xec
    bool fog_ = false;                     // +0xf8
    float fogColor_[3] = {0, 0, 0};        // +0xfc
    std::uint32_t reserved108_ = 0;        // +0x108
    float fogStart_ = 3.4028235e38f;       // +0x10c
    float fogEnd_ = 3.4028235e38f;         // +0x110
    std::int32_t depthOffset_ = 0;         // +0x114
    gd::cGDCombiner defaultCombiner_ = kSC4CtorTexCombiner;         // +0x118
    cSC4PackedRenderState highlightState_;  // +0x124
    gd::cGDCombiner highlightCombiner_ = kSC4CtorTexCombiner;       // +0x12c
    std::vector<cStageInfo> stages_;       // +0x138
    std::int32_t maxStages_ = 1;           // +0x150
    [[maybe_unused]] bool reserved154_ = true;              // +0x154
    cS3DTransform reserved158_;            // +0x158 (zeroed by the original, not used here)
    [[maybe_unused]] std::vector<std::uint8_t> rendererScratch190_;   // +0x190
    bool modelViewValid_ = false;          // +0x1a8
    cS3DTransform modelTransform_;         // +0x1ac
    cS3DTransform viewTransform_;          // +0x1e4
    float modelViewMatrix_[16] = {};       // +0x21c row-major, translation in the last column
    std::vector<cSC4PackedRenderState> stateStack_;   // +0x260
    bool shadeOverride_ = false;           // +0x278
    const std::uint16_t* indexBuffer_ = nullptr;   // +0x280
};
