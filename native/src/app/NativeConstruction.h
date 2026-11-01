// The construction occupant on the native application: NativeConstruction.cpp.
#pragma once

namespace sc4 { class cSC4Lot; }

namespace native {

void InstallConstructionOccupantHost();                 // before the city's components are created
void StartNativeConstructionOccupant(sc4::cSC4Lot* lot);   // the lot developer's StartConstruction with `construct`

}  // namespace native
