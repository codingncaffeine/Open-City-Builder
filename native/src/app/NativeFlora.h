// The flora simulator's services on the native application (the species tables, planting) and the city's simulator.
#pragma once

#include <cstdint>

namespace sc4 { class cSC4FloraSimulator; class cSC4ViewInputControl; }

namespace native {

void InstallNativeFlora();                   // the IFloraSimulatorHost + IFloraHost the city's flora uses
sc4::cSC4FloraSimulator* CityFloraSimulator();   // the city's cSC4FloraSimulator with its species loaded, or null
sc4::cSC4ViewInputControl* CreateFloraTool(std::uint32_t floraType);   // the mayor-mode tree tool (no reference: the view takes it)
void WeatherPostCityInit();   // NativeWeather.cpp: the weather simulator's post-city-init pass (moisture, temperature)

}  // namespace native
