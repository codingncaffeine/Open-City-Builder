// The visual-effects system in a native city (NativeEffects.cpp).
#pragma once

#include <cstdint>

namespace sc4 { class cSC43DRender; class cSC4City; class ITerrainEffect; }

namespace native {

void AttachEffects(sc4::cSC43DRender& render, sc4::cSC4City& city);   // the city view's renderer is up
void DetachEffects();                                                 // the renderer shuts down (leaving the city)
// cSTETerrain's host seam: a visual effect by its (group, type) binding, or null.
sc4::ITerrainEffect* CreateTerrainEffect(std::uint32_t group, std::uint32_t type);

}  // namespace native
