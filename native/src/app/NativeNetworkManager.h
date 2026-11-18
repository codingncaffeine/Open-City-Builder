// The network manager's native side: its view of the network pieces and its road-class tools, through which demolition
// reaches cSC4NetworkTool::DestroyOccupantsAndResolve.
#pragma once
#include <vector>

namespace sc4 { class cSC4NetworkOccupant; class cSC4NetworkTool; }

namespace native {

// Installs the occupant view provider and gives the city's network manager its road-class tools (empty slots only).
void InstallNetworkManagerTools();
// The pipe tool (cSC4PipeTool): one for the program (its destructor traps), shared by the network manager's demolition
// and the pipe drawing tool. Borrowed.
sc4::cSC4NetworkTool* NativePipeTool();
// The network manager's GetAllIntersectionPieces for a piece: the other network pieces, one reference each.
void NetworkIntersectionPieces(sc4::cSC4NetworkOccupant* occupant, std::vector<sc4::cSC4NetworkOccupant*>& out);

}  // namespace native
