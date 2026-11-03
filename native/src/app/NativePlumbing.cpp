// The plumbing simulator's city services (sc4::IPlumbingHost): the occupant manager, the lots, the terrain's water, the pollution
// simulator, the budget's water department, the pipes, the neighbour deals and connections, history, messages, the simulator agents
// and the city file's nested references.
#include "app/NativePlumbing.h"

#include <vector>

#include "app/NativeCity.h"
#include "app/NativePower.h"
#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/Budget.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/DBSegment.h"
#include "sc4/HistoryWarehouse.h"
#include "sc4/Lot.h"
#include "sc4/NeighborConnection.h"
#include "sc4/NeighborDeal.h"
#include "sc4/NeighborsSimulator.h"
#include "sc4/Occupant.h"
#include "sc4/OccupantManager.h"
#include "sc4/Ordinance.h"
#include "sc4/PipeOccupant.h"
#include "sc4/PlumbingSimulator.h"
#include "sc4/ServiceLocator.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4SimGrid.h"
#include "sc4/cSTETerrain.h"

namespace native {
namespace {
using namespace sc4;

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
cSC4HistoryWarehouse* History() { return Component<cSC4HistoryWarehouse>(CityComponent::HistoryWarehouse, kGZCLSID_cSC4HistoryWarehouse); }
cSC4BudgetSimulator* Budget() { return Component<cSC4BudgetSimulator>(CityComponent::BudgetSimulator, kGZCLSID_cSC4BudgetSimulator); }
cSC4NeighborsSimulator* Neighbors() { return Component<cSC4NeighborsSimulator>(CityComponent::NeighborsSimulator, kGZCLSID_cSC4NeighborsSimulator); }
cSC4ZoneDeveloper* LotManager() { return Component<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper, cSC4ZoneDeveloper::kZoneDeveloperClassId); }
cSC4Lot* LotAtCell(int x, int z) { cSC4ZoneDeveloper* lots = LotManager(); return lots ? lots->GetLot(x, z, false) : nullptr; }
cSC4Lot* LotUnder(cISC4Occupant* occupant) {
    float position[3] = {};
    occupant->GetPosition(position);
    return LotAtCell(int(position[0] / 16.0f), int(position[2] / 16.0f));
}
cSTETerrain* Terrain() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* terrain = static_cast<cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
    static_cast<cIGZSerializable*>(raw)->Release();
    return terrain;
}
cISC4OccupantManager* CityOccupants() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::OccupantManager) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
    auto* manager = static_cast<cISC4OccupantManager*>(raw);
    manager->Release();
    return manager;
}
cSC4PipeOccupant* Pipe(cISC4Occupant* occupant) { return dynamic_cast<cSC4PipeOccupant*>(occupant); }

