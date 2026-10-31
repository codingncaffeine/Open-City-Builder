// The network drawing tools (roads, rail, pipes, power lines, ...) on the native application: the hosts of the recovered
// cSC4ViewInputControlNetworkTool, cSC4NetworkToolUI and cSC4NetworkTool over the loaded city.
#pragma once
#include <cstdint>

class cISC4City;
namespace sc4 { class cSC4View3DWin; class cSC4ViewInputControl; }

namespace native {

// Installs the hosts for the loaded city.
void InstallNetworkTool(sc4::cSC4View3DWin& view, cISC4City* city);
// cSC4View3DWin's catalog branch: new cSC4ViewInputControlNetworkTool(type), one reference.
sc4::cSC4ViewInputControl* CreateNetworkTool(int type);

}  // namespace native
