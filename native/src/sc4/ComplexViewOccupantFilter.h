// The occupant filters of the data views and the underground views.
//
// cSC4ComplexViewOccupantFilter (Mac i386 0x0027fbf8 ctor, IsOccupantIncluded 0x0027a246): the filter a data view
// gives the 3D view (cSC4WinMapView::SetDataView builds it from the view's exemplar). Layout (i386):
//   +0x0c std::vector<uint32> occupant types (0x4a0b47e0)
//   +0x18 std::vector<eNetworkType> network types (from the underground mode / 0x4a0b47f1)
//   +0x24 cRZAutoRefCount<cISC4OccupantFilter> the water filter (SetForceIncludePipeOccupants)
//   +0x28 cRZAutoRefCount<cISC4OccupantFilter> the subway filter (SetForceIncludeSubwayOccupants)
//   +0x2c bool: types NOT in the list are included (default false; true when the view lists no types)
//   +0x2d bool: network pieces NOT of a listed network type are included
//   +0x2e bool: plain lots (a lot texture whose lot is not built yet) and two prop types are included (default true)
//
// cSC4WaterOccupantFilter (i386 0x0012ca8e, IsPropertyHolderIncluded 0x0012cb26): what the network manager's
// GetWaterOccupantFilter hands out: surface network pieces, pipe connections and the water buildings.
//
// Serialization (Write / Read) and the GZCOM creators are not ported: the port builds these filters directly.
#pragma once
#include <cstdint>
#include <vector>

#include "sc4/OccupantManager.h"

namespace sc4 {

// What the filter asks of the city: spLotManager GetLot(x, z, false) -> GetState() (lot vslot 0x84), or -1 for no lot.
class IComplexViewFilterHost {
public:
    virtual ~IComplexViewFilterHost() = default;
    virtual int LotStateAt(int x, int z) = 0;
};
void SetComplexViewFilterHost(IComplexViewFilterHost* host);

class cSC4ComplexViewOccupantFilter : public cSC4OccupantFilter {
public:
    cSC4ComplexViewOccupantFilter() = default;                       // 0x0027fbf8
    ~cSC4ComplexViewOccupantFilter() override;
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;        // 0x0027a246

    // 0x002799ac / 0x002799e4: the network manager's water / subway filter (one reference taken), or none.
    void SetForceIncludePipeOccupants(cISC4OccupantFilter* waterFilter);
    void SetForceIncludeSubwayOccupants(cISC4OccupantFilter* subwayFilter);

    std::vector<std::uint32_t>& OccupantTypes() { return types_; }      // +0x0c
    std::vector<std::uint32_t>& NetworkTypes() { return networkTypes_; } // +0x18
    void SetIncludeUnlistedTypes(bool on) { includeUnlistedTypes_ = on; }       // +0x2c
    void SetIncludeUnlistedNetworks(bool on) { includeUnlistedNetworks_ = on; } // +0x2d
    void SetIncludePlainLots(bool on) { includePlainLots_ = on; }               // +0x2e

private:
    std::vector<std::uint32_t> types_;               // +0x0c
    std::vector<std::uint32_t> networkTypes_;        // +0x18
    cISC4OccupantFilter* waterFilter_ = nullptr;     // +0x24 (counted)
    cISC4OccupantFilter* subwayFilter_ = nullptr;    // +0x28 (counted)
    bool includeUnlistedTypes_ = false;              // +0x2c
    bool includeUnlistedNetworks_ = false;           // +0x2d
    bool includePlainLots_ = true;                   // +0x2e
};

class cSC4WaterOccupantFilter : public cSC4OccupantFilter {
public:
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;               // 0x0012ca8e
    bool IsPropertyHolderIncluded(cISCPropertyHolder* holder) override;     // 0x0012cb26
};

}  // namespace sc4
