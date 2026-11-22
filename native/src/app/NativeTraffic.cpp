// The traffic simulator's native host (ITrafficHost): the city's lots as traffic sources and destinations, its network pieces for
// the network model, the roads / mass transit departments, history and the simulator agents. Also what the lots ask of the
// traffic simulator (travel strategy shares, freight scaling, their job capacities).
//
// PARTIAL: a missing department line item (the mass transit department's line item 3) is not created, and the simulator models
// no ferries (sc4/TrafficSimulator.cpp DoLotActivated). The neighbour connections, transit switches and tunnels are answered.
#include "app/NativeTraffic.h"
#include "platform/Unwired.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <tuple>
#include <initializer_list>
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
#include "sc4/PropOccupant.h"
#include "sc4/SurfaceWater.h"
#include "sc4/WaterPathFinder.h"
#include "sc4/cSTETerrain.h"
#include "sc4/Ordinance.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/TrafficSimulator.h"
#include "sc4/TrafficAutomataManager.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4SimGrid.h"
#include "sc4/cSC4City.h"

namespace native {   // NativeAutomataManagerHost.cpp / NativeTrafficAutomata.cpp
void NativeVisibleTerrainRect(float out[4]);
bool NativeCreateTrafficAccidentAtCell(int x, int z, int type, std::uint32_t milliseconds);
}
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

cSTETerrain* TrafficTerrain() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* terrain = static_cast<cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
    static_cast<cIGZSerializable*>(raw)->Release();
    return terrain;
}

cSTESurfaceWater* TrafficSurfaceWater() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::SurfaceWater) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISTESurfaceWater, &raw) || !raw) return nullptr;
    auto* water = static_cast<cSTESurfaceWater*>(static_cast<cIGZSerializable*>(raw));
    static_cast<cIGZSerializable*>(raw)->Release();
    return water;
}

// What the boats' route finder reads (SL::spSurfaceWater, SL::spTerrain, SL::spCity, the occupant in an occupant message).
class WaterPathHost final : public IWaterPathFinderHost {
public:
    void CityCellCount(int& x, int& z) override {
        cSC4City* city = HostCity();
        x = city ? city->CellCountX() : 0;
        z = city ? city->CellCountZ() : 0;
    }
    bool IsNavigable(int x, int z) override { cSTESurfaceWater* w = TrafficSurfaceWater(); return w && w->IsNavigable(x, z, 0, nullptr); }
    float WaterDepthForCell(int x, int z) override { cSTESurfaceWater* w = TrafficSurfaceWater(); return w ? w->GetWaterDepthForCell(x, z) : 0.0f; }
    std::uint16_t BodyOfWater(int x, int z) override {
        cSTESurfaceWater* w = TrafficSurfaceWater();
        cSC4SimGrid<std::uint16_t>* bodies = w ? w->GetBodyOfWaterMap() : nullptr;
        if (!bodies || x < 0 || z < 0 || x >= bodies->GetTractCountX() << bodies->GetTractShift() ||
            z >= bodies->GetTractCountZ() << bodies->GetTractShift())
            return 0;
        return bodies->GetCellValue(x, z);
    }
    bool CellHasWater(int x, int z) override {
        cSTETerrain* t = TrafficTerrain();
        return t && x >= 0 && z >= 0 && t->CellHasWater(std::uint32_t(x), std::uint32_t(z));
    }
    bool OccupantCells(void* occupant, int& x0, int& z0, int& x1, int& z1) override {
        void* raw = nullptr;
        auto* object = static_cast<cIGZUnknown*>(occupant);
        if (!object || !object->QueryInterface(kGZIID_cISC4Occupant, &raw) || !raw) return false;
        auto* o = static_cast<cISC4Occupant*>(raw);
        SC4CellRect r;
        o->GetBoundingCityCells(r);
        o->Release();
        x0 = r.x0; z0 = r.z0; x1 = r.x1; z1 = r.z1;
        return true;
    }
    void AddNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->AddNotification(target, message); }
    void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->RemoveNotification(target, message); }
};

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
    void Populations(std::vector<std::map<std::uint32_t, std::uint16_t>>& out) override { out = lot_->GetPopulations(); }
    bool IsSameLot(ITrafficLot* other) override {
        auto* view = dynamic_cast<LotView*>(other);
        return view && view->lot_ == lot_;
    }
    int Facing() override { return lot_->GetFacing(); }
    void* Building() override { return lot_->GetBuilding(); }
    cSC4Lot* Lot() const { return lot_; }
private:
    cSC4Lot* lot_;
};

