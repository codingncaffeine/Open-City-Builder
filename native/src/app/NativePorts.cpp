// The airport and seaport developers' city services (sc4::IAirportDeveloperHost / sc4::ISeaportHost; both answer the port developer
// base's sc4::IPortDeveloperHost): the simulator agent, the advisors, the zone manager, the lot manager (the zone developer), the
// budget, demand, the lot developer that rebuilds a port at its next stage, the lot configurations, the port exemplars, the traffic
// simulator's trips, the terrain's water and the lot props. Each method names the original call it stands for.
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>

#include "app/NativeCity.h"
#include "app/NativeLotConstruction.h"
#include "sc4/AdvisorSystem.h"
#include "sc4/AirportDeveloper.h"
#include "sc4/Budget.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/DemandSimulator.h"
#include "sc4/Exemplar.h"
#include "sc4/LotDeveloper.h"
#include "sc4/Occupant.h"
#include "sc4/OccupantManager.h"
#include "sc4/PortDeveloper.h"
#include "sc4/PropOccupantInterface.h"
#include "sc4/SeaportDeveloper.h"
#include "sc4/ServiceLocator.h"
#include "sc4/TrafficSimulator.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4ZoneManager.h"
#include "sc4/cSTETerrain.h"
#include "tools/city_probe_plain.h"

namespace native {
namespace {
using namespace sc4;

bool Trace() { static const bool on = std::getenv("SC4_PORTDEBUG") != nullptr; return on; }

template <class T> T* Serializable(CityComponent c, std::uint32_t clsid) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = serializable->GetGZCLSID() == clsid ? static_cast<T*>(serializable) : nullptr;
    serializable->Release();
    return out;
}
cSC4BudgetSimulator* Budget() { return Serializable<cSC4BudgetSimulator>(CityComponent::BudgetSimulator, kGZCLSID_cSC4BudgetSimulator); }
cSC4TrafficSimulator* Traffic() { return Serializable<cSC4TrafficSimulator>(CityComponent::TrafficSimulator, kGZCLSID_cSC4TrafficSimulator); }
cSC4ZoneManager* Zones() { return Serializable<cSC4ZoneManager>(CityComponent::ZoneManager, kZoneManagerClassId); }
cSC4ZoneDeveloper* Lots() { return Serializable<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper, cSC4ZoneDeveloper::kZoneDeveloperClassId); }
cSC4LotDeveloper* LotDeveloper() { return Serializable<cSC4LotDeveloper>(CityComponent::LotDeveloper, kGZCLSID_cSC4LotDeveloper); }
cSC4DemandSimulator* Demand() { return Serializable<cSC4DemandSimulator>(CityComponent::DemandSimulator, kGZCLSID_cSC4DemandSimulator); }
cSTETerrain* Terrain() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* terrain = static_cast<cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
    static_cast<cIGZSerializable*>(raw)->Release();
    return terrain;
}
cISC4OccupantManager* Occupants() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::OccupantManager) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
    auto* manager = static_cast<cISC4OccupantManager*>(raw);
    manager->Release();
    return manager;
}

// ---- the base's services ---------------------------------------------------------------------------------------------------------
class SimulatorApi final : public IPortSimulatorApi {
public:
    void AddAgent(cIGZMessageTarget2* target, int interval, const char* name, std::uint32_t flags) override {   // vslot 0x98
        AddCitySimulatorAgent(target, interval, name, flags);
    }
    void RemoveAgent(cIGZMessageTarget2* target) override { RemoveCitySimulatorAgent(target); }   // vslot 0xa8
} g_simulator;

class AdvisorApi final : public IPortAdvisorSystemApi {
public:
    void PushEvent(int event, void*) override {   // cSC4AdvisorSystem vslot 0x78 (the ports pass no subject)
        if (cSC4AdvisorSystem* system = cSC4AdvisorSystem::Instance()) system->PushEvent(event, nullptr);
        if (Trace()) std::fprintf(stderr, "ports: advisor event %08x\n", static_cast<unsigned>(event));
    }
} g_advisors;

