// The query tool's services on the native application (sc4::IQueryHost): every answer goes through the simulator method the
// original calls (slots resolved from the Mac i386 vtables; the header names the x64 slot = twice the i386 one).
//   PARTIAL: the occupant filters accept any occupant with a model (the original builds one cSC4OccupantFilter subclass per
//   query kind); the view's mouse tips, the coverage ring, the path view, following an automaton, stoplight props, the
//   legend window proc, the residential tract tracking (a debug toggle), the landfill and port developers (still stubs).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "app/NativeCatalogUi.h"
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
#include "sc4/AuraSimulator.h"
#include "sc4/Exemplar.h"
#include "sc4/Budget.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/BuildingDevelopmentSimulator.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/CrimeSimulator.h"
#include "sc4/FireProtectionSimulator.h"
#include "sc4/LandValueSimulator.h"
#include "sc4/NeighborsSimulator.h"
#include "sc4/PlumbingSimulator.h"
#include "sc4/PoliceSimulator.h"
#include "sc4/PollutionSimulator.h"
#include "sc4/PowerSimulator.h"
#include "sc4/RegionalCity.h"
#include "sc4/ResidentialSimulator.h"
#include "sc4/TrafficSimulator.h"
#include "sc4/cSTETerrain.h"
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
// app/NativeSimHosts.cpp: the simulators' own occupant adapters
sc4::IFireOccupant* NativeFireStation(sc4::cISC4Occupant* occupant);
sc4::IPoliceOccupant* NativePoliceStation(sc4::cISC4Occupant* occupant);
sc4::IResidentialOccupant* NativeResidentialOccupant(cIGZUnknown* building);
sc4::IPollutionOccupant* NativePollutionOccupantFor(sc4::cISC4Occupant* occupant);

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
    bool ShowCoordinates() override { return false; }
    void SetTip(std::uint32_t, int, const cIGZString*, const cIGZString*, int) override {}
    void ClearTip(std::uint32_t) override {}
    void RecalcCoverage() override {}
} g_view;

struct AnyFilter final : IOccupantFilter {
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (--refs) return refs;
        delete this;
        return 0;
    }
    bool IsValid(const sc4::cISC4Occupant*) override { return true; }
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
        return new AnyFilter();
    }
    IPickResult* Pick(int x, int y, IOccupantFilter* filter) override {
        s3d::cIS3DModelInstance* model = nullptr;
        cSC43DRender* render = CityRenderer();
        if (Trace()) std::fprintf(stderr, "query: pick %d,%d renderer %p\n", x, y, static_cast<void*>(render));
        if (!render || !render->Pick(x, y, nullptr, model) || !model) return nullptr;
        sc4::cISC4Occupant* occupant = AsOccupant(model->GetOwner());
        if (!occupant) return nullptr;
        if (filter && !filter->IsValid(occupant)) {
            occupant->Release();
            return nullptr;
        }
        if (Trace()) std::fprintf(stderr, "query: pick %d,%d -> occupant %p type %08x\n", x, y, static_cast<void*>(occupant), occupant->GetType());
        return new PickResult(occupant);
    }
    void AddLayer(cIGZUnknown*) override {}
    void RemoveLayer(cIGZUnknown*) override {}
} g_renderer;

struct QueryCity final : IQueryCity {
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        if (cSC4City* city = HostCity()) city->PositionToCell(x, z, cellX, cellZ);
        else cellX = cellZ = 0;
    }
    int CellCountX() override { cSC4City* c = HostCity(); return c ? static_cast<int>(c->CellCountX()) : 0; }
    int CellCountZ() override { cSC4City* c = HostCity(); return c ? static_cast<int>(c->CellCountZ()) : 0; }
    void FollowAutomaton(cIGZUnknown*) override {}
    void SelectStoplightProp(cIGZUnknown*) override {}
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
} g_lots;

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
    // PARTIAL: there is no window location saver natively; the dialog opens centered over the game window each time.
    void RestoreDialogPlace(cIGZWin* dialog) override {
        WinMgr* manager = ActiveWinMgr();
        cIGZWin* main = manager ? manager->GetMainWindow() : nullptr;
        if (main) dialog->GZWinMoveTo((main->GetW() - dialog->GetW()) / 2, (main->GetH() - dialog->GetH()) / 2);
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
    IQueryPathView* CreatePathView() override { return nullptr; }
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
    void AttachLegendWinProc(cIGZWin*) override {}
    void DetachWinProc(cIGZWin*) override {}
    bool IsKeyDown(std::uint32_t) override { return false; }
    bool IsDebugMode() override { return false; }
    void FormatMoneyFloat(double value, cRZString& out) override { SC4MakeMoneyString(static_cast<std::int64_t>(value), out); }
    void LoadString(std::uint32_t group, std::uint32_t instance, cIGZString& out) override { LoadLocalizedString(group, instance, out); }
    bool LoadExemplarString(cIGZString& out, sc4::cISCPropertyHolder* holder, std::uint32_t property) override {
        return native::LoadExemplarString(out, holder, property);
    }
    IQueryLandValueSim* LandValueSim() override { return &g_landValue; }
    IQueryPollutionSim* PollutionSim() override { return &g_pollution; }
    // the rating rows' per-cell maps (pollution sim 0x14(map) -> 0x1c, residential 0x9c / 0xa0, crime 0x14, traffic 0x24 / 0x34)
    std::uint16_t PollutionMapValue(int map, int x, int z) override {
        auto* pollution = CityAs<cSC4PollutionSimulator>(CityComponent::PollutionSimulator);
        return pollution ? static_cast<std::uint16_t>(CellOf(pollution->GetPollutionGrid(static_cast<PollutionType>(map)), x, z)) : 0;
    }
    float SchoolGradeValue(int x, int z) override { return CellOf(NativeResidentialGradeMap(true), x, z); }
    float HospitalGradeValue(int x, int z) override { return CellOf(NativeResidentialGradeMap(false), x, z); }
    std::uint8_t CrimeValue(int x, int z) override {
        auto* crime = CityAs<cSC4CrimeSimulator>(CityComponent::CrimeSimulator);
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

void InstallQueryHost() {
    static Host host;
    sc4::SetQueryHost(&host);
}

}  // namespace native
