// The traffic simulator's native host and the calls the rest of the native game makes into the traffic simulator.
#pragma once
#include <cstdint>

namespace sc4 { template <class T> class cSC4SimGrid; class cISCPropertyHolder; }

namespace native {

void InstallNativeTrafficHost();       // before the city's components are created (the creator reads the host)
float NativeTrafficFreightScaling();                          // traffic simulator slot 27
void NativeTrafficStrategyPercentages(int wealth, float out[3]);   // slot 26 (wealth 0..3 = none, $, $$, $$$)
sc4::cSC4SimGrid<std::uint8_t>* NativeTrafficCongestionMap();      // null until the simulator runs
sc4::cSC4SimGrid<std::uint8_t>* NativeTrafficTripLengthMap();
sc4::cSC4SimGrid<std::uint8_t>* NativeTrafficCommercialMap();
// traffic vslot 0x130 GetConnectedDestinationCount(lot, kind): the most destinations of `kind` in any of the lot's subnetworks
std::uint32_t NativeTrafficConnectedDestinationCount(const sc4::cISCPropertyHolder* lot, int kind);

}  // namespace native
