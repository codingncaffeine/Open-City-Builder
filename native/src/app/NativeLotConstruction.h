// The lot developer's native hosts (NativeLotConstruction.cpp).
#pragma once
#include <cstdint>

namespace sc4 { class cISC4LotConfigurationRef; class cSC4LotConfiguration; class cSC4LotDeveloper; class IPortLotConfiguration; }

namespace native {
void NativeLotDeveloperReport();                                               // kit verb "lotdev"
void InstallLotConstructionHosts();                                            // before the city's components are created
sc4::cISC4LotConfigurationRef* NativeLotConfigurationRef(std::uint32_t id);   // the reference a lot keeps (lot cfg manager 0x40)
sc4::IPortLotConfiguration* NativePortLotConfiguration(std::uint32_t id);    // the same reference as the port developers ask it
sc4::cSC4LotConfiguration* NativeLotConfiguration(sc4::cISC4LotConfigurationRef* reference);   // the configuration behind it (null = none)
}  // namespace native