// The city as the seaport sees it (the base's two calls plus the traffic simulator and the cell centre).
class TrafficApi final : public ISeaportTrafficApi {
public:
    void AddRef() override {}
    void Release() override {}
    std::uint32_t GetTrafficArrived(cISCPropertyHolder* source, std::uint32_t travelType) override {   // vslot 0xa8
        return source ? cSC4TrafficSimulator::GetTrafficArrived(source, travelType) : 0;
    }
    void SetMaxTripCapacity(cISCPropertyHolder* source, std::uint32_t travelType, std::uint32_t capacity) override {   // vslot 0x98
        if (source) cSC4TrafficSimulator::SetMaxTripCapacity(source, travelType, capacity);
    }
} g_traffic;

class CityApi final : public ISeaportCityApi {
public:
    IPortSimulatorApi* GetSimulator() override { return &g_simulator; }             // vslot 0x190
    IPortAdvisorSystemApi* GetAdvisorSystem() override { return cSC4AdvisorSystem::Instance() ? &g_advisors : nullptr; }   // vslot 0x288
    ISeaportTrafficApi* GetTrafficSimulator() override { return &g_traffic; }       // vslot 0x228
    void CellCenterToPosition(std::int32_t cellX, std::int32_t cellZ, float& x, float& z) override {   // vslot 0x308
        if (cSC4City* city = HostCity()) city->CellCenterToPosition(cellX, cellZ, x, z);
        else { x = float(cellX) * 16.0f + 8.0f; z = float(cellZ) * 16.0f + 8.0f; }
    }
} g_city;

class ZoneManagerApi final : public IPortZoneManagerApi {
public:
    // vslot 0x58 PlaceZone(region, 0, true, true, false, true, false, null, null, null): the lot's footprint loses its zone
    void ClearZone(const PortCellRegion& region) override {
        cSC4ZoneManager* zones = Zones();
        if (!zones) return;
        SC4CellRegion<int> cells;
        cells.bounds = SC4Rect<int>{region.x0, region.z0, region.x1, region.z1};
        cells.rowCount = region.rowCount;
        cells.bitCount = region.bitCount;
        cells.wordCount = region.wordCount;
        cells.rows = region.rows;
        zones->PlaceZone(cells, 0, true, true, false, true, false, nullptr, nullptr, nullptr);
    }
} g_zones;

class LotManagerApi final : public IPortLotManagerApi {
public:
    cSC4Lot* GetLot(int x, int z, bool exactCell) override { cSC4ZoneDeveloper* d = Lots(); return d ? d->GetLot(x, z, exactCell) : nullptr; }   // 0x28
    cSC4Lot* GetOccupantLot(cISC4Occupant* occupant) override { cSC4ZoneDeveloper* d = Lots(); return d ? d->GetOccupantLot(occupant) : nullptr; }   // 0x70
    bool CreateLot(int x, int z, int sizeX, int sizeZ, int facing, cSC4Lot** out) override {   // 0xb8
        *out = nullptr;
        cSC4ZoneDeveloper* d = Lots();
        cSC4Lot* lot = nullptr;
        const bool made = d && d->CreateLot(x, z, sizeX, sizeZ, facing, lot) && lot;
        if (Trace()) std::fprintf(stderr, "ports: new lot %d,%d %dx%d facing %d -> %s\n", x, z, sizeX, sizeZ, facing, made ? "made" : "refused");
        if (made) *out = lot;
        return made;
    }
} g_lots;

class BudgetApi final : public IPortBudgetApi {
public:
    void WithdrawFunds(std::int64_t amount) override { if (cSC4BudgetSimulator* b = Budget()) b->WithdrawFunds(amount); }   // vslot 0x40
} g_budget;

