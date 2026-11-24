// The query tool's services on the native application (sc4::IQueryHost): every answer goes through the simulator method the
// original calls (slots resolved from the Mac i386 vtables; the header names the x64 slot = twice the i386 one).
//   PARTIAL: the occupant filters accept any occupant with a model (the original builds one cSC4OccupantFilter subclass per
//   query kind); the coverage ring, the path view, following an automaton, stoplight props, the legend window proc, the
//   residential tract tracking (a debug toggle), the landfill and port developers (not asked: their rows read nothing). The
//   mouse tips go to the view's cursor text.
#include <cstdint>
#include "platform/Unwired.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "app/NativeCatalogUi.h"
#include "app/NativeCheats.h"
#include "app/NativeCity.h"
#include "app/NativeCitySystems.h"
#include "app/NativeCityView.h"
#include "app/NativePlumbing.h"
#include "app/NativePower.h"
#include "app/NativeUi.h"
#include "app/NativeWinMgr.h"
#include "app/NativeZoneDeveloperHost.h"
#include "app/NativeGrowthDevelopers.h"
#include "app/NativeUi.h"
#include "gz/cGZPersistResourceManager.h"
#include "sc4/SeaportDeveloper.h"
#include "sc4/AirportDeveloper.h"
#include "sc4/LandfillDeveloper.h"
#include "sc4/SubBudgetWindow.h"
#include "sc4/AuraSimulator.h"
#include "sc4/Exemplar.h"
#include "sc4/Budget.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/BuildingDevelopmentSimulator.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/TrafficFilters.h"
#include "sc4/OccupantFilters.h"
#include "sc4/CrimeSimulator.h"
#include "sc4/FireProtectionSimulator.h"
#include "sc4/LandValueSimulator.h"
#include "sc4/NeighborsSimulator.h"
#include "sc4/PlumbingSimulator.h"
#include "sc4/PoliceSimulator.h"
#include "sc4/PollutionSimulator.h"
#include "sc4/PowerSimulator.h"
#include "sc4/RegionalCity.h"
#include "sc4/SLBuffer.h"
#include "sc4/ResidentialSimulator.h"
#include "sc4/TrafficSimulator.h"
#include "sc4/cSTETerrain.h"
#include "sc4/WinLocationSaver.h"
#include "sc4/CoverageRadiusEffectHelper.h"
#include "rz/cRZKeyboard.h"
#include "sc4/ViewUtilities.h"
#include "sc4/ZoneDeveloper.h"
#include "gz/WinUtils.h"
#include "gz/WinGen.h"
#include "s3d/ModelInstance.h"
#include "sc4/Lot.h"
#include "sc4/Render3D.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/SC4Utilities.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/View3DWin.h"
#include "sc4/ViewInputControlQuery.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4ZoneManager.h"

namespace native {
sc4::IQueryPathView* NativeCreatePathView();          // app/NativePathView.cpp
bool NativeAddPathView(sc4::IQueryPathView* view);
bool NativeRemovePathView(sc4::IQueryPathView* view);
 sc4::cSC4CrimeSimulator* NativeCrimeSimulator(); }   // app/NativeCrime.cpp

namespace native {
// app/NativeSimHosts.cpp: the simulators' own occupant adapters
sc4::IFireOccupant* NativeFireStation(sc4::cISC4Occupant* occupant);
sc4::IPoliceOccupant* NativePoliceStation(sc4::cISC4Occupant* occupant);
sc4::IResidentialOccupant* NativeResidentialOccupant(cIGZUnknown* building);
sc4::IPollutionOccupant* NativePollutionOccupantFor(sc4::cISC4Occupant* occupant);
const sc4::cSC4SimGrid<std::uint8_t>* NativeFlammabilityGrid();   // app/NativeAdvisorFlammability.cpp

void NativeSwitchStoplightStates(cIGZUnknown* object);               // NativeTrafficNetworkMap.cpp
void NativeFollowAutomaton(cIGZUnknown* automaton);                   // NativeAutomataManagerHost.cpp
namespace {
using namespace sc4;

constexpr std::uint32_t kIIDOccupant = 0x26bb767e, kUIScriptGroup = 0x96a006b0;

template <class T> T* CityAs(CityComponent component) {
    cSC4City* city = HostCity();
    return city ? dynamic_cast<T*>(city->GetComponent(component)) : nullptr;
}
// a grid's value at a city cell, 0 outside it (the original's getters index unchecked; the query only asks lot cells)
template <class T> T CellOf(const cSC4SimGrid<T>* grid, int x, int z) {
    if (!grid || x < 0 || z < 0) return T{};
    const std::int32_t tx = x >> grid->GetTractShift(), tz = z >> grid->GetTractShift();
    if (!grid->TractIsInBounds(tx, tz)) return T{};
    return grid->GetCellValue(x, z);
}

bool Trace() {
    static const bool on = std::getenv("SC4_QUERYDEBUG") != nullptr;
    return on;
}

sc4::cISC4Occupant* AsOccupant(cIGZUnknown* object) {   // one reference
    void* raw = nullptr;
    return object && object->QueryInterface(kIIDOccupant, &raw) ? static_cast<sc4::cISC4Occupant*>(raw) : nullptr;
}

void OccupantCell(sc4::cISC4Occupant* occupant, int& x, int& z) {
    float p[3] = {};
    occupant->GetPosition(p);
    x = static_cast<int>(p[0] / 16.0f);
    z = static_cast<int>(p[2] / 16.0f);
}

// cISC4View3DWin as the tool reads it.
struct QueryView final : IQueryView {
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    static cSC4View3DWin* View() { return CityView3DWin(); }
    void EndInput(int) override { if (cSC4View3DWin* v = View()) v->RemoveCurrentViewInputControl(false); }
    cSC4ViewInputControl* TopControl() override { cSC4View3DWin* v = View(); return v ? v->GetCurrentViewInputControl() : nullptr; }
    cIGZWin* ViewWindow() override { return View(); }
    bool PickTerrain(int x, int y, float out[3], bool flag) override { cSC4View3DWin* v = View(); return v && v->PickTerrain(x, y, out, flag); }
    // cISC4View3DWin 0x138 GetTerrainQueryEnabled: the TerrainQuery cheat makes the tool show the cell under the pointer.
    bool ShowCoordinates() override { cSC4View3DWin* v = View(); return v && v->GetTerrainQueryEnabled(); }
    // cISC4View3DWin 0x140 / 0x148: the tool's hover text goes into the view's cursor text box.
    void SetTip(std::uint32_t id, int order, const cIGZString* title, const cIGZString* text, int icon) override {
        if (cSC4View3DWin* v = View()) v->SetCursorText(id, static_cast<std::uint32_t>(order), title, text, icon);
    }
    void ClearTip(std::uint32_t id) override { if (cSC4View3DWin* v = View()) v->ClearCursorText(id); }
    // view vslot 0x188: the coverage-radius effect helper the view owns
    static cSC4CoverageRadiusEffectHelper* Rings() {
        cSC4View3DWin* v = View();
        return v ? dynamic_cast<cSC4CoverageRadiusEffectHelper*>(v->GetCoverageHelper()) : nullptr;
    }
    void RecalcCoverage() override { if (cSC4CoverageRadiusEffectHelper* rings = Rings()) rings->RecalcCoverage(); }
    std::uint32_t CoverageCategories() override { cSC4CoverageRadiusEffectHelper* rings = Rings(); return rings ? rings->GetCategories() : 0; }
    void SetCoverageCategories(std::uint32_t categories) override { if (cSC4CoverageRadiusEffectHelper* rings = Rings()) rings->SetCategories(categories); }
    std::uint32_t CoverageCategoriesForOccupant(sc4::cISC4Occupant* occupant) override {
        return Rings() ? cSC4CoverageRadiusEffectHelper::GetCategoriesForOccupant(occupant) : 0;
    }
} g_view;

}  // namespace

sc4::cSC4OccupantFilter* NativeQueryFilter(int kind) {
    switch (kind) {
    case 0: return new sc4::cSC4TrafficQueryOccupantFilter();
    case 1: return new sc4::cSC4HoverQueriableOccupantFilter();
    case 2: return new sc4::cSC4AutomatonOccupantFilter();
    case 3: return new sc4::cStoplightPropFilter();   // = cSC4StoplightPropOccupantFilter's test
    case 4: return new sc4::cSC4QueriableOccupantFilter();
    default: return nullptr;
    }
}

namespace {

// The query tool's cSC4OccupantFilter of one kind (Mac i386 0x00216b3a / 0x0020acb4 / 0x00207090): 0 the network
// hover's cSC4TrafficQueryOccupantFilter, 1 cSC4HoverQueriableOccupantFilter, 2 cSC4AutomatonOccupantFilter, 3
// cSC4StoplightPropOccupantFilter (cStoplightPropFilter, the same test), 4 cSC4QueriableOccupantFilter (the query's pick, occupant-manager.md).
struct KindFilter final : IOccupantFilter {
    explicit KindFilter(sc4::cSC4OccupantFilter* f) : filter(f) { filter->AddRef(); }
    ~KindFilter() { filter->Release(); }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (--refs) return refs;
        delete this;
        return 0;
    }
    bool IsValid(const sc4::cISC4Occupant* occupant) override {
        return filter->IsOccupantIncluded(const_cast<sc4::cISC4Occupant*>(occupant));
    }
    sc4::cSC4OccupantFilter* filter;
    std::uint32_t refs = 1;
};

struct PickResult final : IPickResult {
    explicit PickResult(cIGZUnknown* object) : object_(object) {}
    ~PickResult() { if (object_) object_->Release(); }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_) return refs_;
        delete this;
        return 0;
    }
    cIGZUnknown* GetObject() override { return object_; }

