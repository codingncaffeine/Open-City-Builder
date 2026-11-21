// GZCOM creator for sc4::cSC4NetworkTunnelOccupant, wired once the class was recovered.
#include "gz/GZCOM.h"
#include "sc4/NetworkTunnelOccupant.h"

namespace gzcom {

void* GZCOM_CreatecSC4NetworkTunnelOccupant() { return static_cast<void*>(new sc4::cSC4NetworkTunnelOccupant()); }

}  // namespace gzcom
