// The plumbing simulator's native host (app/NativePlumbing.cpp) and the queries the rest of the game makes of the water network.
#pragma once
#include <cstdint>

namespace sc4 { class IPlumbingHost; class cSC4PlumbingSimulator; }

namespace native {

sc4::IPlumbingHost* NativePlumbingHost();
sc4::cSC4PlumbingSimulator* NativePlumbingSimulator();   // the city's plumbing simulator once initialised, else null
bool NativeCellWatered(int x, int z);                    // the watered bit of a city cell (true without a simulator)
void NativePlumbingCityClosed();                         // drops the host's references into the old city
int NativeWaterPollutionAt(int x, int z);                // NativeSimHosts.cpp: the pollution simulator's water value at a cell

}  // namespace native