private:
    cIGZUnknown* object_;   // one reference (the occupant)
    std::uint32_t refs_ = 1;
};

struct QueryRenderer final : IQueryRenderer {
    IOccupantFilter* CreateFilter(int kind) override {
        if (Trace()) std::fprintf(stderr, "query: filter %d\n", kind);
        sc4::cSC4OccupantFilter* filter = NativeQueryFilter(kind);
        return filter ? new KindFilter(filter) : nullptr;   // holds the filter's one reference
    }
    // cSC43DRender::Pick with the filter: each candidate model's occupant is tested before its hit test (render3d.md 11.5).
    IPickResult* Pick(int x, int y, IOccupantFilter* filter) override {
        s3d::cIS3DModelInstance* model = nullptr;
        cSC43DRender* render = CityRenderer();
        if (Trace()) std::fprintf(stderr, "query: pick %d,%d renderer %p\n", x, y, static_cast<void*>(render));
        auto* kind = dynamic_cast<KindFilter*>(filter);
        if (!render || !render->Pick(x, y, kind ? kind->filter : nullptr, model) || !model) return nullptr;
        sc4::cISC4Occupant* occupant = AsOccupant(model->GetOwner());
        model->Release();
        if (!occupant) return nullptr;
        if (filter && !kind && !filter->IsValid(occupant)) {
            occupant->Release();
            return nullptr;
        }
        if (Trace()) std::fprintf(stderr, "query: pick %d,%d -> occupant %p type %08x\n", x, y, static_cast<void*>(occupant), occupant->GetType());
        return new PickResult(occupant);
    }
    // SL::spRenderer vslots 0x80 / 0x84 (i386): AddViewObject(layer, 5, 1000) / RemoveViewObject (the route query's path view).
    void AddLayer(cIGZUnknown* layer) override { if (auto* view = dynamic_cast<IQueryPathView*>(layer)) NativeAddPathView(view); }
    void RemoveLayer(cIGZUnknown* layer) override { if (auto* view = dynamic_cast<IQueryPathView*>(layer)) NativeRemovePathView(view); }
} g_renderer;

struct QueryCity final : IQueryCity {
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        if (cSC4City* city = HostCity()) city->PositionToCell(x, z, cellX, cellZ);
        else cellX = cellZ = 0;
    }
    int CellCountX() override { cSC4City* c = HostCity(); return c ? static_cast<int>(c->CellCountX()) : 0; }
    int CellCountZ() override { cSC4City* c = HostCity(); return c ? static_cast<int>(c->CellCountZ()) : 0; }
    // spCity vslot 0xac GetVehicleManager -> vslot 0x88 SetCameraFollow(automaton).
    void FollowAutomaton(cIGZUnknown* automaton) override {
        NativeFollowAutomaton(automaton);
    }
    // spCity vslot 0xa0 GetTrafficNetwork -> vslot 0x94 SwitchStoplightStates(prop).
    void SelectStoplightProp(cIGZUnknown* prop) override { NativeSwitchStoplightStates(prop); }
} g_city;

// cSC4Lot as the query reads it. The lot manager hands lots out borrowed (the tool AddRefs some and reads others bare), so
// each lot keeps one wrapper for the session and its counting is a no-op.
struct QueryLot final : IQueryLot {
    explicit QueryLot(cSC4Lot* lot) : lot_(lot) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    void GetOccupants(std::vector<sc4::cISC4Occupant*>& out) override {
        std::vector<cIGZUnknown*> objects;
        lot_->GetLotOccupants(objects);
        for (cIGZUnknown* object : objects) {
            // borrowed, as the original's list (the lot holds its occupants; the tool AddRefs the one it keeps)
            if (sc4::cISC4Occupant* occupant = AsOccupant(object)) {
                out.push_back(occupant);
                occupant->Release();
            }
        }
    }
    void GetCellRect(SC4CellRect& rect) override {
        std::int32_t r[4] = {};
        lot_->GetBoundingRect(r);
        rect = SC4CellRect{r[0], r[1], r[2], r[3]};
    }
    int GetState() override { return lot_->GetState(); }
    bool IsHistorical() override { return lot_->IsHistorical(); }
    void SetHistorical(bool on) override { lot_->SetHistorical(on); }
    int GetAbandonReason() override { return lot_->GetVacateReason(); }   // vslot 0x168 (+0xd7)
    sc4::cISCPropertyHolder* AsPropertyHolder() override { return &lot_->Properties(); }
    int GetOccupantWealth() override { return lot_->GetOccupantWealth(); }
    std::uint32_t GetBuildingType(bool flag) override { return lot_->GetBuildingType(flag); }
    std::uint16_t GetCapacity(int wealth) override { return lot_->GetCapacityForWealth(static_cast<std::uint32_t>(wealth)); }
    float GetOccupancy() override { return lot_->GetOccupancy(); }
    void GetCenter(float* xyz) override { lot_->GetCenter(xyz); }
    bool GetNeedsPower() override { return lot_->GetNeedsPower(); }
    bool GetHasPower() override { return lot_->GetHasPower(); }
    sc4::cISC4Occupant* GetBuildingOccupant() override {   // borrowed: the lot holds its building
        cIGZUnknown* building = lot_->GetBuilding();
        sc4::cISC4Occupant* occupant = AsOccupant(building);
        if (occupant) occupant->Release();
        if (Trace()) {
            std::uint32_t queryUi = 0;
            const bool has = occupant && occupant->AsPropertyHolder() &&
                             cSCPropertyHelp::GetPropertyValue(occupant->AsPropertyHolder(), 0x2a499f85, queryUi);
            std::fprintf(stderr, "query: lot building %p occupant %p query ui %s %08x\n", static_cast<void*>(building),
                         static_cast<void*>(occupant), has ? "yes" : "no", queryUi);
        }
        return occupant;
    }
    cSC4Lot* Lot() const { return lot_; }

private:
    cSC4Lot* lot_;
};

QueryLot* WrapLot(cSC4Lot* lot) {
    // never destroyed: the query tool can still ask during the view's shutdown, after static destructors ran
    static auto* wrappers = new std::unordered_map<cSC4Lot*, QueryLot*>();
    QueryLot*& wrapper = (*wrappers)[lot];
    if (!wrapper) wrapper = new QueryLot(lot);
    return wrapper;
}

struct QueryLots final : IQueryLotManager {
    IQueryLot* GetLotForOccupant(sc4::cISC4Occupant* occupant) override {
        if (!occupant) return nullptr;
        int x = 0, z = 0;
        OccupantCell(occupant, x, z);
        return GetLotAtCell(x, z, 0);
    }
    IQueryLot* GetLotAtCell(int cellX, int cellZ, int) override {
        cSC4Lot* lot = NativeLotAtCell(cellX, cellZ, false);
        if (Trace()) std::fprintf(stderr, "query: lot at %d,%d -> %p\n", cellX, cellZ, static_cast<void*>(lot));
        return lot ? WrapLot(lot) : nullptr;
    }
    void GetOccupantsInCellRect(const SC4CellRect& rect, std::vector<sc4::cISC4Occupant*>& out) override {
        out.clear();
        cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(CityComponent::OccupantManager) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) return;
        auto* occupants = static_cast<cISC4OccupantManager*>(raw);
        const int a[2] = {rect.x0, rect.z0}, b[2] = {rect.x1, rect.z1};
        OccupantList found;
        occupants->GetOccupantsByStandardCityCells(found, a, b, static_cast<cISC4OccupantFilter*>(nullptr), 0xffffffffu);
        out.assign(found.begin(), found.end());
        occupants->Release();
    }
} g_lots;

