// cSC4NetworkPreview — the drag preview of the network tools: per cell either a coloured quad (AddUntexturedSelection) or
// the solved piece's texture on a tinted quad (AddTexturedSelection), lying on the terrain (or on the water surface, or
// raised for the elevated networks). The renderer draws it as a view object in two passes: the visible part, then the
// hidden part at 20% opacity.
//
// Recovered from the Mac i386 build with symbols (0x004ca76c..0x004cb3a0, vtables 0x00bebc08 / 0x00bebc2c). i386 layout:
// +0x0c the network occupant filter, +0x10 multimap<cell key, cUntexturedCell>, +0x28 multimap<cell key, cTexturedCell>,
// +0x40/+0x48 the untextured render states (visible / hidden pass), +0x50/+0x58 the textured ones, +0x60 the texture
// combiner, +0x6c inited. A cell (cBaseCell, 0x68 bytes) is four V3F_C4UB_T2F vertices, the corner each vertex stands
// on (+0x60) and the altitude added to every corner (+0x64); a textured cell adds its texture binding.
#pragma once
#include <cstdint>
#include <map>

#include "sc4/DrawContext.h"
#include "sc4/NetworkTool.h"
#include "sc4/PackedRenderState.h"
#include "sc4/Render3D.h"

namespace sc4 {

// Everything outside the object (SL::spTerrain, SL::spTextureBindingFactory).
class INetworkPreviewHost {
public:
    virtual ~INetworkPreviewHost() = default;
    virtual float VertexAltitude(int x, int z) = 0;          // spTerrain vslot 0x38
    virtual float WaterDepth(int x, int z) = 0;              // spTerrain vslot 0x84
    virtual bool WaterShown() = 0;                           // spTerrain vslot 0x14c
    virtual bool TriangulationFlipped(int x, int z) = 0;     // spTerrain vslot 0x118
    // spTextureBindingFactory vslot 0x10: the texture of a resource key (0 = none).
    virtual int Texture(const cGZPersistResourceKey& key) = 0;
};
void SetNetworkPreviewHost(INetworkPreviewHost* host);

class cSC4NetworkPreview final : public INetworkPreview, public cISC4ViewObject3D {
public:
    cSC4NetworkPreview();                                                                   // @ 0x004cb2ec

    // INetworkPreview
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    void Init() override;                                                                    // @ 0x004ca97c
    void Shutdown() override;                                                                // @ 0x004ca76c
    void Clear() override;                                                                   // @ 0x004cafb4
    void Erase(const SC4PointU& cell) override;                                              // @ 0x004cb278
    void AddUntexturedSelection(const SC4PointU& cell, std::uint32_t color, const float* heights, float elevation,
                                bool onTerrainOnly) override;                                // @ 0x004cb068
    void AdjustCellHeights(const SC4PointU& cell, const float* heights, bool onTerrainOnly) override;   // @ 0x004ca8a6
    // @ 0x004cb102: falls back to the untextured quad when the texture does not exist.
    void AddTexturedSelection(const SC4PointU& cell, std::uint32_t color, const float* heights, const cGZPersistResourceKey& texture,
                              int rotation, bool flip, float elevation, bool onTerrainOnly);

    // cISC4ViewObject3D (Update @ 0x004cadd6 -> DrawCells @ 0x004cac08)
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    void DrawViewObject(SC4DrawContext& context) override;

    std::size_t CellCount() const { return untextured_.size() + textured_.size(); }

private:
    struct Vertex {   // V3F_C4UB_T2F
        float x, y, z;
        std::uint8_t bgra[4];
        float u, v;
    };
    struct Cell {     // cBaseCell (+ the texture of a cTexturedCell)
        Vertex vertices[4];
        std::uint8_t corners[4] = {};
        float elevation = 0.0f;
        int texture = 0;
    };
    void SetVertexPositions(Cell& cell, const SC4PointU& at, std::uint32_t color, const float* heights, int rotation,
                            bool flip, float elevation, bool onTerrainOnly);                // @ 0x004cae48
    void SetVertexHeights(Cell& cell, const SC4PointU& at, const float* heights, bool onTerrainOnly);   // @ 0x004ca79a

    std::uint32_t refCount_ = 0;
    std::multimap<std::uint32_t, Cell> untextured_;     // +0x10
    std::multimap<std::uint32_t, Cell> textured_;       // +0x28
    cSC4PackedRenderState states_[4];                   // +0x40 untextured visible, +0x48 hidden, +0x50 / +0x58 textured
    gd::cGDCombiner combiner_{0x01030002u, 0x01030001u, 0u};   // +0x60 (Init)
    bool inited_ = false;                               // +0x6c
};

}  // namespace sc4
