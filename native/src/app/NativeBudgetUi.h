// The budget (balance-sheet) panel on the native application: the recovered cSC4BudgetUIWinProc behind the interface
// cSC4View3DWin::AddGUI keeps it as, with the services it reaches for.
#pragma once
#include <cstdint>

class cIGZWin;
class cIGZUnknown;
namespace sc4 { class IBudgetUIWindow; }

namespace native {

void SetBudgetViewWindow(cIGZWin* view);              // the 3D view window the panel is created in (before AddGUI)
sc4::IBudgetUIWindow* CreateBudgetUiWindow();         // one reference
void InstallLanguageFormatter();                      // RZ::LM()'s number / money format, there from boot (the region signposts use it)
bool BudgetUiOpen(cIGZUnknown* window);
void BudgetUiRefresh(cIGZUnknown* window);

}  // namespace native
