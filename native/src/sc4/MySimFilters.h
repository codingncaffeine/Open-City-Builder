// cMySimBuildingFilter and cMySimPylonFilter — occupant filters the MySim code uses to find buildings a MySim may move into and power
// pylons (3 functions each: IsOccupantIncluded 0x1007aaf18 / 0x1006b88f6, destructors 0x1007ab008/0x1007ab012 and 0x1006b89e2/0x1006b89ec;
// both derive cSC4OccupantFilter and add nothing but +0x14 on the building filter).
//
// Building filter (i386 0x0082c2ce): the occupant must be a building (type 0x278128a0) that answers cISC4BuildingOccupant
// (0x87dda3a9); GetBuildingCondition (i386 0x005422a6: the condition of the building's lot, 0 when it has none) must be at most 3,
// the completion (building slot 9) at least 100%, and - when the filter flag at +0x14 is set - the building profile's purpose
// (slot 11, first word) must be 1 (residential). The building reference is released again.
// Pylon filter: the occupant must be of type 0xa9b6d7de and answer cISC4PylonOccupant (0xc9b6d7d6); its UI state (slot 7) must be
// non-zero, its drag state (slot 16) false, and its owner (slot 20) must answer interface 0x0a1da42f (a MySim).
#pragma once
#include "sc4/OccupantManager.h"

namespace sc4 {

#ifndef SC4_kOccupantTypeBuilding_DEFINED
#define SC4_kOccupantTypeBuilding_DEFINED
constexpr std::uint32_t kOccupantTypeBuilding = 0x278128a0;
#endif
constexpr std::uint32_t kOccupantTypePylon = 0xa9b6d7de;
constexpr std::uint32_t kIidMySimBuilding = 0x87dda3a9;
constexpr std::uint32_t kIidMySimPylon = 0xc9b6d7d6;
constexpr std::uint32_t kIidPylonLink = 0x0a1da42f;

class cISC4BuildingOccupant;

class IMySimFilterHost {
public:
    virtual ~IMySimFilterHost() = default;
    // GetBuildingCondition: SL::spLotManager GetOccupantLot(building->AsOccupant()) -> the lot's condition (cSC4Lot::GetCondition);
    // 0 when the building has no lot.
    virtual std::uint8_t BuildingCondition(cISC4BuildingOccupant* building) = 0;
};
IMySimFilterHost* MySimFilterHost();
void SetMySimFilterHost(IMySimFilterHost* host);

class cMySimBuildingFilter : public cSC4OccupantFilter {
public:
    explicit cMySimBuildingFilter(bool onlyKindOne = false) : onlyKindOne_(onlyKindOne) {}
    ~cMySimBuildingFilter() override = default;           // @ 0x1007ab008 (deleting 0x1007ab012)
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;   // @ 0x1007aaf18
private:
    bool onlyKindOne_ = false;                            // +0x14
};

class cMySimPylonFilter : public cSC4OccupantFilter {
public:
    ~cMySimPylonFilter() override = default;              // @ 0x1006b89e2 (deleting 0x1006b89ec)
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;   // @ 0x1006b88f6
};

}  // namespace sc4