// The landfill and port developers as the query rows ask them (vslots 0x50 GetLandfillUsage, 0x40 GetCurrentBudget; port 0x60 usage
// values for the lot, 0x70 GetMaintenanceCost).
struct QueryLandfill final : IQueryLandfillDeveloper {
    void GetLandfillUsage(std::uint32_t& usage, std::uint32_t& capacity) override {
        usage = capacity = 0;
        if (auto* d = CityAs<cSC4LandfillDeveloper>(CityComponent::LandfillDeveloper)) d->GetLandfillUsage(usage, capacity);
    }
    std::int64_t GetCurrentBudget() override {
        auto* d = CityAs<cSC4LandfillDeveloper>(CityComponent::LandfillDeveloper);
        return d ? d->GetCurrentBudget() : 0;
    }
} g_landfill;
struct QueryAirports final : IQueryPortDeveloper {
    bool GetUsageAndCapacity(IQueryLot* lot, std::uint32_t& usage, std::uint32_t& capacity) override {
        auto* d = CityAs<cSC4AirportDeveloper>(CityComponent::AirportDeveloper);
        auto* q = static_cast<QueryLot*>(lot);
        usage = capacity = 0;
        return d && q && d->GetAirportUsageValues(q->Lot(), usage, capacity);
    }
    std::int64_t GetMaintenanceCost(IQueryLot* lot) override {
        auto* d = CityAs<cSC4AirportDeveloper>(CityComponent::AirportDeveloper);
        auto* q = static_cast<QueryLot*>(lot);
        return d && q ? d->GetMaintenanceCost(q->Lot()) : 0;
    }
} g_airports;
struct QuerySeaports final : IQueryPortDeveloper {
    bool GetUsageAndCapacity(IQueryLot* lot, std::uint32_t& usage, std::uint32_t& capacity) override {
        auto* d = CityAs<cSC4SeaportDeveloper>(CityComponent::SeaportDeveloper);
        auto* q = static_cast<QueryLot*>(lot);
        usage = capacity = 0;
        return d && q && d->GetSeaportUsageValues(q->Lot(), usage, capacity);
    }
    std::int64_t GetMaintenanceCost(IQueryLot* lot) override {
        auto* d = CityAs<cSC4SeaportDeveloper>(CityComponent::SeaportDeveloper);
        auto* q = static_cast<QueryLot*>(lot);
        return d && q ? d->GetMaintenanceCost(q->Lot()) : 0;
    }
} g_seaports;

struct QueryPower final : IQueryPowerSim {
    bool IsPowered(sc4::cISC4Occupant* occupant) override {
        int x = 0, z = 0;
        if (!occupant) return false;
        OccupantCell(occupant, x, z);
        return NativeCellPowered(x, z);
    }
    void GetOccupantPowerProduced(sc4::cISC4Occupant* plant, std::uint32_t& capacity, std::uint32_t& used) override {
        capacity = used = 0;
        if (cSC4PowerSimulator* power = NativePowerSimulator()) power->GetOccupantPowerProduced(plant, capacity, used);
    }
    void GetOccupantMaxPowerProduced(sc4::cISC4Occupant* plant, std::uint32_t& out) override {
        out = 0;
        cSC4PowerSimulator::GetOccupantMaxPowerProduced(plant, out);
    }
    float GetCurrentPlantEfficiencyPercent(sc4::cISC4Occupant* plant) override {
        cSC4PowerSimulator* power = NativePowerSimulator();
        return power ? power->GetCurrentPlantEfficiencyPercent(plant) : 0.0f;
    }
    float GetCurrentPlantConditionPercent(sc4::cISC4Occupant* plant) override {
        cSC4PowerSimulator* power = NativePowerSimulator();
        return power ? power->GetCurrentPlantConditionPercent(plant) : 0.0f;
    }
    bool GetMonthlyPowerGenerationCost(sc4::cISC4Occupant* plant, std::int64_t& cost) override {
        cSC4PowerSimulator* power = NativePowerSimulator();
        return power && power->GetMonthlyPowerGenerationCost(plant, cost);
    }
    bool GetPowerPlantFundingPercentage(sc4::cISC4Occupant* plant, float& out) override {
        cSC4PowerSimulator* power = NativePowerSimulator();
        SC4Percentage percent;
        if (!power || !power->GetPowerPlantFundingPercentage(plant, percent)) return false;
        out = percent.value;
        return true;
    }
    // cSC4PowerSimulator::SetPowerPlantFundingPercentage (i386 0x00180986): the plant's budget item of purpose 0xca58e540, its
    // department's SetLocalFundingPercent (department vslot 0x60).
    void SetPowerPlantFundingPercentage(sc4::cISC4Occupant* plant, float fraction) override {
        auto* budget = CityAs<cSC4BudgetSimulator>(CityComponent::BudgetSimulator);
        sc4::cISCPropertyHolder* holder = plant ? plant->AsPropertyHolder() : nullptr;
        BudgetItem item;
        if (!budget || !holder || !budget->GetBudgetItemForPurpose(holder, 0xca58e540, item)) return;
        if (cSC4DepartmentBudget* department = budget->GetDepartmentBudget(static_cast<std::uint32_t>(item.departmentId)))
            department->SetLocalFundingPercent(holder, fraction, 0xca58e540);
    }
} g_power;

struct QueryPlumbing final : IQueryPlumbingSim {
    bool IsWatered(sc4::cISC4Occupant* occupant) override {
        int x = 0, z = 0;
        if (!occupant) return false;
        OccupantCell(occupant, x, z);
        return NativeCellWatered(x, z);
    }
    int GetNumPipes() override { cSC4PlumbingSimulator* p = NativePlumbingSimulator(); return p ? p->GetNumPipes() : 0; }
    int GetNumDistressedPipes() override { cSC4PlumbingSimulator* p = NativePlumbingSimulator(); return p ? p->GetNumDistressedPipes() : 0; }
    bool GetWaterProducerInfo(sc4::cISC4Occupant* occupant, std::uint32_t* current, std::uint32_t* maximum, float* efficiency,
                              bool* pumpWorking, std::uint32_t* used) override {
        cSC4PlumbingSimulator* plumbing = NativePlumbingSimulator();
        SC4Percentage percent;
        if (!plumbing || !plumbing->GetWaterProducerInfo(occupant, current, maximum, &percent, pumpWorking, used)) return false;
        if (efficiency) *efficiency = percent.value;
        return true;
    }
    bool GetWaterProducerConditionPercent(sc4::cISC4Occupant* occupant, float& out) override {
        cSC4PlumbingSimulator* plumbing = NativePlumbingSimulator();
        SC4Percentage percent;
        if (!plumbing || !plumbing->GetWaterProducerConditionPercent(occupant, percent)) return false;
        out = percent.value;
        return true;
    }
    bool GetOccupantMonthlyUpkeepCost(sc4::cISC4Occupant* occupant, std::int64_t& cost) override {
        cSC4PlumbingSimulator* plumbing = NativePlumbingSimulator();
        return plumbing && plumbing->GetOccupantMonthlyUpkeepCost(occupant, cost);
    }
} g_plumbing;

struct QueryAura final : IQueryAuraSim {
    std::int8_t GetAura(int x, int z) override {
        auto* aura = CityAs<cISC4AuraSimulator>(CityComponent::AuraSimulator);
        return aura ? aura->GetAuraValue(x, z) : 0;
    }
    std::int8_t GetMayorRating() override {
        auto* aura = CityAs<cISC4AuraSimulator>(CityComponent::AuraSimulator);
        return aura ? static_cast<std::int8_t>(aura->GetMayorRating()) : 0;
    }
} g_aura;

struct QuerySimulator final : IQuerySimulator {
    std::uint32_t FrameCount() override {
        cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(CityComponent::Simulator) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return 0;
        auto* serializable = static_cast<cIGZSerializable*>(raw);
        auto* simulator = serializable->GetGZCLSID() == kGZCLSID_cSC4Simulator ? static_cast<cSC4Simulator*>(serializable) : nullptr;
        serializable->Release();
        return simulator ? simulator->Counter(0) : 0;
    }
} g_simulator;

struct QueryResidential final : IQueryResidentialSim {
    void SetQueryCell(int, int) override {}   // PARTIAL: ToggleTractTracking (i386 res vslot 0xec), a debug trace
    static cSC4ResidentialSimulator* Sim() { return CityAs<cSC4ResidentialSimulator>(CityComponent::ResidentialSimulator); }
    static bool Query(sc4::cISC4Occupant* occupant, bool school, QueryEducationRecord& out) {
        cSC4ResidentialSimulator* r = Sim();
        IResidentialOccupant* building = r && occupant ? NativeResidentialOccupant(occupant) : nullptr;
        if (!building) return false;
        ResidentialQueryData data;
        const bool ok = school ? r->GetSchoolQueryData(building, data) : r->GetHospitalQueryData(building, data);
        out.count = data.served;
        out.grade = data.grade;
        out.staffFunding = data.staffFunding;
        out.staffCost = static_cast<std::int32_t>(data.staffCost);
        out.secondFunding = data.vehicleFunding;
        out.secondCost = static_cast<std::int32_t>(data.vehicleCost);
        return ok;
    }
    bool GetSchoolQueryData(sc4::cISC4Occupant* occupant, QueryEducationRecord& out) override { return Query(occupant, true, out); }
    bool GetHospitalQueryData(sc4::cISC4Occupant* occupant, QueryEducationRecord& out) override { return Query(occupant, false, out); }
    void EstimateCurrentOccupantCapacity(sc4::cISC4Occupant* occupant, std::uint32_t& first, std::uint32_t& second) override {
        first = second = 0;
        cSC4ResidentialSimulator* r = Sim();
        if (IResidentialOccupant* building = r && occupant ? NativeResidentialOccupant(occupant) : nullptr)
            r->EstimateCurrentOccupantCapacity(building, first, second);
    }
    void SetOccupantFundingPercentages(sc4::cISC4Occupant* occupant, float staff, float second, bool school) override {
        cSC4ResidentialSimulator* r = Sim();
        if (IResidentialOccupant* building = r && occupant ? NativeResidentialOccupant(occupant) : nullptr)
            r->SetOccupantFundingPercentages(building, staff, second, school);
    }
} g_residential;

