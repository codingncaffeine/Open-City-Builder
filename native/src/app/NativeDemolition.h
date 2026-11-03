// The demolition utility in a native city: the recovered cSC4Demolition (SL::spDemolition) on a native IDemolitionHost.
// The occupants and lots it sees are adapters over the city's cISC4Occupant / cSC4Lot objects, made for one call and dropped
// after it (the adapters hold a reference, so an occupant the call removes stays valid until the call returns).
#pragma once

#include <cstdint>

namespace sc4 { class cISC4Occupant; class cSC4Lot; class cSC4NetworkManager; class IDemolitionHost; class IDemolishFilter; class IDemolishable; struct DemolitionRegion; }

namespace native {

sc4::IDemolitionHost* NativeDemolitionHost();   // installed in gzcom::GetSimHosts() before the city's components are made

// SL::spDemolition calls. `privilege` 0 = none, 1 = the player; `flags` as cSC4Demolition (low byte = effects).
bool DemolishRegion(bool commit, const sc4::DemolitionRegion& region, int privilege, std::uint32_t flags, bool dezone,
                    std::int64_t* cost, sc4::cISC4Occupant* clicked = nullptr, int viewX = 0, int viewY = 0,
                    sc4::IDemolishFilter* filter = nullptr);
bool DemolishOccupant(bool commit, sc4::cISC4Occupant* occupant, std::uint32_t flags, std::int64_t* cost);
// The city occupant behind an occupant a caller's filter is asked about.
sc4::cISC4Occupant* DemolitionOccupant(sc4::IDemolishable* occupant);
sc4::cSC4NetworkManager* CityNetworkManager();   // the city's network manager, or null
bool DemolishLots(bool commit, sc4::cSC4Lot* const* lots, int count, int privilege, std::uint32_t flags, bool dezone,
                  std::int64_t* cost);

}  // namespace native
