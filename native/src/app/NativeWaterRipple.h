// The sea shore's animated foam ribbons (cSTEWaterRipple) on the native application: NativeWaterRipple.cpp.
#pragma once

namespace sc4 { class cSC43DRender; class cSTETerrain; }

namespace native {

// The terrain view's Init part: the ripple object as renderer view object (list 5, priority 1100), ticked by the animation
// tick manager and told about terrain edits. The city size is in meters.
void AttachWaterRipple(sc4::cSC43DRender& render, sc4::cSTETerrain& terrain, float citySizeX, float citySizeZ);
// Leaving the city (before the renderer shuts down).
void DetachWaterRipple();

}  // namespace native