class TrafficHost final : public ITrafficHost {
public:
    // a transit switch's building is the lot's building occupant (TrafficLot::Building), a COM object
    cIGZUnknown* BuildingObject(void* building) override { return static_cast<cIGZUnknown*>(building); }
    void PostMessage(std::uint32_t message) override { Post(message); }
    void PostPotholeMessage(int x, int z, bool potholed) override { Post(kTrafficMessagePotholeChanged, std::uint32_t(x), std::uint32_t(z), potholed ? 1 : 0); }
    void PostPotholeMessage3(int x, int z, std::uint32_t data3) override { Post(kTrafficMessagePotholeChanged, std::uint32_t(x), std::uint32_t(z), data3); }
    void PostTrafficMapChanged() override { Post(0x69247dc7); }
    bool VisibleTerrainRect(float out[4]) override { NativeVisibleTerrainRect(out); return true; }
    bool CreateAccidentAtCell(int x, int z, int type, std::uint32_t milliseconds) override {
        return NativeCreateTrafficAccidentAtCell(x, z, type, milliseconds);
    }
    void PostAccidentMessage(int x, int z) override { Post(0x0c05e4c3, std::uint32_t(x), std::uint32_t(z)); }
    void SetRoadBudgetExpense(int line, int amount) override { SetExpense(kRoadsDepartment, line, amount); }
    // spec traffic-simulator.md 9.1 TG7 (i386 0x0071f1af / 0x0071f263, then 0x0071f336..0x0071f45e)
    void EnsureDepartments(float roadsMax, float transitMax) override {
        cSC4BudgetSimulator* budget = Budget();
        if (!budget) return;
        auto line = [](cSC4DepartmentBudget* d, std::uint32_t id, std::uint32_t name, bool income) {
            if (cSC4LineItem* item = d->CreateLineItem(id, false)) {
                item->SetName(0x6a3ff01c, name);
                if (income) item->SetType(1);
            }
        };
        auto make = [&](std::uint32_t id, std::uint32_t name, float maxFunding, std::initializer_list<std::tuple<std::uint32_t, std::uint32_t, bool>> lines) {
            if (budget->GetDepartmentBudget(id)) return;
            cSC4DepartmentBudget* d = budget->CreateDepartmentBudget(id, 0xaa369059);
            if (!d) return;
            d->SetDepartmentName(0x6a3ff01c, name);
            d->SetMaxAllowedFundingPercentage(maxFunding);
            for (const auto& [n, label, income] : lines) line(d, n, label, income);
        };
        make(kRoadsDepartment, 0x4a493b73, roadsMax, {{1, 0xea8446be, false}, {2, 0x2a8446da, false}, {3, 0x2a8446df, false}});
        make(kMassTransitDepartment, 0xea493b83, transitMax, {{1, 0xea5f96e7, false}, {2, 0x8a5f978c, true}, {3, 0x6a8b0cfb, false}});
        // every Init, loaded departments too, in this order: the lines a department still lacks
        static constexpr struct { bool transit; std::uint32_t id, name; bool income; } kLater[] = {
            {true, 4, 0x2ab7ea93, false}, {true, 5, 0x8bce33e0, false}, {false, 4, 0xcbce33d5, false},
            {true, 6, 0x8bce33e1, false}, {false, 5, 0x0c3bf3c4, true},  {false, 6, 0xea5f96e7, false}};
        for (const auto& later : kLater) {
            cSC4DepartmentBudget* d = budget->GetDepartmentBudget(later.transit ? kMassTransitDepartment : kRoadsDepartment);
            if (d && !d->GetLineItem(later.id)) line(d, later.id, later.name, later.income);
        }
    }
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
    // The lot's building as a property holder (a station's switch rules and capacity are on its exemplar).
    sc4::cISCPropertyHolder* LotBuildingProperties(ITrafficLot* lot) override {
        auto* view = dynamic_cast<LotView*>(lot);
        cIGZUnknown* object = view ? view->Lot()->GetBuilding() : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kGZIID_cISC4Occupant, &raw) || !raw) return nullptr;
        auto* building = static_cast<cISC4Occupant*>(raw);
        sc4::cISCPropertyHolder* holder = building->AsPropertyHolder();
        building->Release();   // the lot keeps it
        return holder;
    }
    // occupant manager i386 0x88 GetFirstOccupantByStandardCityCells(rect, type): the occupant's position as a cell
    bool FirstOccupantCellOfType(const SC4RectInt32& rect, std::uint32_t type, int& x, int& z) override {
        cISC4OccupantManager* occupants = Occupants();
        const int a[2] = {rect.x0, rect.z0}, b[2] = {rect.x1, rect.z1};
        cISC4Occupant* found = nullptr;
        if (!occupants || !occupants->GetFirstOccupantByStandardCityCells(found, a, b, type) || !found) return false;
        float p[3] = {};
        found->GetPosition(p);
        found->Release();
        x = int(p[0] / 16.0f);
        z = int(p[2] / 16.0f);
        if (std::getenv("SC4_TRAFFICDEBUG")) std::fprintf(stderr, "traffic: subway entrance on the lot at %d,%d\n", x, z);
        return true;
    }
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
    // ---- ferries ----
    void PropsInRect(const SC4RectInt32& rect, std::vector<PropCell>& out) override {
        out.clear();
        cISC4OccupantManager* occupants = Occupants();
        if (!occupants) return;
        const int a[2] = {rect.x0, rect.z0}, b[2] = {rect.x1, rect.z1};
        OccupantList found;
        if (occupants->GetOccupantsByStandardCityCells(found, a, b, kOccupantTypeProp, 0xffffffffu))
            for (cISC4Occupant* occupant : found) {
                void* raw = nullptr;
                if (!occupant || !occupant->QueryInterface(0xe9793a65, &raw) || !raw) continue;   // cISC4PropOccupant
                auto* prop = static_cast<cISC4PropOccupant*>(raw);
                float p[3] = {};
                occupant->GetPosition(p);
                int x = 0, z = 0;
                PositionToCell(p[0], p[2], x, z);
                if (std::getenv("SC4_TRAFFICDEBUG")) std::fprintf(stderr, "ferry: prop %08x at %.1f,%.1f,%.1f cell %d,%d\n", prop->GetPropType(), p[0], p[1], p[2], x, z);
                out.push_back({prop->GetPropType(), x, z});
                prop->Release();
            }
        occupants->ReleaseOccupantList(found);
    }
    bool CellHasWater(int x, int z) override { return water_.CellHasWater(x, z); }
    void NeighborConnectionCellsOfType(int type, std::vector<std::pair<int, int>>& out) override {
        out.clear();
        cSC4NeighborsSimulator* n = Neighbors();
        if (!n) return;
        ConnectionList list;
        n->GetConnectionsOfType(std::uint32_t(type), list);
        for (auto& ref : list) {
            int x = 0, z = 0;
            ref->GetConnectionCell(x, z);
            out.emplace_back(x, z);
        }
    }
    IWaterPathFinderHost* WaterPathFinderHost() override { return &water_; }
    void PostFerryRoutesDone() override { Post(0x2c06ba82); }
    void EndOfWork() override { views_.clear(); }

