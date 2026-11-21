// The surface water's views of the city's network pieces and power poles (ISurfaceWaterHost::NetworkOccupantAt,
// AsNetworkOccupant, AsCellOccupant, OccupantsInCell). The original hands cSTESurfaceWater the objects themselves
// (cISC4NetworkOccupant 0xa821ef94, cISC4PowerPoleOccupant 0xa8c86142); the port's pieces answer no cISC4NetworkOccupant,
// so the views here are stateless pass-throughs kept per object address for the process (a new object at a recycled
// address simply gets the same pass-through, rebound to it). The surface water only uses an answer inside the call that
// asked for it, while the traffic network map or the occupant manager still holds the object.
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "sc4/NetworkOccupant.h"
#include "sc4/OccupantManager.h"
#include "sc4/PowerLineOccupant.h"
#include "sc4/PowerPoleOccupant.h"
#include "sc4/SurfaceWater.h"
#include "sc4/TrafficNetworkMap.h"

namespace native {
namespace {
using namespace sc4;

// cISC4NetworkOccupant's slots the water reads: 0x40 HasNetworkFlag, 0x98 IsUsable, 0x180 AsOccupant -> GetPosition,
// 0x140 GetActiveEdges, 0x218 GetPassageCeiling.
class WaterNetworkView final : public IWaterNetworkOccupant {
public:
    explicit WaterNetworkView(cSC4NetworkOccupant* piece) : piece_(piece) {}
    void Rebind(cSC4NetworkOccupant* piece) { piece_ = piece; }
    bool HasNetworkFlag(std::uint32_t flag) override { return piece_->HasNetworkFlag(flag); }
    bool IsUsable() override { return piece_->IsUsable(); }
    void GetPosition(float xyz[3]) override { piece_->GetPosition(xyz); }
    std::uint8_t ActiveEdges() override { return static_cast<std::uint8_t>(piece_->GetActiveEdges()); }
    bool PassageCeiling(float& height) override { return piece_->GetPassageCeiling(height); }
private:
    cSC4NetworkOccupant* piece_;
};

// cISC4PowerPoleOccupant's slots the water reads: 0x40 GetCell, 0x68 IsPreview.
class WaterPoleView final : public IWaterCellOccupant {
public:
    explicit WaterPoleView(cSC4PowerPoleOccupant* pole) : pole_(pole) {}
    void Rebind(cSC4PowerPoleOccupant* pole) { pole_ = pole; }
    void GetCell(int& x, int& z) override { pole_->GetCell(x, z); }
    bool IsPreview() override { return pole_->IsPreview(); }
private:
    cSC4PowerPoleOccupant* pole_;
};

std::mutex g_viewsMutex;   // the surface water is reached from the simulator and from the zone developer
std::map<cSC4NetworkOccupant*, std::unique_ptr<WaterNetworkView>> g_networkViews;
std::map<cSC4PowerPoleOccupant*, std::unique_ptr<WaterPoleView>> g_poleViews;

IWaterNetworkOccupant* NetworkView(cSC4NetworkOccupant* piece) {
    if (!piece) return nullptr;
    std::lock_guard<std::mutex> lock(g_viewsMutex);
    auto& slot = g_networkViews[piece];
    if (!slot) slot = std::make_unique<WaterNetworkView>(piece);
    else slot->Rebind(piece);
    return slot.get();
}

// GZCOM_Interface(object, 0xa8c86142): the power pole behind an occupant, or null.
IWaterCellOccupant* PoleView(cIGZUnknown* object) {
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZCLSID_cSC4PowerPoleOccupant, &raw) || !raw) return nullptr;
    auto* pole = static_cast<cSC4PowerPoleOccupant*>(static_cast<cISC4PowerPoleOccupant*>(raw));   // the interface view
    pole->Release();   // the occupant manager (or the message's sender) keeps the pole
    std::lock_guard<std::mutex> lock(g_viewsMutex);
    auto& slot = g_poleViews[pole];
    if (!slot) slot = std::make_unique<WaterPoleView>(pole);
    else slot->Rebind(pole);
    return slot.get();
}
}  // namespace

// spTrafficNetworkMap vslot 0x50: FindNetworkOccupant(x, z, flags, includeUnusable), borrowed like the map's answer.
IWaterNetworkOccupant* SurfaceWaterNetworkOccupantAt(cIGZUnknown* trafficMap, int x, int z, std::uint32_t mask, bool exact) {
    auto* map = trafficMap ? dynamic_cast<cSC4TrafficNetworkMap*>(trafficMap) : nullptr;
    ITrafficNetworkMapOccupant* found = map ? map->FindNetworkOccupant(x, z, mask, exact) : nullptr;
    cISC4Occupant* occupant = found ? found->AsOccupant() : nullptr;
    return NetworkView(occupant ? dynamic_cast<cSC4NetworkOccupant*>(occupant) : nullptr);
}

// GZCOM_Interface(object, 0xa821ef94) on the usable-changed message's object: the piece itself.
IWaterNetworkOccupant* SurfaceWaterAsNetworkOccupant(cIGZUnknown* object) {
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4Occupant, &raw) || !raw) return nullptr;
    auto* occupant = static_cast<cISC4Occupant*>(raw);
    IWaterNetworkOccupant* view = NetworkView(dynamic_cast<cSC4NetworkOccupant*>(occupant));
    occupant->Release();   // the message keeps the piece for the call
    return view;
}

IWaterCellOccupant* SurfaceWaterAsCellOccupant(cIGZUnknown* object) { return PoleView(object); }

// spOccupantManager vslot 0x140 GetOccupantsByStandardCityCells(list, cell, cell, group, unlimited), then QueryInterface
// 0xa8c86142 on each entry. The list's references are dropped here (the original drops them after its loop); the occupant
// manager keeps the poles.
void SurfaceWaterOccupantsInCell(cIGZUnknown* occupantManager, int x, int z, std::uint32_t group,
                                 std::vector<IWaterCellOccupant*>& out) {
    void* raw = nullptr;
    if (!occupantManager || !occupantManager->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) return;
    auto* manager = static_cast<cISC4OccupantManager*>(raw);
    const int cell[2] = {x, z};
    OccupantList occupants;
    if (manager->GetOccupantsByStandardCityCells(occupants, cell, cell, group, 0xffffffffu)) {
        for (cISC4Occupant* occupant : occupants)
            if (IWaterCellOccupant* pole = PoleView(occupant)) out.push_back(pole);
        manager->ReleaseOccupantList(occupants);
    }
    manager->Release();
}

}  // namespace native
