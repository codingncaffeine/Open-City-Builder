#include "sc4/OccupantFilters.h"
#include "sc4/PropOccupantInterface.h"

#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {
constexpr std::uint32_t kTypeProp = 0xa823821e, kTypeNetwork = 0xc772bf98, kTypeBuilding = 0x278128a0,
                        kTypeFlora = 0x74758926, kTypePole = 0x2890d4de, kTypeLandfill = 0x6a11bd2b;

// QueryInterface used as a pure capability test (the original ignores the interface it gets back).
bool Supports(cISC4Occupant* occupant, std::uint32_t iid) {
    void* iface = nullptr;
    if (!occupant->QueryInterface(iid, &iface)) return false;
    static_cast<cIGZUnknown*>(iface)->Release();
    return true;
}
}  // namespace

bool cSC4OccupantImmovabilityFilter::IsOccupantIncluded(cISC4Occupant* occupant) {
    const std::uint32_t type = occupant->GetType();
    if (type != kTypeProp && type != kTypeNetwork && type != kTypeBuilding && type != kTypePole) return false;
    bool immovable = false;
    cSCPropertyHelp::GetPropertyValue(occupant->AsPropertyHolder(), 0x29f013e6, immovable);
    return !immovable;
}

bool cLCEBlockingLotOccupantFilter::IsOccupantTypeIncluded(std::uint32_t type) {
    return type == kTypeProp || type == kTypeNetwork || type == kTypeBuilding || type == kTypeFlora;
}

bool cAutomataOccupantFilter::IsOccupantIncluded(cISC4Occupant* occupant) {
    const std::uint32_t type = occupant->GetType();
    return type == 0xc7cc75f1 || type == 0x496e5783 || type == 0x480e6151 || type == 0x897f7ec2;
}

bool tCrimePropOccupantFilter::IsOccupantIncluded(cISC4Occupant* occupant) {
    return occupant->GetType() == kTypeProp && occupant->AsPropertyHolder()->HasProperty(0x8aa864d5);
}

bool tCrimeSimOccupantFilter::IsOccupantIncluded(cISC4Occupant* occupant) {
    return occupant->GetType() == 0x496e5783 && occupant->IsOccupantGroup(0x4102);
}

bool cStoplightPropFilter::IsOccupantIncluded(cISC4Occupant* occupant) {
    return occupant->GetType() == kTypeProp && (occupant->IsOccupantGroup(0x5002) || occupant->IsOccupantGroup(0x5003));
}

bool cSC4DemolishByNetworksOccupantFilter::IsOccupantIncluded(cISC4Occupant* occupant) {
    return !Supports(occupant, 0xa9764c8c);
}

bool cSC4DecalFilter::IsOccupantIncluded(cISC4Occupant* occupant) {
    return occupant->GetType() == kTypeProp && Supports(occupant, 0x2b3b7d86);
}

bool cSC4SignpostFilter::IsOccupantIncluded(cISC4Occupant* occupant) {
    return occupant->GetType() == kTypeProp && Supports(occupant, 0x4b44fbe2);
}

bool cSC4SignOrLabelFilter::IsOccupantIncluded(cISC4Occupant* occupant) {
    if (occupant->GetType() != kTypeProp) return false;
    void* raw = nullptr;
    if (!occupant->QueryInterface(0xe9793a65, &raw) || !raw) return false;   // cISC4PropOccupant
    auto* prop = static_cast<cISC4PropOccupant*>(raw);
    const std::uint32_t type = prop->GetPropType();
    prop->Release();
    return type == 0xab72fbb3u || type == 0xcb79919bu;
}

bool cSC4BuildingPropFilter::IsOccupantIncluded(cISC4Occupant* occupant) {
    bool flag = false;
    cSCPropertyHelp::GetPropertyValue(occupant->AsPropertyHolder(), 0x6a95e503, flag);
    return flag;
}

bool cSC4HoverQueriableOccupantFilter::IsOccupantIncluded(cISC4Occupant* occupant) {
    switch (occupant->GetType()) {
        case kTypeProp:
        case kTypeFlora:
            return occupant->AsPropertyHolder()->HasProperty(0x8a416a99);
        case kTypeNetwork:
        case kTypeBuilding:
        case kTypePole:
        case kTypeLandfill:
            return true;
        default:
            return false;
    }
}

bool cSC4QueriableOccupantFilter::IsOccupantIncluded(cISC4Occupant* occupant) {
    if (occupant->AsPropertyHolder()->HasProperty(0xcaa9ab92)) return false;
    return cSC4HoverQueriableOccupantFilter::IsOccupantIncluded(occupant);
}

}  // namespace sc4
