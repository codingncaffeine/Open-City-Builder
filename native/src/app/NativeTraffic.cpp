// The traffic simulator's native host (ITrafficHost): the city's lots as traffic sources and destinations, its network pieces for
// the network model, the roads / mass transit departments, history and the simulator agents. Also what the lots ask of the
// traffic simulator (travel strategy shares, freight scaling, their job capacities).
//
// PARTIAL (each a to-do): the neighbour connections (no regional commuters or exits: every neighbour seam answers "none"), the
// mass transit department's missing line item 3 is not created, transit switches / tunnels / ferries (the simulator models none).
#include "app/NativeTraffic.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "app/NativeCity.h"
#include "app/NativeGrowthDevelopers.h"
#include "app/NativeSimHosts.h"
#include "gz/SimHostRegistry.h"
#include "gz/cGZPersistResourceManager.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/Budget.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/CommonIIDs.h"
#include "sc4/Exemplar.h"
#include "sc4/HistoryWarehouse.h"
#include "sc4/DemandSimulator.h"
#include "sc4/Lot.h"
#include "sc4/NeighborConnection.h"
#include "sc4/NeighborsSimulator.h"
#include "sc4/RegionalCity.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/OccupantManager.h"
#include "sc4/Ordinance.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/TrafficSimulator.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4SimGrid.h"
#include "sc4/cSC4City.h"

namespace native {
namespace {
using namespace sc4;

constexpr std::uint32_t kRoadsDepartment = 0xc921ceea, kMassTransitDepartment = 0xc921ceeb;

template <class T> T* Component(CityComponent c, std::uint32_t clsid) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = serializable->GetGZCLSID() == clsid ? static_cast<T*>(serializable) : nullptr;
    serializable->Release();
    return out;
}
cSC4Simulator* Simulator() { return Component<cSC4Simulator>(CityComponent::Simulator, kGZCLSID_cSC4Simulator); }
cSC4BudgetSimulator* Budget() { return Component<cSC4BudgetSimulator>(CityComponent::BudgetSimulator, kGZCLSID_cSC4BudgetSimulator); }
cSC4ZoneDeveloper* ZoneDeveloper() { return Component<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper, cSC4ZoneDeveloper::kZoneDeveloperClassId); }
cSC4NeighborsSimulator* Neighbors() { return Component<cSC4NeighborsSimulator>(CityComponent::NeighborsSimulator, kGZCLSID_cSC4NeighborsSimulator); }
cSC4RegionalCity* Regional(void* city) { return static_cast<cSC4RegionalCity*>(static_cast<cISC4RegionalCity*>(city)); }
cSC4TrafficSimulator* Traffic() { return Component<cSC4TrafficSimulator>(CityComponent::TrafficSimulator, kGZCLSID_cSC4TrafficSimulator); }
cISC4OccupantManager* Occupants() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::OccupantManager) : nullptr;
    return object ? dynamic_cast<cISC4OccupantManager*>(object) : nullptr;
}

const sc4::cISCPropertyHolder* TrafficExemplar() {
    static cISCResExemplar* exemplar = nullptr;
    static bool asked = false;
    if (!asked && SL::spRM) {
        asked = true;
        void* raw = nullptr;
        const cGZPersistResourceKey key{0x6534284a, 0xe7e2c2db, 0xc9133286};
        if (SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) && raw) exemplar = static_cast<cISCResExemplar*>(raw);
    }
    return exemplar ? exemplar->AsISCPropertyHolder() : nullptr;
}

void Post(std::uint32_t type, std::uint32_t d1 = 0, std::uint32_t d2 = 0, std::uint32_t d3 = 0) {
    if (!SL::spMS2) return;
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    message->SetData1(d1);
    message->SetData2(d2);
    message->SetData3(d3);
    SL::spMS2->MessagePost(message, false);
    message->Release();
}

