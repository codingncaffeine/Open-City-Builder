// The demolition utility in a native city: the recovered cSC4Demolition (SL::spDemolition) on a native IDemolitionHost.
// The occupants and lots it sees are adapters over the city's cISC4Occupant / cSC4Lot objects, made for one call and dropped
// after it (the adapters hold a reference, so an occupant the call removes stays valid until the call returns).
#pragma once

#include <cstdint>

namespace sc4 { class cISC4Occupant; class cSC4Lot; class cSC4NetworkManager; class cSC4NeighborsSimulator; class IDemolitionHost; class IDemolishFilter; class IDemolishable; struct DemolitionRegion; }

namespace native {

sc4::IDemolitionHost* NativeDemolitionHost();   // installed in gzcom::GetSimHosts() before the city's components are made

// SL::spDemolition calls. `privilege` 0 = none, 1 = the player; `flags` as cSC4Demolition (low byte = effects).
bool DemolishRegion(bool commit, const sc4::DemolitionRegion& region, int privilege, std::uint32_t flags, bool dezone,
                    std::int64_t* cost, sc4::cISC4Occupant* clicked = nullptr, int viewX = 0, int viewY = 0,
                    sc4::IDemolishFilter* filter = nullptr);
bool DemolishOccupant(bool commit, sc4::cISC4Occupant* occupant, std::uint32_t flags, std::int64_t* cost);
bool NativeHasDemolition();   // SL::spDemolition is up
// The city occupant behind an occupant a caller's filter is asked about.
sc4::cISC4Occupant* DemolitionOccupant(sc4::IDemolishable* occupant);
sc4::cSC4NetworkManager* CityNetworkManager();   // the city's network manager, or null
sc4::cSC4NeighborsSimulator* CityNeighborsSimulator();   // the city's neighbours simulator, or null
void NativeLayNetworkLine(std::uint32_t type, int x1, int z1, int x2, int z2);   // kit verb `netline` (NativeNetworkTool.cpp)
void NativeBridgeReport(int x0, int z0, int x1, int z1);   // kit verb `bridges` (NativeNetworkTool.cpp)
void NativeOneWayReport(int x0, int z0, int x1, int z1);   // kit verb `oneway` (NativeNetworkTool.cpp)
void NativeBridgeChooser(std::uint32_t type, int x1, int z1, int x2, int z2, int row);   // kit verb `bridgechoose` (NativeNetworkTool.cpp)
void NativeTunnelParams(std::uint32_t type);                 // kit verb `tunnelparams` (NativeNetworkTool.cpp)
void NativeTerrainLine(int x1, int z1, int x2, int z2);     // kit verb `heights`
void NativeTunnelReport();                                  // kit verb `tunnels`
void NativeConstructionCrewReport(int x0, int z0, int x1, int z1);   // kit verb `crews [X0 Z0 X1 Z1]` (NativeNetworkCrewReport.cpp)
void NativeTrafficTunnelReport();                           // kit verb `tunnels`, the traffic simulator's half (NativeTraffic.cpp)
bool DemolishLots(bool commit, sc4::cSC4Lot* const* lots, int count, int privilege, std::uint32_t flags, bool dezone,
                  std::int64_t* cost);
// SL::spDemolition slot 9 ModifyTerrainHeight(apply, perVertex, vertex rect, height, exempt lot, reason, null): the vertices changed.
int NativeModifyTerrainHeight(bool apply, bool perVertex, int x0, int z0, int x1, int z1, float height, sc4::cSC4Lot* exempt,
                              std::uint32_t reason);

}  // namespace native
