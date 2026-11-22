// GZCOM creator for sc4::cSC4PropOccupantBurntBuilding (class 0xc9efb016), wired once the class was recovered: saves that hold
// burnt lots read their charred floors back through it.
#include "gz/GZCOM.h"
#include "sc4/PropOccupantBurntBuilding.h"

namespace gzcom {

void* GZCOM_CreatecSC4PropOccupantBurntBuilding() { return static_cast<void*>(static_cast<sc4::cSC4PropOccupant*>(new sc4::cSC4PropOccupantBurntBuilding())); }

}  // namespace gzcom
