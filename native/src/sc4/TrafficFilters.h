// The three traffic occupant filters: cTrafficNetworkFilter_NetworkMap (0x1002a073c) and cTrafficNetworkFilter_PlayerDrive
// (0x1006fbc84) accept network occupants carrying any of the road / rail / transit flags 0x1fcf; cSC4TrafficQueryOccupantFilter
// (0x1002dc006) accepts buildings and every network occupant of a road-like type and refuses property holders.
// The network-occupant calls go through ITrafficNetworkOccupant, the slots 0x48 (HasAnyNetworkFlag) and 0xb0 (IsOfType) of the
// interface that QueryInterface(0xa821ef94) hands back (cSC4NetworkOccupant; see TRAFFICFILTER-1).
#pragma once
#include <cstdint>
#include "sc4/CommonIIDs.h"

#include "sc4/OccupantManager.h"

namespace sc4 {

constexpr std::uint32_t kTrafficFilterNetworkFlags = 0x1fcf;
constexpr std::uint32_t kTrafficFilterOccupantNetwork = 0xc772bf98;
constexpr std::uint32_t kTrafficFilterOccupantBuilding = 0x278128a0;

class ITrafficNetworkOccupant : public cIGZUnknown {
public:
    virtual bool HasAnyNetworkFlag(std::uint32_t flags) const = 0;     // slot 0x48
    virtual bool IsOfType(std::uint32_t networkType) const = 0;        // slot 0xb0
};

// Included: network occupants with any of the flags 0x1fcf.
class cTrafficNetworkFilter_NetworkMap : public cSC4OccupantFilter {
public:
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;   // 0x1002a073c
};
class cTrafficNetworkFilter_PlayerDrive : public cSC4OccupantFilter {
public:
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;   // 0x1006fbc84
};
// Included: buildings, and network occupants that are of network type 0, 1, 2, 3, 6, 7, 8, 9, 0xc, 10 or 0xb (checked in that order).
class cSC4TrafficQueryOccupantFilter : public cSC4OccupantFilter {
public:
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;   // 0x1002dc006
    bool IsPropertyHolderIncluded(cISCPropertyHolder*) override { return false; }   // 0x1002dc16c
};

}  // namespace sc4