// A lot as the traffic simulator sees it; holds a reference while it lives.
class LotView final : public ITrafficLot {
public:
    explicit LotView(cSC4Lot* lot) : lot_(lot) { lot_->AddRef(); }
    ~LotView() override { lot_->Release(); }
    sc4::cISCPropertyHolder* Properties() override { return &lot_->Properties(); }
    void OccupiedRect(SC4RectInt32& rect) override {
        std::int32_t r[4];
        lot_->GetBoundingRect(r);
        rect.x0 = r[0]; rect.z0 = r[1]; rect.x1 = r[2]; rect.z1 = r[3];
    }
    void* TrafficSource() override { return lot_; }
    std::list<cSC4TripData>* TripList(void*) override { return &lot_->GetTripDataList(); }
    void RefreshTrafficSource(void*) override { lot_->UpdateTrafficVolume(); }
    void Location(int& x, int& z) override { lot_->GetLocation(x, z); }
    int ZoneType() override { return lot_->GetZoneType(); }
    bool HasRoadAccess() override { return lot_->HasRoadAccess(); }
    bool IsSameLot(ITrafficLot* other) override {
        auto* view = dynamic_cast<LotView*>(other);
        return view && view->lot_ == lot_;
    }
    cSC4Lot* Lot() const { return lot_; }
private:
    cSC4Lot* lot_;
};

class TrafficHost final : public ITrafficHost {
public:
    void PostMessage(std::uint32_t message) override { Post(message); }
    void PostPotholeMessage(int x, int z, bool potholed) override { Post(kTrafficMessagePotholeChanged, std::uint32_t(x), std::uint32_t(z), potholed ? 1 : 0); }
    void PostPotholeMessage3(int x, int z, std::uint32_t data3) override { Post(kTrafficMessagePotholeChanged, std::uint32_t(x), std::uint32_t(z), data3); }
    void PostTrafficMapChanged() override { Post(0x69247dc7); }
    void SetRoadBudgetExpense(int line, int amount) override { SetExpense(kRoadsDepartment, line, amount); }
    void SetMassTransitBudgetExpense(int line, int amount) override { SetExpense(kMassTransitDepartment, line, amount); }
    float MassTransitFundingFraction() override { return Funding(kMassTransitDepartment); }
    float RoadFundingFraction() override { return Funding(kRoadsDepartment); }
    void SetBudgetIncome(bool massTransit, int line, std::int64_t amount) override {
        cSC4BudgetSimulator* budget = Budget();
        cSC4DepartmentBudget* department = budget ? budget->GetDepartmentBudget(massTransit ? kMassTransitDepartment : kRoadsDepartment) : nullptr;
        if (cSC4LineItem* item = department ? department->GetLineItem(std::uint32_t(line)) : nullptr) item->SetIncome(amount);
    }
    float CityCellSize() override { return 16.0f; }
    bool IsRightHandTraffic() override { return true; }
    short DemandGridValue(std::uint32_t gridId, int x, int z) override {
        cSC4SimGrid<std::int16_t>* grid = GrowthDeveloperPopulationMap(gridId);
        if (!grid) return 0;
        const std::int32_t shift = grid->GetTractShift();
        return grid->TractIsInBounds(x >> shift, z >> shift) ? grid->GetCellValue(x, z) : 0;
    }
    void ActiveOrdinanceStrategyAdjustments(std::vector<std::vector<std::int32_t>>& out) override {
        auto* ordinances = Component<cSC4OrdinanceSimulator>(CityComponent::OrdinanceSimulator, kGZCLSID_cSC4OrdinanceSimulator);
        if (!ordinances) return;
        std::uint32_t count = static_cast<std::uint32_t>(ordinances->GetOrdinanceOnCount());
        std::vector<std::uint32_t> ids(count);
        if (count) ordinances->GetOrdinanceOnArray(ids.data(), count);
        for (std::uint32_t id : ids) {
            cISC4Ordinance* ordinance = ordinances->GetOrdinanceByID(id);
            const sc4::cISCPropertyHolder* holder = ordinance ? ordinance->GetMiscProperties() : nullptr;
            std::vector<std::int32_t> values;
            if (holder && cSCPropertyHelp::GetPropertyValue(holder, 0x8a612fee, values)) out.push_back(values);
        }
    }
    bool OrdinanceOn(std::uint32_t id) override {
        auto* ordinances = Component<cSC4OrdinanceSimulator>(CityComponent::OrdinanceSimulator, kGZCLSID_cSC4OrdinanceSimulator);
        if (!ordinances) return false;
        std::uint32_t count = static_cast<std::uint32_t>(ordinances->GetOrdinanceOnCount());
        std::vector<std::uint32_t> ids(count);
        if (count) ordinances->GetOrdinanceOnArray(ids.data(), count);
        for (std::uint32_t on : ids)
            if (on == id) return true;
        return false;
    }
    ITrafficLot* LotAt(int x, int z, int filter) override {
        cSC4ZoneDeveloper* developer = ZoneDeveloper();
        return View(developer ? developer->GetLot(x, z, filter == 1) : nullptr);
    }
    // ---- neighbour connections: the neighbours simulator's (loaded with the city) and the region's cities on the other side ----
    void* NeighborFindConnection(int type, int x, int z) override {
        cSC4NeighborsSimulator* n = Neighbors();
        return n ? n->FindConnection(std::uint32_t(type), x, z) : nullptr;
    }
    void* NeighborFindConnectionOnOtherSide(void* connection) override {
        cSC4NeighborsSimulator* n = Neighbors();
        return n && connection ? n->FindConnectionOnOtherSide(static_cast<cISC4NeighborConnection*>(connection)) : nullptr;
    }
    void NeighborGetAllConnections(std::vector<void*>& out) override {   // borrowed: the simulator keeps its references
        cSC4NeighborsSimulator* n = Neighbors();
        if (!n) return;
        ConnectionList all;
        n->GetAllConnections(all);
        for (auto& ref : all) out.push_back(ref.get());
    }
    void NeighborGetConnectedCities(std::vector<void*>& out) override {   // borrowed: the region keeps its cities
        cSC4NeighborsSimulator* n = Neighbors();
        if (!n) return;
        std::list<cRZAutoRefCount<cISC4RegionalCity>> cities;
        n->GetConnectedCities(cities, false);
        for (auto& ref : cities) out.push_back(ref.get());
    }
    sc4::cISCPropertyHolder* NeighborConnectionProperties(void* connection) override {
        auto* concrete = dynamic_cast<cSC4NeighborConnection*>(static_cast<cISC4NeighborConnection*>(connection));
        return concrete ? static_cast<cSCPropertyHolder*>(concrete) : nullptr;
    }
    void NeighborConnectionCell(void* connection, int& x, int& z) override { static_cast<cISC4NeighborConnection*>(connection)->GetConnectionCell(x, z); }
    int NeighborConnectionType(void* connection) override { return int(static_cast<cISC4NeighborConnection*>(connection)->GetType()); }
    void* NeighborConnectionCity(void* connection) override { return static_cast<cISC4NeighborConnection*>(connection)->GetCityOnOtherSideOfConnection(); }
    bool NeighborCityEstablished(void* city) override { return city && Regional(city)->GetEstablished(); }
    void NeighborCityRegionCell(void* city, int& x, int& z) override { Regional(city)->GetPosition(x, z); }
    int NeighborCityExtrapolatedPopulation(void* city, std::uint32_t purpose) override { return Regional(city)->GetExtrapolatedPopulation(purpose); }
    int RegionDemandTotal(std::uint32_t id) override {   // the regional node (demand.md §9): GetDemand(id, 0x10000)
        auto* demand = Component<cSC4DemandSimulator>(CityComponent::DemandSimulator, kGZCLSID_cSC4DemandSimulator);
        cSC4Demand* node = demand ? demand->GetDemand(id, 0x10000) : nullptr;
        return node ? static_cast<int>(node->QueryDemandValue()) : 0;
    }
    SC4RectInt32 TrafficSourceStartRect(void* source) override {
        SC4RectInt32 rect{};
        if (auto* lot = static_cast<cSC4Lot*>(source)) {
            std::int32_t r[4];
            lot->GetBoundingRect(r);
            rect = SC4RectInt32{r[0], r[1], r[2], r[3]};
        }
        return rect;
    }

