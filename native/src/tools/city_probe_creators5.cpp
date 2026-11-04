#include <cstdint>

#include "sc4/CameraControl.h"
#include "sc4/TrailerVehicle.h"
#include "sc4/Vehicle.h"
#include "tools/city_probe_plain.h"

namespace sc4 {
ITrafficAutomatonHost* NewStubITrafficAutomatonHost();   // app/hosts/IfaceITrafficAutomatonHost.cpp
IAutomatonHost* NewStubIAutomatonHost();                  // app/hosts/IfaceIAutomatonHost.cpp
IVehicleHost* NewStubIVehicleHost();                      // app/hosts/IfaceIVehicleHost.cpp
extern ITrafficAutomatonHost* g_loadTrafficHost;          // tools/city_probe_hostglobals.cpp; the game sets the native one
extern IVehicleHost* g_loadVehicleHost;                    // tools/city_probe_hostglobals.cpp; the game sets the native one
}  // namespace sc4

cIGZSerializable* CreateCityProbeSerializable5(std::uint32_t classId) {
    switch (classId) {
        case 0x896e714a: {
            auto* object = new sc4::cSC4Vehicle(sc4::g_loadTrafficHost ? sc4::g_loadTrafficHost : sc4::NewStubITrafficAutomatonHost(), sc4::AutomatonHost() ? sc4::AutomatonHost() : sc4::NewStubIAutomatonHost(), sc4::g_loadVehicleHost ? sc4::g_loadVehicleHost : sc4::NewStubIVehicleHost());
            object->AddRef();
            return object;
        }
        case 0x4a5b69ed: {   // cSC4TrailerVehicle: a vehicle pulling trailer cars (its leader refs point at it)
            auto* object = new sc4::cSC4TrailerVehicle(sc4::g_loadTrafficHost ? sc4::g_loadTrafficHost : sc4::NewStubITrafficAutomatonHost(), sc4::AutomatonHost() ? sc4::AutomatonHost() : sc4::NewStubIAutomatonHost(), sc4::g_loadVehicleHost ? sc4::g_loadVehicleHost : sc4::NewStubIVehicleHost());
            object->AddRef();
            return object;
        }
        case 0xc9c628ec: {   // cSC4CameraControl: the view state record's saved camera (target, zoom, rotation)
            auto* object = new sc4::cSC4CameraControl();
            object->AddRef();
            return object;
        }
        default: return nullptr;
    }
}
