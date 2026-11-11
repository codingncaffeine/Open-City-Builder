#include "sc4/NetworkOccupantModel.h"

#include "sc4/DrawContext.h"
#include "sc4/PackedRenderState.h"

namespace sc4 {
namespace {
INetworkModelHost* g_host = nullptr;

// sTriangleIndices / sFlippedTriangleIndices (0x100a19f10 / 0x100a19f1c, read from the binary): the two
// triangulations of the perimeter-ordered corners.
const std::uint16_t kTriangleIndices[6] = {0, 1, 2, 0, 2, 3};
const std::uint16_t kFlippedTriangleIndices[6] = {0, 1, 3, 3, 1, 2};

struct States {
    cSC4PackedRenderState blend, noBlend;
    States() {
        // cSC4NetworkOccupant::StaticInit's two states
        blend.PackRenderState(true, true, true, true, true, 1, true, 3, 4, 5, 4, 0.1f, 1, -1);
        noBlend.PackRenderState(true, true, true, false, false, 1, true, 3, -1, -1, -1, -1.0f, 1, -1);
    }
};
const States& PackedStates() {
    static const States states;
    return states;
}

// The combined edge configurations whose pieces draw blended (Draw's switch at 0x10019e115).
bool EdgeConfigBlends(std::uint32_t config) {
    switch (config) {
        case 1: case 3: case 0x100: case 0x300: case 0x301: case 0x10000: case 0x30000: case 0x30100:
        case 0x1000000: case 0x1000003: case 0x3000000: case 0x3010000:
            return true;
        default:
            return false;
    }
}
}  // namespace

INetworkModelHost* NetworkModelHost() { return g_host; }
void SetNetworkModelHost(INetworkModelHost* host) { g_host = host; }

cSC4NetworkOccupantModel::cSC4NetworkOccupantModel(cSC4NetworkOccupant& occupant) : occupant_(occupant) {
    SetOwner(static_cast<cISC4Occupant*>(&occupant));
}

void cSC4NetworkOccupantModel::GetWorldBounds(cS3DBoundingBox& out) { occupant_.GetBoundingBox(out.min, out.max); }

void cSC4NetworkOccupantModel::GetTransform(cS3DTransform& out) {
    // GetModelInstances puts the tile's position in the transform; the quad itself is in world coordinates.
    cS3DModelInstanceBasic::GetTransform(out);
    float position[3];
    occupant_.GetPosition(position);
    for (int i = 0; i < 3; ++i) out.translation[i] = position[i];
}

void cSC4NetworkOccupantModel::GetSortValue(std::uint32_t& out) const {
    out = GetTexture(occupant_.PieceId() + occupant_.GetVariation() * 0x10u, 0, 0);
}

std::uint32_t cSC4NetworkOccupantModel::GetTexture(std::uint32_t id, std::uint32_t fallback, int zoom) {
    if (!g_host) return 0;
    const std::uint32_t texture = g_host->NetworkTexture(id + ((id & 0xf) == 0 ? static_cast<std::uint32_t>(zoom) : 0u));
    if (texture || fallback == id || fallback == 0) return texture;
    return g_host->NetworkTexture(fallback + ((fallback & 0xf) == 0 ? static_cast<std::uint32_t>(zoom) : 0u));
}

int cSC4NetworkOccupantModel::Draw(SC4DrawContext* context, bool highlight) {
    if (!context || highlight) return 1;
    const int zoom = context->Zoom();
    // Pieces with a support pylon and an opaque first corner, and every piece with flag 0x80000, choose their state
    // from the edge configuration; the rest draw blended.
    const cSC4PackedRenderState* state = &PackedStates().blend;
    const bool byEdges = occupant_.HasNetworkFlag(0x80000) ||
                         (occupant_.HasNetworkFlag(kNetworkFlagSupportPylon) && occupant_.Corner(0).color[3] == 0xff);
    if (byEdges && !EdgeConfigBlends(occupant_.GetEdgeStore().CombinedConfig())) state = &PackedStates().noBlend;
    context->SetRenderState(*state);
    context->ResetModelViewTransform();
    context->ResetShade();
    if (occupant_.HasAnyNetworkFlag(kNetworkFlagQuadDrawA | kNetworkFlagQuadDrawB)) {
        std::uint32_t textures[2] = {0, 0};
        // ([0] the under texture, [1] the piece texture at the variation, falling back to the plain piece when the
        // variation has no texture; i386 0x00133da8 -> GetTexture 0x00135342. The under texture has no fallback.)
        if (const std::uint32_t under = occupant_.UnderTexture()) textures[0] = GetTexture(under, 0, zoom);
        textures[1] = GetTexture(occupant_.GetVariation() * 0x10u + occupant_.PieceId(), occupant_.PieceId(), zoom);
        if (textures[1]) {
            context->SetTexWrapModes(2, 2, 0);
            context->SetTexFiltering(1, 1, 0);
            context->SetVertexBuffer(10, 4, &occupant_.Corner(0));
            const std::uint16_t* indices =
                occupant_.HasNetworkFlag(0x1000000) ? kFlippedTriangleIndices : kTriangleIndices;
            for (std::uint32_t texture : textures) {
                if (!texture) continue;
                context->SetTexture(static_cast<int>(texture), 0);
                if (occupant_.HasNetworkFlag(kNetworkFlagQuadDrawA)) context->DrawPrimsIndexed(0, 3, indices);
                if (occupant_.HasNetworkFlag(kNetworkFlagQuadDrawB)) context->DrawPrimsIndexed(0, 3, indices + 3);
            }
        }
    }
    const std::vector<SC4AddOnPart>& parts = occupant_.AddOnParts();
    if (!parts.empty()) {
        context->SetTexWrapModes(3, 3, 0);
        context->SetTexFiltering(1, 1, 0);
        for (const SC4AddOnPart& part : parts) {
            const std::uint32_t texture = GetTexture(part.id, 0, zoom);
            if (!texture) continue;
            context->SetTexture(static_cast<int>(texture), 0);
            context->DrawPrims(0, 10, 3, part.corner);
        }
    }
    return 1;
}

}  // namespace sc4
