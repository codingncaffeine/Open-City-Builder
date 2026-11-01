// The city view's render stack on the native Metal application: what cSC4View3DWin::DoPreCityInit / DoPostCityInit build when a
// city comes up (the camera, the camera control and the 3D renderer) and the window attaches them.
#pragma once

namespace gd { class cIGZGDriver; }
namespace gz { class cIGZDrawContext; }
namespace sc4 { class cSC4View3DWin; class cSC43DRender; }
class cISC4City;

namespace native {

// The city view's renderer (null before the city view exists).
sc4::cSC43DRender* CityRenderer();
// Its view utilities' eye position (+0x44, three floats; zeros without a view) and zoom (+0x14): NativeCityViewQueries.cpp.
const float* CityViewEye();
int CityViewZoom();
// The vehicle or pedestrian automata layer is active (NativeCityQueries.cpp).
bool CityAutomataLayersActive();
// Builds the stack for the city that is loaded and attaches it to the view; true when the renderer came up.
// SC4_DEBUG: prints how many occupants of each type the city holds, one line per type ("<label>: type T xN").
void PrintOccupantCensus(cISC4City* city, const char* label);
bool AttachCityRenderStack(sc4::cSC4View3DWin& view, cISC4City* city, gd::cIGZGDriver* driver, gz::cIGZDrawContext* screen, int viewportWidth, int viewportHeight);

}  // namespace native