struct QueryFire final : IQueryFireSim {
    static cSC4FireProtectionSimulator* Sim() { return CityAs<cSC4FireProtectionSimulator>(CityComponent::FireProtectionSimulator); }
    float StationEffectiveness(sc4::cISC4Occupant* station) override {
        cSC4FireProtectionSimulator* fire = Sim();
        IFireOccupant* occupant = fire && station ? NativeFireStation(station) : nullptr;
        return occupant ? fire->GetStationEfficiency(occupant) : 0.0f;
    }
    bool StationFunding(sc4::cISC4Occupant* station, int* cost, float* fraction) override {
        cSC4FireProtectionSimulator* fire = Sim();
        IFireOccupant* occupant = fire && station ? NativeFireStation(station) : nullptr;
        SC4Percentage percent;
        if (!occupant || !fire->GetStationFunding(occupant, cost, &percent)) return false;
        if (fraction) *fraction = percent.value;
        return true;
    }
    void SetStationFunding(sc4::cISC4Occupant* station, float fraction) override {
        cSC4FireProtectionSimulator* fire = Sim();
        if (IFireOccupant* occupant = fire && station ? NativeFireStation(station) : nullptr) fire->SetStationFunding(occupant, SC4Percentage(fraction));
    }
    bool DispatchInfo(sc4::cISC4Occupant* station, std::uint32_t* availability, std::uint32_t* baseCall, std::uint32_t* count) override {
        IFireOccupant* occupant = station ? NativeFireStation(station) : nullptr;
        return occupant && cSC4FireProtectionSimulator::GetDispatchesAtStation(occupant, availability, baseCall, count);
    }
} g_fire;

struct QueryPolice final : IQueryPoliceSim {
    static cSC4PoliceSimulator* Sim() { return CityAs<cSC4PoliceSimulator>(CityComponent::PoliceSimulator); }
    static IPoliceOccupant* Station(sc4::cISC4Occupant* occupant) { return occupant ? NativePoliceStation(occupant) : nullptr; }
    float StationEffectiveness(sc4::cISC4Occupant* station) override {
        cSC4PoliceSimulator* police = Sim();
        IPoliceOccupant* occupant = police ? Station(station) : nullptr;
        return occupant ? police->GetStationEfficiency(occupant) : 0.0f;
    }
    int ArrestCount(sc4::cISC4Occupant* station) override {
        IPoliceOccupant* occupant = Station(station);
        return occupant ? cSC4PoliceSimulator::GetPoliceStationArrestCount(occupant) : 0;
    }
    int CrimeCount(sc4::cISC4Occupant* station) override {
        IPoliceOccupant* occupant = Station(station);
        return occupant ? cSC4PoliceSimulator::GetPoliceStationCrimeCount(occupant) : 0;
    }
    bool StationFunding(sc4::cISC4Occupant* station, int* cost, float* fraction) override {
        cSC4PoliceSimulator* police = Sim();
        IPoliceOccupant* occupant = police ? Station(station) : nullptr;
        SC4Percentage percent;
        if (!occupant || !police->GetStationFunding(occupant, cost, &percent)) return false;
        if (fraction) *fraction = percent.value;
        return true;
    }
    void SetStationFunding(sc4::cISC4Occupant* station, float fraction) override {
        cSC4PoliceSimulator* police = Sim();
        if (IPoliceOccupant* occupant = police ? Station(station) : nullptr) police->SetStationFunding(occupant, SC4Percentage(fraction));
    }
    bool DispatchInfo(sc4::cISC4Occupant* station, std::uint32_t* availability, std::uint32_t* baseCall, std::uint32_t* count) override {
        IPoliceOccupant* occupant = Station(station);
        return occupant && cSC4PoliceSimulator::GetDispatchesAtStation(occupant, availability, baseCall, count);
    }
    float JailEfficiency() override { cSC4PoliceSimulator* police = Sim(); return police ? police->GetJailedPercentage() : 0.0f; }
    int JailCapacity(sc4::cISC4Occupant* jail) override {
        cSC4PoliceSimulator* police = Sim();
        IPoliceOccupant* occupant = police ? Station(jail) : nullptr;
        return occupant ? police->GetJailCapacity(occupant) : 0;
    }
    // cSC4PoliceSimulator::GetJailCurrentUpkeepCost(cISC4Occupant*) (i386 0x00077444): the jail's budget item 0xea56768a, its
    // cost times its local funding, truncated.
    std::int64_t JailMaintenance(sc4::cISC4Occupant* jail) override {
        auto* budget = CityAs<cSC4BudgetSimulator>(CityComponent::BudgetSimulator);
        sc4::cISCPropertyHolder* holder = jail ? jail->AsPropertyHolder() : nullptr;
        BudgetItem item;
        if (!budget || !holder || !budget->GetBudgetItemForPurpose(holder, 0xea56768a, item)) return 0;
        return static_cast<std::int64_t>(static_cast<float>(item.cost) * item.localFunding);
    }
} g_police;

struct QueryBudget final : IQueryBudgetSim {
    static cSC4BudgetSimulator* Sim() { return CityAs<cSC4BudgetSimulator>(CityComponent::BudgetSimulator); }
    bool OccupantValueCost(sc4::cISCPropertyHolder* holder, std::uint32_t value, std::int64_t& cost) override {
        Item item;
        if (!GetBudgetItemForPurpose(holder, value, item)) return false;
        cost = item.cost;
        return true;
    }
    bool GetBudgetItemForPurpose(sc4::cISCPropertyHolder* holder, std::uint32_t purpose, Item& out) override {
        cSC4BudgetSimulator* budget = Sim();
        BudgetItem item;
        out = Item{};
        if (!budget || !holder || !budget->GetBudgetItemForPurpose(holder, purpose, item)) return false;
        out.department = static_cast<std::uint32_t>(item.departmentId);
        out.cost = item.cost;
        return true;
    }
    bool DepartmentFundingFraction(std::uint32_t id, float& fraction) override {
        cSC4BudgetSimulator* budget = Sim();
        cSC4DepartmentBudget* department = budget ? budget->GetDepartmentBudget(id) : nullptr;
        if (!department) return false;
        fraction = department->GetFundingPercentage();
        return true;
    }
} g_budget;

// cSC4RegionalCity as the neighbor connection rows read it (its names); one reference.
struct NeighborCity final : IQueryNeighborCity {
    explicit NeighborCity(cSC4RegionalCity* city) : city_(city) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_) return refs_;
        delete this;
        return 0;
    }
    void GetCityName(cIGZString& out) const override { city_->GetCityName(out); }
    void GetMayorName(cIGZString& out) const override { city_->GetMayorName(out); }
    cSC4RegionalCity* City() const { return city_; }

private:
    cSC4RegionalCity* city_;
    std::uint32_t refs_ = 1;
};

struct QueryNeighbors final : IQueryNeighborsSim {
    static cSC4NeighborsSimulator* Sim() { return CityAs<cSC4NeighborsSimulator>(CityComponent::NeighborsSimulator); }
    bool IsValidConnectionCell(int x, int z) override { return cSC4NeighborsSimulator::IsValidConnectionCell(x, z, nullptr); }
    bool FindConnection(std::uint32_t type, int x, int z) override {
        cSC4NeighborsSimulator* neighbors = Sim();
        return neighbors && neighbors->FindConnection(type, x, z) != nullptr;
    }
    IQueryNeighborCity* FindCityOnOtherSide(int x, int z) override {
        cSC4NeighborsSimulator* neighbors = Sim();
        auto* city = neighbors ? dynamic_cast<cSC4RegionalCity*>(neighbors->FindCityOnOtherSideOfConnection(x, z)) : nullptr;
        return city ? new NeighborCity(city) : nullptr;
    }
    void FindDeals(std::uint32_t dealType, IQueryNeighborCity* city, std::vector<Deal>& out) override {
        out.clear();
        cSC4NeighborsSimulator* neighbors = Sim();
        auto* regional = static_cast<NeighborCity*>(city);
        if (!neighbors || !regional) return;
        DealList deals;
        neighbors->FindDeals(dealType, regional->City(), deals);
        for (const cRZAutoRefCount<cISC4NeighborDeal>& deal : deals) {
            if (!deal) continue;
            out.push_back(Deal{deal->CheckFlagsAnd(2), deal->GetAmount(), deal->GetCost()});
        }
    }
} g_neighbors;