private:
    WaterPathHost water_;
    ITrafficLot* View(cSC4Lot* lot) {
        if (!lot) return nullptr;
        auto& view = views_[lot];
        if (!view) view = std::make_unique<LotView>(lot);
        return view.get();
    }
    // spec 4.9.1: GetLineItem(i) then SetFullExpenses; Init made every line (EnsureDepartments), so a missing one is a broken save
    void SetExpense(std::uint32_t departmentId, int line, int amount) {
        cSC4BudgetSimulator* budget = Budget();
        cSC4DepartmentBudget* department = budget ? budget->GetDepartmentBudget(departmentId) : nullptr;
        cSC4LineItem* item = department ? department->GetLineItem(std::uint32_t(line)) : nullptr;
        if (item) item->SetFullExpenses(amount);
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

std::uint32_t NativeTrafficConnectedDestinationCount(const sc4::cISCPropertyHolder* lot, int kind) {
    cSC4TrafficSimulator* traffic = Traffic();
    return traffic && traffic->IsInitialized() ? traffic->GetConnectedDestinationCount(lot, kind) : 0;
}

cSC4SimGrid<std::uint8_t>* NativeTrafficCommercialMap() {
    cSC4TrafficSimulator* traffic = Traffic();
    return traffic && traffic->IsInitialized() ? traffic->GetCommercialTrafficMap() : nullptr;
}

cSC4SimGrid<std::uint8_t>* NativeTrafficTripLengthMap() {
    cSC4TrafficSimulator* traffic = Traffic();
    return traffic && traffic->IsInitialized() ? traffic->GetTripLengthMap() : nullptr;
}

}  // namespace native

