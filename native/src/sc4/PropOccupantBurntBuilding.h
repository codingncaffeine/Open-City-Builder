// cSC4PropOccupantBurntBuilding — the charred floor a building leaves when its lot burns down (ExplodeLot, lot-developer.md 7.2):
// a prop occupant that is also a model instance of its own, drawing one textured quad over the old building's footprint.
// Class 0xc9efb016, Mac i386 0x00102c24..0x001062d2; record lot-records.md 10.6.
//
// Original layout: +0x00 cSC4PropOccupant (its occupant at +0x08), +0x68 cS3DModelInstanceBasic (draw attributes' shade at
// +0x80), +0x90 five texture bindings (one per zoom, all of {0x7ab50e44, 0x1abe787d, 0xa9f0e710}), +0x9c the source occupant's
// bounds (saved), +0xb4 the four floor vertices (format 2, 0x14 bytes each, 0.1 above the box floor), +0xb8 the packed render
// state, +0xc0 the texture origin (the box minimum / 16).
//
// The view's flag 2 inserts it in the renderer (or takes it out); Draw lays the zoom's texture with a texture matrix of scale
// 1/16 and the origin projected to view coordinates, the construction floor's way (cSC4ConstructionOccupant).
#pragma once
#include <array>
#include <cstdint>

#include "s3d/ModelInstance.h"
#include "sc4/PackedRenderState.h"
#include "sc4/PropOccupant.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4PropOccupantBurntBuilding = 0xc9efb016;
constexpr std::uint16_t kBurntBuildingRecordVersion = 3;
constexpr std::uint32_t kBurntBuildingTextureInstance = 0xa9f0e710;   // type 0x7ab50e44, group 0x1abe787d

// What the burnt building reaches outside itself: the texture binding factory (cIS3DTextureBindingFactory slot 0x10: the
// binding's texture id), SL::spViewUtilities ProjectPointToViewCoordinates and SL::spRenderer vslots 0x6c / 0x70 / 0x74.
class IBurntBuildingHost {
public:
    virtual ~IBurntBuildingHost() = default;
    virtual std::int32_t TextureId(std::uint32_t type, std::uint32_t group, std::uint32_t instance) = 0;
    virtual void ProjectToViewCoordinates(float point[3]) = 0;
    virtual bool IsModelInserted(s3d::cIS3DModelInstance* model) = 0;
    virtual bool InsertModel(s3d::cIS3DModelInstance* model) = 0;
    virtual bool RemoveModel(s3d::cIS3DModelInstance* model) = 0;
};
void SetBurntBuildingHost(IBurntBuildingHost* host);

class cSC4PropOccupantBurntBuilding final : public cSC4PropOccupant, public s3d::cS3DModelInstanceBasic {
public:
    struct FloorVertex { float x = 0, y = 0, z = 0, u = 0, v = 0; };   // format 2 (0x14 bytes)

    cSC4PropOccupantBurntBuilding();                                  // @ 0x00105d76
    ~cSC4PropOccupantBurntBuilding() override;                        // @ 0x001062d2

    bool QueryInterface(std::uint32_t iid, void** out) override;      // @ 0x00102c40 (the prop's)
    std::uint32_t AddRef() override { return cSC4PropOccupant::AddRef(); }      // @ 0x00102c24
    std::uint32_t Release() override { return cSC4PropOccupant::Release(); }    // @ 0x00102c32

    // @ 0x001031b2: the prop's box becomes the occupant's, kept as the source bounds; the floor is rebuilt.
    void SetBoundsFromOccupant(cISC4Occupant* occupant);
    void Rebuild();                                                    // @ 0x00102fc8

    // cSC4PropOccupant / cSC4Occupant
    bool Init() override;                                              // @ 0x00105660
    bool Shutdown() override;                                          // @ 0x0010412e
    bool Write(cIGZOStream& stream) const override;                    // @ 0x00105010
    bool Read(cIGZIStream& stream) override;                           // @ 0x00105452
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4PropOccupantBurntBuilding; }   // @ 0x0065e622
    void SetAllFlags(std::uint32_t flags) override;                    // @ 0x00102c64

    // cIS3DModelInstance
    cIGZUnknown* GetOwner() override { return static_cast<cISC4Occupant*>(this); }   // @ 0x0065e640: the occupant
    void SetOwner(cIGZUnknown*) override {}                            // @ 0x0065e656
    void GetWorldBounds(cS3DBoundingBox& out) override;                // @ 0x0010321c
    void GetSortValue(std::uint32_t& out) const override;              // @ 0x00103dfe
    int Draw(SC4DrawContext* context, bool highlight) override;        // @ 0x00103258

    // test / report access
    bool HasFloor() const { return hasFloor_; }
    const std::array<FloorVertex, 4>& Floor() const { return floor_; }
    const float* SourceMin() const { return sourceMin_; }
    const float* SourceMax() const { return sourceMax_; }
    std::int32_t Texture(int zoom) const { return zoom >= 0 && zoom < 5 && hasTextures_ ? textures_[std::size_t(zoom)] : 0; }

private:
    std::array<std::int32_t, 5> textures_{};   // +0x90
    bool hasTextures_ = false;
    float sourceMin_[3] = {}, sourceMax_[3] = {};   // +0x9c
    std::array<FloorVertex, 4> floor_{};       // +0xb4
    bool hasFloor_ = false;
    cSC4PackedRenderState renderState_;        // +0xb8
    float texOrigin_[3] = {};                  // +0xc0
};

}  // namespace sc4
