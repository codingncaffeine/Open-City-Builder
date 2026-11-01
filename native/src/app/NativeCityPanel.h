// The city toolbar's gauges: RCI demand bars and opinion polls (NativeCityPanel.cpp).
#pragma once

namespace sc4 { class cSC4View3DWin; }

namespace native {

void InstallCityPanelHosts();                       // once, before any city UI is built
void ConfigureCityPanels(sc4::cSC4View3DWin& view); // after the city UI exists (DoPostCityInit's gauge set-up)
void UpdateMayorRatingBar(sc4::cSC4View3DWin& view); // the view's monthly 0x66956816 (UpdateMayorRatingBar)

}  // namespace native
