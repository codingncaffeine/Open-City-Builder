// SC4TrainJunctionView (Mac i386 0x004b3826..0x004b7d54, 0x4c bytes): the U-Drive-It tool's view object (renderer layer 5,
// priority 1000) that marks every train junction in view. Per junction cell and path type it draws one arrow per path the
// junction can switch to (12 units long from the path's midpoint, textured straight / left / right by how the path turns
// against the cell's entry), the switched-to path's arrow brighter, then the junction lever model (0x8bb70000, or
// 0x8bb80000 on the cell last clicked) above the switched path's midpoint. The cell under the pointer and the cell last
// clicked draw stronger. Each arrow is lifted 0.01 above the previous one so overlapping arrows do not fight.
//
// Original layout: cSC4ViewObject3D (+0, +4 vtables, +8 count), +0xc initialised, +0x10 cSC4PackedRenderState,
// +0x18 the arrows' running lift (0.1 per Update), +0x1c / +0x20 the highlighted (pointer) cell, +0x24 / +0x28 the mouse-down
// (clicked) cell, +0x2c..+0x40 six cS3DTextureBinding* {0x7ab50e44, 0x1abe787d, 0x6be09921..0x6be09926}, +0x44 / +0x48 the
// two lever models. kfCellVectors (0x00c19620) is the 4 x 4 table of edge-midpoint to edge-midpoint vectors Init fills.
#pragma once
#include <cstdint>
#include <vector>

#include "sc4/PackedRenderState.h"
#include "sc4/PylonOccupant.h"   // IPylonTextureBinding
#include "sc4/Render3D.h"

class SC4DrawContext;
struct cGZPersistResourceKey;
namespace s3d { class cIS3DModelInstanceAnimated; }

namespace sc4 {

class cISC4PathInfo;
class ITrafficNetworkMapOccupant;

class ITrainJunctionViewHost {
public:
    virtual ~ITrainJunctionViewHost() = default;
    // SL::spRenderer: presence, and vslot 0x28 GetDrawContext (Init calls it and drops the result).
    virtual bool RendererPresent() = 0;
    virtual void RendererGetDrawContext() = 0;
    // cIS3DTextureBindingFactory vslot 0x10 with {0x7ab50e44, 0x1abe787d, instance}: one reference (null when missing).
    virtual IPylonTextureBinding* CreateTextureBinding(std::uint32_t instance) = 0;
    // GetAnimatedModelFromResourceKey (0x00139f80): one reference in *out.
    virtual bool GetAnimatedModelFromResourceKey(const cGZPersistResourceKey& key, s3d::cIS3DModelInstanceAnimated** out) = 0;
    // SL::spCity vslot 0xa0 GetTrafficNetwork: false without one; else its GetVisibleCellRect (x0, z0, x1, z1).
    virtual bool VisibleCellRect(int rect[4]) = 0;
    // The traffic network map's GetTileInfoAtCell(x, z) chain (+0x88 next): each tile's network occupant (borrowed).
    virtual void CellOccupants(int cellX, int cellZ, std::vector<ITrafficNetworkMapOccupant*>& out) = 0;
    // The map's GetTrainJunctionKeys(occupant, pathType, keys, current): how many keys (0 = no junction of that type).
    virtual std::size_t TrainJunctionKeys(ITrafficNetworkMapOccupant* occupant, std::uint8_t pathType, std::vector<std::uint32_t>& keys,
                                          std::uint16_t& current) = 0;
    // The network occupant's path info (cISC4NetworkOccupant vslot 0xc4; borrowed), or null.
    virtual const cISC4PathInfo* PathInfo(ITrafficNetworkMapOccupant* occupant) = 0;
    // SL::spTerrain vslot 0x30 GetAltitude(x, z).
    virtual float TerrainAltitude(float x, float z) = 0;
};
ITrainJunctionViewHost* TrainJunctionViewHost();
void SetTrainJunctionViewHost(ITrainJunctionViewHost* host);

struct TrainJunctionVertex {   // sVF_V3F_T2F
    float x = 0, y = 0, z = 0;
    float u = 0, v = 0;
};

class SC4TrainJunctionView final : public cISC4ViewObject3D {
public:
    SC4TrainJunctionView() = default;                         // @ 0x004b7292
    ~SC4TrainJunctionView();                                  // @ 0x008022bc

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override;

    bool Init();                                              // @ 0x004b56ec
    void Shutdown();                                          // @ 0x004b3826
    void DrawViewObject(SC4DrawContext& context) override { Update(&context); }
    bool Update(SC4DrawContext* context);                     // @ 0x004b7d54
    void SetHighlightCell(int x, int z) { highlightX_ = x; highlightZ_ = z; }   // inlined
    void SetMouseDownCell(int x, int z) { mouseDownX_ = x; mouseDownZ_ = z; }   // inlined

    void DrawTrainJunction(SC4DrawContext* context, int cellX, int cellZ, ITrafficNetworkMapOccupant* occupant, std::size_t count,
                           const std::vector<std::uint32_t>& keys, std::uint16_t current);   // @ 0x004b780a
    // The arrow's quad from the path's midpoint along the cell vector [from][to] (12 units, 12 wide), on the terrain or the
    // midpoint's height, whichever is higher, plus the running lift (+3 on monorail paths, type 7).
    void GetArrowBitmapQuad(TrainJunctionVertex* quad, const cS3DVector3& midpoint, std::uint8_t from, std::uint8_t to,
                            const cISC4PathInfo* pathInfo, const std::vector<cS3DVector3>* path, std::uint32_t key);   // @ 0x004b4130
    // The arrow texture for a path: straight, left or right by the up component of cross(cell vector, path end direction).
    IPylonTextureBinding* GetPathBitmap(std::uint8_t from, std::uint8_t to, const cISC4PathInfo* pathInfo,
                                        const std::vector<cS3DVector3>* path, bool selected) const;   // @ 0x004b5502

    static const cS3DVector3& CellVector(int from, int to);   // kfCellVectors

    // accessors for the tests
    bool Initialized() const { return initialized_; }
    float Lift() const { return lift_; }
    IPylonTextureBinding* Binding(int slot) const { return bindings_[slot]; }
    s3d::cIS3DModelInstanceAnimated* LeverModel(bool clicked) const { return clicked ? leverClicked_ : lever_; }

private:
    std::uint32_t refs_ = 0;
    bool initialized_ = false;                                // +0x0c
    cSC4PackedRenderState state_{0, 0};                       // +0x10
    float lift_ = 0.0f;                                       // +0x18
    int highlightX_ = -1, highlightZ_ = -1;                   // +0x1c / +0x20
    int mouseDownX_ = -1, mouseDownZ_ = -1;                   // +0x24 / +0x28
    IPylonTextureBinding* bindings_[6] = {};                  // +0x2c..+0x40 (owned)
    s3d::cIS3DModelInstanceAnimated* lever_ = nullptr;        // +0x44 (counted)
    s3d::cIS3DModelInstanceAnimated* leverClicked_ = nullptr; // +0x48 (counted)
};

}  // namespace sc4