class DemandApi final : public IPortDemandApi {
public:
    void UpdateOccupantEffects(std::uint32_t buildingType, const SC4Percentage& oldUsage, const SC4Percentage& newUsage) override {   // vslot 0x38
        if (cSC4DemandSimulator* d = Demand()) d->UpdateOccupantEffects(buildingType, oldUsage, newUsage);
    }
} g_demand;

class LotDeveloperApi final : public IPortLotDeveloperApi {
public:
    bool ConstructDefaultLot(cSC4Lot* lot, IPortLotConfiguration* configuration, std::uint32_t objectId, bool flag) override {   // vslot 0x38
        cSC4LotDeveloper* d = LotDeveloper();
        cSC4LotConfiguration* resolved = configuration ? NativeLotConfiguration(configuration) : nullptr;
        const bool built = d && resolved && d->ConstructDefaultLot(lot, resolved, objectId, flag);
        if (Trace()) std::fprintf(stderr, "ports: lot configuration %08x built -> %d\n", configuration ? configuration->GetID() : 0u, built ? 1 : 0);
        return built;
    }
    void StartDemolishLot(cSC4Lot* lot) override { if (cSC4LotDeveloper* d = LotDeveloper()) d->StartDemolishLot(lot); }   // vslot 0x60
    void EndDemolishLot(cSC4Lot* lot) override { if (cSC4LotDeveloper* d = LotDeveloper()) d->EndDemolishLot(lot); }         // vslot 0x68
} g_lotDeveloper;

class BuildingDevelopmentApi final : public IPortBuildingDevelopmentApi {
public:
    IPortLotDeveloperApi* GetLotDeveloper() override { return LotDeveloper() ? &g_lotDeveloper : nullptr; }   // vslot 0x120
} g_buildingDevelopment;

class LotConfigurationsApi final : public IPortLotConfigurationManagerApi {
public:
    // vslot 0x40 GetLotConfiguration: the reference the lots keep answers the port's questions too (app/NativeLotConstruction.cpp)
    IPortLotConfiguration* GetLotConfiguration(std::uint32_t id) override { return NativePortLotConfiguration(id); }
} g_configurations;

class ResourceApi final : public IPortResourceApi {
public:
    cISCPropertyHolder* LoadExemplar(std::uint32_t instance) override {   // spRM GetResource({0x6534284a, 0x67cd5fa1, instance}, 0xa52160f5)
        void* raw = nullptr;
        const cGZPersistResourceKey key{0x6534284a, 0x67cd5fa1, instance};
        if (!SL::spRM || !SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) || !raw) return nullptr;
        auto* exemplar = static_cast<cISCResExemplar*>(raw);
        cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
        // The caller releases the holder: hand it its own reference and drop the exemplar's (the holder keeps the exemplar alive).
        holder->AddRef();
        exemplar->Release();
        return holder;
    }
} g_resources;

// The base's IPortDeveloperHost, shared by the two concrete hosts.
template <class Base> class PortHostOf : public Base {
public:
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    IPortSimulatorApi* Simulator() override { return &g_simulator; }
    IPortCityApi* City() override { return &g_city; }
    IPortZoneManagerApi* ZoneManager() override { return &g_zones; }
    IPortLotManagerApi* LotManager() override { return &g_lots; }
    IPortBudgetApi* Budget() override { return &g_budget; }
    IPortDemandApi* Demand() override { return &g_demand; }
    IPortBuildingDevelopmentApi* BuildingDevelopment() override { return &g_buildingDevelopment; }
    IPortLotConfigurationManagerApi* LotConfigurationManager() override { return &g_configurations; }
    IPortResourceApi* ResourceManager() override { return &g_resources; }
    // A lot-state message carries a copy of the lot (the zone developer host's LotObject, plain view); the port works on the lot
    // the lot manager holds at the same cell.
    cSC4Lot* MessageLot(cIGZUnknown* payload) override {
        void* raw = nullptr;
        if (!payload || !payload->QueryInterface(kGZIID_PlainObject, &raw) || !raw) return nullptr;
        auto* copy = static_cast<cSC4Lot*>(raw);
        int x = 0, z = 0;
        copy->GetLocation(x, z);
        cSC4ZoneDeveloper* d = Lots();
        return d ? d->GetLot(x, z, false) : nullptr;
    }
};

