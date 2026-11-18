// cSC4FoundationModel — the model of a foundation occupant: the four walls of the box a building or lot stands on, from its
// top down to below the lowest ground around it, and its top face (Mac i386 0x0028a1e8..0x0028b0a4, 18 functions, object
// 0x26c bytes). cSC4ModelMaker::LoadFoundationModels builds one per foundation occupant whose foundation is not drawn with
// building-style models, hands it the foundation exemplar's side and top texture ids (five per zoom level) and texture world
// sizes, the occupant's box (its top raised by 0.2) and lets it reach down to the terrain.
//
// Object layout (i386): +0x08 cS3DModelInstanceBasic, +0x28 built flag, +0x2c cS3DBoundingBox (min x, bottom, min z; max x, top,
// max z), +0x44 s8 zoom level and +0x45 s8 view rotation (0xff = none yet), +0x46 u8[5] zoom levels seen, +0x4c / +0x50 the two
// visible edges (0 / 1 until the first view), +0x54 side texture world size (16), +0x58 u32[5] side texture ids, +0x6c side
// texture binding, +0x70 sVF_V3F_C4UB_T2F[16] the four side quads (one per lot edge, 0x60 bytes each), +0x1f0 top texture world
// size (16), +0x1f4 u32[5] top texture ids, +0x208 top binding, +0x20c sVF_V3F_C4UB_T2F[4] the top quad.
//
// Quirks kept:
//  * The u coordinate of every side quad is the box width / 16 (also on the two edges that run along z), and the top quad's
//    uv uses the side texture world size (+0x54); the top's own size (+0x1f0) is stored but never read.
//  * SetupTextures retries a side id that does not bind as 0x69f20000 + zoom and a top id as 0x0912220f + zoom (and keeps it).
//  * AdjustHeightToTerrain only moves the bottom down (to whole cells below the top), never up, and with `force` false leaves
//    the vertices alone when nothing moved.
//  * GetSortValue answers 0xe9862ab6 without a binding, else the side (or top) texture id, ORed with 0x2ab6.
//  * Draw draws the sides only once the current zoom level has been seen (DoZoomChange marks it) and only the two visible edges.
#pragma once
#include <cstdint>

#include "gz/StreamOps.h"
#include "s3d/ModelInstance.h"
#include "sc4/PackedRenderState.h"

class SC4DrawContext;

namespace sc4 {

// cS3DTextureBinding as the model reads it: the texture id, and Release.
class IFoundationTextureBinding {
public:
    virtual ~IFoundationTextureBinding() = default;
    virtual std::uint32_t TextureId() const = 0;
    virtual void Release() = 0;
};

// The engine services of the original, one method per call.
class IFoundationModelHost {
public:
    virtual ~IFoundationModelHost() = default;
    // cS3DTextureBindingFactory::GetBinding(instance) (i386 0x00240c7a): the texture {0x7ab50e44, 0x1abe787d, instance}; null
    // when it is missing. One reference.
    virtual IFoundationTextureBinding* GetBinding(std::uint32_t instance) = 0;
    // SC4ViewUtilities::GetVisibleEdges (the draw context's view utilities): the two lot edges that face the camera.
    virtual void VisibleEdges(int& edgeA, int& edgeB) = 0;
    // cISC4LightingManager vslot 0x5c (i386): the light colour for a surface facing `normal`.
    virtual void LightColor(const float normal[3], std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) = 0;
    virtual int TerrainCellCountX() = 0;              // SL::spTerrain vslot 0x1c (i386)
    virtual float TerrainAltitude(int vertex) = 0;    // vslot 0x34 GetAltitude(index)
    virtual float TerrainWaterLevel(int vertex) = 0;  // vslot 0x78 GetWaterLevel(index)
    // SetupPackedRenderState: the original asks SL::spRenderer for its draw context (vslot 0x28) and drops the answer.
    virtual void TouchRenderer() {}
};
IFoundationModelHost* FoundationModelHost();
void SetFoundationModelHost(IFoundationModelHost* host);

struct FoundationVertex {   // sVF_V3F_C4UB_T2F, 0x18 bytes (format 10)
    float x = 0.0f, y = 0.0f, z = 0.0f;
    std::uint8_t color[4] = {0, 0, 0, 0};
    float u = 0.0f, v = 0.0f;
};
static_assert(sizeof(FoundationVertex) == 0x18, "foundation vertex");

class cSC4FoundationModel final : public s3d::cS3DModelInstanceBasic {
public:
    cSC4FoundationModel();                                               // @ 0x0028ae5c
    ~cSC4FoundationModel() override;                                    // @ 0x0028b02e

    void GetWorldBounds(cS3DBoundingBox& out) override { out = bounds_; }   // @ 0x0028b010
    void GetSortValue(std::uint32_t& out) const override;                    // @ 0x0028a306
    int Draw(SC4DrawContext* context, bool highlight) override;             // @ 0x0028acc6

    void SetTextureIDs(const std::uint32_t* side, const std::uint32_t* top);   // @ 0x0028a214 (a null array is skipped)
    void SetTextureWorldSize(float side, float top);                         // @ 0x0028a250
    void SetWorldBounds(const cS3DBoundingBox& bounds) { bounds_ = bounds; }   // @ 0x0028aff2
    bool AdjustHeightToTerrain(bool force);                                  // @ 0x0028aa4c
    void SetupVertices(bool geometry, bool lighting);                        // @ 0x0028a4ae
    void DoZoomChange(int oldZoom, int newZoom);                             // @ 0x0028a446
    void DoRotationChange(int oldRotation, int newRotation);                 // @ 0x0028ac76
    void SetupTextures(int oldZoom, int newZoom);                            // @ 0x0028a336
    static void SetupPackedRenderState();                                    // @ 0x0028a268

    // for tests
    const cS3DBoundingBox& Bounds() const { return bounds_; }
    const FoundationVertex* SideVertices() const { return side_; }
    const FoundationVertex* TopVertices() const { return top_; }
    std::uint32_t SideTextureId(int zoom) const { return sideIds_[zoom]; }
    std::uint32_t TopTextureId(int zoom) const { return topIds_[zoom]; }
    int EdgeA() const { return edgeA_; }
    int EdgeB() const { return edgeB_; }

private:
    bool built_ = true;                                  // +0x28
    cS3DBoundingBox bounds_;                             // +0x2c
    std::int8_t zoom_ = -1, rotation_ = -1;              // +0x44, +0x45
    std::uint8_t zoomSeen_[5] = {0, 0, 0, 0, 0};         // +0x46
    int edgeA_ = 0, edgeB_ = 1;                          // +0x4c, +0x50
    float sideWorldSize_ = 16.0f;                        // +0x54
    std::uint32_t sideIds_[5] = {0, 0, 0, 0, 0};         // +0x58
    IFoundationTextureBinding* sideBinding_ = nullptr;   // +0x6c
    FoundationVertex side_[16];                          // +0x70
    float topWorldSize_ = 16.0f;                         // +0x1f0
    std::uint32_t topIds_[5] = {0, 0, 0, 0, 0};          // +0x1f4
    IFoundationTextureBinding* topBinding_ = nullptr;    // +0x208
    FoundationVertex top_[4];                            // +0x20c
};

}  // namespace sc4
