// cSC4FoundationModel (Mac i386 0x0028a1e8..0x0028b0a4). See FoundationModel.h.
#include "sc4/FoundationModel.h"

#include <cstring>

#include "sc4/DrawContext.h"
#include "sc4/ViewUtilities.h"

namespace sc4 {

namespace {
IFoundationModelHost* g_host = nullptr;
cSC4PackedRenderState g_packedRenderState;   // cSC4FoundationModel::mPackedRenderState
bool g_staticInitialized = false;            // mbStaticInitialized

constexpr float kCellsPerMetre = 0.0625f;              // 0x00a70700
constexpr float kMetresPerCell = 16.0f;                // 0x00a70778
constexpr float kMaxStoplightDistance = 1.0f;          // cSC4TrafficAutomaton::kfMaxStoplightDistance, the shared 1.0
constexpr float kRoundUp = 0.49900001f;                // 0x00a7086c (0x3eff7cee)
constexpr std::uint32_t kSideRetryBase = 0x69f20000;
constexpr std::uint32_t kTopRetryBase = 0x0912220f;
constexpr std::uint32_t kSortNoBinding = 0xe9862ab6;

// (v + 2^23 * 1.5) read as an int, minus the bias: v rounded to the nearest integer (ties to even).
int RoundToInt(float v) {
    const float biased = v + 12582912.0f;
    std::uint32_t bits;
    std::memcpy(&bits, &biased, sizeof bits);
    return static_cast<int>(bits - 0x4b400000u);
}
}  // namespace

IFoundationModelHost* FoundationModelHost() { return g_host; }
void SetFoundationModelHost(IFoundationModelHost* host) { g_host = host; }

cSC4FoundationModel::cSC4FoundationModel() = default;   // the original zeroes the arrays and sets the built flag last

cSC4FoundationModel::~cSC4FoundationModel() {
    if (sideBinding_) sideBinding_->Release();
    if (topBinding_) topBinding_->Release();
}

void cSC4FoundationModel::SetTextureIDs(const std::uint32_t* side, const std::uint32_t* top) {
    for (int i = 0; i < 5; ++i) {
        if (side) sideIds_[i] = side[i];
        if (top) topIds_[i] = top[i];
    }
}

void cSC4FoundationModel::SetTextureWorldSize(float side, float top) {
    sideWorldSize_ = side;
    topWorldSize_ = top;
}

void cSC4FoundationModel::SetupPackedRenderState() {
    if (g_host) g_host->TouchRenderer();
    g_packedRenderState.PackRenderState(true, true, true, false, false, 1, false, 3, -1, -1, -1, -1.0f, 1, -1);
}

void cSC4FoundationModel::GetSortValue(std::uint32_t& out) const {
    const IFoundationTextureBinding* binding = sideBinding_ ? sideBinding_ : topBinding_;
    out = binding ? binding->TextureId() : kSortNoBinding;
    out |= 0x2ab6;
}

void cSC4FoundationModel::SetupTextures(int oldZoom, int newZoom) {
    if (oldZoom == newZoom || !g_host) return;
    if (newZoom < 0 || newZoom > 4) return;
    if (sideBinding_) {
        sideBinding_->Release();
        sideBinding_ = nullptr;
    }
    if (sideIds_[newZoom] != 0) {
        sideBinding_ = g_host->GetBinding(sideIds_[newZoom]);
        if (!sideBinding_) {
            sideIds_[newZoom] = static_cast<std::uint32_t>(newZoom) + kSideRetryBase;
            sideBinding_ = g_host->GetBinding(sideIds_[newZoom]);
        }
    }
    if (topBinding_) {
        topBinding_->Release();
        topBinding_ = nullptr;
    }
    if (topIds_[newZoom] != 0) {
        topBinding_ = g_host->GetBinding(topIds_[newZoom]);
        if (!topBinding_) {
            topIds_[newZoom] = static_cast<std::uint32_t>(newZoom) + kTopRetryBase;
            topBinding_ = g_host->GetBinding(topIds_[newZoom]);
        }
    }
}

void cSC4FoundationModel::DoZoomChange(int, int newZoom) {
    const int oldZoom = zoom_;
    zoom_ = static_cast<std::int8_t>(newZoom);
    if (g_host) g_host->VisibleEdges(edgeA_, edgeB_);
    if (zoom_ >= 0 && zoom_ < 5) zoomSeen_[zoom_] = 1;
    if (!g_staticInitialized) {
        g_staticInitialized = true;
        SetupPackedRenderState();
    }
    SetupTextures(oldZoom, newZoom);
}

void cSC4FoundationModel::DoRotationChange(int, int newRotation) {
    rotation_ = static_cast<std::int8_t>(newRotation);
    if (g_host) g_host->VisibleEdges(edgeA_, edgeB_);
    SetupVertices(false, true);
}

// @ 0x0028a4ae. The geometry pass writes the four side quads (edge 0 at min z, 1 at max x, 2 at max z, 3 at min x; each from the
// top down to the bottom of the box) and the top quad; the lighting pass colours each side by the light on its outward normal and
// the top by the light from above.
void cSC4FoundationModel::SetupVertices(bool geometry, bool lighting) {
    const float x0 = bounds_.min[0], z0 = bounds_.min[2], top = bounds_.max[1], bottom = bounds_.min[1];
    const float width = bounds_.max[0] - x0, depth = bounds_.max[2] - z0;
    const float perSize = kMaxStoplightDistance / sideWorldSize_;
    const float u = width * kCellsPerMetre;                        // every side's u extent (quirk: the width, on all four)
    const float topU = u * kMetresPerCell * perSize;               // the top quad's uv (quirk: in the side's world size)
    const float topV = perSize * kMetresPerCell * depth * kCellsPerMetre;
    if (geometry) {
        const float height = (top - bottom) * kCellsPerMetre;      // every side's v extent
        const float x1 = x0 + width, z1 = z0 + depth;
        auto set = [](FoundationVertex& vertex, float x, float y, float z, float uu, float vv) {
            vertex.x = x;
            vertex.y = y;
            vertex.z = z;
            vertex.u = uu;
            vertex.v = vv;
        };
        set(side_[0], x1, top, z0, 0.0f, 0.0f);
        set(side_[1], x1, bottom, z0, 0.0f, height);
        set(side_[2], x0, bottom, z0, u, height);
        set(side_[3], x0, top, z0, u, 0.0f);
        set(side_[4], x1, top, z1, 0.0f, 0.0f);
        set(side_[5], x1, bottom, z1, 0.0f, height);
        set(side_[6], x1, bottom, z0, u, height);
        set(side_[7], x1, top, z0, u, 0.0f);
        set(side_[8], x0, top, z1, 0.0f, 0.0f);
        set(side_[9], x0, bottom, z1, 0.0f, height);
        set(side_[10], x1, bottom, z1, u, height);
        set(side_[11], x1, top, z1, u, 0.0f);
        set(side_[12], x0, top, z0, 0.0f, 0.0f);
        set(side_[13], x0, bottom, z0, 0.0f, height);
        set(side_[14], x0, bottom, z1, u, height);
        set(side_[15], x0, top, z1, u, 0.0f);
    }
    if (lighting && g_host) {
        for (int edge = 0; edge < 4; ++edge) {
            float normal[3] = {0.0f, 0.0f, 0.0f};
            SC4ViewUtilities::GetVectorForEdge(normal, edge);
            std::uint8_t r = 0, g = 0, b = 0;
            g_host->LightColor(normal, r, g, b);
            for (int i = 0; i < 4; ++i) {
                FoundationVertex& vertex = side_[edge * 4 + i];
                vertex.color[0] = r;
                vertex.color[1] = g;
                vertex.color[2] = b;
                vertex.color[3] = 0xff;
            }
        }
    }
    if (geometry) {
        const float x1 = x0 + width, z1 = z0 + depth;
        top_[0] = FoundationVertex{x0, top, z0, {top_[0].color[0], top_[0].color[1], top_[0].color[2], top_[0].color[3]}, 0.0f, 0.0f};
        top_[1] = FoundationVertex{x0, top, z1, {top_[1].color[0], top_[1].color[1], top_[1].color[2], top_[1].color[3]}, 0.0f, topV};
        top_[2] = FoundationVertex{x1, top, z1, {top_[2].color[0], top_[2].color[1], top_[2].color[2], top_[2].color[3]}, topU, topV};
        top_[3] = FoundationVertex{x1, top, z0, {top_[3].color[0], top_[3].color[1], top_[3].color[2], top_[3].color[3]}, topU, 0.0f};
    }
    if (lighting && g_host) {
        const float up[3] = {0.0f, 1.0f, 0.0f};
        std::uint8_t r = 0, g = 0, b = 0;
        g_host->LightColor(up, r, g, b);
        for (FoundationVertex& vertex : top_) {
            vertex.color[0] = r;
            vertex.color[1] = g;
            vertex.color[2] = b;
            vertex.color[3] = 0xff;
        }
    }
}

// @ 0x0028aa4c. The deepest point of the ground (or water) under the box's border cells, below the top, in whole cells (plus one):
// when that is deeper than the box the bottom moves down to it. Returns whether the vertices were rebuilt.
bool cSC4FoundationModel::AdjustHeightToTerrain(bool force) {
    const int cellX0 = RoundToInt(bounds_.min[0] * kCellsPerMetre + 0.0f);
    const int cellZ0 = RoundToInt(bounds_.min[2] * kCellsPerMetre + 0.0f);
    const int cellX1 = RoundToInt(bounds_.max[0] * kCellsPerMetre + kRoundUp);
    const int cellZ1 = RoundToInt(bounds_.max[2] * kCellsPerMetre + kRoundUp);
    const int heightCells = static_cast<int>(((bounds_.max[1] - bounds_.min[1]) + kMetresPerCell) * kCellsPerMetre);
    float deepest = 0.0f;
    if (g_host) {
        const int stride = g_host->TerrainCellCountX() + 1;
        for (int x = cellX0; x <= cellX1; ++x) {
            for (int z = cellZ0; z <= cellZ1; ++z) {
                if (x != cellX0 && z != cellZ0 && x != cellX1 && z != cellZ1) continue;
                const int vertex = x + stride * z;
                const float ground = g_host->TerrainAltitude(vertex);
                const float water = g_host->TerrainWaterLevel(vertex);
                const float surface = water <= ground ? ground : water;
                const float below = bounds_.max[1] - surface;
                if (below > deepest) deepest = below;
            }
        }
    }
    const int neededCells = static_cast<int>((deepest + kMetresPerCell) * kCellsPerMetre);
    if (heightCells < neededCells) {
        bounds_.min[1] = bounds_.max[1] - static_cast<float>(neededCells << 4);
    } else if (!force) {
        return false;
    }
    SetupVertices(true, false);
    return true;
}

// @ 0x0028acc6.
int cSC4FoundationModel::Draw(SC4DrawContext* context, bool highlight) {
    if (highlight || !context) return 1;
    if (context->ViewRotation() != rotation_) DoRotationChange(rotation_, context->ViewRotation());
    if (context->Zoom() != zoom_) DoZoomChange(zoom_, context->Zoom());
    const bool seen = zoom_ >= 0 && zoom_ < 5 && zoomSeen_[zoom_] != 0;
    if (!seen && !topBinding_) return 1;
    context->ResetModelViewTransform();
    context->SetRenderState(g_packedRenderState);
    if (seen && (edgeA_ >= 0 || edgeB_ >= 0) && sideBinding_) {
        if (const std::uint32_t texture = sideBinding_->TextureId()) {
            context->SetTexture(static_cast<int>(texture), 0);
            if (edgeA_ >= 0) context->DrawPrims(6, 10, 4, &side_[edgeA_ * 4]);
            if (edgeB_ >= 0) context->DrawPrims(6, 10, 4, &side_[edgeB_ * 4]);
        }
    }
    if (topBinding_) {
        if (const std::uint32_t texture = topBinding_->TextureId()) {
            context->SetTexture(static_cast<int>(texture), 0);
            context->DrawPrims(6, 10, 4, top_);
        }
    }
    return 1;
}

}  // namespace sc4
