// GZCOM creator for sc4::cSC4SubwayConnectionOccupant, wired once the class was recovered.
#include "gz/GZCOM.h"
#include "sc4/SubwayConnectionOccupant.h"

namespace gzcom {

void* GZCOM_CreatecSC4SubwayConnectionOccupant() { return static_cast<void*>(new sc4::cSC4SubwayConnectionOccupant()); }

}  // namespace gzcom
