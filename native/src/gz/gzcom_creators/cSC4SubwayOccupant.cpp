// GZCOM creator for sc4::cSC4SubwayOccupant, wired once the class was recovered.
#include "gz/GZCOM.h"
#include "sc4/SubwayOccupant.h"

namespace gzcom {

void* GZCOM_CreatecSC4SubwayOccupant() { return static_cast<void*>(new sc4::cSC4SubwayOccupant()); }

}  // namespace gzcom
