// The traffic simulator's native host and the calls the rest of the native game makes into the traffic simulator.
#pragma once
#include <cstdint>

namespace sc4 { template <class T> class cSC4SimGrid; }

namespace native {

void InstallNativeTrafficHost();       // before the city's components are created (the creator reads the host)
float NativeTrafficFreightScaling();                          // traffic simulator slot 27
void NativeTrafficStrategyPercentages(int wealth, float out[3]);   // slot 26 (wealth 0..3 = none, $, $$, $$$)
sc4::cSC4SimGrid<std::uint8_t>* NativeTrafficCongestionMap();      // null until the simulator runs
sc4::cSC4SimGrid<std::uint8_t>* NativeTrafficTripLengthMap();

}  // namespace native
