// city_probe_hostglobals - (Open City Builder's Linux probe) the load-time hosts and factories the city_probe_creators read;
// null makes them fall back to the stub hosts (the native app's NativeTrafficHosts.cpp and others set real ones).
#include "sc4/EmergencyVehicle.h"
#include "sc4/FireTruck.h"
#include "sc4/PatrolCar.h"
#include "sc4/TrafficAutomaton.h"
#include "sc4/Vehicle.h"

namespace sc4 {
ITrafficAutomatonHost* g_loadTrafficHost = nullptr;
IVehicleHost* g_loadVehicleHost = nullptr;
IEmergencyVehicleHost* g_loadEmergencyHost = nullptr;   // app/NativeDispatch.cpp sets the emergency vehicles' three hosts
IFireTruckHost* g_loadFireTruckHost = nullptr;
IPatrolCarHost* g_loadPatrolCarHost = nullptr;
// app/NativeAutomataGenerator.cpp sets it: saved automata generators (class 0x4a1628c5) then load as real controllers.
cIGZSerializable* (*g_createLoadedGenerator)() = nullptr;
}  // namespace sc4
