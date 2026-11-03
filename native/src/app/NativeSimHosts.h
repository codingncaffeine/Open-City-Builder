// Native hosts of the city simulators that ran on the city probe's bindings (NativeSimHosts.cpp).
#pragma once

namespace native {
void InstallNativeSimHosts();   // after InstallCityProbeBindings, before the city's components are created
float NativeResidentialWorkforcePercent(int x, int z);   // residential simulator slot 30 (a lot's commuter share)
}
