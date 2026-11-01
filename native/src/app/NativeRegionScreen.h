// The host of the region screen (IRegionScreenHost) and the view host of the region map (IRegionViewHost) for the native
// application: the screen's services over the real nation / region / regional-city objects (NativeRegion), the region map's tiled
// canvas drawn through the screen's draw context, and the adapters between the three descriptions of a city the recovered classes use
// (the screen's IRegionalCity, the view's IRegionCityView and the record cSC4RegionalCity).
#pragma once
#include <functional>
#include <string>

#include "rz/cRZAutoRefCount.h"
#include "sc4/ISC4RegionalCity.h"

#include "app/NativeGraphics.h"
#include "sc4/WinRegionScreen.h"
#include "sc4/WinRegionView.h"

class cSC4App;

namespace native {

struct RegionScreenServices {
    cSC4App* app = nullptr;
    Graphics* graphics = nullptr;
    cIGZWin* (*createWindowFromScript)(const cGZPersistResourceKey&, cIGZWin*, std::uint32_t) = nullptr;
    std::function<cGZBuffer*(std::uint32_t group, std::uint32_t instance)> loadBitmap;
    std::function<bool(const std::string& path, cRZAutoRefCount<cISC4RegionalCity>* holder)> loadCity;   // loads a city file and enters it

};

// Installs both hosts (sc4::SetRegionScreenHost / SetRegionViewHost).
void InstallRegionScreenHost(const RegionScreenServices& services);

// Draws the region map's tiled canvas layers when they changed; true when it drew (the windows above must repaint).
bool DrawRegionLayers();

}  // namespace native
