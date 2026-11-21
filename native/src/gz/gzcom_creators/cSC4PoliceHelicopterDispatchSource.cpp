// GZCOM creator for sc4::cSC4PoliceHelicopterDispatchSource, wired once the class was recovered (operator new + the constructor; the
// caller takes the first reference of the primary, cISC4DispatchSource, view).
#include "gz/GZCOM.h"
#include "sc4/AircraftDispatchSource.h"

namespace gzcom {

void* GZCOM_CreatecSC4PoliceHelicopterDispatchSource() {
    return static_cast<void*>(static_cast<cIGZUnknown*>(static_cast<sc4::IDispatchSource*>(new sc4::cSC4PoliceHelicopterDispatchSource())));
}

}  // namespace gzcom