struct QueryTraffic final : IQueryTrafficSim {
    static cSC4TrafficSimulator* Sim() { return CityAs<cSC4TrafficSimulator>(CityComponent::TrafficSimulator); }
    std::uint8_t CongestionCell(int x, int z) override {
        cSC4TrafficSimulator* traffic = Sim();
        return traffic ? CellOf(traffic->GetCongestionMap(), x, z) : 0;
    }
    bool GetTransitSwitchQueryData(int x, int z, TransitData& out) override {
        cSC4TrafficSimulator* traffic = Sim();
        TransitSwitchQueryData data;
        out = TransitData{};
        if (!traffic || !traffic->GetTransitSwitchQueryData(x, z, data)) return false;
        out.served = data.served;
        out.capacity = data.capacity;
        out.percent = data.percent;
        out.usage = data.usage;
        out.maintenance = data.maintenance;
        return true;
    }
} g_traffic;

struct QueryLandValue final : IQueryLandValueSim {
    int GetLandValueType(int x, int z) override {
        auto* landValue = CityAs<cSC4LandValueSimulator>(CityComponent::LandValueSimulator);
        return landValue ? landValue->GetLandValueType(x, z) : 0;
    }
} g_landValue;

// The garbage rows: the simulator's figures, and per building the adapter the simulator lists it under.
struct QueryPollution final : IQueryPollutionSim {
    static cSC4PollutionSimulator* Sim() { return CityAs<cSC4PollutionSimulator>(CityComponent::PollutionSimulator); }
    static IPollutionOccupant* Building(sc4::cISC4Occupant* occupant) { return occupant ? NativePollutionOccupantFor(occupant) : nullptr; }
    std::int64_t GarbageProducedPrevMonth() override { auto* p = Sim(); return p ? p->GetGarbageProducedPrevMonth() : 0; }
    std::int64_t GarbageRecycledPrevMonth() override { auto* p = Sim(); return p ? p->GetGarbageRecycledPrevMonth() : 0; }
    std::int64_t GarbageLandfilledPrevMonth() override { auto* p = Sim(); return p ? p->GetGarbageLandfilledPrevMonth() : 0; }
    int RecyclingCenterCount() override { auto* p = Sim(); return p ? p->GetRecyclingCenterCount() : 0; }
    float GarbageScalingFactor() override { auto* p = Sim(); return p ? p->GetGarbageScalingFactor() : 0.0f; }
    void RecyclingCenterMaxCapacity(sc4::cISC4Occupant* o, float& out) override {
        out = 0.0f;
        auto* p = Sim();
        if (IPollutionOccupant* b = p ? Building(o) : nullptr) p->GetRecyclingCenterMaxCapacity(b, out);
    }
    void RecyclingCenterCurrentCapacity(sc4::cISC4Occupant* o, float& out) override {
        out = 0.0f;
        auto* p = Sim();
        if (IPollutionOccupant* b = p ? Building(o) : nullptr) p->GetRecyclingCenterCurrentCapacity(b, out);
    }
    bool CurrentOccupantEfficiency(sc4::cISC4Occupant* o, float& out) override {
        out = 0.0f;
        auto* p = Sim();
        IPollutionOccupant* b = p ? Building(o) : nullptr;
        return b && p->GetCurrentOccupantEfficiency(b, out);
    }
    void IncineratorUsedCapacity(sc4::cISC4Occupant* o, std::uint32_t& out) override {
        out = 0;
        auto* p = Sim();
        if (IPollutionOccupant* b = p ? Building(o) : nullptr) p->GetIncineratorUsedCapacity(b, out);
    }
    void WasteToEnergyUsedCapacity(sc4::cISC4Occupant* o, std::uint32_t& out) override {
        out = 0;
        auto* p = Sim();
        if (IPollutionOccupant* b = p ? Building(o) : nullptr) p->GetWasteToEnergyUsedCapacity(b, out);
    }
    void IncineratorCurrentCapacity(sc4::cISC4Occupant* o, std::uint32_t& out) override {
        out = 0;
        auto* p = Sim();
        if (IPollutionOccupant* b = p ? Building(o) : nullptr) p->GetIncineratorCurrentCapacity(b, out);
    }
} g_pollution;

// cSC4NetworkOccupant as IQueryNetworkOccupant (the game's network occupant answers iid 0xa821ef94 with its own interface).
struct NetworkPiece final : IQueryNetworkOccupant {
    explicit NetworkPiece(cSC4NetworkOccupant* network) : network_(network) { network_->AddRef(); }
    ~NetworkPiece() { network_->Release(); }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_) return refs_;
        delete this;
        return 0;
    }
    bool IsOfType(std::uint32_t type) const override { return network_->IsOfType(type); }

private:
    cSC4NetworkOccupant* network_;
    std::uint32_t refs_ = 1;
};

template <class T> T* Serializable(CityComponent component, std::uint32_t clsid) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(component) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = serializable->GetGZCLSID() == clsid ? dynamic_cast<T*>(serializable) : nullptr;
    serializable->Release();
    return out;
}

// The route query's data (sc4::IQueryRoutes).
class QueryRoutes final : public IQueryRoutes {
public:
    // lot manager vslot 0x3c: every lot, its trips (cISC4TrafficSource vslot 0xc) and cell rect (vslot 0x14). The neighbours
    // simulator's connections follow in the original; their trip lists are not kept natively (cSC4NeighborConnection's list is a
    // placeholder), so the route query shows the lots' trips.
    void Sources(std::vector<QueryRouteSource>& lots, std::vector<QueryRouteSource>& neighbors) override {
        neighbors.clear();
        lots.clear();
        cSC4ZoneDeveloper* developer = Serializable<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper, cSC4ZoneDeveloper::kZoneDeveloperClassId);
        if (!developer) return;
        for (cSC4Lot* lot : developer->Lots()) {
            if (!lot) continue;
            QueryRouteSource source;
            source.trips = &lot->GetTripDataList();
            lot->GetBoundingRect(source.rect);
            lots.push_back(source);
        }
        SC4_UNWIRED("cSC4ViewInputControlQuery: the neighbor connections' trips are not kept natively (the route query shows the lots' routes)");
    }
    void CellCenter(int x, int z, float& worldX, float& worldZ) override {
        if (cSC4City* city = HostCity()) city->CellCenterToPosition(x, z, worldX, worldZ);
        else worldX = worldZ = 0.0f;
    }
    float Altitude(float x, float z) override {   // SafeGetAltitude: 0 off the terrain
        cSTETerrain* terrain = Terrain();
        return terrain && terrain->LocationIsInBounds(x, z) ? terrain->GetAltitude(x, z) : 0.0f;
    }
    float SeaLevel() override { cSTETerrain* terrain = Terrain(); return terrain ? terrain->GetSeaLevel() : 0.0f; }
    int Zoom() override {
        cSC43DRender* render = CityRenderer();
        SC4ViewUtilities* view = render ? render->GetViewUtilities() : nullptr;
        int zoom = 0, rotation = 0;
        if (view) view->GetZoomAndRotation(zoom, rotation);
        return zoom;
    }
    bool ButtonToggled(cIGZWin* window, std::uint32_t id) override { return window && nsGZWinUtils::GetButtonToggleState(window, id); }
    const std::vector<std::uint8_t>* FerryRoute(int x1, int z1, int x2, int z2) override {
        cSC4TrafficSimulator* traffic = Serializable<cSC4TrafficSimulator>(CityComponent::TrafficSimulator, kGZCLSID_cSC4TrafficSimulator);
        return traffic ? traffic->GetFerryRouteBetweenTiles(x1, z1, x2, z2) : nullptr;
    }
    // cISC4Occupant vslot 0x1c GetType: a building (its lot's rect: lot manager vslot 0x38 GetOccupantLot) or a network piece (its
    // cell and each type's edges).
    bool Describe(sc4::cISC4Occupant* occupant, QueryRouteOccupant& out) override {
        out = QueryRouteOccupant{};
        if (!occupant) return false;
        if (occupant->GetType() == 0x278128a0u) {
            cSC4ZoneDeveloper* developer = Serializable<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper, cSC4ZoneDeveloper::kZoneDeveloperClassId);
            cSC4Lot* lot = developer ? developer->GetOccupantLot(occupant) : nullptr;
            if (!lot) return false;
            out.kind = 1;
            lot->GetBoundingRect(out.rect);
            out.trips = &lot->GetTripDataList();
            out.bigBuilding = occupant->IsOccupantGroup(0x1003);
            return true;
        }
        auto* network = dynamic_cast<cSC4NetworkOccupant*>(occupant);
        if (!network) return false;
        out.kind = 2;
        std::uint32_t x = 0, z = 0;
        network->GetOccupiedCell(x, z);
        out.rect[0] = out.rect[2] = static_cast<std::int32_t>(x);
        out.rect[1] = out.rect[3] = static_cast<std::int32_t>(z);
        out.subway = network->IsOfType(7);
        for (std::uint32_t type = 0; type < 13; ++type) {
            if (!network->IsOfType(type)) continue;
            out.types |= 1u << type;
            out.edges[type] = network->GetEdgeTypesFlag(static_cast<std::uint8_t>(type));
        }
        return true;
    }
    std::uint16_t EdgeDensity(int x, int z, int travelType, bool reverse) override {
        cSC4TrafficSimulator* traffic = Serializable<cSC4TrafficSimulator>(CityComponent::TrafficSimulator, kGZCLSID_cSC4TrafficSimulator);
        return traffic ? traffic->GetTrafficEdgeDensity(x, z, travelType, reverse).density : 0;
    }

