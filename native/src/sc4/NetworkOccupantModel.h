// The drawable half of cSC4NetworkOccupant: in the original the occupant *is* a cS3DModelInstanceBasic (its +0x68
// subobject) and GetModelInstances (0x10019eb90) hands that subobject to the model maker; Draw (0x10019e09a) paints the
// tile's quad (an under texture, then the piece texture at the occupant's variation) and its add-on triangles, with
// GetTexture (0x10019d976) resolving the network textures {0x7ab50e44, 0x1abe787d, id + zoom}. The port's occupant is not
// a model instance, so cSC4NetworkOccupantModel stands in for that subobject: a model instance that draws its occupant.
//
// Not ported: the highlight pass (SetSelfLitShade + the highlight state, and the one-way arrow's highlighted draw), the
// lighting refresh (UpdateLighting, flag 0x8000: the saved vertex colours are used as they are) and the add-on parts'
// "under water" test against the terrain.
#pragma once
#include <cstdint>

#include "s3d/ModelInstance.h"
#include "sc4/NetworkOccupant.h"

struct cSC4PackedRenderState;

namespace sc4 {

// SL::spTextureBindingFactory as GetTexture uses it: the driver texture id of frame 0 of {0x7ab50e44, 0x1abe787d,
// instance}, or 0 when the texture does not exist.
class INetworkModelHost {
public:
    virtual ~INetworkModelHost() = default;
    virtual std::uint32_t NetworkTexture(std::uint32_t instance) = 0;
};
INetworkModelHost* NetworkModelHost();
void SetNetworkModelHost(INetworkModelHost* host);

class cSC4NetworkOccupantModel final : public s3d::cS3DModelInstanceBasic {
public:
    explicit cSC4NetworkOccupantModel(cSC4NetworkOccupant& occupant);

    void GetWorldBounds(cS3DBoundingBox& out) override;          // 0x10019ef44
    void GetTransform(cS3DTransform& out) override;
    int Draw(SC4DrawContext* context, bool highlight) override;  // 0x10019e09a
    void GetSortValue(std::uint32_t& out) const override;        // 0x10019ed38
    // The occupant's translucency (flag 2 and the alpha) over this model's own attributes: in the original they are one
    // record, the occupant's model instance.
    s3d::SC4DrawAttributes GetDrawAttributes() const override {
        s3d::SC4DrawAttributes attributes = cS3DModelInstanceBasic::GetDrawAttributes();
        const s3d::SC4DrawAttributes& own = occupant_.ModelDrawAttributes();
        attributes.flags = (attributes.flags & ~2u) | (own.flags & 2u);
        attributes.shade[3] = own.shade[3];
        return attributes;
    }

    // GetTexture (0x10019d976): the texture for `id` (plus the zoom when its low nibble is free), else `fallback`.
    static std::uint32_t GetTexture(std::uint32_t id, std::uint32_t fallback, int zoom);

private:
    // DrawOneWayArrow (i386 0x0013270a): the arrow decal over a one-way piece.
    void DrawOneWayArrow(SC4DrawContext* context, const cSC4PackedRenderState& state, std::uint8_t directions, int zoom);

    cSC4NetworkOccupant& occupant_;   // not referenced: the occupant's placeable object owns this model
};

}  // namespace sc4
