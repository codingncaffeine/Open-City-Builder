// cSC4CellTextureModel (0x1000b5146..0x1000b5f5c, 0x100 bytes, iid 0xa9e2831d): the model a lot base texture
// occupant shows for one of its cells - a single textured quad on the cell, drawn in the static view. It is a
// cS3DModelInstanceBasic (no meshes or materials of its own): Draw sets the render state itself.
//
// Original layout (from +0x3c): +0x3c initialised, +0x40 the specification (LotBaseTextureSpecification: texture id,
// cell x / z at +0x44 / +0x45, rotation / flip at +0x46 / +0x47, RGBA at +0x48..+0x4b, combine mode +0x4c, frame +0x4d),
// +0x50 texture has alpha, +0x51 the zoom the textures were loaded for (0xff = none), +0x52 the view rotation,
// +0x58 cS3DTextureBinding*, +0x60 / +0x6c world bounds min / max, +0x78 four V3F_C4UB_T2F vertices (0x18 each, a
// strip over (x0, z0), (x1, z0), (x0, z1), (x1, z1)), +0xd8 "on a locked lot" (the quad is flat at the lot's altitude),
// +0xdc the four uv pairs again for the terrain cell draw.
//
// Seams: the lot manager / lot developer / terrain / texture binding factory (ICellTextureModelHost). The lighting
// manager colouring of the locked-lot quad (SetupVertices' second half) is left out: that quad takes the terrain shade.
//
// Port notes:
//  * The unlocked path draws through the draw context's terrain cell helper (+0x78 vslot 0x78) in the original, which
//    is not recovered; the port draws the same cell as its quad over the four terrain vertex altitudes.
//  * The combine path (a combine mode other than 0xff) tints with the spec colour through the lot texture combiner
//    (0x21020000 with the mode in the first byte, then 0x0001 / 0x03); the light factor the original multiplies the
//    colour by (the draw context's lighting, +0x68 vslot 0x50) is taken as white, and the mode-3 fallback for devices
//    without that combine (render properties +0x778: mode 1 at half alpha) is not ported.
#pragma once
#include <cstdint>

#include "s3d/ModelInstance.h"
#include "sc4/LotBaseTextureOccupant.h"

namespace sc4 {

// cS3DTextureBinding as this model reads it: +0 the frame texture ids, +8 the frame count.
struct CellTextureBinding {
    const std::uint32_t* frames = nullptr;
    std::uint32_t frameCount = 0;
    bool hasAlpha = false;   // the binding factory's texture info (vslot 0x40) flag
};

class ICellTextureModelHost {
public:
    virtual ~ICellTextureModelHost() = default;
    // SL::spLotDeveloper vslot 0x170 false and SL::spLotManager vslot 0x28 (x, z, 0) gives a lot whose vslot 0x60 bit 0
    // (vertices locked) is set: true with the lot's altitude (vslot 0x80).
    virtual bool LockedLotAltitude(int cellX, int cellZ, float& altitude) = 0;
    // SL::spTerrain vslot 0x88: the four corner altitudes of a cell, (x, z), (x + 1, z), (x, z + 1), (x + 1, z + 1).
    virtual void CellCornerAltitudes(int cellX, int cellZ, float out[4]) = 0;
    // The terrain's lit vertex colour at the same four corners (what the terrain cell draw the unlocked path uses in the
    // original modulates the texture with), 0..255 grey.
    virtual void CellCornerShades(int cellX, int cellZ, std::uint8_t out[4]) = 0;
    // The terrain's triangulation of the cell (vertex flags & 0x3000): the terrain cell draw splits the quad along the
    // same diagonal as the ground (cSTETerrainView3D::DrawTexturedTerrainQuad). False: the (x, z)-(x + 1, z + 1) diagonal.
    virtual bool CellTriangulationFlipped(int cellX, int cellZ) { (void)cellX; (void)cellZ; return false; }
    // SL::spTextureBindingFactory vslot 0x20 with {0x7ab50e44, 0x0986135e, instance}; null when missing. Not owned.
    virtual const CellTextureBinding* TextureBinding(std::uint32_t instance) = 0;
};
ICellTextureModelHost* CellTextureModelHost();
void SetCellTextureModelHost(ICellTextureModelHost* host);

class cSC4CellTextureModel final : public s3d::cS3DModelInstanceBasic, public ILotCellTextureModel {
public:
    cSC4CellTextureModel() = default;   // @ 0x1000b5146
    ~cSC4CellTextureModel() override = default;

    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x1000b5408
    std::uint32_t AddRef() override { return s3d::cS3DModelInstanceBasic::AddRef(); }     // @ 0x1000b5f26
    std::uint32_t Release() override { return s3d::cS3DModelInstanceBasic::Release(); }   // @ 0x1000b5f34

    void Init(const LotBaseTextureSpecification& spec);             // @ 0x1000b5252

    // cIS3DModelInstance
    void GetWorldBounds(cS3DBoundingBox& out) override;             // @ 0x1000b5482
    bool Pick(cSC4PickContext*) override { return false; }          // @ 0x1000b54dc
    int Draw(SC4DrawContext* context, bool highlight) override;     // @ 0x1000b54f4
    void GetSortValue(std::uint32_t& out) const override;           // @ 0x1000b57f8
    void SetDrawAttributes(const s3d::SC4DrawAttributes&) override {}   // @ 0x1000b5f42

    // ILotCellTextureModel
    void UpdateTextures(int zoom, const LotBaseTextureSpecification& spec) override;   // @ 0x1000b5968
    void SetupVertices(bool updateVertices, bool updateLighting, void* terrain, void* lighting) override;   // @ 0x1000b5b60
    sc4::cIS3DModelInstance* ModelInstance() override {
        return reinterpret_cast<sc4::cIS3DModelInstance*>(static_cast<s3d::cIS3DModelInstance*>(this));
    }

private:
    struct Vertex {   // V3F_C4UB_T2F
        float x = 0, y = 0, z = 0;
        std::uint8_t rgba[4] = {0, 0, 0, 0};
        float u = 0, v = 0;
    };
    static_assert(sizeof(Vertex) == 0x18, "vertex stride");

    void DoZoomChange(int zoom);                                    // @ 0x1000b579e
    bool LoadTextures(int zoom);                                    // @ 0x1000b5994
    void UpdateTextureRotation();                                   // @ 0x1000b5a7e

    bool initialized_ = false;                       // +0x3c
    LotBaseTextureSpecification spec_{};             // +0x40 (the constructor sets combine mode / frame to 0xff)
    bool textureHasAlpha_ = false;                   // +0x50
    std::uint8_t loadedZoom_ = 0xff;                 // +0x51
    std::uint8_t viewRotation_ = 0xff;               // +0x52
    const CellTextureBinding* binding_ = nullptr;    // +0x58
    float boundsMin_[3] = {1, 1, 1};                 // +0x60
    float boundsMax_[3] = {0, 0, 0};                 // +0x6c
    Vertex vertices_[4];                             // +0x78
    bool onLockedLot_ = false;                       // +0xd8
    float cellUVs_[8] = {};                          // +0xdc
};

}  // namespace sc4
