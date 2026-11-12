// The city view's render stack on the native Metal application: what cSC4View3DWin::DoPreCityInit / DoPostCityInit build when a
// city comes up (the camera, the camera control and the 3D renderer) and the window attaches them.
#pragma once
#include <cstdint>

namespace gd { class cIGZGDriver; }
namespace gz { class cIGZDrawContext; }
namespace sc4 { class cSC4View3DWin; class cSC43DRender; }
class cISC4City;

namespace native {

// The city view's renderer (null before the city view exists).
sc4::cSC43DRender* CityRenderer();
// Listen for the next city load's saved camera (the view state record) and open the city there; call before LoadCity.
void ListenForSavedView();
// Its view utilities' eye position (+0x44, three floats; zeros without a view) and zoom (+0x14): NativeCityViewQueries.cpp.
const float* CityViewEye();
int CityViewZoom();
int CityViewRotation();   // SC4ViewUtilities +0x18 (i386 +0x10): the view rotation 0..3
// The vehicle or pedestrian automata layer is active (NativeCityQueries.cpp).
bool CityAutomataLayersActive();
// Builds the stack for the city that is loaded and attaches it to the view; true when the renderer came up.
// SC4_DEBUG: prints how many occupants of each type the city holds, one line per type ("<label>: type T xN").
void PrintOccupantCensus(cISC4City* city, const char* label);
// The buildings near a ground point with their models and texture sizes (scenario `probe`).
void ProbeBuildingsAt(float x, float z, float radius);
// app/NativeDispatch.cpp: the dispatch tool's drop without a call box (police 0, fire 1) at a world position; the dispatch id or 0.
std::uint32_t NativeDispatchAt(std::uint32_t type, float x, float z);
void NativeDispatchReport();   // every dispatch record, its vehicle and state
void NativeGeneratorReport();  // every automaton an automata generator made and still holds (app/NativeAutomataGenerator.cpp)
void NativePedestrianReport(int limit);  // the pedestrian manager's count and the first `limit` on screen (app/NativeAutomataManagerHost.cpp)
void NativeIgniteCell(int x, int z, bool force);   // kit command `ignite` (app/NativeDisasters.cpp)
void NativeFireReport();                          // kit command `fires`
void NativeStartRiot(int cellX, int cellZ);       // kit command `riot` (app/NativeRiot.cpp)
void NativeRiotReport();                          // kit command `riots`
void NativeQueryReport(std::uint32_t skipUi, int limit);   // the query tool's buildings (querylist kit command; app/NativeQuery.cpp)
void NativeTrailerReport(int limit);  // the trailer vehicles on screen and the cars they pull (app/NativeAutomataGenerator.cpp)
bool AttachCityRenderStack(sc4::cSC4View3DWin& view, cISC4City* city, gd::cIGZGDriver* driver, gz::cIGZDrawContext* screen, int viewportWidth, int viewportHeight);

}  // namespace native
