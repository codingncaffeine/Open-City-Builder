// GZCOM creator for sc4::cSC4PropOccupantTerrainDecal (class 0x2977aa48), wired once the class was recovered: saves that hold
// terrain decals (burnt lots, settled lava) read them back through it.
#include "gz/GZCOM.h"
#include "sc4/PropOccupantTerrainDecal.h"

namespace gzcom {

void* GZCOM_CreatecSC4PropOccupantTerrainDecal() { return static_cast<void*>(new sc4::cSC4PropOccupantTerrainDecal()); }

}  // namespace gzcom
