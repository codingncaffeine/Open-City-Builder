#include "sc4/TrafficFilters.h"
#include "sc4/NetworkOccupant.h"

namespace sc4 {

namespace {
// QI the occupant for its network interface; the caller Releases the result.
ITrafficNetworkOccupant* NetworkInterface(cISC4Occupant* occupant) {
    void* raw = nullptr;
    if (!occupant->QueryInterface(kGZIID_cISC4NetworkOccupant, &raw)) return nullptr;
    return static_cast<ITrafficNetworkOccupant*>(raw);
}

// The port's network pieces do not answer cISC4NetworkOccupant through QueryInterface (it is shared with other seams); the piece
// itself answers the same questions.
cSC4NetworkOccupant* Piece(cISC4Occupant* occupant) { return dynamic_cast<cSC4NetworkOccupant*>(occupant); }
bool HasTrafficNetworkFlags(cISC4Occupant* occupant) {
    if (occupant->GetType() != kTrafficFilterOccupantNetwork) return false;
    if (cSC4NetworkOccupant* piece = Piece(occupant)) return piece->HasAnyNetworkFlag(kTrafficFilterNetworkFlags);
    ITrafficNetworkOccupant* network = NetworkInterface(occupant);
    if (!network) return false;
    const bool result = network->HasAnyNetworkFlag(kTrafficFilterNetworkFlags);
    network->Release();
    return result;
}
}  // namespace

bool cTrafficNetworkFilter_NetworkMap::IsOccupantIncluded(cISC4Occupant* occupant) { return HasTrafficNetworkFlags(occupant); }
bool cTrafficNetworkFilter_PlayerDrive::IsOccupantIncluded(cISC4Occupant* occupant) { return HasTrafficNetworkFlags(occupant); }

bool cSC4TrafficQueryOccupantFilter::IsOccupantIncluded(cISC4Occupant* occupant) {
    const std::uint32_t type = occupant->GetType();
    if (type == kTrafficFilterOccupantBuilding) return true;
    if (type != kTrafficFilterOccupantNetwork) return false;
    if (cSC4NetworkOccupant* piece = Piece(occupant)) {
        for (std::uint32_t networkType : {0u, 1u, 2u, 3u, 6u, 7u, 8u, 9u, 0xcu, 10u, 0xbu})
            if (piece->IsOfType(networkType)) return true;
        return false;
    }
    ITrafficNetworkOccupant* network = NetworkInterface(occupant);
    if (!network) return false;
    bool result = false;
    for (std::uint32_t networkType : {0u, 1u, 2u, 3u, 6u, 7u, 8u, 9u, 0xcu, 10u, 0xbu}) {
        if (network->IsOfType(networkType)) { result = true; break; }
    }
    network->Release();
    return result;
}

}  // namespace sc4
