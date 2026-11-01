// One component's Init / Shutdown by its recovered class (NativeCityComponents.cpp dispatches over the list below).
#pragma once

#include "tools/city_probe_plain.h"

class cIGZUnknown;

namespace native {

// The component is the class (dynamic_cast: a city-probe wrapper or a neutral creator object does not match): runs Init (when the class
// has a no-argument Init) or Shutdown and answers true; false when it is not that class or the class has no Init.
template <class T> bool TryCityComponentInit(cIGZUnknown* object, bool init) {
    T* concrete = dynamic_cast<T*>(object);
    // A "plain" class (no COM base) is held by the city in its serializable adapter: its object is the plain view.
    if (!concrete) {
        if (auto* adapter = dynamic_cast<sc4::PlainSerializableAdapter<T>*>(object)) {
            void* raw = nullptr;
            if (adapter->QueryInterface(sc4::kGZIID_PlainObject, &raw)) concrete = static_cast<T*>(raw);
        }
    }
    if (!concrete) return false;
    if (init) {
        if constexpr (requires { concrete->Init(); }) (void)concrete->Init();
        else return false;
    } else {
        if constexpr (requires { concrete->Shutdown(); }) (void)concrete->Shutdown();
    }
    return true;
}

#define SC4_CITY_COMPONENTS(X) X(cSC4Simulator) X(cSC4HistoryWarehouse) X(cSTETerrain) X(cSTESurfaceWater) X(cSC4AutomataScriptSystem) X(cSC4AutomataControllerManager) X(cSC4OccupantManager) X(cSC4ZoneManager) X(cSC4LotConfigurationManager) X(cSC4ZoneDeveloper) X(cSC4LandfillDeveloper) X(cSC4AirportDeveloper) X(cSC4SeaportDeveloper) X(cSC4ParkManager) X(cSC424HourClock) X(cSC4PropManager) X(cSC4NetworkManager) X(cSC4DispatchManager) X(cSC4PropDeveloper) X(cSC4NetworkLotManager) X(cSC4BudgetSimulator) X(cSC4OrdinanceSimulator) X(cSC4NeighborsSimulator) X(cSC4BuildingDevelopmentSimulator) X(cSC4TractDeveloper) X(cSC4DemandSimulator) X(cSC4AuraSimulator) X(cSC4CommercialSimulator) X(cSC4FlammabilitySimulator) X(cSC4IndustrialSimulator) X(cSC4LandValueSimulator) X(cSC4PollutionSimulator) X(cSC4FireProtectionSimulator) X(cSC4PoliceSimulator) X(cSC4WeatherSimulator) X(cSC4MySimAgentSimulator) X(cSC4CivicBuildingSimulator) X(cSC4VehicleManager) X(cSC4PedestrianManager) X(cSC4AircraftManager) X(cSC4WatercraftManager) X(cSC4TrafficNetworkMap) X(cSC4AdvisorSystem) X(cSC4CitySituationManager) X(cSC4TutorialSystem) X(cSC4CityDetailManager)
#define SC4_DECLARE_CITY_COMPONENT(c) bool CityComponentInit_##c(cIGZUnknown* object, bool init);
SC4_CITY_COMPONENTS(SC4_DECLARE_CITY_COMPONENT)
#undef SC4_DECLARE_CITY_COMPONENT

}  // namespace native
