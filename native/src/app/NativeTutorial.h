// Tutorials on the native build (see NativeTutorial.cpp).
#pragma once

struct cGZMessage;
namespace sc4 { class cISC4WinCatalogView; class ICityMouseRouter; }

namespace native {

// The hosts of the tutorial system, its tasks and its instruction window (replace the generated stubs).
void InstallTutorialHosts();
// nsSC4TutorialSystem::FilterUICommandMessage for the open city: false when a running tutorial vetoes the UI command.
bool NativeTutorialFilterUiCommand(cGZMessage& message);
// A catalog the city view opened, handed to a running tutorial (cISC4TutorialSystem::HandleCatalogMenu).
void NativeTutorialCatalogOpened(sc4::cISC4WinCatalogView* catalog);
// cISC4TutorialSystem::IsTutorial of the open city (NativeDisasters.cpp).
bool NativeTutorialRunning();
// cSC4ViewInputControlZone::PlaceZone's tutorial gate (i386 0x002db59c): while a running tutorial's current task has a
// region (x0 >= 0), only exactly that rectangle may be zoned, and not by a flood fill; anything else tells the tutorial
// (OnTutorialEvent(1): the zone task's hints and try-again text) and places nothing.
// Kits: the tutorial cheat as typed ("Tutorial next", "Tutorial step N") handed straight to the tutorial system's DoMessage.
bool NativeTutorialCheat(const char* text);
// The city view's mouse router (cSC4View3DWin::GZOnMouseDownL / UpL / Move reach SL::spCity vslot 0x290, the tutorial
// system): null without a tutorial system in the open city.
sc4::ICityMouseRouter* NativeTutorialMouseRouter();
bool NativeTutorialAllowsZone(int x0, int z0, int x1, int z1, bool flood);

}  // namespace native
