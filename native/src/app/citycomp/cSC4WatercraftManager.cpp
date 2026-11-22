// The watercraft manager component (app/NativeWatercraft.cpp) is a cSC4AutomataManager whose virtual Init / Shutdown run the
// recovered cSC4WatercraftManager, as the original's subclass does.
#include "app/citycomp/CityComponentInit.h"
#include "sc4/AutomataManager.h"

namespace native {
bool CityComponentInit_cSC4WatercraftManager(cIGZUnknown* object, bool init) { return TryCityComponentInit<sc4::cSC4AutomataManager>(object, init); }
}  // namespace native