// ---- the airport ----------------------------------------------------------------------------------------------------------------
class AirportHost final : public PortHostOf<IAirportDeveloperHost> {
public:
    cSC4BudgetSimulator* BudgetSimulator() override { return native::Budget(); }
    cSC4TrafficSimulator* TrafficSimulator() override { return Traffic(); }
} g_airport;

// ---- the seaport: the budget, the terrain, the occupant manager and the lot props -----------------------------------------------
class LineItemApi final : public ISeaportLineItemApi {
public:
    explicit LineItemApi(cSC4LineItem* item) : item_(item) {}
    void AddRef() override {}
    void Release() override {}
    std::int64_t GetFullExpenses() const override { return item_->GetFullExpenses(); }
    std::int64_t GetCurrentExpenses() const override { return item_->GetCurrentExpenses(); }
    void SetFullExpenses(std::int64_t value) override { item_->SetFullExpenses(value); }
    void SetDisplayFlag(std::uint32_t flag, bool on) override { item_->SetDisplayFlag(flag, on); }
private:
    cSC4LineItem* item_;
};

class DepartmentApi final : public ISeaportDepartmentBudgetApi {
public:
    explicit DepartmentApi(cSC4DepartmentBudget* department) : department_(department) {}
    void AddRef() override {}
    void Release() override {}
    void SetDepartmentName(std::uint32_t group, std::uint32_t instance) override { department_->SetDepartmentName(group, instance); }
    void SetMaxAllowedFundingPercentage(const SC4Percentage& percentage) override { department_->SetMaxAllowedFundingPercentage(percentage.value); }
    ISeaportLineItemApi* CreateLineItem(std::uint32_t id, bool localFunding) override { return View(department_->CreateLineItem(id, localFunding)); }
    ISeaportLineItemApi* GetLineItem(std::uint32_t id) override { return View(department_->GetLineItem(id)); }
private:
    ISeaportLineItemApi* View(cSC4LineItem* item) {
        if (!item) return nullptr;
        auto& view = items_[item];
        if (!view) view = std::make_unique<LineItemApi>(item);
        return view.get();
    }
    cSC4DepartmentBudget* department_;
    std::map<cSC4LineItem*, std::unique_ptr<LineItemApi>> items_;
};

class BudgetSimApi final : public ISeaportBudgetSimApi {
public:
    ISeaportDepartmentBudgetApi* CreateDepartmentBudget(std::uint32_t id, std::uint32_t group) override {   // vslot 0xc8
        cSC4BudgetSimulator* b = native::Budget();
        return View(b ? b->CreateDepartmentBudget(id, group) : nullptr);
    }
    ISeaportDepartmentBudgetApi* GetDepartmentBudget(std::uint32_t id) override {   // vslot 0xd8
        cSC4BudgetSimulator* b = native::Budget();
        return View(b ? b->GetDepartmentBudget(id) : nullptr);
    }
private:
    ISeaportDepartmentBudgetApi* View(cSC4DepartmentBudget* department) {
        if (!department) return nullptr;
        auto& view = departments_[department];
        if (!view) view = std::make_unique<DepartmentApi>(department);
        return view.get();
    }
    std::map<cSC4DepartmentBudget*, std::unique_ptr<DepartmentApi>> departments_;
} g_seaportBudget;

