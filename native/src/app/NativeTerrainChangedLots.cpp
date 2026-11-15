// What the building development simulator does when the ground moves under lots (cSC4BuildingDevelopmentSimulator::
// DoTerrainChanged), in a native city: the lots of the changed cells, the ground under them, and the demolition, zone and
// budget services a lot that lost its ground goes through. Installed with the simulator's other host
// (NativeBuildingDevelopment.cpp). Trace: SC4_TERRAINLOTDEBUG=1.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <memory>
#include <vector>

#include "app/NativeCity.h"
#include "app/NativeDemolition.h"
#include "rz/cRZAutoRefCount.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/BuildingDevelopmentSimulator.h"
#include "sc4/Lot.h"
#include "sc4/LotDeveloper.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4ZoneManager.h"
#include "sc4/cSTETerrain.h"

namespace native {
namespace {
using namespace sc4;

bool Trace() {
    static const bool on = std::getenv("SC4_TERRAINLOTDEBUG") != nullptr;
    return on;
}
// SC4_TERRAINLOTSKIP=lots,clearzone,demolish: a bisect switch, leaves the named steps out.
bool Skip(const char* what) {
    static const char* list = std::getenv("SC4_TERRAINLOTSKIP");
    return list && std::strstr(list, what) != nullptr;
}

template <class T> T* Component(CityComponent c, std::uint32_t clsid) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = serializable->GetGZCLSID() == clsid ? dynamic_cast<T*>(serializable) : nullptr;
    serializable->Release();
    return out;
}
cSC4ZoneDeveloper* Lots() { return Component<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper, cSC4ZoneDeveloper::kZoneDeveloperClassId); }
cSC4ZoneManager* Zones() { return Component<cSC4ZoneManager>(CityComponent::ZoneManager, kZoneManagerClassId); }
cSC4LotDeveloper* LotDeveloper() { return Component<cSC4LotDeveloper>(CityComponent::LotDeveloper, kGZCLSID_cSC4LotDeveloper); }
cSC4BudgetSimulator* Budget() { return Component<cSC4BudgetSimulator>(CityComponent::BudgetSimulator, kGZCLSID_cSC4BudgetSimulator); }
cSTETerrain* Terrain() {
    cSC4City* city = HostCity();
    return city ? dynamic_cast<cSTETerrain*>(city->GetComponent(CityComponent::Terrain)) : nullptr;
}

struct LotView final : ITerrainChangedLot {
    explicit LotView(cSC4Lot* l) : lot(l) {}
    void GetBoundingRect(std::int32_t rect[4]) override { lot->GetBoundingRect(rect); }
    void GetCenter(float xyz[3]) override { lot->GetCenter(xyz); }
    float GetSlopeAtTimeOfConstruction() override { return lot->GetSlopeAtTimeOfConstruction(); }
    bool IsInDemolishableState() override { return lot->IsInDemolishableState(); }
    cRZAutoRefCount<cSC4Lot> lot;
};
cSC4Lot* Raw(ITerrainChangedLot* lot) { return static_cast<LotView*>(lot)->lot.get(); }

class Host final : public ITerrainChangedHost {
public:
    void LotsInCellRect(const std::int32_t rect[4], std::vector<ITerrainChangedLot*>& out) override {
        views_.clear();
        cSC4ZoneDeveloper* lots = Lots();
        if (!lots || Skip("lots")) return;
        std::list<cRZAutoRefCount<cSC4Lot>> found;
        lots->GetLotsInCellRect(found, rect[0], rect[1], rect[2], rect[3], false);
        for (cRZAutoRefCount<cSC4Lot>& lot : found)
            if (lot.get()) views_.push_back(std::make_unique<LotView>(lot.get()));
        for (std::unique_ptr<LotView>& view : views_) out.push_back(view.get());
        if (Trace() && !views_.empty()) std::fprintf(stderr, "terrain lots: cells %d,%d..%d,%d hold %zu lots\n", rect[0], rect[1], rect[2], rect[3], views_.size());
    }
    void LotsDone() override { views_.clear(); }
    float TerrainAltitude(float x, float z) override { cSTETerrain* terrain = Terrain(); return terrain ? terrain->GetAltitude(x, z) : 0.0f; }
    float LotSlope(ITerrainChangedLot* lot) override { cSC4LotDeveloper* developer = LotDeveloper(); return developer ? developer->GetLotSlope(*Raw(lot), false) : 0.0f; }
    float CellWidthX() override { cSC4City* city = HostCity(); return city ? city->CellWidthX() : 16.0f; }
    // PARTIAL: cSC4LotDeveloper::UpdateLotElements (i386 0x000f2b1c) also refits the lot's foundation, retaining walls and
    // base texture, which the port does not make yet; a developed lot takes the new slope as its own (the original's last step).
    void UpdateLotElements(ITerrainChangedLot* lot, std::uint32_t) override {
        if (Raw(lot)->GetState() >= 2) Raw(lot)->UpdateSlope();
    }
    void ClearZone(const std::int32_t rect[4], bool mayorPays) override {
        cSC4ZoneManager* zones = Zones();
        if (!zones || Skip("clearzone")) return;
        SC4CellRegion<int> region = SC4CellRegion<int>::Make(SC4Rect<int>{rect[0], rect[1], rect[2], rect[3]}, true);
        const std::uint16_t before = Terrain() ? Terrain()->GetTerrainCellFlags(static_cast<std::uint32_t>(rect[0]), static_cast<std::uint32_t>(rect[1])) : 0;
        const bool cleared = zones->PlaceZone(region, 0, true, mayorPays, false, true, true, nullptr, nullptr, nullptr);
        if (Trace())
            std::fprintf(stderr, "terrain lots: zone cleared at %d,%d..%d,%d -> %d (terrain cell flags %x -> %x)\n", rect[0], rect[1], rect[2], rect[3], cleared, before,
                         Terrain() ? Terrain()->GetTerrainCellFlags(static_cast<std::uint32_t>(rect[0]), static_cast<std::uint32_t>(rect[1])) : 0);
    }
    std::int64_t DemolishLots(const std::vector<ITerrainChangedLot*>& lots) override {
        if (lots.empty() || Skip("demolish")) return 0;
        std::vector<cSC4Lot*> raw;
        for (ITerrainChangedLot* lot : lots) raw.push_back(Raw(lot));
        std::int64_t cost = 0;
        const bool ok = native::DemolishLots(true, raw.data(), static_cast<int>(raw.size()), 0, 1, true, &cost);
        if (Trace()) std::fprintf(stderr, "terrain lots: %zu lots lost their ground -> demolished %d, cost %lld\n", raw.size(), ok, static_cast<long long>(cost));
        return cost;
    }
    void ChargeFunds(std::int64_t cost) override {
        if (cSC4BudgetSimulator* budget = Budget()) budget->SetTotalFunds(budget->GetTotalFunds() - cost);
    }
private:
    std::vector<std::unique_ptr<LotView>> views_;
} g_host;

}  // namespace

void InstallTerrainChangedHost() { sc4::SetTerrainChangedHost(&g_host); }

}  // namespace native