    // ---- the live simulator ----
    void CityCellCount(int& x, int& z) override {
        cSC4City* city = HostCity();
        x = city ? city->CellCountX() : 0;
        z = city ? city->CellCountZ() : 0;
    }
    void AddAgent(cIGZMessageTarget2* target, int level, const char* name, std::uint32_t flags) override { AddCitySimulatorAgent(target, level, name, flags); }
    void RemoveAgent(cIGZMessageTarget2* target) override { RemoveCitySimulatorAgent(target); }
    void AddNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->AddNotification(target, message); }
    void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->RemoveNotification(target, message); }
    const sc4::cISCPropertyHolder* SimulatorExemplar() override { return TrafficExemplar(); }
    int DayOfMonth() override {
        int day = 0;
        if (cSC4Simulator* s = Simulator()) s->GetSimDate(nullptr, nullptr, &day, nullptr, nullptr);
        return day;
    }
    std::uint32_t SimDateNumber() override { cSC4Simulator* s = Simulator(); return s ? s->GetSimDateNumber() : 0; }
    void AllLots(std::vector<std::unique_ptr<ITrafficLot>>& out) override {
        cSC4ZoneDeveloper* developer = ZoneDeveloper();
        if (!developer) return;
        for (cSC4Lot* lot : developer->Lots())
            if (lot) out.push_back(std::make_unique<LotView>(lot));
    }
    ITrafficLot* LotFromMessage(cIGZUnknown* object) override {
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kGZIID_PlainObject, &raw) || !raw) return nullptr;
        auto* copy = static_cast<cSC4Lot*>(raw);
        std::int32_t r[4];
        copy->GetBoundingRect(r);
        cSC4ZoneDeveloper* developer = ZoneDeveloper();
        return View(developer ? developer->GetLot(r[0], r[1], false) : nullptr);
    }
    void NetworkOccupantsInRect(int x0, int z0, int x1, int z1, std::vector<cSC4NetworkOccupant*>& out) override {
        cISC4OccupantManager* occupants = Occupants();
        if (!occupants) return;
        const int a[2] = {x0, z0}, b[2] = {x1, z1};
        OccupantList found;
        std::set<cSC4NetworkOccupant*> seen;
        if (occupants->GetOccupantsByStandardCityCells(found, a, b, kOccupantTypeNetwork, 0xffffffffu))
            for (cISC4Occupant* occupant : found)
                if (auto* network = dynamic_cast<cSC4NetworkOccupant*>(occupant); network && seen.insert(network).second) {
                    network->AddRef();
                    out.push_back(network);
                }
        occupants->ReleaseOccupantList(found);
    }
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        cellX = static_cast<int>(std::floor(x / 16.0f));
        cellZ = static_cast<int>(std::floor(z / 16.0f));
    }
    void RecordHistory(std::uint32_t id, std::uint32_t value) override {
        auto* history = Component<cSC4HistoryWarehouse>(CityComponent::HistoryWarehouse, kGZCLSID_cSC4HistoryWarehouse);
        if (history) history->RecordHistoryData(id, static_cast<int>(value));
    }
    void EndOfWork() override { views_.clear(); }