private:
    static cSTETerrain* Terrain() {
        cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(CityComponent::Terrain) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
        auto* terrain = static_cast<cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
        static_cast<cIGZSerializable*>(raw)->Release();
        return terrain;
    }
} g_routes;

// The debug query's read-outs (IQueryDebugReadouts): each one the simulator call the original makes, on the city's own objects.
struct QueryDebug final : IQueryDebugReadouts {
    static cSC4LandValueSimulator* LandValue() { return CityAs<cSC4LandValueSimulator>(CityComponent::LandValueSimulator); }
    static cSC4PollutionSimulator* Pollution() { return QueryPollution::Sim(); }
    std::uint8_t IntrinsicLandValue(int x, int z) override {
        cSC4LandValueSimulator* landValue = LandValue();
        return landValue ? CellOf(landValue->GetIntrinsicLandValueMap(), x, z) : 0;
    }
    std::uint8_t LandValue(int x, int z) override { cSC4LandValueSimulator* l = LandValue(); return l ? l->GetLandValue(x, z) : 0; }
    int LandValueType(int x, int z) override { cSC4LandValueSimulator* l = LandValue(); return l ? l->GetLandValueType(x, z) : 0; }
    float Altitude(float x, float z) override { return g_routes.Altitude(x, z); }
    bool CellPowered(int x, int z) override { cSC4PowerSimulator* power = NativePowerSimulator(); return power && power->IsPowered(x, z); }
    void PollutionValues(int x, int z, int& air, int& water, int& garbage, bool& radioactive) override {
        air = water = garbage = 0;
        radioactive = false;
        cSC4PollutionSimulator* pollution = Pollution();
        if (!pollution) return;
        const auto ux = static_cast<unsigned>(x), uz = static_cast<unsigned>(z);
        pollution->GetAirValue(ux, uz, air);
        pollution->GetWaterValue(ux, uz, water);
        pollution->GetGarbageValue(ux, uz, garbage);
        radioactive = pollution->IsRadioactive(ux, uz);
    }
    std::uint8_t Flammability(int x, int z) override { return CellOf(NativeFlammabilityGrid(), x, z); }
    bool NetworkOccupantAt(int x, int z) override {
        cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(CityComponent::OccupantManager) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) return false;
        auto* occupants = static_cast<cISC4OccupantManager*>(raw);
        sc4::cISC4Occupant* found = nullptr;
        const bool any = occupants->GetFirstOccupantByStandardCityCell(found, x, z, 0xc772bf98u);
        if (found) found->Release();
        occupants->Release();
        return any;
    }
    float NetworkCapacity(int type, int x, int z) override { cSC4TrafficSimulator* t = QueryTraffic::Sim(); return t ? t->GetCapacity(type, x, z) : 0.0f; }
    std::uint16_t EdgeDensity(int x, int z, int travelType, bool reverse) override { return g_routes.EdgeDensity(x, z, travelType, reverse); }
    std::uint8_t TripLength(int x, int z) override {
        cSC4TrafficSimulator* traffic = QueryTraffic::Sim();
        return traffic ? CellOf(traffic->GetTripLengthMap(), x, z) : 0;
    }
    float TripScale() override { cSC4TrafficSimulator* t = QueryTraffic::Sim(); return t ? t->GetTripScale() : 0.0f; }
    bool LotAt(int x, int z, bool& hasTravelDesignate, std::uint32_t jobs[3]) override {
        cSC4Lot* lot = NativeLotAtCell(x, z, false);
        hasTravelDesignate = false;
        jobs[0] = jobs[1] = jobs[2] = 0;
        if (!lot) return false;
        hasTravelDesignate = lot->GetTravelDesignate() != nullptr;
        for (std::uint32_t i = 0; i < 3; ++i) jobs[i] = cSC4TrafficSimulator::GetMaxTripCapacity(&lot->Properties(), i);
        return true;
    }
    bool OccupantPowerConsumed(sc4::cISC4Occupant* occupant, std::uint32_t& out) override {   // power 0x80 (i386 0x0018074a)
        sc4::cISCPropertyHolder* holder = occupant ? occupant->AsPropertyHolder() : nullptr;
        sc4::cISCProperty* property = holder ? holder->GetProperty(kPowerConsumed) : nullptr;
        cIGZVariant* value = property ? property->GetPropertyValue() : nullptr;
        if (value) out = value->AsUint32();
        return property != nullptr;
    }
    bool OccupantPowerProduced(sc4::cISC4Occupant* occupant, std::uint32_t& capacity, std::uint32_t& used) override {
        capacity = used = 0;
        cSC4PowerSimulator* power = NativePowerSimulator();
        return power && occupant && power->GetOccupantPowerProduced(occupant, capacity, used);
    }
    bool PollutionCurrentCapacity(std::uint32_t property, sc4::cISC4Occupant* occupant, std::uint32_t& out) override {
        out = 0;
        cSC4PollutionSimulator* pollution = Pollution();
        IPollutionOccupant* building = pollution ? QueryPollution::Building(occupant) : nullptr;
        return building && pollution->GetCurrentCapacity(property, building, out);
    }
    bool RecyclingCurrentCapacity(sc4::cISC4Occupant* occupant, float& out) override {
        out = 0.0f;
        cSC4PollutionSimulator* pollution = Pollution();
        IPollutionOccupant* building = pollution ? QueryPollution::Building(occupant) : nullptr;
        return building && pollution->GetRecyclingCenterCurrentCapacity(building, out);
    }
    bool IsBuildingWatered(sc4::cISC4Occupant* occupant) override {
        cSC4PlumbingSimulator* plumbing = NativePlumbingSimulator();
        return plumbing && plumbing->IsBuildingWatered(occupant);
    }
    std::uint32_t WaterProduction(sc4::cISC4Occupant* occupant) override {
        cSC4PlumbingSimulator* plumbing = NativePlumbingSimulator();
        return plumbing ? plumbing->GetWaterProduction(occupant) : 0;
    }
    bool DeveloperCell(std::uint32_t type, int x, int z, std::uint16_t& maxCapacity, std::uint16_t& capacity,
                       std::int16_t& desirability) override {
        // the original dereferences the simulator's answer unguarded; a type without a developer reads zeros
        return GrowthDeveloperCell(type, x, z, maxCapacity, capacity, desirability);
    }
    bool LotOccupancy(int x, int z, std::vector<Occupancy>& out) override {
        out.clear();
        cSC4Lot* lot = NativeLotAtCell(x, z, false);
        if (!lot) return false;
        std::map<std::uint32_t, float> shares;
        lot->GetOccupancyByType(shares);
        for (const auto& [type, share] : shares) {
            Occupancy entry;
            entry.type = type;
            entry.share = share;
            std::uint32_t subType = 0;
            if (GrowthDeveloperSubType(type, subType)) entry.available = lot->GetAvailableCapacity(subType, false);
            out.push_back(entry);
        }
        return true;
    }
} g_debug;

