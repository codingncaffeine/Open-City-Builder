#include "app/NativeZoneTool.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "app/NativeAudio.h"
#include "app/NativeDemolition.h"
#include "app/NativeTutorial.h"
#include "app/NativeZoneOverlay.h"
#include "app/NativeZoneDeveloperHost.h"
#include "gz/cGZPersistResourceKey.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/OccupantManager.h"
#include "sc4/ServiceLocator.h"
#include "sc4/SLString.h"
#include "sc4/View3DWin.h"
#include "sc4/ViewInputControlZone.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4ZoneManager.h"
#include "sc4/cSTETerrain.h"

namespace native {
namespace {

// cRZMessage2ZoneChange (i386 ctor 0x00674f36, dtor 0x00674e9a): a standard message that owns the SC4CellRegion in void2 and
// deletes it with itself, so the zone change can be posted.
class cRZMessage2ZoneChange final : public cRZMessage2Standard {
public:
    ~cRZMessage2ZoneChange() override { delete static_cast<sc4::SC4CellRegion<int>*>(GetVoid2()); }
};

// The current city: SL::spCity, as the original asks (a pointer kept from an earlier city outlives it).
sc4::cSC4City* CurrentCity() { return dynamic_cast<sc4::cSC4City*>(SL::spCity); }
sc4::cSC4View3DWin* g_view = nullptr;

// A city component as its concrete class, through its serializable view (dynamic_cast is unreliable here).
template <class T> T* Component(sc4::CityComponent c, std::uint32_t clsid) {
    cIGZUnknown* object = CurrentCity() ? CurrentCity()->GetComponent(c) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = serializable->GetGZCLSID() == clsid ? static_cast<T*>(serializable) : nullptr;
    serializable->Release();
    return out;
}
sc4::cSC4ZoneManager* Zones() { return Component<sc4::cSC4ZoneManager>(sc4::CityComponent::ZoneManager, sc4::kZoneManagerClassId); }
sc4::cSC4BudgetSimulator* Budget() { return Component<sc4::cSC4BudgetSimulator>(sc4::CityComponent::BudgetSimulator, sc4::kGZCLSID_cSC4BudgetSimulator); }
sc4::cSTETerrain* Terrain() {
    cIGZUnknown* object = CurrentCity() ? CurrentCity()->GetComponent(sc4::CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* terrain = static_cast<sc4::cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
    static_cast<cIGZSerializable*>(raw)->Release();
    return terrain;
}
sc4::cISC4ZoneDeveloper* Developer() {
    cIGZUnknown* object = CurrentCity() ? CurrentCity()->GetComponent(sc4::CityComponent::ZoneDeveloper) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe818ed2bu, &raw) || !raw) return nullptr;
    auto* developer = static_cast<sc4::cISC4ZoneDeveloper*>(raw);
    developer->Release();
    return developer;
}
sc4::cISC4OccupantManager* Occupants() {
    cIGZUnknown* object = CurrentCity() ? CurrentCity()->GetComponent(sc4::CityComponent::OccupantManager) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(sc4::kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
    auto* manager = static_cast<sc4::cISC4OccupantManager*>(raw);
    manager->Release();
    return manager;
}

// The drag preview and the zones already placed are drawn by NativeZoneOverlay.
class TerrainViewAdapter final : public sc4::IZoneToolTerrainView {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 2; }
    std::uint32_t Release() override { return 2; }
    void HighlightCell(int x, int z, std::uint32_t color) override { SetDragHighlightCell(x, z, 0x90000000u | (color >> 8)); }   // RRGGBBAA -> 0xAARRGGBB
    void HighlightRegionError(const sc4::SC4Rect<int>& rect) override { SetDragErrorRect(rect.x0, rect.z0, rect.x1, rect.z1); }
    void ClearHighlight() override { ClearDragHighlight(); ClearDragError(); }
    bool SetGridShown(bool shown) override { const bool old = grid_; grid_ = shown; return old; }
    bool GridShown() override { return grid_; }

private:
    bool grid_ = false;
};
TerrainViewAdapter g_terrainView;

struct Tick { cIGZMessageTarget2* target; std::uint32_t interval; };
std::vector<Tick> g_ticks;

class ZoneToolHost final : public sc4::IZoneToolHost {
public:
    sc4::cISC4View3DWin* View() override { return g_view; }
    float CellSize() override { return 16.0f; }
    sc4::IZoneToolTerrainView* TerrainView() override { return &g_terrainView; }
    sc4::cSC4ZoneManager* ZoneManager() override { return Zones(); }
    bool TutorialAllowsZone(const sc4::SC4CellRegion<int>& region, bool flood) override {
        return NativeTutorialAllowsZone(region.bounds.x0, region.bounds.z0, region.bounds.x1, region.bounds.z1, flood);
    }
    sc4::cISC4ZoneDeveloper* ZoneDeveloper() override { return Developer(); }
    int CityCellsX() override { sc4::cSTETerrain* t = Terrain(); return t ? static_cast<int>(t->CellCountX()) : 0; }
    int CityCellsZ() override { sc4::cSTETerrain* t = Terrain(); return t ? static_cast<int>(t->CellCountZ()) : 0; }
    bool CellIsOpenGround(int x, int z) override {
        sc4::cISC4ZoneDeveloper* developer = Developer();
        if (developer && developer->GetLot(x, z, false)) return false;
        return !CellHasNetwork(x, z) && !CellIsWater(x, z);
    }
    int HighlightLot(sc4::cSC4Lot* lot, int mode) override { return lot && lot->GetBuilding() ? 1 : 0; }
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    void StartTick(cIGZMessageTarget2* target, std::uint32_t interval) override { g_ticks.push_back({target, interval}); }
    void StopTick(cIGZMessageTarget2* target, std::uint32_t interval) override {
        g_ticks.erase(std::remove_if(g_ticks.begin(), g_ticks.end(),
                                     [&](const Tick& t) { return t.target == target && (interval == 0xffffffffu || t.interval == interval); }),
                      g_ticks.end());
    }
    void PlaySound(std::uint32_t id) override { PlayGameSound(id); }
    void LoadString(std::uint32_t group, std::uint32_t instance, std::string& out) override {
        sc4::cSLString text(cGZPersistResourceKey{0x2026960b, group, instance}, true);
        out = text.ToChar() ? text.ToChar() : "";
    }

