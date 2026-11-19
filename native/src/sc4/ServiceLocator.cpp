#include "sc4/ServiceLocator.h"

#include "gz/GZApp.h"
#include "gz/cIGZFrameWork.h"

namespace SL {

cISC4App* spSC4 = nullptr;
std::uint8_t* spPreferences = nullptr;
cIGZUnknown *spCCM = nullptr, *spCommandServer = nullptr, *spRenderProperties = nullptr, *spGF = nullptr,
    *spStringDetokenizer = nullptr, *spWinLocationSaver = nullptr, *spGlyphTextureManager = nullptr, *spLua = nullptr,
    *spGS = nullptr, *spGS2 = nullptr, *spGDriver = nullptr, *spFeatureManager = nullptr, *spRDD = nullptr, *spSS = nullptr,
    *spTextureBindingFactory = nullptr;
cIGZPersistResourceManager* spRM = nullptr;
cIGZMessageServer* spMS = nullptr;
cIGZMessageServer2* spMS2 = nullptr;
IWinSC4App* spWinSC4App = nullptr;
cISC4City* spCity = nullptr;
ICitySystems* spCitySystems = nullptr;
cIGZUnknown *spTerrain = nullptr, *spTerrainMap = nullptr, *spTerrainView = nullptr, *spSurfaceWater = nullptr, *spDemolition = nullptr,
    *spDispatchManager = nullptr, *spLotManager = nullptr, *spOccupantManager = nullptr, *spPropManager = nullptr,
    *spZoneManager = nullptr, *spLotConfigurationManager = nullptr, *spEffectsManager = nullptr, *spSimulator = nullptr,
    *spLotDeveloper = nullptr, *spTractDeveloper = nullptr, *spLandfillDeveloper = nullptr, *spAirportDeveloper = nullptr,
    *spSeaportDeveloper = nullptr, *spAdvisorSystem = nullptr, *spTutorialSystem = nullptr, *spDisasterLayer = nullptr,
    *spParkManager = nullptr, *spVehicleManager = nullptr, *spPedestrianManager = nullptr, *spAircraftManager = nullptr,
    *spWatercraftManager = nullptr, *spTrafficNetworkMap = nullptr, *spNetworkLotManager = nullptr, *spAuraSim = nullptr,
    *spBudgetSim = nullptr, *spBuildingDevelopmentSim = nullptr, *spCommercialSim = nullptr, *spCrimeSim = nullptr,
    *spDemandSim = nullptr, *spFireProtectionSim = nullptr, *spFlammabilitySim = nullptr, *spFloraSim = nullptr,
    *spIndustrialSim = nullptr, *spLandValueSim = nullptr, *spNeighborsSim = nullptr, *spOrdinanceSim = nullptr,
    *spPlumbingSim = nullptr, *spPoliceSim = nullptr, *spPollutionSim = nullptr, *spPowerSim = nullptr,
    *spResidentialSim = nullptr, *spTrafficSim = nullptr, *spWeatherSim = nullptr, *spZoneDeveloper = nullptr,
    *spCivicBuildingSim = nullptr, *spPropDeveloper = nullptr, *sp24HourClock = nullptr, *spHistoryWarehouse = nullptr;
IView3DSystems* spView3D = nullptr;
SC4ViewUtilities* spViewUtilities = nullptr;
cIGZUnknown *spView3DWin = nullptr, *spModelMaker = nullptr, *spRenderer = nullptr,
    *spLightingManager = nullptr;

namespace {
cIGZFrameWork* g_framework = nullptr;
ITerrainViewSource* g_terrainViewSource = nullptr;

// RZGetFramework()->GetSystemService(service, iid, &out): the object (reference dropped again), or null.
cIGZUnknown* Service(std::uint32_t serviceId, std::uint32_t iid) {
    if (!g_framework) return nullptr;
    cIGZUnknown* object = nullptr;
    g_framework->GetSystemService(serviceId, iid, reinterpret_cast<void**>(&object));
    return object;
}
// the cRZSysServPtr that went out of scope released its reference; the locator keeps the bare pointer
cIGZUnknown* ServiceNonOwning(std::uint32_t serviceId, std::uint32_t iid) {
    cIGZUnknown* object = Service(serviceId, iid);
    if (object) object->Release();
    return object;
}
}  // namespace

void SetFramework(cIGZFrameWork* framework) { g_framework = framework; }
void SetTerrainViewSource(ITerrainViewSource* source) { g_terrainViewSource = source; }

void AppDirectory(cIGZString& out) { if (spSC4) spSC4->GetAppDirectory(out); }
void DataDirectory(cIGZString& out) { if (spSC4) spSC4->GetDataDirectory(out, -1); }
void PluginDirectory(cIGZString& out) { if (spSC4) spSC4->GetPluginDirectory(out); }
void UserDataDirectory(cIGZString& out) { if (spSC4) spSC4->GetUserDataDirectory(out); }

bool OnAppCreate(cISC4App* app) {
    spSC4 = app;
    return true;
}

bool OnAppInit1() {
    spCCM = spSC4->GetCheatCodeManager();
    spCommandServer = g_framework ? ServiceNonOwning(0xeb903a32, 0xcb90253a) : nullptr;
    spRenderProperties = spSC4->GetRenderProperties();
    spGF = spSC4->GetGimexFactory();
    spPreferences = spSC4->GetPreferences();
    spStringDetokenizer = spSC4->GetStringDetokenizer();
    spWinLocationSaver = spSC4->GetWinLocationSaver();
    spGlyphTextureManager = spSC4->GetGlyphTextureManager();
    spLua = spSC4->GetLuaInterpreter();

    // the graphics system: its second interface is looked up but not kept referenced
    cIGZUnknown* graphics = g_framework ? Service(0xc416025c, 0x0073283c) : nullptr;
    spGS = graphics;
    if (graphics) {
        cIGZUnknown* second = nullptr;
        if (graphics->QueryInterface(0xc47b747c, reinterpret_cast<void**>(&second))) {
            spGS2 = second;
            second->Release();
        }
    }
    // spGS2->GetGDriver() would follow; the graphics-system interface is not recovered yet, so the driver is set by
    // whoever owns the driver (SetGraphicsDriver)

    if (auto* rm = ServiceNonOwning(0x056b906e, 0x656b8efc)) spRM = reinterpret_cast<cIGZPersistResourceManager*>(rm);
    spFeatureManager = spSC4->GetFeatureManager();
    if (auto* rdd = ServiceNonOwning(0x8414a2c2, 0x6054a2fb)) spRDD = rdd;
    if (auto* sound = ServiceNonOwning(0x23cecba0, 0x106077d2)) spSS = sound;
    if (auto* ms = ServiceNonOwning(0x64063387, 0xc1085722)) spMS = reinterpret_cast<cIGZMessageServer*>(ms);
    if (auto* ms2 = ServiceNonOwning(0x04fa845b, 0x652294c7)) spMS2 = reinterpret_cast<cIGZMessageServer2*>(ms2);
    if (g_framework) {
        if (auto* factory = ServiceNonOwning(0x1ac0e11a, 0xfac0e219)) spTextureBindingFactory = factory;
    }
    return true;
}

// The window object of the application: the framework's application's main-window slot.
bool OnAppInit2() {
    cIGZFrameWork* framework = g_framework ? g_framework : nullptr;
    if (!framework) return true;
    // the application object is the cSC4App behind the framework's cIGZApp view (+0x48); its window slot is +0x88
    if (auto* app = dynamic_cast<IApplicationWindowSource*>(framework->Application())) spWinSC4App = app->WinSC4App();
    return true;
}

bool OnCityCreate(cISC4City* city, ICitySystems* systems) {
    spCity = city;
    spCitySystems = systems;
    return true;
}

bool OnCityInit() {
    ICitySystems& city = *spCitySystems;
    spTerrain = city.Terrain();
    spTerrainMap = spTerrain;
    spSurfaceWater = city.SurfaceWater();
    spDemolition = city.Demolition();
    spDispatchManager = city.DispatchManager();
    spLotManager = city.LotManager();
    spOccupantManager = city.OccupantManager();
    spPropManager = city.PropManager();
    spZoneManager = city.ZoneManager();
    spLotConfigurationManager = city.LotConfigurationManager();
    spEffectsManager = city.EffectsManager();
    spLotDeveloper = city.LotDeveloper();
    spTractDeveloper = city.TractDeveloper();
    spLandfillDeveloper = city.LandfillDeveloper();
    spAirportDeveloper = city.AirportDeveloper();
    spSeaportDeveloper = city.SeaportDeveloper();
    spAdvisorSystem = city.AdvisorSystem();
    spTutorialSystem = city.TutorialSystem();
    spDisasterLayer = city.DisasterLayer();
    spParkManager = city.ParkManager();
    spVehicleManager = city.VehicleManager();
    spPedestrianManager = city.PedestrianManager();
    spAircraftManager = city.AircraftManager();
    spWatercraftManager = city.WatercraftManager();
    spTrafficNetworkMap = city.TrafficNetworkMap();
    spNetworkLotManager = city.NetworkLotManager();
    spSimulator = city.Simulator();
    spAuraSim = city.AuraSim();
    spBudgetSim = city.BudgetSim();
    spBuildingDevelopmentSim = city.BuildingDevelopmentSim();
    spCommercialSim = city.CommercialSim();
    spCrimeSim = city.CrimeSim();
    spDemandSim = city.DemandSim();
    spFireProtectionSim = city.FireProtectionSim();
    spFlammabilitySim = city.FlammabilitySim();
    spFloraSim = city.FloraSim();
    spIndustrialSim = city.IndustrialSim();
    spLandValueSim = city.LandValueSim();
    spNeighborsSim = city.NeighborsSim();
    spOrdinanceSim = city.OrdinanceSim();
    spPlumbingSim = city.PlumbingSim();
    spPoliceSim = city.PoliceSim();
    spPollutionSim = city.PollutionSim();
    spPowerSim = city.PowerSim();
    spResidentialSim = city.ResidentialSim();
    spTrafficSim = city.TrafficSim();
    spWeatherSim = city.WeatherSim();
    spZoneDeveloper = city.ZoneDeveloper();
    spCivicBuildingSim = city.CivicBuildingSim();
    spPropDeveloper = city.PropDeveloper();
    sp24HourClock = city.Clock24Hour();
    spHistoryWarehouse = city.HistoryWarehouse();
    return true;
}

bool OnCityShutdown() {
    for (cIGZUnknown** slot : {&spSurfaceWater, &spTerrain, &spTerrainMap, &spTerrainView, &spDemolition, &spDispatchManager,
                               &spLotManager, &spOccupantManager, &spPropManager, &spZoneManager, &spLotConfigurationManager,
                               &spEffectsManager, &spSimulator, &spLotDeveloper, &spTractDeveloper, &spLandfillDeveloper,
                               &spAirportDeveloper, &spSeaportDeveloper, &spAdvisorSystem, &spTutorialSystem, &spDisasterLayer,
                               &spParkManager, &spVehicleManager, &spPedestrianManager, &spAircraftManager, &spWatercraftManager,
                               &spTrafficNetworkMap, &spNetworkLotManager, &spAuraSim, &spBudgetSim, &spBuildingDevelopmentSim,
                               &spCommercialSim, &spCrimeSim, &spDemandSim, &spFireProtectionSim, &spFlammabilitySim, &spFloraSim,
                               &spIndustrialSim, &spLandValueSim, &spNeighborsSim, &spOrdinanceSim, &spPlumbingSim, &spPoliceSim,
                               &spPollutionSim, &spPowerSim, &spResidentialSim, &spTrafficSim, &spWeatherSim, &spZoneDeveloper,
                               &spCivicBuildingSim, &spPropDeveloper, &sp24HourClock, &spHistoryWarehouse})
        *slot = nullptr;
    return true;
}

bool OnCityDestroy(cISC4City* city) {
    if (spCity == city) spCity = nullptr;
    return true;
}

bool OnView3DCreate(IView3DSystems* view) {
    spView3D = view;
    return true;
}

bool OnView3DInit() {
    bool haveWindow = spWinSC4App != nullptr;
    if (haveWindow) {
        spView3D = spWinSC4App->View3D();
        spView3DWin = spView3D->View3DWin();
        spModelMaker = spView3D->ModelMaker();
        spRenderer = spView3D->Renderer();
        spViewUtilities = spView3D->RendererViewUtilities();
        spLightingManager = spView3D->RendererLightingManager();
    }
    return haveWindow;
}

bool OnView3DShutdown() {
    spView3D = nullptr;
    spView3DWin = nullptr;
    spModelMaker = nullptr;
    spRenderer = nullptr;
    spViewUtilities = nullptr;
    spLightingManager = nullptr;
    return true;
}

cIGZUnknown* TerrainView() { return spTerrain && g_terrainViewSource ? g_terrainViewSource->TerrainView() : nullptr; }
cIGZUnknown* ViewManager() { return spView3D->ViewManager(); }
cIGZUnknown* ATM() { return g_framework ? ServiceNonOwning(0x86ad10ec, 0x86ad10ed) : nullptr; }
cIGZUnknown* AEMS() { return g_framework ? ServiceNonOwning(0xac1fae31, 0x8c1faeb7) : nullptr; }
cIGZUnknown* NetworkManager() { return spCity && spCitySystems ? spCitySystems->NetworkManager() : nullptr; }
cIGZUnknown* OccupantManager() { return spOccupantManager; }
SC4ViewUtilities* ViewUtilities() { return spViewUtilities; }
cISC4City* City() { return spCity; }
cIGZUnknown* Terrain() { return spTerrain; }
cIGZUnknown* SS() { return spSS; }
cIGZPersistResourceManager* RM() { return spRM; }

}  // namespace SL
