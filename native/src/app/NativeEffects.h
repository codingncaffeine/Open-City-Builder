// The visual-effects system in a native city (NativeEffects.cpp).
#pragma once

#include <cstdint>

namespace sc4 { class cSC43DRender; class cSC4City; class ITerrainEffect; class cSC4VisualEffect; }

namespace native {

void AttachEffects(sc4::cSC43DRender& render, sc4::cSC4City& city);   // the city view's renderer is up
void DetachEffects();                                                 // the renderer shuts down (leaving the city)
// cSTETerrain's host seam: a visual effect by its (group, type) binding, or null.
sc4::ITerrainEffect* CreateTerrainEffect(std::uint32_t group, std::uint32_t type);
// A visual effect by name (effects manager slot 7, "buzzards"): 1 = made and started at (x, y, z), 0 = no such effect,
// -1 = no effects manager (nothing can show it).
int StartNamedEffect(const char* name, float x, float y, float z);
bool HasNamedEffect(const char* name);
// A visual effect by name with one reference, not started (the caller places, starts and releases it), or null.
sc4::cSC4VisualEffect* CreateNamedEffect(const char* name);
// The effect bound to a (group, type) key (cSC4EffectsManager::CreateVisualEffect(uint, uint)); one reference, null when unbound.
sc4::cSC4VisualEffect* CreateKeyedEffect(std::uint32_t group, std::uint32_t type);

}  // namespace native
