#include "app/NativeZoneOverlay.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "sc4/DrawContext.h"
#include "sc4/PackedRenderState.h"
#include "sc4/Render3D.h"
#include "sc4/ViewUtilities.h"
#include "sc4/cSC4ZoneManager.h"
#include "sc4/cSTETerrain.h"

namespace native {
namespace {

struct Vertex {   // V3F_C4UB_T2F
    float x, y, z;
    std::uint8_t rgba[4];
    float u, v;
};

sc4::cSC4ZoneManager* g_zones = nullptr;
sc4::cSTETerrain* g_terrain = nullptr;
std::map<std::pair<int, int>, std::uint32_t> g_drag;
bool g_hasError = false;
int g_error[4] = {};

class OverlayObject final : public cISC4ViewObject3D {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override { return --refs_; }

    void DrawViewObject(SC4DrawContext& context) override {
        if (!g_terrain) return;
        std::vector<Vertex> quads;   // triangles, six per cell
        auto add = [&](int x, int z, std::uint32_t argb, int alphaScale) {
            const float x0 = x * 16.0f, z0 = z * 16.0f, x1 = x0 + 16.0f, z1 = z0 + 16.0f;
            const float y00 = g_terrain->GetAltitudeAtVertex(x, z) + 0.6f, y10 = g_terrain->GetAltitudeAtVertex(x + 1, z) + 0.6f;
            const float y01 = g_terrain->GetAltitudeAtVertex(x, z + 1) + 0.6f, y11 = g_terrain->GetAltitudeAtVertex(x + 1, z + 1) + 0.6f;
            const std::uint8_t a = static_cast<std::uint8_t>(((argb >> 24) & 0xff) * alphaScale / 255);
            const std::uint8_t r = (argb >> 16) & 0xff, g = (argb >> 8) & 0xff, b = argb & 0xff;
            auto v = [&](float vx, float vy, float vz) { Vertex out{vx, vy, vz, {r, g, b, a}, 0, 0}; quads.push_back(out); };
            v(x0, y00, z0); v(x1, y10, z0); v(x0, y01, z1);
            v(x1, y10, z0); v(x1, y11, z1); v(x0, y01, z1);
        };
        if (g_zones) {
            const int size = g_terrain->CellCountX();
            for (int x = 0; x < size; ++x)
                for (int z = 0; z < size; ++z) {
                    const int type = g_zones->GetZoneType(x, z);
                    if (type > 0 && type < 16) add(x, z, (g_zones->GetZoneDragColor(type) >> 8) | 0xff000000u, 110);   // the colours are stored RRGGBBAA
                }
        }
        for (const auto& cell : g_drag) add(cell.first.first, cell.first.second, cell.second, 255);
        if (g_hasError)
            for (int x = g_error[0]; x <= g_error[2]; ++x)
                for (int z = g_error[1]; z <= g_error[3]; ++z) add(x, z, 0x80ff2020u, 255);
        if (quads.empty()) return;
        static cSC4PackedRenderState state;
        static bool packed = false;
        if (!packed) { state.PackRenderState(true, false, true, false, true, 0, false, 3, 4, 5, -1, -1.0f, 1, -1); packed = true; }
        context.ResetModelViewTransform();
        context.SetRenderState(state);
        context.SetDepthOffset(-2);
        // the draw call takes whole batches: keep each below the vertex-buffer size
        for (std::size_t i = 0; i < quads.size(); i += 3000)
            context.DrawPrims(0 /* triangle list */, 10 /* V3F_C4UB_T2F */, static_cast<int>(std::min<std::size_t>(3000, quads.size() - i)), quads.data() + i);
        context.SetDepthOffset(0);
    }

private:
    std::uint32_t refs_ = 1;
};

}  // namespace

void AttachZoneOverlay(sc4::cSC43DRender& render, sc4::cSTETerrain& terrain) {
    g_terrain = &terrain;
    // list 3: drawn every frame just before the moving objects (the zones and the tools' drag cells change while the static view
    // stays in the backing store; list 0 would draw them into the store once)
    render.AddViewObject(new OverlayObject(), 3, 200);
}
void SetZoneOverlayManager(sc4::cSC4ZoneManager* zones) { g_zones = zones; }
void SetDragHighlightCell(int x, int z, std::uint32_t argb) { g_drag[{x, z}] = argb; }
void ClearDragHighlight() { g_drag.clear(); }
void SetDragErrorRect(int x0, int z0, int x1, int z1) { g_hasError = true; g_error[0] = x0; g_error[1] = z0; g_error[2] = x1; g_error[3] = z1; }
void ClearDragError() { g_hasError = false; }

}  // namespace native
