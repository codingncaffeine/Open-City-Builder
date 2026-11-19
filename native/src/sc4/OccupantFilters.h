// Small stateless occupant filters used by the tools and simulators (each a cSC4OccupantFilter subclass whose
// only real code is IsOccupantIncluded / IsOccupantTypeIncluded; the rest are compiler destructors).
// 0x1000cb624.. (LotProp is stateful and in a later group), 0x1000ee9xx, 0x10014906c, 0x100196138.. etc.
#pragma once
#include <cstdint>

#include "sc4/OccupantManager.h"

namespace sc4 {

// Occupants carrying property 0x29f013e6 (immovable) are excluded; only props, networks, buildings and poles are considered.
class cSC4OccupantImmovabilityFilter : public cSC4OccupantFilter {
public:
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;
};
// Flora occupants (type 0x74758926).
class cSC4BlockingLotOccupantFilter : public cSC4OccupantFilter {
public:
    bool IsOccupantTypeIncluded(std::uint32_t type) override { return type == 0x74758926; }
};
// Lot-configuration-editor variant: props, networks, buildings and flora.
class cLCEBlockingLotOccupantFilter : public cSC4OccupantFilter {
public:
    bool IsOccupantTypeIncluded(std::uint32_t type) override;
};
// Vehicles / pedestrians (the four automata occupant types).
class cAutomataOccupantFilter : public cSC4OccupantFilter {
public:
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;
};
class cSC4AutomatonOccupantFilter : public cSC4OccupantFilter {
public:
    bool IsOccupantTypeIncluded(std::uint32_t type) override { return type == 0xc7cc75f1 || type == 0x496e5783; }
    bool IsPropertyHolderIncluded(cISCPropertyHolder*) override { return false; }
};
// Props that carry crime-sim property 0x8aa864d5.
class tCrimePropOccupantFilter : public cSC4OccupantFilter {
public:
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;
};
// Automata in occupant group 0x4102.
class tCrimeSimOccupantFilter : public cSC4OccupantFilter {
public:
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;
};
// Stoplight props: occupant groups 0x5002 / 0x5003.
class cStoplightPropFilter : public cSC4OccupantFilter {
public:
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;
};
class cSC4DemolishByNetworksOccupantFilter : public cSC4OccupantFilter {
public:
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;
};
// Everything except types 0x69d25c5f and 0x74758926 (flora).
class cSC4ObliterateOccupantFilter : public cSC4OccupantFilter {
public:
    bool IsOccupantTypeIncluded(std::uint32_t type) override { return type != 0x69d25c5f && type != 0x74758926; }
};
class cSC4ActiveToxicOccupantFilter : public cSC4OccupantFilter {
public:
    bool IsOccupantIncluded(cISC4Occupant* occupant) override { return occupant->GetType() == 0xabf98cb4; }
};
class cSC4DecalFilter : public cSC4OccupantFilter {
public:
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;
};
class cSC4SignpostFilter : public cSC4OccupantFilter {
public:
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;
};
// Signs and labels: props whose prop type is the signpost's (0xab72fbb3) or the text decal's (0xcb79919b) (PC 0x004d7680).
class cSC4SignOrLabelFilter : public cSC4OccupantFilter {
public:
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;
};
// Buildings whose property 0x6a95e503 (bool) is set.
class cSC4BuildingPropFilter : public cSC4OccupantFilter {
public:
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;
};
// Occupants that can be hovered / queried with the query tool.
class cSC4HoverQueriableOccupantFilter : public cSC4OccupantFilter {
public:
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;
    bool IsPropertyHolderIncluded(cISCPropertyHolder*) override { return false; }
};
class cSC4QueriableOccupantFilter : public cSC4HoverQueriableOccupantFilter {
public:
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;
    bool IsPropertyHolderIncluded(cISCPropertyHolder*) override { return false; }
};

}  // namespace sc4
