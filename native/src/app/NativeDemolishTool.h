// The bulldozer tool on the native application (NativeDemolishTool.cpp).
#pragma once

namespace sc4 { class cSC4View3DWin; class cSC4ViewInputControl; }

namespace native {

void InstallDemolishTool(sc4::cSC4View3DWin& view);   // with the city view, like the place-lot host
sc4::cSC4ViewInputControl* CreateDemolishTool();      // IView3DWinHost::CreateDemolishTool

}  // namespace native
