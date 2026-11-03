// The power simulator's native host (app/NativePower.cpp) and the queries the rest of the game makes of the grid.
#pragma once
#include <cstdint>

namespace sc4 { class IPowerHost; class cSC4PowerSimulator; class cISCPropertyHolder; }

namespace native {

sc4::IPowerHost* NativePowerHost();
sc4::cSC4PowerSimulator* NativePowerSimulator();   // the city's power simulator once initialised, else null
bool NativeCellPowered(int x, int z);              // the powered bit of a city cell (true without a simulator)
void NativePowerCityOpened();                      // a new city without a supply grid gets one
void NativePowerCityClosed();                      // drops the host's references into the old city
const sc4::cISCPropertyHolder* NativeSimulatorExemplar(std::uint32_t instance);   // NativeSimHosts.cpp

}  // namespace native