class PlumbingHost final : public IPlumbingHost {
public:
    int CityCellsX() override { cSC4City* c = HostCity(); return c ? c->CellCountX() : 0; }
    int CityCellsZ() override { cSC4City* c = HostCity(); return c ? c->CellCountZ() : 0; }
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        cellX = int(x / 16.0f);
        cellZ = int(z / 16.0f);
    }
    void OccupantsAtCell(int x, int z, std::uint32_t type, std::vector<cISC4Occupant*>& out) override {
        Drop(held_);
        cISC4OccupantManager* occupants = CityOccupants();
        if (!occupants) return;
        const int cell[2] = {x, z};
        if (occupants->GetOccupantsByStandardCityCells(held_, cell, cell, type, 0xffffffffu))
            for (cISC4Occupant* occupant : held_) out.push_back(occupant);   // borrowed until the next query
    }
    // The zone tools' test (all four corners of the cell under water).
    bool IsWaterCell(int x, int z) override {
        cSTETerrain* t = Terrain();
        return t && t->IsWaterVertex(std::uint32_t(x), std::uint32_t(z)) && t->IsWaterVertex(std::uint32_t(x + 1), std::uint32_t(z)) &&
               t->IsWaterVertex(std::uint32_t(x), std::uint32_t(z + 1)) && t->IsWaterVertex(std::uint32_t(x + 1), std::uint32_t(z + 1));
    }
    int WaterPollutionAt(int x, int z) override { return NativeWaterPollutionAt(x, z); }
    float OrdinanceWaterFactor() override {
        auto* ordinances = Component<cSC4OrdinanceSimulator>(CityComponent::OrdinanceSimulator, kGZCLSID_cSC4OrdinanceSimulator);
        return ordinances ? ordinances->GetCumulativeFactor(0xa8f4eb0c, 0) : 1.0f;
    }
    const sc4::cISCPropertyHolder* SimulatorExemplar() override { return NativeSimulatorExemplar(0xc911e35b); }
    bool LotRectAt(int x, int z, PlumbingRect& out) override {
        cSC4Lot* lot = LotAtCell(x, z);
        if (!lot) return false;
        std::int32_t rect[4] = {};
        lot->GetBoundingRect(rect);
        out.left = rect[0];
        out.top = rect[1];
        out.right = rect[2];
        out.bottom = rect[3];
        return true;
    }
    bool ProducerLotUnpowered(cISC4Occupant* producer) override {
        cSC4Lot* lot = LotUnder(producer);
        return lot && lot->GetNeedsPower() && !lot->GetHasPower();
    }
    void SetLotConditionAt(int x, int z, int condition) override { if (cSC4Lot* lot = LotAtCell(x, z)) lot->SetCondition(std::uint32_t(condition)); }
    float DepartmentFundingFraction() override {
        cSC4DepartmentBudget* department = Department();
        return department ? department->GetFundingPercentage() : 1.0f;
    }
    // Init: a city without a water department gets one (named, its pipe infrastructure line item 2, the funding cap); one that has it
    // loses the old line item 1.
    void EnsureDepartment(float maxFunding) override {
        cSC4BudgetSimulator* budget = Budget();
        if (!budget) return;
        if (cSC4DepartmentBudget* department = budget->GetDepartmentBudget(kWaterDepartment)) {
            department->RemoveLineItem(1);
            return;
        }
        cSC4DepartmentBudget* department = budget->CreateDepartmentBudget(kWaterDepartment, kWaterDepartmentGroup);
        if (!department) return;
        department->SetDepartmentName(0x6a3ff01c, 0x2a4937d3);
        department->SetMaxAllowedFundingPercentage(maxFunding);
        if (cSC4LineItem* item = department->CreateLineItem(2, false)) item->SetName(0x6a3ff01c, 0x8a75f61e);
    }
    void SetPipeCost(std::int64_t cost) override {
        cSC4DepartmentBudget* department = Department();
        if (cSC4LineItem* item = department ? department->GetLineItem(2) : nullptr) item->SetFullExpenses(cost);
    }
    PlumbingBudgetEntry GetBudgetEntry(cISC4Occupant* occupant) override {
        PlumbingBudgetEntry entry;
        cSC4BudgetSimulator* budget = Budget();
        BudgetItem item;
        sc4::cISCPropertyHolder* holder = occupant ? occupant->AsPropertyHolder() : nullptr;
        if (!budget || !holder || !budget->GetBudgetItemForPurpose(holder, kBudgetItemWater, item)) return entry;
        entry.tracked = true;
        entry.monthlyCost = item.cost;
        cSC4DepartmentBudget* department = budget->GetDepartmentBudget(std::uint32_t(item.departmentId));
        entry.hasExplicitFunding = department && department->IsLocallyFundedObjectInDepartment(holder, kBudgetItemWater);
        entry.funding = !department ? 1.0f : entry.hasExplicitFunding ? item.localFunding : department->GetFundingPercentage();
        return entry;
    }
    // @ i386 0x000712a4..0x000713c2: the building's water budget item lives on a line item of its department named by the building
    // type (created with display flag 2 when missing); the line's expense and count follow the building in and out.
    void ApplyBuildingBudget(cISC4Occupant* building, bool added) override {
        cSC4BudgetSimulator* budget = Budget();
        sc4::cISCPropertyHolder* holder = building->AsPropertyHolder();
        BudgetItem item;
        if (!budget || !holder || !budget->GetBudgetItemForPurpose(holder, kBudgetItemWater, item)) return;
        cSC4DepartmentBudget* department = budget->GetDepartmentBudget(std::uint32_t(item.departmentId));
        if (item.lineItemId == 0) {
            void* raw = nullptr;
            if (building->QueryInterface(kGZIID_cISC4BuildingOccupant, &raw) && raw) {
                item.lineItemId = std::int32_t(static_cast<cISC4BuildingOccupant*>(raw)->GetBuildingType());
                static_cast<cISC4BuildingOccupant*>(raw)->Release();
            }
            budget->ChangeBudgetItemLine(holder, int(item.purpose), std::uint32_t(item.lineItemId));
            if (item.lineItemId == 0) return;
        }
        if (!department) return;
        cSC4LineItem* line = department->GetLineItem(std::uint32_t(item.lineItemId));
        if (!line && (line = department->CreateLineItemForBuildingType(std::uint32_t(item.lineItemId), false))) line->SetDisplayFlag(2, true);
        if (!line) return;
        line->AddToFullExpenses(added ? item.cost : -item.cost);
        line->SetSecondaryInfoField(line->GetSecondaryInfoField() + (added ? 1 : -1));
        cSCPropertyHelp::SetPropertyValue(holder, kPropBuildingBudgetCost, item.cost);
    }
    void SetProducerExpense(cISC4Occupant* producer, std::int64_t expense) override {
        cSC4BudgetSimulator* budget = Budget();
        sc4::cISCPropertyHolder* holder = producer->AsPropertyHolder();
        BudgetItem item;
        if (!budget || !holder || !budget->GetBudgetItemForPurpose(holder, kBudgetItemWater, item) || item.cost == expense) return;
        cSC4DepartmentBudget* department = budget->GetDepartmentBudget(std::uint32_t(item.departmentId));
        if (cSC4LineItem* line = department ? department->GetLineItem(std::uint32_t(item.lineItemId)) : nullptr) {
            line->AddToFullExpenses(expense - item.cost);
            budget->ChangeBudgetItemCost(holder, item.purpose, expense);
        }
    }
    bool IsPipe(cISC4Occupant* occupant) override {
        cSC4PipeOccupant* pipe = Pipe(occupant);
        return pipe && !pipe->HasNetworkFlag(0x80000000u);
    }
    std::uint32_t PipeFlags(cISC4Occupant* pipe) override { cSC4PipeOccupant* p = Pipe(pipe); return p ? p->GetNetworkFlag() : 0; }
    void SetPipeFlag(cISC4Occupant* pipe, std::uint32_t flag, bool on) override {
        if (cSC4PipeOccupant* p = Pipe(pipe)) on ? p->SetNetworkFlag(flag) : p->ClearNetworkFlag(flag);
    }
    bool PipeConnects(cISC4Occupant* pipe, int edge) override {
        cSC4PipeOccupant* p = Pipe(pipe);
        return p && p->ConnectsToNeighbor(edge, std::uint8_t(kPipeNetworkType));
    }
    void PipeCell(cISC4Occupant* pipe, int& x, int& z) override {
        std::uint32_t cx = 0, cz = 0;
        if (cSC4PipeOccupant* p = Pipe(pipe)) p->GetOccupiedCell(cx, cz);
        x = int(cx);
        z = int(cz);
    }
    void SetPipeWatered(cISC4Occupant* pipe, bool watered) override { if (cSC4PipeOccupant* p = Pipe(pipe)) p->SetWateredState(watered); }
    // PARTIAL: the pipe-burst disaster (disaster layer 0x4a1332e8, the leak effect at the cell) is not started natively; the pipe's
    // burst flag and the count still follow.
    void BurstPipeAt(int, int) override {}
    void Deals(std::uint32_t type, std::vector<cISC4NeighborDeal*>& out) override {
        cSC4NeighborsSimulator* neighbors = Neighbors();
        if (!neighbors) return;
        DealList deals;
        neighbors->FindDeals(type, deals);
        for (auto& deal : deals) if (deal) out.push_back(deal.get());   // the neighbours simulator keeps them
    }
    void* DealCity(cISC4NeighborDeal* deal) override { return deal ? deal->GetCityOnOtherSideOfConnection() : nullptr; }
    void Connections(void* city, std::vector<cISC4NeighborConnection*>& out) override {
        cSC4NeighborsSimulator* neighbors = Neighbors();
        if (!neighbors) return;
        ConnectionList connections;
        if (city) neighbors->GetConnectionsOfTypeToCity(kWaterConnectionType, static_cast<const cISC4RegionalCity*>(city), connections);
        else neighbors->GetConnectionsOfType(kWaterConnectionType, connections);
        for (auto& connection : connections) if (connection) out.push_back(connection.get());
    }
    cISC4NeighborConnection* UnvisitedConnection(void* city) override {
        cSC4NeighborsSimulator* neighbors = Neighbors();
        return neighbors ? neighbors->FindUnvisitedConnection(kWaterConnectionType, static_cast<const cISC4RegionalCity*>(city)) : nullptr;
    }
    bool OtherSideActive(cISC4NeighborConnection* connection) override {
        cSC4NeighborsSimulator* neighbors = Neighbors();
        cISC4NeighborConnection* other = neighbors ? neighbors->FindConnectionOnOtherSide(connection) : nullptr;
        return other && other->GetIsActive();
    }
    void RefreshNeighborDeals() override { if (cSC4NeighborsSimulator* neighbors = Neighbors()) neighbors->MarkDealsDirty(); }
    void PostMessage(std::uint32_t type, std::uint32_t data) override {
        if (!SL::spMS2) return;
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(type);
        message->SetData1(data);
        SL::spMS2->MessagePost(message, false);
        message->Release();
    }
    void SendUpdated(cIGZUnknown* simulator) override {
        if (!SL::spMS2) return;
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(kMsgPlumbingUpdated);
        message->SetVoid1(simulator);
        message->SetIGZUnknown(simulator);
        SL::spMS2->MessageSend(message);
        message->Release();
    }
    void AddHistory(std::uint32_t id, std::int64_t value) override {
        if (cSC4HistoryWarehouse* history = History()) history->RecordHistoryData(id, static_cast<int>(value));
    }
    void AddAgent(cIGZMessageTarget2* target, int interval, const char* name, std::uint32_t flags) override { AddCitySimulatorAgent(target, interval, name, flags); }
    void RemoveAgent(cIGZMessageTarget2* target) override { RemoveCitySimulatorAgent(target); }
    void AddNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->AddNotification(target, message); }
    void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->RemoveNotification(target, message); }
    void WriteGrid(cIGZOStream& stream, cSC4SimGrid<std::uint8_t>* grid) override {
        SC4WriteGZSerializable(stream, grid ? static_cast<const cIGZUnknown*>(static_cast<cIGZSerializable*>(grid)) : nullptr);
    }
    bool ReadGrid(cIGZIStream& stream, cSC4SimGrid<std::uint8_t>** out) override {
        *out = nullptr;
        void* raw = nullptr;
        const bool ok = SC4ReadGZSerializable(stream, kGZIID_cIGZSerializableSimGrid, &raw, false);   // the serializable view
        auto* serializable = static_cast<cIGZSerializable*>(raw);
        *out = serializable ? dynamic_cast<cSC4SimGrid<std::uint8_t>*>(serializable) : nullptr;
        if (serializable && !*out) serializable->Release();
        return ok;
    }
    void WriteDepartment(cIGZOStream& stream) override {
        cSC4DepartmentBudget* department = Department();
        SC4WriteGZSerializable(stream, department ? static_cast<const cIGZUnknown*>(static_cast<cIGZSerializable*>(department)) : nullptr);
    }
    bool ReadDepartment(cIGZIStream& stream) override {
        void* raw = nullptr;
        const bool ok = SC4ReadGZSerializable(stream, kGZIID_cISC4DepartmentBudget, &raw, false);
        if (raw) static_cast<cIGZUnknown*>(raw)->Release();   // the budget simulator owns the department
        return ok;
    }
    void WriteOccupant(cIGZOStream& stream, cISC4Occupant* occupant) override { SC4WriteGZSerializable(stream, occupant); }
    bool ReadOccupant(cIGZIStream& stream, std::uint32_t iid, cISC4Occupant** out) override {
        *out = nullptr;
        void* raw = nullptr;
        const bool ok = SC4ReadGZSerializable(stream, iid, &raw, false);
        if (raw && iid != kGZIID_cISC4Occupant) {   // a pipe reference: the occupant view of it
            auto* unknown = static_cast<cIGZUnknown*>(raw);
            void* occupant = nullptr;
            unknown->QueryInterface(kGZIID_cISC4Occupant, &occupant);
            unknown->Release();
            raw = occupant;
        }
        *out = static_cast<cISC4Occupant*>(raw);
        return ok;
    }
    cSC4DepartmentBudget* Department() {
        cSC4BudgetSimulator* budget = Budget();
        return budget ? budget->GetDepartmentBudget(kWaterDepartment) : nullptr;
    }
    void CityClosed() { Drop(held_); }

private:
    static void Drop(OccupantList& list) {
        if (list.empty()) return;
        if (cISC4OccupantManager* occupants = CityOccupants()) occupants->ReleaseOccupantList(list);
        else for (cISC4Occupant* occupant : list) occupant->Release();
        list.clear();
    }
    OccupantList held_;
};

PlumbingHost g_plumbing;

}  // namespace

sc4::IPlumbingHost* NativePlumbingHost() { return &g_plumbing; }

sc4::cSC4PlumbingSimulator* NativePlumbingSimulator() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::PlumbingSimulator) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4PlumbingSimulator, &raw) || !raw) return nullptr;
    auto* simulator = static_cast<cSC4PlumbingSimulator*>(static_cast<cISC4PlumbingSimulator*>(raw));
    simulator->Release();
    return simulator->IsInitialized() ? simulator : nullptr;
}

bool NativeCellWatered(int x, int z) {
    cSC4PlumbingSimulator* simulator = NativePlumbingSimulator();
    return simulator ? simulator->IsCellWatered(x, z) : true;   // no simulator: every cell counts as watered (as before the port)
}

void NativePlumbingCityClosed() { g_plumbing.CityClosed(); }

}  // namespace native
