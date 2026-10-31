#include "sc4/NetworkPreview.h"

#include <cmath>

#include "gz/StreamOps.h"

namespace sc4 {

namespace {
INetworkPreviewHost* g_host = nullptr;
constexpr int kXOffset[4] = {0, 0, 1, 1};                       // (anonymous)::kXOffset 0x008500f0
constexpr int kZOffset[4] = {0, 1, 1, 0};                       // (anonymous)::kZOffset 0x008500e0
constexpr std::uint8_t kVertexMap[2][4] = {{0, 1, 2, 3}, {1, 2, 3, 0}};   // SetVertexPositions::sVertexMap 0x00850120
constexpr std::uint16_t kIndices[6] = {0, 1, 2, 0, 2, 3};      // DrawCells::nIndices 0x00850110
constexpr float kHiddenOpacity = 0.2f;                          // 0x3e4ccccd
}  // namespace

void SetNetworkPreviewHost(INetworkPreviewHost* host) { g_host = host; }

cSC4NetworkPreview::cSC4NetworkPreview() = default;

std::uint32_t cSC4NetworkPreview::Release() {
    if (refCount_ > 1) return --refCount_;
    refCount_ = 0;
    delete this;
    return 0;
}

void cSC4NetworkPreview::Init() {
    if (inited_) return;
    // The second argument of each pack is the texture stage count, the eighth the depth test (3 = in front, 4 = behind).
    states_[0].PackRenderState(true, true, true, false, true, 0, true, 3, 4, 5, -1, -1.0f, 1, -1);
    states_[2].PackRenderState(true, true, true, false, true, 1, true, 3, 4, 5, -1, -1.0f, 1, -1);
    states_[1].PackRenderState(true, true, true, false, true, 0, true, 4, 4, 5, -1, -1.0f, 1, -1);
    states_[3].PackRenderState(true, true, true, false, true, 1, true, 4, 4, 5, -1, -1.0f, 1, -1);
    combiner_ = gd::cGDCombiner{0x01030002u, 0x01030001u, 0u};
    inited_ = true;
}

void cSC4NetworkPreview::Shutdown() {
    Clear();
    inited_ = false;
}

void cSC4NetworkPreview::Clear() {
    untextured_.clear();
    textured_.clear();
}

void cSC4NetworkPreview::Erase(const SC4PointU& cell) {
    const std::uint32_t key = cell.x + (cell.z << 16);
    untextured_.erase(key);
    textured_.erase(key);
}

// Every corner: the terrain at its vertex (the water surface where the water is shown), raised to the solved height
// when one is given, plus the cell's elevation. `onTerrainOnly` starts from 0 instead of the terrain.
void cSC4NetworkPreview::SetVertexHeights(Cell& cell, const SC4PointU& at, const float* heights, bool onTerrainOnly) {
    const bool water = g_host && g_host->WaterShown();
    for (int i = 0; i < 4; ++i) {
        const std::uint8_t corner = cell.corners[i];
        const int vx = static_cast<int>(at.x) + kXOffset[corner], vz = static_cast<int>(at.z) + kZOffset[corner];
        float y = 0.0f;
        if (!onTerrainOnly && g_host) {
            y = g_host->VertexAltitude(vx, vz);
            if (water) {
                const float depth = g_host->WaterDepth(vx, vz);
                y += (depth + std::fabs(depth)) * 0.5f;   // only a positive depth lifts the corner
            }
        }
        if (heights && y < heights[corner]) y = heights[corner];
        cell.vertices[i].y = y + cell.elevation;
    }
}

void cSC4NetworkPreview::SetVertexPositions(Cell& cell, const SC4PointU& at, std::uint32_t color, const float* heights,
                                            int rotation, bool flip, float elevation, bool onTerrainOnly) {
    const int row = g_host && g_host->TriangulationFlipped(static_cast<int>(at.x), static_cast<int>(at.z)) ? 1 : 0;
    for (int i = 0; i < 4; ++i) {
        const std::uint8_t corner = kVertexMap[row][i];
        int u = kXOffset[corner];
        const int v0 = kZOffset[corner];
        cell.corners[i] = corner;
        Vertex& vertex = cell.vertices[i];
        vertex.x = static_cast<float>((u + static_cast<int>(at.x)) * 16);
        vertex.z = static_cast<float>((static_cast<int>(at.z) + v0) * 16);
        // ARGB in, R G B A in memory (the i386 byte swap).
        vertex.bgra[0] = static_cast<std::uint8_t>(color >> 16);
        vertex.bgra[1] = static_cast<std::uint8_t>(color >> 8);
        vertex.bgra[2] = static_cast<std::uint8_t>(color);
        vertex.bgra[3] = static_cast<std::uint8_t>(color >> 24);
        if (flip) u ^= 1;
        int v = v0;
        if (rotation & 1) {
            v = u ^ 1;
            u = v0;
        }
        if (rotation & 2) {
            u ^= 1;
            v ^= 1;
        }
        vertex.u = static_cast<float>(u);
        vertex.v = static_cast<float>(v);
    }
    cell.elevation = elevation;
    SetVertexHeights(cell, at, heights, onTerrainOnly);
}

void cSC4NetworkPreview::AddUntexturedSelection(const SC4PointU& at, std::uint32_t color, const float* heights, float elevation,
                                                bool onTerrainOnly) {
    Cell cell;
    SetVertexPositions(cell, at, color, heights, 0, false, elevation, onTerrainOnly);
    untextured_.emplace(at.x + (at.z << 16), cell);
}

void cSC4NetworkPreview::AddTexturedSelection(const SC4PointU& at, std::uint32_t color, const float* heights,
                                              const cGZPersistResourceKey& texture, int rotation, bool flip, float elevation,
                                              bool onTerrainOnly) {
    Cell cell;
    SetVertexPositions(cell, at, color, heights, rotation, flip, elevation, onTerrainOnly);
    cell.texture = g_host ? g_host->Texture(texture) : 0;
    if (!cell.texture) {
        AddUntexturedSelection(at, color, heights, elevation, onTerrainOnly);
        return;
    }
    textured_.emplace(at.x + (at.z << 16), cell);
}

void cSC4NetworkPreview::AdjustCellHeights(const SC4PointU& at, const float* heights, bool onTerrainOnly) {
    const std::uint32_t key = at.x + (at.z << 16);
    for (auto range = untextured_.equal_range(key); range.first != range.second; ++range.first)
        SetVertexHeights(range.first->second, at, heights, onTerrainOnly);
    for (auto range = textured_.equal_range(key); range.first != range.second; ++range.first)
        SetVertexHeights(range.first->second, at, heights, onTerrainOnly);
}

void cSC4NetworkPreview::DrawViewObject(SC4DrawContext& context) {
    if (untextured_.empty() && textured_.empty()) return;
    context.SetDepthOffset(4);
    context.ResetModelViewTransform();
    cS3DTransform identity;
    context.SetModelTransform(identity);
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 0) context.ResetTransparency();
        else context.SetTransparency(kHiddenOpacity);
        context.SetRenderState(states_[pass]);
        for (const auto& entry : untextured_) {
            context.SetVertexBuffer(10, 4, entry.second.vertices);
            context.DrawPrimsIndexed(0, 6, kIndices);
        }
        context.SetRenderState(states_[2 + pass]);
        context.SetTexWrapModes(2, 2, 0);
        context.SetTexCombiner(combiner_, 0);
        for (const auto& entry : textured_) {
            context.SetTexture(entry.second.texture, 0);
            context.SetVertexBuffer(10, 4, entry.second.vertices);
            context.DrawPrimsIndexed(0, 6, kIndices);
        }
    }
    context.ResetTransparency();
    context.SetDepthOffset(0);
}

}  // namespace sc4