class Host final : public IQueryHost {
public:
    IQueryNetworkOccupant* NetworkOccupant(sc4::cISC4Occupant* occupant) override {
        auto* network = dynamic_cast<cSC4NetworkOccupant*>(occupant);
        return network ? new NetworkPiece(network) : nullptr;
    }
    std::uint32_t ZoneTypeAt(int x, int z) override {
        cSC4ZoneManager* zones = Serializable<cSC4ZoneManager>(CityComponent::ZoneManager, kZoneManagerClassId);
        return zones ? static_cast<std::uint32_t>(zones->GetZoneType(x, z)) : 0xff;
    }
    int RunModalDialog(cIGZWin* dialog) override {
        WinMgr* manager = ActiveWinMgr();
        const int result = manager ? manager->RunPopup(dialog) : -1;
        if (Trace()) std::fprintf(stderr, "query: dialog %p closed with %d\n", static_cast<void*>(dialog), result);
        return result;
    }
    void EndModalDialog(cIGZWin* dialog, std::uint32_t result) override {
        if (WinMgr* manager = ActiveWinMgr()) manager->EndDialog(0xf, dialog, result);
    }
    // PARTIAL: the window location saver runs natively (SL::spWinLocationSaver) but is not asked here; the dialog opens
    // centered over the game window each time.
    // SL::spWinLocationSaver vslot 0x24 RestoreWindowLocation(dialog, true): where the player last left a dialog of this id.
    // UNVERIFIED: with no saved place the original leaves the dialog where its script put it; the port centres it (the
    // native script service does not place top-level dialogs for the window size).
    void RestoreDialogPlace(cIGZWin* dialog) override {
        auto* saver = static_cast<cSLWinLocationSaver*>(SL::spWinLocationSaver);
        if (saver && saver->RestoreWindowLocation(dialog, true)) return;
        WinMgr* manager = ActiveWinMgr();
        cIGZWin* main = manager ? manager->GetMainWindow() : nullptr;
        if (main) dialog->GZWinMoveTo((main->GetW() - dialog->GetW()) / 2, (main->GetH() - dialog->GetH()) / 2);
    }
    // vslot 0x14 SaveWindowLocation(dialog) after the modal loop
    void SaveDialogPlace(cIGZWin* dialog) override {
        if (auto* saver = static_cast<cSLWinLocationSaver*>(SL::spWinLocationSaver)) saver->SaveWindowLocation(dialog);
    }
    void CloseDialog(cIGZWin* dialog) override {
        WinMgr* manager = ActiveWinMgr();
        if (!manager) return;
        manager->DestroyWindow(dialog);
        if (cSC4View3DWin* view = CityView3DWin()) manager->GZSetFocus(view);
    }
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    IQueryView* QueryView() override { return &g_view; }
    IQueryRenderer* Renderer() override { return &g_renderer; }
    IQueryRoutes* Routes() override { return &g_routes; }
    IQueryCity* City() override { return &g_city; }
    IQueryLotManager* LotManager() override { return &g_lots; }
    IQueryPowerSim* PowerSim() override { return &g_power; }
    IQueryPlumbingSim* PlumbingSim() override { return &g_plumbing; }
    IQueryAuraSim* AuraSim() override { return &g_aura; }
    IQuerySimulator* Simulator() override { return &g_simulator; }
    IQueryResidentialSim* ResidentialSim() override { return &g_residential; }
    IQueryFireSim* FireSim() override { return &g_fire; }
    IQueryPoliceSim* PoliceSim() override { return &g_police; }
    IQueryBudgetSim* BudgetSim() override { return &g_budget; }
    IQueryNeighborsSim* NeighborsSim() override { return &g_neighbors; }
    IQueryTrafficSim* TrafficSim() override { return &g_traffic; }
    IQueryPathView* CreatePathView() override { return NativeCreatePathView(); }   // new cSC4PathView + Init (app/NativePathView.cpp)
    cISC4WinCalloutBox* CreateCalloutBox() override { return native::CreateCalloutBox(); }
    cIGZWin* CreateWindowFromScript(std::uint32_t instance, cIGZWin* parent, std::uint32_t id) override {
        if (!parent) {   // SC4CreateWindowFromScript puts a parentless window on the main window
            WinMgr* manager = ActiveWinMgr();
            parent = manager ? manager->GetMainWindow() : nullptr;
        }
        cIGZWin* window = native::CreateWindowFromScript(cGZPersistResourceKey{0, kUIScriptGroup, instance}, parent, id);
        if (Trace()) std::fprintf(stderr, "query: script window %08x -> %p\n", instance, static_cast<void*>(window));
        return window;
    }
    void AttachLegendWinProc(cIGZWin* window, gz::cIGZWinProc* proc) override { SetProc(window, proc); }
    void DetachWinProc(cIGZWin* window) override { SetProc(window, nullptr); }
    static void SetProc(cIGZWin* window, gz::cIGZWinProc* proc) {
        void* raw = nullptr;
        if (!window || !window->QueryInterface(gz::kGZIID_cIGZWinGen, &raw) || !raw) return;
        static_cast<gz::cIGZWinGen*>(raw)->SetWinProc(proc);
        static_cast<gz::cIGZWinGen*>(raw)->Release();
    }
    // RZ::WM() vslot 0x98 GetKeyboard -> vslot 0xc IsKeyDown: the window manager's tracked key state (i386 0x0020b5a6)
    bool IsKeyDown(std::uint32_t key) override {
        WinMgr* manager = ActiveWinMgr();
        auto* keyboard = manager ? dynamic_cast<cRZKeyboard*>(manager->GetKeyboard()) : nullptr;
        return keyboard && keyboard->IsKeyDown(key);
    }
    bool IsDebugMode() override { return native::GameDebugMode(); }   // SL::spSC4: the app's debug functionality flag
    IQueryDebugReadouts* DebugReadouts() override { return &g_debug; }
    void FormatMoneyFloat(double value, cRZString& out) override { SC4MakeMoneyString(static_cast<std::int64_t>(value), out); }
    void LoadString(std::uint32_t group, std::uint32_t instance, cIGZString& out) override { LoadLocalizedString(group, instance, out); }
    bool LoadExemplarString(cIGZString& out, sc4::cISCPropertyHolder* holder, std::uint32_t property) override {
        return native::LoadExemplarString(out, holder, property);
    }
    IQueryLandValueSim* LandValueSim() override { return &g_landValue; }
    IQueryPollutionSim* PollutionSim() override { return &g_pollution; }
    // SL::spLandfillDeveloper / the city's airport (slot 0x268) and seaport (slot 0x260) developers.
    IQueryLandfillDeveloper* LandfillDeveloper() override { return CityAs<cSC4LandfillDeveloper>(CityComponent::LandfillDeveloper) ? &g_landfill : nullptr; }
    IQueryPortDeveloper* AirportDeveloper() override { return CityAs<cSC4AirportDeveloper>(CityComponent::AirportDeveloper) ? &g_airports : nullptr; }
    IQueryPortDeveloper* SeaportDeveloper() override { return CityAs<cSC4SeaportDeveloper>(CityComponent::SeaportDeveloper) ? &g_seaports : nullptr; }
    // the rating rows' per-cell maps (pollution sim 0x14(map) -> 0x1c, residential 0x9c / 0xa0, crime 0x14, traffic 0x24 / 0x34)
    std::uint16_t PollutionMapValue(int map, int x, int z) override {
        auto* pollution = CityAs<cSC4PollutionSimulator>(CityComponent::PollutionSimulator);
        return pollution ? static_cast<std::uint16_t>(CellOf(pollution->GetPollutionGrid(static_cast<PollutionType>(map)), x, z)) : 0;
    }
    float SchoolGradeValue(int x, int z) override { return CellOf(NativeResidentialGradeMap(true), x, z); }
    float HospitalGradeValue(int x, int z) override { return CellOf(NativeResidentialGradeMap(false), x, z); }
    std::uint8_t CrimeValue(int x, int z) override {
        cSC4CrimeSimulator* crime = NativeCrimeSimulator();   // a plain object behind its adapter
        return crime ? CellOf(crime->GetCrimeGrid(), x, z) : 0;
    }
    std::uint8_t TrafficValue(int x, int z) override {
        cSC4TrafficSimulator* traffic = QueryTraffic::Sim();
        return traffic ? CellOf(traffic->GetCommercialTrafficMap(), x, z) : 0;
    }
    std::uint8_t TripLengthValue(int x, int z) override {
        cSC4TrafficSimulator* traffic = QueryTraffic::Sim();
        return traffic ? CellOf(traffic->GetTripLengthMap(), x, z) : 0;
    }
    // FindBestDeveloper (i386 0x0020eab6): the lot's centre cell and zone; a built lot's developer by its building's purpose and
    // wealth (building development sim 0x9c profile, 0x78 GetDeveloperType, 0x84 developer), an empty one by desirability.
    IQueryGrowthDeveloper* FindBestDeveloper(IQueryLot* lot) override {
        if (!lot) return nullptr;
        float center[3] = {};
        lot->GetCenter(center);
        int cellX = 0, cellZ = 0;
        g_city.PositionToCell(center[0], center[2], cellX, cellZ);
        const std::uint32_t zone = ZoneTypeAt(cellX, cellZ);
        const std::uint32_t type = lot->GetBuildingType(false);
        std::uint32_t developerType = 0;
        if (type != 0) {
            IBuildingDevelopmentHost* types = BuildingDevelopment();
            if (!types) return nullptr;
            const BuildingProfile& profile = types->Profile(type);
            developerType = cSC4BuildingDevelopmentSimulator::GetDeveloperType_Static(profile.purpose, static_cast<std::int32_t>(profile.wealth));
            if (developerType == 0) return nullptr;
        }
        return NativeQueryGrowthDeveloper(developerType, zone, cellX, cellZ);
    }
    std::uint32_t BuildingTypeWealth(std::uint32_t type) override {
        IBuildingDevelopmentHost* types = BuildingDevelopment();
        return types ? types->Profile(type).wealth : 0;
    }
    std::uint32_t DeveloperTypeOf(std::uint32_t type, std::uint32_t wealth) override {
        IBuildingDevelopmentHost* types = BuildingDevelopment();
        if (!types) return 0;
        return cSC4BuildingDevelopmentSimulator::GetDeveloperType_Static(types->Profile(type).purpose, static_cast<std::int32_t>(wealth));
    }
    cIGZBuffer* LoadTipImage(std::uint32_t instance) override {
        cSLBuffer image(cGZPersistResourceKey{0x856ddbac, 0x46a006b0, instance}, 1, false);
        cIGZBuffer* buffer = image.GetBuffer();
        if (buffer) buffer->AddRef();
        static const bool trace = std::getenv("SC4_TIPDEBUG") != nullptr;
        if (trace) std::printf("tip image %08x: %s\n", instance, buffer ? "loaded" : "missing");
        return buffer;
    }
    bool TerrainCellHasWater(int x, int z) override {
        cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(CityComponent::Terrain) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return false;
        auto* terrain = static_cast<cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
        static_cast<cIGZSerializable*>(raw)->Release();
        return x >= 0 && z >= 0 && terrain->CellHasWater(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z));
    }
    // SL::spRM GetResource(key, 0xa52160f5): the exemplar's properties; kept for the session like the simulators' exemplars
    sc4::cISCPropertyHolder* GetTuningProperties(const cGZPersistResourceKey& key) override {
        static auto* held = new std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, cISCResExemplar*>();
        const auto id = std::make_tuple(key.type, key.group, key.instance);
        auto it = held->find(id);
        if (it == held->end()) {
            void* raw = nullptr;
            cISCResExemplar* exemplar = SL::spRM && SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) && raw ? static_cast<cISCResExemplar*>(raw) : nullptr;
            it = held->emplace(id, exemplar).first;
        }
        return it->second ? it->second->AsISCPropertyHolder() : nullptr;
    }
};

}  // namespace

