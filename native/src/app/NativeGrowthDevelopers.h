// cSC4GrowthDeveloper for the building development host, as an opaque handle (NativeGrowthDevelopers.cpp says why).
#pragma once

#include <cstdint>
#include <string>

#include <array>
#include <vector>

namespace sc4 { class cISCPropertyHolder; template <class T> class cSC4SimGrid; class cSC4GrowthDeveloper; class cSC4Demand; }

namespace native {

void* NewGrowthDeveloper();                                          // new cSC4GrowthDeveloper, one reference
bool GrowthDeveloperInit(void* developer, sc4::cISCPropertyHolder* exemplar);   // vslot 0x18
void GrowthDeveloperStartMapUpdate(void* developer);                 // vslot 0xd8
bool GrowthDeveloperMapUpdateFinished(void* developer);              // vslot 0xe0
std::uint32_t GrowthDeveloperSubType(void* developer);               // vslot 0x30
bool GrowthDeveloperListsValid(void* developer);                     // vslot 0x50
void GrowthDeveloperGrowthList(void* developer, std::vector<std::array<std::int32_t, 3>>& out);   // vslot 0x58 {x, z, style}
float GrowthDeveloperActiveDemand(void* developer);                  // vslot 0x90 -> demand vslot 0x60
sc4::cSC4GrowthDeveloper* GrowthDeveloperOfType(std::uint32_t type); // the developer of one developer type (or null)
// cSC4TractDeveloper::Grow for one developer's tract (NativeTractDevelopment.cpp); > 0 when something was built
int NativeTractGrow(void* developer, int tractX, int tractZ, std::uint8_t style);
void InstallTractDeveloperHost();
// The demand simulator's view (NativeDemandHost.cpp): the zone kind (+0x28, 1..3), the source demand channel, the desirability at a
// cell, every developer.
std::uint32_t GrowthDeveloperKind(void* developer);
sc4::cSC4Demand* GrowthDeveloperDemandChannel(void* developer);
std::int16_t GrowthDeveloperDesirabilityAt(void* developer, unsigned x, unsigned z);
sc4::cSC4SimGrid<std::int16_t>* GrowthDeveloperDesirabilityMap(std::uint32_t developerType);   // the land value simulator
std::vector<void*> AllGrowthDevelopers();
void InstallDemandSimulatorHost();   // NativeDemandHost.cpp
void InstallLotConfigurationHosts();   // NativeLotConfigHost.cpp: the lot configuration manager and the lot configurations   // the demand simulator's IDemandSimulatorHost (DEMAND-1)   // the tract developer's ITractDeveloperHost (growth layer 3)

// The land value simulator's maps (NativeGrowthMaps.cpp)
sc4::cSC4SimGrid<std::uint8_t>* NativeTotalLandValueMap();
sc4::cSC4SimGrid<std::uint8_t>* NativeIntrinsicLandValueMap();
sc4::cSC4SimGrid<float>* NativeSlopeMap();

void InstallGrowthDeveloperHost();
// " subtype:growth/decline" per developer ('!' when the lists are not valid yet), for SC4_SIMDEBUG
std::string GrowthDevelopersSummary();   // the growth developers' IGrowthDeveloperHost (growth layer 2)

}  // namespace native
