// The region layer of the native application: the nation (the regions the player has), each region (its cities and their
// places), and the regional city records read from the city files. The classes are the recovered cSC4Nation / cSC4Region /
// cSC4RegionalCity; this file supplies the hosts (INationHost, IRegionHost, IRegionalCityHost) they reach the application through,
// and the save-file services they read city files with.
#pragma once
#include <functional>

#include "sc4/Nation.h"
#include "sc4/Region.h"
#include "sc4/RegionalCity.h"

class cSC4App;
class cISC4RegionalCity;

namespace native {

struct RegionCallbacks {
    std::function<bool()> isRegionScreenCurrent;
    std::function<void()> destroyCurrentScreen;
    std::function<void()> createRegionScreen;
    std::string dataDirectory;   // GameData/ (with the trailing slash)
};

// Installs the three hosts and the save-game services (city segment and regional city factories).
void InstallRegionServices(cSC4App& app, const RegionCallbacks& callbacks);
// The neighbours simulator's and its connections' region side (after the per-city stub hosts, before the components Init).
void InstallNeighborHosts();
cISC4RegionalCity* NativeCurrentRegionalCity();   // the open city's regional city (cSC4App::GetRegionalCity)

// Creates the nation (one reference) and initialises it: the regions directory is scanned and the active region chosen from the
// preferences. Call after InstallRegionServices and after the application's Init.
cISC4Nation* CreateNation();

}  // namespace native
