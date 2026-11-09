// SC4MarkedCellView (Mac i386 0x0051a2d4..0x0051b1d0): the renderer view object that tints a list of city cells over the
// terrain in a colour per cell. The data-view map hands it the cells whose palette colour has alpha (sc4::IWinMapViewMarkedCells);
// the map view registers it with the renderer as view object list 5, priority 1000.
//   PARTIAL: the network-altitude mode (SetShouldUseNetworkAltitude: the cell's network occupant from the occupant manager with a
//   cSC4NetworkOccupantFilter(0x1f4f), its altitude at each corner + 15.5 for flags 0x304) keeps the terrain altitude; the
//   depth-inverted second pass (SetCellColorDepthInvert) has no caller natively.
#include <cmath>
#include <cstdint>
#include <vector>

#include "app/NativeCity.h"
#include "app/NativeCityView.h"
#include "sc4/DrawContext.h"
#include "sc4/PackedRenderState.h"
#include "sc4/Render3D.h"
#include "sc4/WinMapView.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

namespace native {
bool NativePointInViewFrustum(const float* xyz);   // app/NativeAutomataManagerHost.cpp
namespace {
using namespace sc4;

cSTETerrain* CityTerrain() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* terrain = static_cast<cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
    static_cast<cIGZSerializable*>(raw)->Release();
    return terrain;
}

struct CellVertex {   // V3F_C4UB (vertex format 1, stride 16): the colour is the ABGR word, i.e. bytes R, G, B, A
    float x, y, z;
    std::uint32_t color;
};
static_assert(sizeof(CellVertex) == 16);

// DrawCells' function statics (i386 0x00850f60.. 0x00850fb8).
constexpr float kCornerMeters[4][2] = {{0, 0}, {0, 16}, {16, 16}, {16, 0}};
constexpr int kCornerCells[4][2] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
constexpr std::uint16_t kTriReverse[6] = {0, 1, 3, 3, 1, 2};
constexpr std::uint16_t kTriForward[6] = {0, 1, 2, 0, 2, 3};
constexpr int kFlushAt = 0x400;   // vertices per draw call

class MarkedCellView final : public IWinMapViewMarkedCells, public cISC4ViewObject3D {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override { return --refs_; }

    // 0x0051a2d4: pack the render state once (depth test only when the depth buffer is honoured).
    void Init() override {
        if (initialized_) return;
        if (CityRenderer()) {
            state_.PackRenderState(true, false, !ignoreDepth_, false, true, 0, true, ignoreDepth_ ? -1 : 3, 4, 5, -1, -1.0f, 1, -1);
        }
        initialized_ = true;
    }
    void Shutdown() override { initialized_ = false; }
    void SetShouldIgnoreDepthBuffer(bool on) override { ignoreDepth_ = on; }
    void SetShouldUseWaterAltitude(bool on) override { useWater_ = on; }
    void SetShouldUseNetworkAltitude(bool on) override { useNetwork_ = on; }
    void ClearMarkedCells() override { cells_.clear(); }
    void AddMarkedCells(const std::vector<WinMapCell>& cells) override { cells_.insert(cells_.end(), cells.begin(), cells.end()); }
    void SetCellColors(const std::vector<std::uint32_t>& colors) override { colors_ = colors; }
    // 0x0051a48c: the cell size and its inverse (the original divides 1.0 by it).
    void SetCellSize(float x, float z) override {
        cellX_ = x;
        cellZ_ = z;
        invCellX_ = 1.0f / x;
        invCellZ_ = 1.0f / z;
    }

    // Update (0x0051ade0): nothing without cells; one colour for all when no per-cell colours were given.
    void DrawViewObject(SC4DrawContext& context) override {
        if (cells_.empty()) return;
        const std::uint32_t* single = colors_.empty() ? &cellColor_ : nullptr;
        const int depthOffset = context.GetDepthOffset();
        context.SetDepthOffset(4);
        context.ResetModelViewTransform();
        context.SetRenderState(state_);
        const cS3DTransform identity;   // MakeIdentity
        context.SetModelTransform(identity);
        DrawCells(context, single);
        context.SetDepthOffset(depthOffset);
    }

private:
    float Altitude(const cSTETerrain& terrain, std::int32_t index) const {
        float y = terrain.GetAltitude(index);
        if (useWater_) {
            const float water = terrain.GetWaterLevel(index);
            if (y < water) y = water;
        }
        return y;
    }

