// The lot developer's native hosts (NativeLotConstruction.cpp).
#pragma once
#include <cstdint>

namespace sc4 { class cISC4LotConfigurationRef; class cSC4LotDeveloper; }

namespace native {
void InstallLotConstructionHosts();                                            // before the city's components are created
sc4::cISC4LotConfigurationRef* NativeLotConfigurationRef(std::uint32_t id);   // the reference a lot keeps (lot cfg manager 0x40)
void ReadLotDeveloperTunables(sc4::cSC4LotDeveloper* developer);              // once per lot developer
}  // namespace native
