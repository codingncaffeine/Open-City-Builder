// SL — the game's service locator (25 functions, 0x1000404e0..0x1000411b0 and 0x1003f75a0..0x1003f79a0): a set of
// process-wide pointers to the managers every system needs, filled in stages as the application, the city and the
// 3D view come up (OnAppCreate -> OnAppInit1/2 -> OnCityCreate -> OnCityInit -> OnView3DCreate -> OnView3DInit) and
// cleared again on shutdown. The pointers are not owning (the one interface obtained by QueryInterface is released
// again at once).
//
// The systems whose classes are not recovered are held as plain cIGZUnknown pointers; the accessors that were member
// calls on the city / terrain / view are reached through the small source interfaces below.
#pragma once
#include <cstdint>

#include "gz/cIGZMessage2.h"
#include "gz/cGZMessageServer.h"
#include "gz/cIGZPersistResourceManager.h"
#include "gz/cIGZString.h"
#include "gz/cIGZUnknown.h"
#include "sc4/ISC4App.h"

class cIGZFrameWork;
class SC4ViewUtilities;   // global scope (src/sc4/ViewUtilities.h)

namespace SL {

// What OnCityInit pulls out of the city: one accessor per slot of the city interface (offsets in the comments).
class ICitySystems {
public:
    virtual ~ICitySystems() = default;
    virtual cIGZUnknown* HistoryWarehouse() = 0;      // 0x100
    virtual cIGZUnknown* Demolition() = 0;            // 0xf8
    virtual cIGZUnknown* LotManager() = 0;            // 0x108
    virtual cIGZUnknown* OccupantManager() = 0;       // 0x110
    virtual cIGZUnknown* PropManager() = 0;           // 0x118
    virtual cIGZUnknown* ZoneManager() = 0;           // 0x120
    virtual cIGZUnknown* LotConfigurationManager() = 0;   // 0x128
    virtual cIGZUnknown* NetworkManager() = 0;        // 0x130
    virtual cIGZUnknown* DispatchManager() = 0;       // 0x138
    virtual cIGZUnknown* TrafficNetworkMap() = 0;     // 0x140
    virtual cIGZUnknown* PropDeveloper() = 0;         // 0x148
    virtual cIGZUnknown* NetworkLotManager() = 0;     // 0x150
    virtual cIGZUnknown* VehicleManager() = 0;        // 0x158
    virtual cIGZUnknown* PedestrianManager() = 0;     // 0x160
    virtual cIGZUnknown* AircraftManager() = 0;       // 0x168
    virtual cIGZUnknown* WatercraftManager() = 0;     // 0x170
    virtual cIGZUnknown* Simulator() = 0;             // 0x190
    virtual cIGZUnknown* AuraSim() = 0;               // 0x198
    virtual cIGZUnknown* BudgetSim() = 0;             // 0x1a0
    virtual cIGZUnknown* BuildingDevelopmentSim() = 0;    // 0x1a8
    virtual cIGZUnknown* CommercialSim() = 0;         // 0x1b0
    virtual cIGZUnknown* CrimeSim() = 0;              // 0x1b8
    virtual cIGZUnknown* DemandSim() = 0;             // 0x1c0
    virtual cIGZUnknown* FireProtectionSim() = 0;     // 0x1c8
    virtual cIGZUnknown* FlammabilitySim() = 0;       // 0x1d0
    virtual cIGZUnknown* FloraSim() = 0;              // 0x1d8
    virtual cIGZUnknown* IndustrialSim() = 0;         // 0x1e0
    virtual cIGZUnknown* LandValueSim() = 0;          // 0x1e8
    virtual cIGZUnknown* NeighborsSim() = 0;          // 0x1f0
    virtual cIGZUnknown* OrdinanceSim() = 0;          // 0x1f8
    virtual cIGZUnknown* PlumbingSim() = 0;           // 0x200
    virtual cIGZUnknown* PoliceSim() = 0;             // 0x208
    virtual cIGZUnknown* PollutionSim() = 0;          // 0x210
    virtual cIGZUnknown* PowerSim() = 0;              // 0x218
    virtual cIGZUnknown* ResidentialSim() = 0;        // 0x220
    virtual cIGZUnknown* TrafficSim() = 0;            // 0x228
    virtual cIGZUnknown* WeatherSim() = 0;            // 0x230
    virtual cIGZUnknown* DisasterLayer() = 0;         // 0x240
    virtual cIGZUnknown* CivicBuildingSim() = 0;      // 0x248
    virtual cIGZUnknown* ParkManager() = 0;           // 0x250
    virtual cIGZUnknown* ZoneDeveloper() = 0;         // 0x258
    virtual cIGZUnknown* SeaportDeveloper() = 0;      // 0x260
    virtual cIGZUnknown* AirportDeveloper() = 0;      // 0x268
    virtual cIGZUnknown* LandfillDeveloper() = 0;     // 0x270
    virtual cIGZUnknown* LotDeveloper() = 0;          // 0x278
    virtual cIGZUnknown* TractDeveloper() = 0;        // 0x280
    virtual cIGZUnknown* AdvisorSystem() = 0;         // 0x288
    virtual cIGZUnknown* TutorialSystem() = 0;        // 0x290
    virtual cIGZUnknown* SurfaceWater() = 0;          // 0x298
    virtual cIGZUnknown* Terrain() = 0;               // 0x2a0
    virtual cIGZUnknown* EffectsManager() = 0;        // 0x2a8
    virtual cIGZUnknown* Clock24Hour() = 0;           // 0x2b0
};

// The 3D view (cISC4View3DWin) and its renderer, as OnView3DInit uses them.
class IView3DSystems {
public:
    virtual ~IView3DSystems() = default;
    virtual cIGZUnknown* View3DWin() = 0;             // vslot 0x18
    virtual cIGZUnknown* Renderer() = 0;              // vslot 0x20
    virtual cIGZUnknown* ModelMaker() = 0;            // vslot 0x28
    virtual cIGZUnknown* ViewManager() = 0;           // vslot 0x30
    virtual SC4ViewUtilities* RendererViewUtilities() = 0; // renderer vslot 0x40 (a plain object, not COM)
    virtual cIGZUnknown* RendererLightingManager() = 0;   // renderer vslot 0x48
};

// The main application window object (cWinSC4App) hands out the 3D view.
class IWinSC4App {
public:
    virtual ~IWinSC4App() = default;
    virtual IView3DSystems* View3D() = 0;
};

// The application object seen from the locator: it owns the main window object (cSC4App +0x88).
class IApplicationWindowSource {
public:
    virtual ~IApplicationWindowSource() = default;
    virtual IWinSC4App* WinSC4App() const = 0;
};

// The terrain's view (terrain vslot 0x210).
class ITerrainViewSource {
public:
    virtual ~ITerrainViewSource() = default;
    virtual cIGZUnknown* TerrainView() = 0;
};

// ---- the pointers
extern cISC4App* spSC4;                       // OnAppCreate
extern std::uint8_t* spPreferences;           // the 0x1734-byte preference blob
extern cIGZUnknown *spCCM, *spCommandServer, *spRenderProperties, *spGF, *spStringDetokenizer,
    *spWinLocationSaver, *spGlyphTextureManager, *spLua, *spGS, *spGS2, *spGDriver, *spFeatureManager, *spRDD, *spSS,
    *spTextureBindingFactory;
extern cIGZPersistResourceManager* spRM;
extern cIGZMessageServer* spMS;
extern cIGZMessageServer2* spMS2;
extern IWinSC4App* spWinSC4App;               // OnAppInit2 (also spWinSC4AppWin)
extern cISC4City* spCity;
extern ICitySystems* spCitySystems;
extern cIGZUnknown *spTerrain, *spTerrainMap, *spTerrainView, *spSurfaceWater, *spDemolition, *spDispatchManager, *spLotManager,
    *spOccupantManager, *spPropManager, *spZoneManager, *spLotConfigurationManager, *spEffectsManager, *spSimulator,
    *spLotDeveloper, *spTractDeveloper, *spLandfillDeveloper, *spAirportDeveloper, *spSeaportDeveloper, *spAdvisorSystem,
    *spTutorialSystem, *spDisasterLayer, *spParkManager, *spVehicleManager, *spPedestrianManager, *spAircraftManager,
    *spWatercraftManager, *spTrafficNetworkMap, *spNetworkLotManager, *spAuraSim, *spBudgetSim, *spBuildingDevelopmentSim,
    *spCommercialSim, *spCrimeSim, *spDemandSim, *spFireProtectionSim, *spFlammabilitySim, *spFloraSim, *spIndustrialSim,
    *spLandValueSim, *spNeighborsSim, *spOrdinanceSim, *spPlumbingSim, *spPoliceSim, *spPollutionSim, *spPowerSim,
    *spResidentialSim, *spTrafficSim, *spWeatherSim, *spZoneDeveloper, *spCivicBuildingSim, *spPropDeveloper,
    *sp24HourClock, *spHistoryWarehouse;
extern IView3DSystems* spView3D;
extern cIGZUnknown *spView3DWin, *spModelMaker, *spRenderer, *spLightingManager;
extern SC4ViewUtilities* spViewUtilities;   // the renderer's view utilities: a plain object in the original too

// The framework the locator asks for services (RZGetFramework()).
void SetFramework(cIGZFrameWork* framework);
// The city interface source / terrain view source the member calls were made on.
void SetTerrainViewSource(ITerrainViewSource* source);

void AppDirectory(cIGZString& out);          // @ 0x1000404e0
void DataDirectory(cIGZString& out);         // @ 0x100040506 (language folder not included: GetDataDirectory(out, -1))
void PluginDirectory(cIGZString& out);       // @ 0x100040530
void UserDataDirectory(cIGZString& out);     // @ 0x100040556
bool OnAppCreate(cISC4App* app);             // @ 0x10004057c
bool OnAppInit1();                           // @ 0x10004058c
bool OnAppInit2();                           // @ 0x10004094c
bool OnCityCreate(cISC4City* city, ICitySystems* systems);   // @ 0x10004099e
bool OnCityInit();                           // @ 0x1000409ae
bool OnCityShutdown();                       // @ 0x100040e52
bool OnCityDestroy(cISC4City* city);         // @ 0x100040fd0
bool OnView3DCreate(IView3DSystems* view);   // @ 0x100040fec
bool OnView3DInit();                         // @ 0x100040ffc
bool OnView3DShutdown();                     // @ 0x10004107c
cIGZUnknown* TerrainView();                  // @ 0x1000410b0
cIGZUnknown* ViewManager();                  // @ 0x1000410ce
cIGZUnknown* ATM();                          // @ 0x1000410e0  animation tick manager
cIGZUnknown* AEMS();                         // @ 0x100041134  audio event system
cIGZUnknown* NetworkManager();               // @ 0x100041188
cIGZUnknown* OccupantManager();              // @ 0x1003f75a0
SC4ViewUtilities* ViewUtilities();           // @ 0x1003f75b0
cISC4City* City();                           // @ 0x1003f75f0
cIGZUnknown* Terrain();                      // @ 0x1003f78a0
cIGZUnknown* SS();                           // @ 0x1003f7980
cIGZPersistResourceManager* RM();            // @ 0x1003f7990

}  // namespace SL
