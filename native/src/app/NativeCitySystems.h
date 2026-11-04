// The service locator's city-phase table (SL::spTerrain, SL::spLotManager, the simulators ...) filled from the loaded city's
// components: what the original does in OnCityCreate / OnCityInit.
#pragma once

class cISC4City;
namespace sc4 { class cSC4View3DWin; class cSC43DRender; }
namespace native {
void BindCityServices(cISC4City* city);
void BindSimGridCity(cISC4City* city);   // only cSC4SimGrid's city dimensions (part of BindCityServices)
// SL::spView3D / spView3DWin / spRenderer / spLightingManager (OnView3DCreate + OnView3DInit); the model maker and view utilities stay pending.
void BindView3DServices(sc4::cSC4View3DWin* view, sc4::cSC43DRender* render);
// The city view bound last (null before a city view exists).
sc4::cSC4View3DWin* CityView3DWin();
// Its view level (1 above ground, 2 underground; 1 before a city view exists).
int CityViewLevel();
}