// Kit report: every lot whose building has a query window (property 0x2a499f85) other than `skipUi`, with its name, window and
// world centre (the `querylist` kit command; a scenario aims the query tool with `look` + a click at the screen centre).
void NativeQueryReport(std::uint32_t skipUi, int limit) {
    cSC4City* city = HostCity();
    if (!city) { std::printf("querylist: no city\n"); return; }
    std::set<cSC4Lot*> seen;
    int shown = 0, total = 0;
    for (int x = 0; x < static_cast<int>(city->CellCountX()); ++x) {
        for (int z = 0; z < static_cast<int>(city->CellCountZ()); ++z) {
            cSC4Lot* lot = NativeLotAtCell(x, z, false);
            if (!lot || !seen.insert(lot).second) continue;
            sc4::cISC4Occupant* occupant = AsOccupant(lot->GetBuilding());
            if (!occupant) continue;
            std::uint32_t ui = 0;
            sc4::cISCPropertyHolder* holder = occupant->AsPropertyHolder();
            if (holder && cSCPropertyHelp::GetPropertyValue(holder, 0x2a499f85, ui) && ui != 0 && ui != skipUi) {
                ++total;
                if (shown < limit) {
                    cRZString name;
                    void* raw = nullptr;
                    if (occupant->QueryInterface(0x87dda3a9, &raw) && raw) {
                        static_cast<cISC4BuildingOccupant*>(raw)->GetName(name);
                        static_cast<cISC4BuildingOccupant*>(raw)->Release();
                    }
                    float c[3] = {};
                    lot->GetCenter(c);
                    std::printf("querylist: ui %08x at %.0f,%.0f '%s'\n", ui, c[0], c[2], name.ToChar());
                    ++shown;
                }
            }
            occupant->Release();
        }
    }
    std::printf("querylist: %d buildings\n", total);
}

// cSC4ViewInputControlQuery::DoQueryDialog for the advisors' window_query (i386 LUAExtension_window_query 0x00445ab4).
bool NativeOpenQueryDialog(sc4::cISC4Occupant* occupant) {
    return occupant && sc4::cSC4ViewInputControlQuery::DoQueryDialog(occupant);
}

void InstallQueryHost() {
    static Host host;
    sc4::SetQueryHost(&host);
}


// The single-building budget dialog's service queries (budget-window-bodies.md section 5.3), through the query tool's own
// simulator adapters: hospital / school query data and capacity, police / fire efficiency and station funding, power output,
// funding and cost.
bool NativeBuildingService(sc4::cISCPropertyHolder* holder, std::uint32_t kind, sc4::ISubBudgetWindowHost::BuildingService& out) {
    out = sc4::ISubBudgetWindowHost::BuildingService{};
    void* raw = nullptr;
    if (!holder || !holder->QueryInterface(sc4::kGZIID_cISC4Occupant, &raw) || !raw) return false;
    auto* occupant = static_cast<sc4::cISC4Occupant*>(raw);
    switch (kind) {
        case 0xca565486: case 0xea5654b6: {   // hospital / school
            QueryEducationRecord record;
            out.residential = kind == 0xea5654b6 ? g_residential.GetSchoolQueryData(occupant, record) : g_residential.GetHospitalQueryData(occupant, record);
            if (out.residential) {
                std::uint32_t capacity = 0, other = 0;
                g_residential.EstimateCurrentOccupantCapacity(occupant, capacity, other);
                out.served = static_cast<int>(record.count);
                out.capacity = static_cast<int>(capacity);
                out.staffFraction = record.staffFunding;
                out.staffCost = static_cast<std::uint32_t>(record.staffCost);
                out.vehicleFraction = record.secondFunding;
                out.vehicleCost = static_cast<std::uint32_t>(record.secondCost);
            }
            break;
        }
        case 0x0a567baa: case 0xea567bc3: {   // police / fire: the efficiency, then the cost and funding (its result unused)
            const bool police = kind == 0x0a567baa;
            out.station = true;
            out.efficiency = police ? g_police.StationEffectiveness(occupant) : g_fire.StationEffectiveness(occupant);
            int cost = 0;
            float funding = 0.0f;
            if (police) g_police.StationFunding(occupant, &cost, &funding);
            else g_fire.StationFunding(occupant, &cost, &funding);
            out.baseCost = cost;
            out.funding = funding;
            break;
        }
        case 0xca58e540: {   // power: output (used / capacity), funding, then cost
            std::uint32_t capacity = 0, used = 0;
            g_power.GetOccupantPowerProduced(occupant, capacity, used);
            out.denominator = static_cast<int>(capacity);
            out.numerator = static_cast<int>(used);
            out.powerFunding = g_power.GetPowerPlantFundingPercentage(occupant, out.funding);
            if (out.powerFunding) out.powerCost = g_power.GetMonthlyPowerGenerationCost(occupant, out.powerBaseCost);
            break;
        }
        default: break;
    }
    occupant->Release();
    return true;
}

// SetOccupantFundingLevel's calls: power / police / fire set their only funding; a hospital or school keeps the other half of
// its query data and sets the pair.
bool NativeSetBuildingFunding(sc4::cISCPropertyHolder* holder, std::uint32_t kind, bool secondary, float fraction) {
    void* raw = nullptr;
    if (!holder || !holder->QueryInterface(sc4::kGZIID_cISC4Occupant, &raw) || !raw) return false;
    auto* occupant = static_cast<sc4::cISC4Occupant*>(raw);
    bool done = false;
    switch (kind) {
        case 0xca58e540: if (!secondary) { g_power.SetPowerPlantFundingPercentage(occupant, fraction); done = true; } break;
        case 0x0a567baa: if (!secondary) { g_police.SetStationFunding(occupant, fraction); done = true; } break;
        case 0xea567bc3: if (!secondary) { g_fire.SetStationFunding(occupant, fraction); done = true; } break;
        case 0xca565486: case 0xea5654b6: {
            const bool school = kind == 0xea5654b6;
            QueryEducationRecord record;
            if (school ? g_residential.GetSchoolQueryData(occupant, record) : g_residential.GetHospitalQueryData(occupant, record)) {
                float staff = record.staffFunding, second = record.secondFunding;
                if (secondary) second = fraction;
                else staff = fraction;
                g_residential.SetOccupantFundingPercentages(occupant, staff, second, school);
                done = true;
            }
            break;
        }
        default: break;
    }
    occupant->Release();
    return done;
}

}  // namespace native
