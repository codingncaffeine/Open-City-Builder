// The civic building menus and the place-lot tool's services on the native application.
#pragma once
#include <cstdint>
#include <map>
#include <set>

namespace sc4 { class cISC4CatalogItem; class cSC4View3DWin; }

namespace native {

// cSC4View3DWin::AddBuildingsToItemList (Mac i386 0x5228aa): one catalog item per plopped building type whose occupant groups
// meet `include` and miss `exclude`. `rewardsUnlocked` is the view's +0x456 byte (the reward cheat).
void AddBuildingsToItemList(const std::set<std::uint32_t>& include, const std::set<std::uint32_t>& exclude, bool rewardsUnlocked,
                            std::multimap<int, sc4::cISC4CatalogItem*>& items);

// The IPlaceLotHost the place-lot tool runs on (replaces the city-load stub).
void InstallPlaceLotHost(sc4::cSC4View3DWin& view);

}  // namespace native
