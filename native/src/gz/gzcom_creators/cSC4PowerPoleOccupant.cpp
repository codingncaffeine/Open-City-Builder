// GZCOM creator for sc4::cSC4PowerPoleOccupant, wired once the class was recovered.
#include "gz/GZCOM.h"
#include "sc4/PowerPoleOccupant.h"

namespace gzcom {

// The occupant view (cISC4Occupant, where the object's cIGZUnknown sits for the GZCOM callers).
void* GZCOM_CreatecSC4PowerPoleOccupant() { return static_cast<void*>(static_cast<sc4::cISC4Occupant*>(new sc4::cSC4PowerPoleOccupant())); }

}  // namespace gzcom
