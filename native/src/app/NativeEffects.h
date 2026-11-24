// The visual-effects system in a native city (NativeEffects.cpp).
#pragma once

#include <cstdint>

namespace sc4 {
class cSC4EffectMaps; class cSC43DRender; class cSC4City; class ITerrainEffect; class cSC4VisualEffect; }

namespace native {

// The effects manager's host and its effects' services, at start-up (before any city makes its manager).
void InstallEffectsManagerHost();
void DetachEffects();   // the renderer shuts down (leaving the city): the effects pass leaves it
// The effects manager's effect maps (QueryInterface 0x0a5eaa1a), borrowed; null without a manager.
sc4::cSC4EffectMaps* EffectMaps();
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
// Kit verb "pollutionfx" (NativeSimHosts.cpp): the pollution simulator's per-block effect slots (count, started, first N with
// their positions) and its worst air / water / garbage tracts.
void NativePollutionEffectReport(int limit);
// Kit verb "trafficair" (NativeSimHosts.cpp): the traffic air the pollution host hands the block walk (cells, sum, the busiest
// cell) and the pollution simulator's average and that cell's air.
void NativeTrafficAirReport();
// Kit verb "particlehits [X Z]" (NativeEffects.cpp): the particle systems' building-collision queries and the particles they
// stopped; with a cell, the host's answer for a particle just above that cell and one high above it.
void NativeParticleCollisionReport(int cellX = -1, int cellZ = -1);

// The effects manager's Snow cheat with its command line ("Snow [low] [high] [bias]"), as the cheat box runs it; prints
// the snowline state after it. False without an effects manager.
bool RunSnowCheat(const char* text);
}  // namespace native
