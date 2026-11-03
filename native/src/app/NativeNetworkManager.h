// The network manager's native side: its view of the network pieces and its road-class tools, through which demolition
// reaches cSC4NetworkTool::DestroyOccupantsAndResolve.
#pragma once
#include <vector>

namespace sc4 { class cSC4NetworkOccupant; }

namespace native {

// Installs the occupant view provider and gives the city's network manager its road-class tools (empty slots only).
void InstallNetworkManagerTools();
// The network manager's GetAllIntersectionPieces for a piece: the other network pieces, one reference each.
void NetworkIntersectionPieces(sc4::cSC4NetworkOccupant* occupant, std::vector<sc4::cSC4NetworkOccupant*>& out);

}  // namespace native