    static bool CellHasNetwork(int x, int z) {
        sc4::cISC4OccupantManager* occupants = Occupants();
        sc4::cISC4Occupant* occupant = nullptr;
        if (!occupants || !occupants->GetFirstOccupantByStandardCityCell(occupant, x, z, 0xc772bf98) || !occupant) return false;
        occupant->Release();
        return true;
    }
    static bool CellIsWater(int x, int z) {
        sc4::cSTETerrain* t = Terrain();
        return t && t->IsWaterVertex(x, z) && t->IsWaterVertex(x + 1, z) && t->IsWaterVertex(x, z + 1) && t->IsWaterVertex(x + 1, z + 1);
    }
};

class PlacementHost final : public sc4::IZonePlacementHost {
public:
    std::int64_t Funds() override { sc4::cSC4BudgetSimulator* b = Budget(); return b ? b->GetTotalFunds() : 0; }
    bool SpendFunds(std::int64_t cost) override { sc4::cSC4BudgetSimulator* b = Budget(); return b && b->WithdrawFunds(cost); }
    bool NetworkAt(int x, int z) override { return ZoneToolHost::CellHasNetwork(x, z); }
    bool IsWater(int x, int z) override { return ZoneToolHost::CellIsWater(x, z); }
    sc4::cSC4Lot* LotAt(int x, int z) override { sc4::cISC4ZoneDeveloper* d = Developer(); return d ? d->GetLot(x, z, false) : nullptr; }
    void LotCells(sc4::cSC4Lot* lot, int& x0, int& z0, int& x1, int& z1) override {
        std::int32_t rect[4] = {};
        lot->GetBoundingRect(rect);
        x0 = rect[0]; z0 = rect[1]; x1 = rect[2]; z1 = rect[3];
    }
    int LotKind(sc4::cSC4Lot* lot) override { return lot->GetZoneType(); }
    bool LotCompatibleWithZone(sc4::cSC4Lot* lot, int zoneType) override { return lot->GetZoneType() == zoneType; }
    // spDemolition DemolishLots (zones.md §6): DemolishLots(false, lots, 1, 1, 0, 0, &cost, 0) to price, (true, lots, 1, 0x2000, ...)
    // to clear them.
    std::int64_t DemolishCost(const std::vector<sc4::cSC4Lot*>& lots) override {
        std::int64_t cost = 0;
        native::DemolishLots(false, lots.data(), static_cast<int>(lots.size()), 1, 1, false, &cost);
        return cost;
    }
    void DemolishLots(const std::vector<sc4::cSC4Lot*>& lots) override {
        native::DemolishLots(true, lots.data(), static_cast<int>(lots.size()), 1, 0x2000, false, nullptr);
    }
    void ApplyZone(const sc4::SC4CellRegion<int>& changed, int zoneType, bool flag) override {
        if (sc4::cISC4ZoneDeveloper* d = Developer()) d->DoParcellization(changed, zoneType, flag);   // vslot 0x30
    }
    // PlaceZone's message 0x07699455 (i386 0x00130570): the simulators that keep per-zone state (the growth developers' zone cell
    // counts, land value, network lots, landfill) and the tutorial's zone task hear about the change. void1 / the unknown = the zone
    // manager, void2 = a heap copy of the changed region that the message owns (cRZMessage2ZoneChange), data3 = the zone type,
    // data4 = the flag. It is POSTED, as the original does: sent at once, the tutorial finished its zone task inside the zone
    // tool's mouse-up and removed the tool before the tool released its capture.
    void PostZoneChanged(const sc4::SC4CellRegion<int>& changed, int zoneType, bool flag) override {
        if (!SL::spMS2) return;
        sc4::cSC4ZoneManager* zones = Zones();
        auto* message = new cRZMessage2ZoneChange();
        message->AddRef();
        message->SetType(0x07699455);
        message->SetVoid1(zones);
        message->SetIGZUnknown(zones ? static_cast<cIGZUnknown*>(static_cast<cIGZSerializable*>(zones)) : nullptr);
        message->SetVoid2(new sc4::SC4CellRegion<int>(changed));
        message->SetData3(static_cast<std::uint64_t>(static_cast<std::int64_t>(zoneType)));
        message->SetData4(flag ? 1 : 0);
        SL::spMS2->MessagePost(message, false);
        message->Release();
    }
};

ZoneToolHost g_toolHost;
PlacementHost g_placement;

}  // namespace

