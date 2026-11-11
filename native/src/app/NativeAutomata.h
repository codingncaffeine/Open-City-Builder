// The automata (vehicles, pedestrians, aircraft ...) on the native city: the services the recovered cSC4Automaton asks of the engine.
#pragma once
#include <vector>

class cIGZUnknown;

namespace sc4 { class cSC43DRender; class cSC4City; }

namespace native {

// Installs the IAutomatonHost; the renderer and city can be given later (models made before are inserted when the renderer arrives).
void InstallAutomataHost();
void SetAutomataRenderer(sc4::cSC43DRender* render, sc4::cSC4City* city);

void PostCityInitToAutomataManagers(sc4::cSC4City* city);   // after the renderer is attached: starts the restored automata
void TickAutomata();                                        // once per frame: advances the started automata by the elapsed time
void AnnounceCameraScroll(bool begin);                      // a camera move from outside the scroll path, announced as a scroll (begin, then end)
std::vector<void*> LoadedAutomataOf(cIGZUnknown* manager);  // NativeAutomataList.cpp

}  // namespace native
