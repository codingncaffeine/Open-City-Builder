#include <cstdint>

#include "sc4/CameraControl.h"
#include "sc4/SpecialVehicles.h"
#include "sc4/TrailerVehicle.h"
#include "sc4/Train.h"
#include "sc4/Vehicle.h"
#include "tools/city_probe_plain.h"

namespace sc4 {
ITrafficAutomatonHost* NewStubITrafficAutomatonHost();   // app/hosts/IfaceITrafficAutomatonHost.cpp
IAutomatonHost* NewStubIAutomatonHost();                  // app/hosts/IfaceIAutomatonHost.cpp
IVehicleHost* NewStubIVehicleHost();                      // app/hosts/IfaceIVehicleHost.cpp
extern ITrafficAutomatonHost* g_loadTrafficHost;          // tools/city_probe_hostglobals.cpp; the game sets the native one
extern IVehicleHost* g_loadVehicleHost;                    // tools/city_probe_hostglobals.cpp; the game sets the native one
}  // namespace sc4

// The vehicle classes take the same three hosts: the game's when it has set them, the stubs in the probe.
template <class T>
static cIGZSerializable* NewVehicle() {
    auto* object = new T(sc4::g_loadTrafficHost ? sc4::g_loadTrafficHost : sc4::NewStubITrafficAutomatonHost(),
                         sc4::AutomatonHost() ? sc4::AutomatonHost() : sc4::NewStubIAutomatonHost(),
                         sc4::g_loadVehicleHost ? sc4::g_loadVehicleHost : sc4::NewStubIVehicleHost());
    object->AddRef();
    return object;
}

cIGZSerializable* CreateCityProbeSerializable5(std::uint32_t classId) {
    switch (classId) {
        case sc4::kBusGZCLSID: return NewVehicle<sc4::cSC4Bus>();
        case sc4::kMayorLimoGZCLSID: return NewVehicle<sc4::cSC4MayorLimo>();
        case sc4::kTrainCarGZCLSID: return NewVehicle<sc4::cSC4TrainCar>();
        case sc4::kTrainGZCLSID: return NewVehicle<sc4::cSC4Train>();
        case sc4::kMassTransitTrainGZCLSID: return NewVehicle<sc4::cSC4MassTransitTrain>();
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
        case sc4::kCarjackedVehicleGZCLSID: {   // the crime layer's carjacked car
            auto* object = new sc4::cSC4CarjackedVehicle(sc4::g_loadTrafficHost ? sc4::g_loadTrafficHost : sc4::NewStubITrafficAutomatonHost(), sc4::AutomatonHost() ? sc4::AutomatonHost() : sc4::NewStubIAutomatonHost(), sc4::g_loadVehicleHost ? sc4::g_loadVehicleHost : sc4::NewStubIVehicleHost());
            object->AddRef();
            return object;
        }
        case sc4::kGetawayVanGZCLSID: {   // the crime layer's getaway van
            auto* object = new sc4::cSC4GetawayVan(sc4::g_loadTrafficHost ? sc4::g_loadTrafficHost : sc4::NewStubITrafficAutomatonHost(), sc4::AutomatonHost() ? sc4::AutomatonHost() : sc4::NewStubIAutomatonHost(), sc4::g_loadVehicleHost ? sc4::g_loadVehicleHost : sc4::NewStubIVehicleHost());
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