    // 0x0051a50c: four corners per cell on the terrain (16 m cells take the vertex grid directly and follow the cell's
    // triangulation), culled when neither the first nor the third corner is in the frustum, drawn 0x400 vertices at a time.
    void DrawCells(SC4DrawContext& context, const std::uint32_t* single) {
        cSTETerrain* terrain = CityTerrain();
        if (!terrain) return;
        const std::uint32_t cellsX = terrain->CellCountX(), cellsZ = terrain->CellCountZ();
        const int stride = static_cast<int>(cellsX) + 1;
        std::vector<CellVertex> vertices;
        std::vector<std::uint16_t> indices;
        vertices.reserve(kFlushAt + 4);
        indices.reserve((kFlushAt + 4) / 4 * 6);
        auto flush = [&] {
            if (vertices.empty()) return;
            context.SetVertexBuffer(1, static_cast<std::uint32_t>(vertices.size()), vertices.data());
            context.DrawPrimsIndexed(0, static_cast<int>(indices.size()), indices.data());
            vertices.clear();
            indices.clear();
        };
        const bool cityCells = cellX_ == 16.0f && cellZ_ == 16.0f;
        for (std::size_t i = 0; i < cells_.size(); ++i) {
            const std::uint32_t color = single ? *single : (i < colors_.size() ? colors_[i] : 0);
            const WinMapCell& cell = cells_[i];
            CellVertex corner[4];
            bool flipped = false;
            if (cityCells) {
                const auto x = static_cast<std::uint32_t>(cell.x), z = static_cast<std::uint32_t>(cell.z);
                if (x >= cellsX || z >= cellsZ) continue;
                const int steps[4] = {0, stride, 1, -stride};
                int index = static_cast<int>(x) + stride * static_cast<int>(z);
                flipped = terrain->IsTriangulationFlipped(x, z);
                for (int k = 0; k < 4; ++k) {
                    index += steps[k];
                    corner[k] = {static_cast<float>(static_cast<int>(x << 4)) + kCornerMeters[k][0], Altitude(*terrain, index),
                                 static_cast<float>(static_cast<int>(z << 4)) + kCornerMeters[k][1], color};
                }
            } else {
                for (int k = 0; k < 4; ++k) {
                    std::int32_t ux = cell.x + kCornerCells[k][0], uz = cell.z + kCornerCells[k][1];
                    if (static_cast<std::uint32_t>(ux) > cellsX) ux = ux < 0 ? 0 : static_cast<std::int32_t>(cellsX);
                    if (static_cast<std::uint32_t>(uz) > cellsZ) uz = uz < 0 ? 0 : static_cast<std::int32_t>(cellsZ);
                    const float px = static_cast<float>(ux) * cellX_, pz = static_cast<float>(uz) * cellZ_;
                    // the original rounds (v - 0.5) through the 12582912 trick: the floor of the vertex coordinate
                    const int vx = static_cast<int>(std::floor(px * 0.0625f)), vz = static_cast<int>(std::floor(pz * 0.0625f));
                    corner[k] = {px, Altitude(*terrain, vx + vz * stride), pz, color};
                }
            }
            if (!NativePointInViewFrustum(&corner[0].x) && !NativePointInViewFrustum(&corner[2].x)) continue;
            const auto base = static_cast<std::uint16_t>(vertices.size());
            for (const CellVertex& v : corner) vertices.push_back(v);
            for (std::uint16_t n : (flipped ? kTriReverse : kTriForward)) indices.push_back(static_cast<std::uint16_t>(base + n));
            if (static_cast<int>(vertices.size()) >= kFlushAt) flush();
        }
        flush();
    }

    std::uint32_t refs_ = 1;
    bool initialized_ = false;          // +0xc
    bool ignoreDepth_ = true;           // +0xd
    bool useWater_ = false;             // +0xe
    bool useNetwork_ = false;           // +0xf (PARTIAL, see the top)
    std::vector<WinMapCell> cells_;     // +0x10
    float cellX_ = 16.0f, cellZ_ = 16.0f, invCellX_ = 0.0625f, invCellZ_ = 0.0625f;   // +0x1c..+0x28
    std::uint32_t cellColor_ = 0x80c000c0u;   // +0x2c
    std::vector<std::uint32_t> colors_;  // +0x30
    cSC4PackedRenderState state_;        // +0x3c
};

}  // namespace

sc4::IWinMapViewMarkedCells* NativeCreateMarkedCells() { return new MarkedCellView(); }

// SL::spRenderer vslots 0x100 / 0x108: AddViewObject(view, 5, 1000) / RemoveViewObject.
bool NativeAddMarkedCells(sc4::IWinMapViewMarkedCells* cells) {
    sc4::cSC43DRender* renderer = CityRenderer();
    return renderer && cells && renderer->AddViewObject(static_cast<MarkedCellView*>(cells), 5, 1000);
}
bool NativeRemoveMarkedCells(sc4::IWinMapViewMarkedCells* cells) {
    sc4::cSC43DRender* renderer = CityRenderer();
    return renderer && cells && renderer->RemoveViewObject(static_cast<MarkedCellView*>(cells));
}

}  // namespace native
