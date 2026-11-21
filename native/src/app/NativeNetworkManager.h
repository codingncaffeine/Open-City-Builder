// The network manager's native side: its host (the services the original's cSC4NetworkManager reaches directly), its view of
// the network pieces and its road-class tools, through which demolition reaches cSC4NetworkTool::DestroyOccupantsAndResolve.
#pragma once
#include <vector>

namespace sc4 { class cSC4NetworkOccupant; class cSC4NetworkTool; }

namespace native {

// Installs the network manager's host, its occupant view provider and the path info factory (before a city loads: the
// manager's constructor reads its ini and its Init registers its messages and makes its tools through the host).
void InstallNetworkManagerHost();
// The read-only kit verb `netmanager`: what the host answers (message server, ini, language flag, night) and the city manager's
// remap tables, tools and filters.
void PrintNetworkManagerHost();
// The pipe tool (cSC4PipeTool): one for the program (its destructor traps), shared by the network manager's demolition
// and the pipe drawing tool. Borrowed.
sc4::cSC4NetworkTool* NativePipeTool();
// The network manager's GetAllIntersectionPieces for a piece: the other network pieces, one reference each.
void NetworkIntersectionPieces(sc4::cSC4NetworkOccupant* occupant, std::vector<sc4::cSC4NetworkOccupant*>& out);

}  // namespace native