void InstallZoneTool(sc4::cSC4View3DWin& view, cISC4City* city, sc4::cSC43DRender& render) {
    g_view = &view;
    sc4::SetZoneToolHost(&g_toolHost);
    sc4::cSC4ZoneManager* zones = Zones();
    if (zones) zones->SetPlacementHost(&g_placement);
    // With the zone developer up, zoning makes real lots that draw the game's zone textures (cSC4LotDeveloper::SetEmptyLotBaseTexture),
    // so the native overlay keeps only the drag preview; without it the overlay still paints the zone grid.
    const bool developer = InstallZoneDeveloperHost(CurrentCity());
    SetZoneOverlayManager(developer ? nullptr : zones);
    if (sc4::cSTETerrain* terrain = Terrain()) AttachZoneOverlay(render, *terrain);
    std::printf("city view: zone tool %s\n", zones ? "up" : "no zone manager");
}

void NativeZoneRect(int type, int x0, int z0, int x1, int z1) {
    auto* tool = new sc4::cSC4ViewInputControlZone(type);
    tool->AddRef();
    sc4::SC4Rect<int> rect;
    rect.x0 = std::min(x0, x1); rect.z0 = std::min(z0, z1); rect.x1 = std::max(x0, x1); rect.z1 = std::max(z0, z1);
    sc4::SC4CellRegion<int> region = sc4::SC4CellRegion<int>::Make(rect, true);
    const bool placed = tool->PlaceZone(type, region, false, false, nullptr, nullptr);
    int zoned = 0;
    if (sc4::cSC4ZoneManager* zones = Zones())
        for (int x = rect.x0; x <= rect.x1; ++x)
            for (int z = rect.z0; z <= rect.z1; ++z) zoned += zones->GetZoneType(x, z) == type;
    std::printf("zonerect: type %d (%d,%d)-(%d,%d) %s, %d cells now of that type\n", type, rect.x0, rect.z0, rect.x1, rect.z1,
                placed ? "placed" : "refused", zoned);
    tool->Release();
}

sc4::cSC4ViewInputControl* CreateZoneTool(int type) {
    auto* tool = new sc4::cSC4ViewInputControlZone(type);
    tool->AddRef();
    return tool;
}

void StartToolTick(cIGZMessageTarget2* target, std::uint32_t interval) { g_ticks.push_back({target, interval}); }
void StopToolTick(cIGZMessageTarget2* target) {
    g_ticks.erase(std::remove_if(g_ticks.begin(), g_ticks.end(), [&](const Tick& t) { return t.target == target; }), g_ticks.end());
}

void PumpToolTicks(int elapsedMs) {
    if (g_ticks.empty()) return;
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(0x86ad10ee);
    message->SetData3(static_cast<std::uint64_t>(elapsedMs));
    const std::vector<Tick> targets = g_ticks;
    for (const Tick& t : targets) t.target->DoMessage(message);
    message->Release();
}

}  // namespace native
