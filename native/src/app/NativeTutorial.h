// Tutorials on the native build (see NativeTutorial.cpp).
#pragma once

struct cGZMessage;
namespace sc4 { class cISC4WinCatalogView; }

namespace native {

// The hosts of the tutorial system, its tasks and its instruction window (replace the generated stubs).
void InstallTutorialHosts();
// nsSC4TutorialSystem::FilterUICommandMessage for the open city: false when a running tutorial vetoes the UI command.
bool NativeTutorialFilterUiCommand(cGZMessage& message);
// A catalog the city view opened, handed to a running tutorial (cISC4TutorialSystem::HandleCatalogMenu).
void NativeTutorialCatalogOpened(sc4::cISC4WinCatalogView* catalog);
// cISC4TutorialSystem::IsTutorial of the open city (NativeDisasters.cpp).
bool NativeTutorialRunning();

}  // namespace native
