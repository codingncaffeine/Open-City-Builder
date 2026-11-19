// The "Obliterate City" scene's host on the native application (NativeObliterate.cpp).
#pragma once

namespace sc4 { class cSC4ViewInputControl; }

namespace native {
void InstallObliterateCityHost();                       // sc4/ObliterateCity.h's IObliterateCityHost
sc4::cSC4ViewInputControl* CreateObliterateCityTool();   // new cSC4ObliterateCity (the city view's obliterate command)
}  // namespace native
