// The bulldozer's services on the native application (cSC4ViewInputControlDemolish's IDemolishToolHost): the terrain and
// occupant picks of the city view, the lot map, the demolition utility (NativeDemolition.cpp), the budget, and the marked
// cells drawn through the zone overlay's highlight cells in the draw context's highlight colours 9 (can go) and 10 (cannot).
#include "app/NativeDemolishTool.h"

#include "sc4/DrawContext.h"
#include "sc4/PackedRenderState.h"
#include "sc4/Render3D.h"
#include "app/NativeCity.h"
#include "app/NativeCityView.h"
#include "app/NativeDemolition.h"
#include "app/NativeZoneDeveloperHost.h"
#include "app/NativeZoneOverlay.h"
#include "s3d/ModelInstance.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/Lot.h"
#include "sc4/OccupantManager.h"
#include "sc4/View3DWin.h"
#include "sc4/ViewInputControlDemolish.h"
#include "sc4/cSC4City.h"

namespace native {
namespace {
using namespace sc4;

constexpr std::uint32_t kIIDOccupant = 0x26bb767e;      // cISC4Occupant
constexpr std::uint32_t kIIDSignpost = 0x4b44fbe2;
constexpr std::uint32_t kIIDPowerPole = 0xa8c86142;

cSC4View3DWin* g_view = nullptr;

cIGZUnknown* Component(CityComponent c) {
    cSC4City* city = HostCity();
    return city ? city->GetComponent(c) : nullptr;
}
cISC4OccupantManager* Occupants() {
    cIGZUnknown* object = Component(CityComponent::OccupantManager);
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
    auto* manager = static_cast<cISC4OccupantManager*>(raw);
    manager->Release();
    return manager;
}
cSC4BudgetSimulator* Budget() {
    cIGZUnknown* object = Component(CityComponent::BudgetSimulator);
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    auto* out = serializable->GetGZCLSID() == kGZCLSID_cSC4BudgetSimulator ? static_cast<cSC4BudgetSimulator*>(serializable) : nullptr;
    serializable->Release();
    return out;
}
sc4::cISC4Occupant* AsOccupant(cIGZUnknown* object) {   // one reference
    void* raw = nullptr;
    return object && object->QueryInterface(kIIDOccupant, &raw) ? static_cast<sc4::cISC4Occupant*>(raw) : nullptr;
}
// cS3DVector4 highlight colours 9 and 10 of the draw context (SC4DrawContext::Init) as the overlay's ARGB.
std::uint32_t Argb(float r, float g, float b, float a) {
    auto byte = [](float v) { return static_cast<std::uint32_t>((v < 0 ? 0 : v > 1 ? 1 : v) * 255.0f + 0.5f); };
    return byte(a) << 24 | byte(r) << 16 | byte(g) << 8 | byte(b);
}

class Host final : public IDemolishToolHost {
public:
    bool PickCell(int x, int y, int& cellX, int& cellZ, float position[3]) override {
        cSC4City* city = HostCity();
        if (!g_view || !city || !g_view->PickTerrain(x, y, position, false)) return false;
        city->PositionToCell(position[0], position[2], cellX, cellZ);
        return true;
    }
    // PARTIAL: the pick runs without cSC4DemolishableOccupantFilter (any occupant with a model).
    sc4::cISC4Occupant* PickOccupant(int x, int y, bool& signpost) override {
        signpost = false;
        s3d::cIS3DModelInstance* model = nullptr;
        sc4::cSC43DRender* render = CityRenderer();
        if (!render || !render->Pick(x, y, nullptr, model) || !model) return nullptr;
        sc4::cISC4Occupant* occupant = AsOccupant(model->GetOwner());
        if (!occupant) return nullptr;
        void* raw = nullptr;
        if (occupant->QueryInterface(kIIDSignpost, &raw) && raw) {
            signpost = true;
            static_cast<cIGZUnknown*>(raw)->Release();
        }
        return occupant;
    }
    bool OccupantCells(sc4::cISC4Occupant* occupant, DemolitionCellRect& out) override {
        void* raw = nullptr;
        if (occupant->QueryInterface(kIIDPowerPole, &raw) && raw) {   // a power pole takes its own cell
            static_cast<cIGZUnknown*>(raw)->Release();
            float p[3] = {};
            occupant->GetPosition(p);
            out = DemolitionCellRect{static_cast<int>(p[0] / 16.0f), static_cast<int>(p[2] / 16.0f), static_cast<int>(p[0] / 16.0f), static_cast<int>(p[2] / 16.0f)};
            return true;
        }
        SC4CellRect cells{};
        occupant->GetBoundingCityCells(cells);   // occupant vslot 0x2c
        out = DemolitionCellRect{cells.x0, cells.z0, cells.x1, cells.z1};
        return true;
    }
    void OccupantPosition(sc4::cISC4Occupant* occupant, float out[3]) override { occupant->GetPosition(out); }
    cSC4Lot* LotAt(int x, int z) override { return NativeLotAtCell(x, z, false); }
    sc4::cISC4Occupant* LotBuilding(cSC4Lot* lot) override { return AsOccupant(lot->GetBuilding()); }
    sc4::cISC4Occupant* NetworkOccupantAt(int x, int z) override {
        cISC4OccupantManager* occupants = Occupants();
        sc4::cISC4Occupant* occupant = nullptr;
        return occupants && occupants->GetFirstOccupantByStandardCityCell(occupant, x, z, 0xc772bf98u) ? occupant : nullptr;
    }
    std::uint32_t Highlight(sc4::cISC4Occupant* occupant) override { return occupant->GetHighlight(); }
    void SetHighlight(sc4::cISC4Occupant* occupant, std::uint32_t highlight) override { occupant->SetHighlight(static_cast<int>(highlight), false); }
    bool DemolishRegion(bool commit, const DemolitionRegion& region, std::int64_t* cost, sc4::cISC4Occupant* clicked, int x, int y) override {
        return native::DemolishRegion(commit, region, 1, 1, false, cost, clicked, x, y);
    }
    bool DemolishOccupant(bool commit, sc4::cISC4Occupant* occupant, std::int64_t* cost) override { return native::DemolishOccupant(commit, occupant, 1, cost); }
    void PayFromFunds(std::int64_t cost) override {
        if (cSC4BudgetSimulator* budget = Budget()) budget->SetTotalFunds(budget->GetTotalFunds() - cost);
    }
    void SetCostIndicator(std::int64_t cost) override { if (g_view) g_view->SetCostIndicator(static_cast<std::uint32_t>(cost)); }
    void ClearCostIndicator(bool force) override { if (g_view) g_view->ClearCostIndicator(force); }
    void MarkCells(const DemolitionRegion& region, bool valid) override {
        ClearDragHighlight();
        const std::uint32_t colour = valid ? Argb(0.0f, 0.7f, 0.2f, 0.3f) : Argb(0.7f, 0.0f, 0.2f, 0.3f);
        for (int x = region.bounds.x0; x <= region.bounds.x1; ++x)
            for (int z = region.bounds.z0; z <= region.bounds.z1; ++z)
                if (region.Contains(x, z)) SetDragHighlightCell(x, z, colour);
    }
    void ClearMarkedCells() override { ClearDragHighlight(); }
} g_host;

}  // namespace

void InstallDemolishTool(cSC4View3DWin& view) {
    g_view = &view;
    SetDemolishToolHost(&g_host);
}

cSC4ViewInputControl* CreateDemolishTool() { return new cSC4ViewInputControlDemolish(); }

}  // namespace native