class TerrainApi final : public ISeaportTerrainApi {
public:
    void AddRef() override {}
    void Release() override {}
    std::int32_t CellCountX() override { cSTETerrain* t = Terrain(); return t ? std::int32_t(t->CellCountX()) : 0; }   // vslot 0x38
    std::int32_t CellCountZ() override { cSTETerrain* t = Terrain(); return t ? std::int32_t(t->CellCountZ()) : 0; }   // vslot 0x40
    bool CellHasWater(std::uint32_t x, std::uint32_t z) override { cSTETerrain* t = Terrain(); return t && t->CellHasWater(x, z); }   // 0x110
    void SetAltitudeAtVertex(std::int32_t x, std::int32_t z, float altitude) override { if (cSTETerrain* t = Terrain()) t->SetAltitudeAtVertex(x, z, altitude); }   // 0x190
} g_terrain;

class OccupantManagerApi final : public ISeaportOccupantManagerApi {
public:
    void GetOccupantsByStandardCityCells(OccupantList& out, const int* a, const int* b, cISC4OccupantFilter* filter, std::uint32_t maxCount) override {   // 0x148
        if (cISC4OccupantManager* o = Occupants()) o->GetOccupantsByStandardCityCells(out, a, b, filter, maxCount);
    }
    void ReleaseOccupantList(OccupantList& list) override {   // 0x180
        if (cISC4OccupantManager* o = Occupants()) o->ReleaseOccupantList(list);
        else list.clear();
    }
} g_occupants;

class PropApi final : public ISeaportPropOccupantApi {
public:
    explicit PropApi(cISC4PropOccupant* prop) : prop_(prop) {}
    void AddRef() override { prop_->AddRef(); }
    void Release() override {
        prop_->Release();
        delete this;
    }
    void SetState(std::uint32_t state) override { prop_->SetState(state); }   // 0x58
private:
    cISC4PropOccupant* prop_;
};

class SeaportHostImpl final : public PortHostOf<ISeaportHost> {
public:
    ISeaportCityApi* SeaportCity() override { return &g_city; }
    ISeaportBudgetSimApi* BudgetSimulator() override { return &g_seaportBudget; }
    ISeaportTrafficApi* TrafficSimulator() override { return &g_traffic; }
    ISeaportTerrainApi* Terrain() override { return &g_terrain; }
    ISeaportOccupantManagerApi* OccupantManager() override { return &g_occupants; }
    ISeaportPropOccupantApi* AsPropOccupant(cISC4Occupant* occupant) override {   // QI 0xe9793a65, the caller releases it
        void* raw = nullptr;
        if (!occupant || !occupant->QueryInterface(kGZIID_cISC4PropOccupant, &raw) || !raw) return nullptr;
        return new PropApi(static_cast<cISC4PropOccupant*>(raw));
    }
} g_seaport;

}  // namespace

// The port developers' real hosts (replace the generated stub's): the base's calls go to the airport host, which serves both.
void InstallPortDeveloperHosts() {
    SetPortDeveloperHost(&g_airport);
    SetAirportDeveloperHost(&g_airport);
    SetSeaportHost(&g_seaport);
}

// Kit command `ports`: each port developer's ports (cell, stage, efficiency, usage, trips) and its required budget.
void NativePortReport() {
    auto report = [](const char* name, cSC4PortDeveloper* developer) {
        if (!developer) { std::printf("ports: no %s developer\n", name); return; }
        std::printf("ports: %s %zu ports, required budget %lld, average efficiency %.2f\n", name, developer->PortLots().size(),
                    static_cast<long long>(developer->GetRequiredBudget()), developer->GetAvgEfficiency());
        for (cPortLot* port : developer->PortLots())
            std::printf("ports: %s at %d,%d stage %d efficiency %.2f usage %.2f trips %d\n", name, port->X(), port->Z(), port->Stage(),
                        port->Efficiency(), port->Usage(), port->Trips());
    };
    report("airport", Serializable<cSC4AirportDeveloper>(CityComponent::AirportDeveloper, kGZCLSID_cSC4AirportDeveloper));
    report("seaport", Serializable<cSC4SeaportDeveloper>(CityComponent::SeaportDeveloper, kSeaportDeveloperClassId));
}

}  // namespace native