private:
    ITrafficLot* View(cSC4Lot* lot) {
        if (!lot) return nullptr;
        auto& view = views_[lot];
        if (!view) view = std::make_unique<LotView>(lot);
        return view.get();
    }
    void SetExpense(std::uint32_t departmentId, int line, int amount) {
        cSC4BudgetSimulator* budget = Budget();
        cSC4DepartmentBudget* department = budget ? budget->GetDepartmentBudget(departmentId) : nullptr;
        if (cSC4LineItem* item = department ? department->GetLineItem(std::uint32_t(line)) : nullptr) item->SetFullExpenses(amount);
    }
    float Funding(std::uint32_t departmentId) {
        cSC4BudgetSimulator* budget = Budget();
        cSC4DepartmentBudget* department = budget ? budget->GetDepartmentBudget(departmentId) : nullptr;
        return department ? department->GetFundingPercentage() : 1.0f;
    }
    std::map<cSC4Lot*, std::unique_ptr<LotView>> views_;
} g_traffic;

}  // namespace

void InstallNativeTrafficHost() { gzcom::GetSimHosts().traffic = &g_traffic; }

float NativeTrafficFreightScaling() {
    cSC4TrafficSimulator* traffic = Traffic();
    return traffic ? traffic->GetFreightScalingFactor() : 0.0f;
}

void NativeTrafficStrategyPercentages(int wealth, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    cSC4TrafficSimulator* traffic = Traffic();
    if (!traffic || wealth < 0 || wealth > 3) return;
    std::vector<SC4Percentage> pct;
    traffic->GetTravelStrategyPercentages(unsigned(wealth), pct);
    for (int i = 0; i < 3 && i < int(pct.size()); ++i) out[i] = pct[std::size_t(i)].value;
}

cSC4SimGrid<std::uint8_t>* NativeTrafficCongestionMap() {
    cSC4TrafficSimulator* traffic = Traffic();
    return traffic && traffic->IsInitialized() ? traffic->GetCongestionMap() : nullptr;
}

cSC4SimGrid<std::uint8_t>* NativeTrafficTripLengthMap() {
    cSC4TrafficSimulator* traffic = Traffic();
    return traffic && traffic->IsInitialized() ? traffic->GetTripLengthMap() : nullptr;
}

}  // namespace native
