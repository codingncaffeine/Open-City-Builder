// The volcano's lava in the native game (app/NativeVolcanoLava.cpp): the recovered flow over the city's terrain, drawn by
// the city view's renderer.
#pragma once

namespace sc4 { class IVolcanoLava; class cSC43DRender; }

namespace native {

// A new lava over the city's terrain, initialized; one reference for the caller. Null without a city.
sc4::IVolcanoLava* CreateVolcanoLava();
// The city view's renderer is up: the lava registers its drawing there.
void AttachVolcanoLava(sc4::cSC43DRender& render);
// The city view goes: any lava left is shut down while the effects and the overlays still exist.
void DetachVolcanoLava();

}  // namespace native
