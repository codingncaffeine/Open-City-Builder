// The emergency vehicles a city file stores (patrol car, fire truck, ambulance). Own translation unit: sc4/AircraftManager.h and the
// vehicle headers both declare names from AutomataManager.h.
#include <cstdint>

#include "platform/Unwired.h"
#include "sc4/Ambulance.h"
#include "sc4/FireTruck.h"
#include "sc4/PatrolCar.h"
#include "tools/city_probe_plain.h"

namespace sc4 {
ITrafficAutomatonHost* NewStubITrafficAutomatonHost();   // app/hosts/IfaceITrafficAutomatonHost.cpp
IAutomatonHost* NewStubIAutomatonHost();                  // app/hosts/IfaceIAutomatonHost.cpp
IVehicleHost* NewStubIVehicleHost();                      // app/hosts/IfaceIVehicleHost.cpp
extern ITrafficAutomatonHost* g_loadTrafficHost;          // tools/city_probe_creators5.cpp, set by NativeTrafficHosts.cpp
extern IVehicleHost* g_loadVehicleHost;
extern IEmergencyVehicleHost* g_loadEmergencyHost;   // the native game's (app/NativeDispatch.cpp); null in the probe
extern IFireTruckHost* g_loadFireTruckHost;
extern IPatrolCarHost* g_loadPatrolCarHost;
}  // namespace sc4

// The native game answers IEmergencyVehicleHost / IPatrolCarHost / IFireTruckHost (app/NativeDispatch.cpp); the probe has none, so a
// loaded vehicle there keeps its record but cannot dispatch.
template <class T, class... Extra>
static cIGZSerializable* NewEmergencyVehicle(Extra... extra) {
    auto* object = new T(sc4::g_loadTrafficHost ? sc4::g_loadTrafficHost : sc4::NewStubITrafficAutomatonHost(),
                         sc4::AutomatonHost() ? sc4::AutomatonHost() : sc4::NewStubIAutomatonHost(),
                         sc4::g_loadVehicleHost ? sc4::g_loadVehicleHost : sc4::NewStubIVehicleHost(), sc4::g_loadEmergencyHost, extra...);
    object->AddRef();
    return object;
}

cIGZSerializable* CreateCityProbeSerializable8(std::uint32_t classId) {
    switch (classId) {
        case 0x4a222b98:
            if (!sc4::g_loadPatrolCarHost) SC4_UNWIRED("IPatrolCarHost / IEmergencyVehicleHost (loaded patrol car has no crime, police or siren services)");
            return NewEmergencyVehicle<sc4::cSC4PatrolCar>(sc4::g_loadPatrolCarHost);
        case 0xaa222b83:
            if (!sc4::g_loadFireTruckHost) SC4_UNWIRED("IFireTruckHost / IEmergencyVehicleHost (loaded fire truck has no fire, crew or siren services)");
            return NewEmergencyVehicle<sc4::cSC4FireTruck>(sc4::g_loadFireTruckHost);
        case 0x6c1acde1:
            if (!sc4::g_loadEmergencyHost) SC4_UNWIRED("IEmergencyVehicleHost (loaded ambulance has no siren or route services)");
            return NewEmergencyVehicle<sc4::cSC4Ambulance>();
        default: return nullptr;
    }
}
