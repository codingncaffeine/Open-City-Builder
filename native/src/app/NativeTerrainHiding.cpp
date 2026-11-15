// What the model maker asks of the occupants on a cell when it works out which terrain triangles they hide
// (cSC4ModelMaker::DoTerrainHidingForCells), in a native city. Its own file: the network, underground, foundation and lot
// headers do not go together with the model maker host's (NativeModelMaker.cpp).
#include <cstdint>
#include <cstdio>
#include <map>
#include <utility>

#include "app/NativeCity.h"
#include "sc4/FoundationOccupant.h"
#include "sc4/Lot.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/OccupantManager.h"
#include "sc4/UndergroundOccupant.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4City.h"

namespace native {

// cISC4NetworkOccupant::HasNetworkFlag for each bit of `mask`; false when the occupant is not a network piece.
bool TerrainHidingNetworkFlags(sc4::cISC4Occupant* occupant, std::uint32_t mask, std::uint32_t& flags) {
    flags = 0;
    auto* piece = dynamic_cast<sc4::cSC4NetworkOccupant*>(occupant);
    if (!piece) return false;
    for (std::uint32_t bit = 1; bit != 0; bit <<= 1)
        if ((mask & bit) != 0 && piece->HasNetworkFlag(bit)) flags |= bit;
    return true;
}

// cISC4UndergroundOccupant::GetHiddenTriangles: the two triangles of its cell an underground piece hides.
bool TerrainHidingUndergroundTriangles(sc4::cISC4Occupant* occupant, bool& even, bool& odd) {
    even = odd = false;
    auto* piece = dynamic_cast<sc4::cSC4UndergroundOccupant*>(occupant);
    if (!piece) return false;
    bool first = false;
    piece->GetHiddenTriangles(first, even, odd);
    return true;
}

int TerrainHidingFoundationUsage(sc4::cISC4Occupant* occupant) {
    auto* foundation = dynamic_cast<sc4::cSC4FoundationOccupant*>(occupant);
    return foundation ? static_cast<int>(foundation->GetUsage()) : -1;
}

// The lot at the cell (the lot manager's GetLot(x, z, false)) and its flags.
bool TerrainHidingLotFlagsAt(int x, int z, std::uint32_t& flags) {
    flags = 0;
    sc4::cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(sc4::CityComponent::ZoneDeveloper) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return false;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    auto* lots = serializable->GetGZCLSID() == sc4::cSC4ZoneDeveloper::kZoneDeveloperClassId ? dynamic_cast<sc4::cSC4ZoneDeveloper*>(serializable) : nullptr;
    serializable->Release();
    sc4::cSC4Lot* lot = lots ? lots->GetLot(x, z, false) : nullptr;
    if (!lot) return false;
    flags = lot->GetFlags();
    return true;
}

// The network pieces on a cell, for the `terrainflags` report: class, flags, piece.
void TerrainHidingDescribeCell(int x, int z) {
    sc4::cSC4City* city = HostCity();
    auto* occupants = city ? dynamic_cast<sc4::cISC4OccupantManager*>(city->GetComponent(sc4::CityComponent::OccupantManager)) : nullptr;
    if (!occupants) return;
    sc4::OccupantList list;
    const int low[2] = {x, z}, high[2] = {x, z};
    occupants->GetOccupantsByStandardCityCells(list, low, high, static_cast<sc4::cISC4OccupantFilter*>(nullptr), 0xffffffffu);
    for (sc4::cISC4Occupant* occupant : list) {
        if (!occupant) continue;
        auto* piece = dynamic_cast<sc4::cSC4NetworkOccupant*>(occupant);
        auto* underground = dynamic_cast<sc4::cSC4UndergroundOccupant*>(occupant);
        std::printf("terrainflags:   cell %d,%d holds type %08x", x, z, occupant->GetType());
        if (piece) std::printf(" network class %08x flags %08x piece %08x%s", piece->GetGZCLSID(), piece->GetNetworkFlag(), piece->PieceId(), underground ? " (underground)" : "");
        std::printf("\n");
    }
    occupants->ReleaseOccupantList(list);
}

// What the original's default view filter (cSC4DefaultOccupantFilter) leaves out of the normal view, counted by occupant
// type (and class, for network pieces): the `terrainflags` report prints it.
void TerrainHidingDescribeDefaultFilter() {
    sc4::cSC4City* city = HostCity();
    auto* occupants = city ? dynamic_cast<sc4::cISC4OccupantManager*>(city->GetComponent(sc4::CityComponent::OccupantManager)) : nullptr;
    if (!occupants) return;
    sc4::OccupantList list;
    occupants->GetOccupantsByStandardCityCells(list, nullptr, nullptr, static_cast<sc4::cISC4OccupantFilter*>(nullptr), 0xffffffffu);
    sc4::cSC4DefaultOccupantFilter filter;
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> excluded;
    int kept = 0;
    for (sc4::cISC4Occupant* occupant : list) {
        if (!occupant) continue;
        if (filter.IsOccupantIncluded(occupant)) { ++kept; continue; }
        auto* piece = dynamic_cast<sc4::cSC4NetworkOccupant*>(occupant);
        ++excluded[{occupant->GetType(), piece ? piece->GetGZCLSID() : 0}];
    }
    std::printf("terrainflags: the default view filter keeps %d occupants", kept);
    for (const auto& [kind, count] : excluded) std::printf(", leaves out %d of type %08x class %08x", count, kind.first, kind.second);
    std::printf("\n");
    occupants->ReleaseOccupantList(list);
}

}  // namespace native
