// The terrain in the city view on the native Metal application: the recovered terrain view (cSTETerrainView3D) drawn through the
// recovered draw context (SC4DrawContext) onto the Metal driver, its textures decoded from the game's FSH resources.
#pragma once

#include <cstdint>

namespace gd { class cIGZGDriver; }
namespace sc4 { class cSC43DRender; class cSTETerrain; }

namespace native {

// Creates the terrain view for `terrain` and registers its terrain and water passes with the renderer.
bool AttachTerrainView(sc4::cSC43DRender& render, sc4::cSTETerrain& terrain, gd::cIGZGDriver* driver);
// The terrain view's draw-frustum culling of the last frame (cSTETerrainView3D::GetCullingInfo: rows are z cells, colStart[z]..colEnd[z]
// the x cells drawn in that row); false before the view exists.
bool TerrainCullingInfo(int& rowBegin, int& rowEnd, const std::int32_t*& colStart, const std::int32_t*& colEnd);
// The same for the whole viewport (the view frustum): for passes drawn every frame outside the terrain draw.
bool TerrainViewCullingInfo(int& rowBegin, int& rowEnd, const std::int32_t*& colStart, const std::int32_t*& colEnd);

}  // namespace native
