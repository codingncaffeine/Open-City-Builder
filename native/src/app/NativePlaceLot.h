// The civic building menus and the place-lot tool's services on the native application.
#pragma once
#include <string>
#include <cstdint>
#include <map>
#include <set>

namespace sc4 { class cISC4CatalogItem; class cSC4View3DWin; }

namespace native {

// cSC4View3DWin::AddBuildingsToItemList (Mac i386 0x5228aa): one catalog item per plopped building type whose occupant groups
// meet `include` and miss `exclude`. `rewardsUnlocked` is the view's +0x456 byte (the reward cheat).
void AddBuildingsToItemList(const std::set<std::uint32_t>& include, const std::set<std::uint32_t>& exclude, bool rewardsUnlocked,
                            std::multimap<int, sc4::cISC4CatalogItem*>& items);

// Kit verb placelot: the place-lot tool of a lot configuration (or, given "b:ID", the first lot configuration holding that
// building type) made current on the view, then its own CanPlaceLot and PlaceLot at the cell's centre with a facing. No input
// events; prints the outcome.
void NativePlaceLotAt(const std::string& id, int x, int z, int facing, int x1 = -1, int z1 = -1);   // x1, z1: search to there

// The IPlaceLotHost the place-lot tool runs on (replaces the city-load stub).
void InstallPlaceLotHost(sc4::cSC4View3DWin& view);

}  // namespace native
