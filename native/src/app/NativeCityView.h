// The city view's render stack on the native Metal application: what cSC4View3DWin::DoPreCityInit / DoPostCityInit build when a
// city comes up (the camera, the camera control and the 3D renderer) and the window attaches them.
#pragma once
#include <cstdint>
#include <vector>

namespace gd { class cIGZGDriver; }
namespace gz { class cIGZDrawContext; }
namespace s3d { class cIS3DModelInstance; }
namespace sc4 { class cSC4View3DWin; class cSC43DRender; class cISC4Occupant; }
class cISC4City;

namespace native {

// The city view's renderer (null before the city view exists).
sc4::cSC43DRender* CityRenderer();
// The city camera sends its zoom / rotation messages (cSC4CameraControl::SendPostViewChangeMessage, kind 2) as after a zoom change.
void AnnounceCameraZoomRotation();
// The renderer's ChangeModel(model, change) (vslot 0xf8); false without a city view.
bool CityRendererChangeModel(s3d::cIS3DModelInstance* model, int change);
// Listen for the next city load's saved camera (the view state record) and open the city there; call before LoadCity.
void ListenForSavedView();
// Its view utilities' eye position (+0x44, three floats; zeros without a view) and zoom (+0x14): NativeCityViewQueries.cpp.
const float* CityViewEye();
int CityViewZoom();
int CityViewRotation();   // SC4ViewUtilities +0x18 (i386 +0x10): the view rotation 0..3
// The kit's `cityview`: the view's compass faces, alert border, progress screen and style selector (app/NativeView3DWinHost.cpp).
void NativeCityViewReport();
// The vehicle or pedestrian automata layer is active (NativeCityQueries.cpp).
bool CityAutomataLayersActive();
// Builds the stack for the city that is loaded and attaches it to the view; true when the renderer came up.
// SC4_DEBUG: prints how many occupants of each type the city holds, one line per type ("<label>: type T xN").
void PrintOccupantCensus(cISC4City* city, const char* label);
// The buildings near a ground point with their models and texture sizes (scenario `probe`).
void ProbeBuildingsAt(float x, float z, float radius);
// app/NativeDispatch.cpp: the dispatch tool's drop without a call box (police 0, fire 1) at a world position; the dispatch id or 0.
std::uint32_t NativeDispatchAt(std::uint32_t type, float x, float z);
void NativePedestriansInBox(const float* lo, const float* hi, std::vector<sc4::cISC4Occupant*>& out);   // app/NativeTrafficAutomata.cpp: the pedestrians in a box (one reference each)
void NativeDispatchReport();   // every dispatch record, its vehicle and state
void NativeGeneratorReport();  // every automaton an automata generator made and still holds (app/NativeAutomataGenerator.cpp)
void NativePedestrianReport(int limit);  // the pedestrian manager's count and the first `limit` on screen (app/NativeAutomataManagerHost.cpp)
void NativeIgniteCell(int x, int z, bool force);   // kit command `ignite` (app/NativeDisasters.cpp)
void NativeFireReport();                          // kit command `fires`
void NativePipeBurstStart(int x, int z);           // kit command `pipeburst X Z` (-1 -1: the manager's own search)
void NativePipeBurstReport();                      // kit command `pipebursts`
void NativeLandfillReport();                        // kit command `landfill` (app/NativeLandfill.cpp)
void NativeToxicStart(int cellX, int cellZ);         // kit command `toxic` (app/NativeToxic.cpp)
void NativeToxicReport();                          // kit command `toxics`
void NativePortReport();                           // kit command `ports` (app/NativePorts.cpp)
void NativeWallReport();                           // kit command `walls` (app/NativeLotWalls.cpp)
void NativeStartRiot(int cellX, int cellZ);       // kit command `riot` (app/NativeRiot.cpp)
void NativeRiotReport();                          // kit command `riots`
void NativeQueryReport(std::uint32_t skipUi, int limit);   // the query tool's buildings (querylist kit command; app/NativeQuery.cpp)
void NativeTrailerReport(int limit);  // the trailer vehicles on screen and the cars they pull (app/NativeAutomataGenerator.cpp)
void NativeAircraftReport();          // the aircraft manager's airports, runway markers and aircraft (app/NativeAircraft.cpp)
void NativeTrainReport(int limit);    // track cells, trains and buses, the first engines with their cars (app/NativeTrafficAutomata.cpp)
// The city view's render stack, built as the original's view does: SetCityRenderTarget when the city screen is made,
// PrepareCityRenderStack on DoPreCityInit (before cSC4City::Init: camera, control, 3D render), FinishCityRenderStack on
// DoPostCityInit (after it: the city's size, terrain view, lighting, models, automata, effects, tools, city UI).
void SetCityRenderTarget(sc4::cSC4View3DWin& view, gd::cIGZGDriver* driver, gz::cIGZDrawContext* screen, int viewportWidth, int viewportHeight);
bool PrepareCityRenderStack(cISC4City* city);
bool FinishCityRenderStack(cISC4City* city);

}  // namespace native
