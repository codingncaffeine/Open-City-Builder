#include "sc4/NetworkOccupantModel.h"
#include "sc4/ViewUtilities.h"
#include "sc4/GeometryUtilities.h"

#include "platform/Unwired.h"
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
    if (highlight) SC4_UNWIRED("cSC4NetworkOccupantModel::Draw: the highlight pass (SetSelfLitShade + highlight state, one-way arrow) is not ported");
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
            // The one-way arrow over the piece (vslot 0x114 GetOneWayDirections, vslot 0x170 DrawOneWayArrow).
            if (const std::uint8_t directions = occupant_.GetOneWayDirections()) DrawOneWayArrow(context, *state, directions, zoom);
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

namespace {
// DrawOneWayArrow(...)::kArrowOrientation (i386 0x00844fe0, read from the binary): per set of edges (bit i = edge i of
// the one-way type), the texture rotation/flip of the arrow for a positive and a negative direction sum; 0xff = no arrow.
constexpr std::uint8_t kArrowOrientation[16][2] = {
    {0xff, 0xff}, {0xff, 0xff}, {0xff, 0xff}, {0x83, 0x02}, {0xff, 0xff}, {0x01, 0x03}, {0x03, 0x82}, {0xff, 0xff},
    {0xff, 0xff}, {0x01, 0x80}, {0x02, 0x00}, {0xff, 0xff}, {0x81, 0x00}, {0xff, 0xff}, {0xff, 0xff}, {0xff, 0xff}};
}  // namespace

// Only a plain straight or curved one-way piece (two edges, each of connection kind 1..3, in one of the six simple
// configurations) gets an arrow: texture 0xbe35eb0 on a straight, 0xbe35ec0 on a curve (whose edges 1 and 2 read their
// direction pair swapped). The direction bits vote (1 = +1, 2 = -1) for the arrow's orientation. The arrow borrows the
// quad's corners with other texture coordinates and puts the piece's own coordinates back afterwards.
void cSC4NetworkOccupantModel::DrawOneWayArrow(SC4DrawContext* context, const cSC4PackedRenderState& state, std::uint8_t directions, int zoom) {
    const std::uint32_t edges = occupant_.GetEdgeTypesFlag(10);   // vslot 0x98
    if ((edges & 0xfcfcfcfcu) != 0) return;
    const std::uint32_t edgeSet = ((((edges & 0x03030303u) + 0x03030303u) & 0x04040404u) * 0x01020408u) >> 26;
    std::uint8_t orientation = kArrowOrientation[edgeSet][0];
    if (orientation == 0xff) return;
    switch (edges) {
        case 0x301: case 0x20002: case 0x30100: case 0x1000003: case 0x2000200: case 0x3010000: break;
        default: return;
    }
    std::uint32_t bits = directions;
    std::uint32_t textureId = 0xbe35eb0;
    if (((1u << edgeSet) & 0x1248u) != 0) {   // the curves (edges 0+1, 1+2, 0+3, 2+3)
        bits = (bits & ~0x3cu) + (bits & 0x14u) * 2 + ((bits & 0x28u) >> 1);
        textureId = 0xbe35ec0;
    }
    int sum = 0;
    for (int shift = 0; shift != 8; shift += 2) {
        const std::uint32_t d = ((bits & 0xffu) >> shift) & 3u;
        if (d == 1) ++sum;
        else if (d == 2) --sum;
    }
    if (sum < 0) orientation = kArrowOrientation[edgeSet][1];
    const std::uint32_t texture = GetTexture(textureId, 0, zoom);   // vslot 0x180
    if (!texture) return;
    context->SetRenderState(PackedStates().blend);                 // sPackedRenderState
    context->SetTexture(static_cast<int>(texture), 0);
    occupant_.SetTextureCoordinates(orientation);
    const std::uint16_t* indices = occupant_.HasNetworkFlag(0x1000000) ? kFlippedTriangleIndices : kTriangleIndices;
    if (occupant_.HasNetworkFlag(kNetworkFlagQuadDrawA)) context->DrawPrimsIndexed(0, 3, indices);
    if (occupant_.HasNetworkFlag(kNetworkFlagQuadDrawB)) context->DrawPrimsIndexed(0, 3, indices + 3);
    context->SetRenderState(state);
    occupant_.SetTextureCoordinates(occupant_.GetRotationAndFlip());
}

// cSC4NetworkOccupant::Pick (i386 0x00133abc): the ray against the piece's bounding box with its top raised 0.1 m (a flat piece
// still has a face to hit); a hit no farther than the nearest so far becomes the nearest.
bool cSC4NetworkOccupantModel::Pick(cSC4PickContext* context) {
    if (!context) return false;
    float boxMin[3], boxMax[3];
    occupant_.GetBoundingBox(boxMin, boxMax);
    boxMax[1] += 0.1f;   // i386 0x00a70714
    float hit[3] = {0.0f, 0.0f, 0.0f};
    if (!SC4GeometryUtilities::RayIntersectsBoundingBox(&context->origin.x, &context->direction.x, boxMin, boxMax, hit)) return false;
    const float distance = SC4GeometryUtilities::LengthOfLine(&context->origin.x, hit);
    if (distance > context->maxDistance) return false;
    context->maxDistance = distance;
    return true;
}

}  // namespace sc4