namespace native {
// Kit verb `tunnels` (second half): the traffic simulator's tunnel records (traffic-network.md §2.5), each portal cell's
// wormhole bits and the car subnetwork labels of both portals (a tunnel joins them into one).
void NativeTrafficTunnelReport() {
    sc4::cSC4TrafficSimulator* traffic = Traffic();
    if (!traffic) {
        std::printf("traffic tunnels: no simulator\n");
        return;
    }
    std::printf("traffic tunnels: %zu records\n", traffic->TunnelCount());
    for (int x = 0; x < 256; ++x)
        for (int z = 0; z < 256; ++z) {
            const sc4::cSC4TrafficSimulator::TunnelInfo* tunnel = traffic->TunnelAt(x, z);
            if (!tunnel) continue;
            const sc4::NetworkInfo* here = traffic->GetNetworkInfo(1, x, z);
            const sc4::NetworkInfo* far = traffic->GetNetworkInfo(1, tunnel->otherX, tunnel->otherZ);
            std::printf("traffic tunnel %d,%d -> %u,%u types %03x network %u car cost %.4f wormhole %u, car subnetworks %u/%u/%u/%u -> %u/%u/%u/%u\n",
                        x, z, tunnel->otherX, tunnel->otherZ, tunnel->travelTypes, tunnel->networkType, tunnel->cost[1],
                        traffic->CellConnections(x, z).wormhole, here ? here->subnetwork[0] : 0, here ? here->subnetwork[1] : 0,
                        here ? here->subnetwork[2] : 0, here ? here->subnetwork[3] : 0, far ? far->subnetwork[0] : 0,
                        far ? far->subnetwork[1] : 0, far ? far->subnetwork[2] : 0, far ? far->subnetwork[3] : 0);
        }
}
// Kit command `accidentcheck N`: up to N monthly accident checks in a row (the real-time spacing cleared before each), stopping at
// the first that makes an accident. Lists the published accident-prone intersections first.
void NativeAccidentCheck(int tries) {
    cSC4TrafficSimulator* traffic = Traffic();
    if (!traffic) { std::printf("accidentcheck: no traffic simulator\n"); return; }
    std::printf("accidentcheck: %zu accident-prone intersections\n", traffic->AccidentProneIntersections().size());
    cSC4City* city = HostCity();
    auto* vehicles = city ? dynamic_cast<cSC4TrafficAutomataManager*>(city->GetComponent(CityComponent::VehicleManager)) : nullptr;
    int x0 = -1, z0 = -1;
    const bool before = vehicles && vehicles->EffectManager().GetMostRecentAccidentLocation(x0, z0);
    for (int i = 0; i < tries; ++i) {
        traffic->ClearAccidentClock();
        traffic->CheckForAccident();
        int x = -1, z = -1;
        if (vehicles && vehicles->EffectManager().GetMostRecentAccidentLocation(x, z) && (!before || x != x0 || z != z0)) {
            std::printf("accidentcheck: accident at %d,%d after %d checks\n", x, z, i + 1);
            return;
        }
    }
    std::printf("accidentcheck: no accident in %d checks\n", tries);
}

// Kit command `ferries`: every ferry terminal (cells, type, riders), its links to other terminals and to neighbour connections
// (cost, route length, counters), the departure cell's wormhole byte and the ferry routes in use (read-only).
void NativeFerryReport() {
    cSC4TrafficSimulator* traffic = Traffic();
    if (!traffic) { std::printf("ferries: no traffic simulator\n"); return; }
    std::printf("ferries: %zu terminals (passenger %d, car %d), rider total %u, cycle state %u\n", traffic->FerryTerminals().size(),
                traffic->GetFerryTerminalCount(0), traffic->GetFerryTerminalCount(1), traffic->FerryRiderTotal(), traffic->CycleState());
    for (const auto& t : traffic->FerryTerminals()) {
        std::printf("ferry terminal: lot %d,%d type %u mask %x departure %d,%d (wormhole %u) arrival %d,%d water %d,%d riders %u/%u links %zu neighbor links %zu\n",
                    t.lotX, t.lotZ, unsigned(t.type), t.travelTypes, t.departureX, t.departureZ,
                    unsigned(traffic->CellConnections(t.departureX, t.departureZ).wormhole), t.arrivalX, t.arrivalZ, t.waterX, t.waterZ,
                    unsigned(t.ridersLast), unsigned(t.ridersCurrent), t.links.size(), t.neighborLinks.size());
        for (const auto& l : t.links)
            std::printf("ferry link: to %d,%d arrival %d,%d cost %.2f route %zu bytes counters %u %u %u %u\n", l.key >> 8, l.key & 0xff,
                        l.arrivalX, l.arrivalZ, double(l.cost), l.route.size(), l.lastA, l.currentA, l.lastB, l.currentB);
        for (const auto& l : t.neighborLinks)
            std::printf("ferry neighbor link: off-map %d,%d edge %d,%d cost %.2f routes %zu/%zu bytes counters %u %u %u %u\n", l.x, l.z,
                        l.innerX, l.innerZ, double(l.cost), l.toEdge.size(), l.fromEdge.size(), l.lastA, l.currentA, l.lastB, l.currentB);
    }
    std::list<cSC4TrafficSimulator::FerryRouteInfo> inUse;
    traffic->GetFerryRoutesInUse(inUse);
    std::printf("ferries: %zu routes in use\n", inUse.size());
}

}  // namespace native

