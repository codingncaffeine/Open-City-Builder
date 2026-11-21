// GZCOM creator for sc4::cSC4NetworkBridgeOccupant, wired once the class was recovered.
#include "gz/GZCOM.h"
#include "sc4/NetworkBridgeOccupant.h"

namespace gzcom {

void* GZCOM_CreatecSC4NetworkBridgeOccupant() { return static_cast<void*>(new sc4::cSC4NetworkBridgeOccupant()); }

}  // namespace gzcom
