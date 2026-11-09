// The power simulator's city services (sc4::IPowerHost): the occupant manager, the zones, the budget's power department, the lots,
// the neighbour deals and connections, history, messages, the simulator agents and the city file's nested references.
#include "app/NativePower.h"

#include <list>
#include <vector>

#include "app/NativeCity.h"
#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/Budget.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/CommonIIDs.h"
#include "sc4/DBSegment.h"
#include "sc4/HistoryWarehouse.h"
#include "sc4/Lot.h"
#include "sc4/NeighborConnection.h"
#include "sc4/NeighborDeal.h"
#include "sc4/NeighborsSimulator.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/Occupant.h"
#include "sc4/OccupantManager.h"
#include "sc4/Ordinance.h"
#include "sc4/PowerLineOccupant.h"
#include "sc4/PowerPoleOccupant.h"
#include "sc4/PowerSimulator.h"
#include "sc4/ServiceLocator.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4SimGrid.h"
#include "sc4/cSC4ZoneManager.h"

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
cSC4ZoneManager* Zones() { return Component<cSC4ZoneManager>(CityComponent::ZoneManager, kZoneManagerClassId); }

constexpr std::uint32_t kPowerDepartment = 0x8910bc8a, kPowerDepartmentGroup = 0x4a357eaf;
constexpr std::uint32_t kPowerConnectionType = 4;
constexpr std::uint32_t kPlantExpenseProperty = 0x2a58d350;   // the plant's monthly expense (budget item cost)

cISC4OccupantManager* CityOccupants() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::OccupantManager) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
    auto* manager = static_cast<cISC4OccupantManager*>(raw);
    manager->Release();
    return manager;
}
cSC4BudgetSimulator* Budget() { return Component<cSC4BudgetSimulator>(CityComponent::BudgetSimulator, kGZCLSID_cSC4BudgetSimulator); }
cSC4NeighborsSimulator* Neighbors() { return Component<cSC4NeighborsSimulator>(CityComponent::NeighborsSimulator, kGZCLSID_cSC4NeighborsSimulator); }
cSC4ZoneDeveloper* LotManager() { return Component<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper, cSC4ZoneDeveloper::kZoneDeveloperClassId); }
cSC4Lot* LotAtCell(int x, int z) { cSC4ZoneDeveloper* lots = LotManager(); return lots ? lots->GetLot(x, z, false) : nullptr; }
cSC4Lot* LotUnder(cISC4Occupant* occupant) {
    float position[3] = {};
    occupant->GetPosition(position);
    return LotAtCell(int(position[0] / 16.0f), int(position[2] / 16.0f));
}

class PowerHost final : public IPowerHost {
public:
    int CityCellsX() override { cSC4City* c = HostCity(); return c ? c->CellCountX() : 0; }
    int CityCellsZ() override { cSC4City* c = HostCity(); return c ? c->CellCountZ() : 0; }
    float CellSizeX() override { return 16.0f; }
    float CellSizeZ() override { return 16.0f; }
    void OccupantsInRect(const PowerRect& rect, std::vector<cISC4Occupant*>& out) override {
        Drop(held_);
        cISC4OccupantManager* occupants = CityOccupants();
        if (!occupants) return;
        const int a[2] = {rect.left, rect.top}, b[2] = {rect.right, rect.bottom};
        if (occupants->GetOccupantsByStandardCityCells(held_, a, b, static_cast<cISC4OccupantFilter*>(nullptr), 0xffffffffu))
            for (cISC4Occupant* occupant : held_) out.push_back(occupant);   // borrowed until the next query
    }
    bool WireCell(cISC4Occupant* occupant, int& x, int& z) override {
        switch (occupant->GetType()) {
        case kPowerOccupantPole: {
            auto* pole = dynamic_cast<cSC4PowerPoleOccupant*>(occupant);
            if (!pole || pole->IsPreview()) return false;
            pole->GetCell(x, z);
            return true;
        }
        case kPowerOccupantNetwork: {
            auto* line = dynamic_cast<cSC4PowerLineOccupant*>(occupant);
            if (!line || line->IsPreview()) return false;
            std::uint32_t cx = 0, cz = 0;
            line->GetCell(cx, cz);
            x = int(cx);
            z = int(cz);
            return true;
        }
        case kPowerOccupantPylon: {
            auto* network = dynamic_cast<cSC4NetworkOccupant*>(occupant);
            if (!network) return false;
            const std::uint32_t flags = network->GetNetworkFlag();
            if ((flags & 0x80000000u) || !(flags & 0x40000u)) return false;
            std::uint32_t cx = 0, cz = 0;
            network->GetOccupiedCell(cx, cz);
            x = int(cx);
            z = int(cz);
            return true;
        }
        default: return false;
        }
    }
    bool IsZoned(int x, int z) override { cSC4ZoneManager* m = Zones(); return m && m->GetZoneType(x, z) != 0; }
    void SetSupplyValue(int x, int z, std::uint8_t value) override {
        if (!supply_) EnsureSupplyGrid();
        if (supply_ && supply_->TractIsInBounds(x, z)) supply_->SetTractValue(x, z, value);
    }
    float OrdinancePowerFactor() override {
        auto* ordinances = Component<cSC4OrdinanceSimulator>(CityComponent::OrdinanceSimulator, kGZCLSID_cSC4OrdinanceSimulator);
        return ordinances ? ordinances->GetCumulativeFactor(0x0911e117, 0) : 1.0f;
    }
    PowerPlantBudgetEntry GetPlantBudgetEntry(cISC4Occupant* plant) override {
        PowerPlantBudgetEntry entry;
        cSC4BudgetSimulator* budget = Budget();
        BudgetItem item;
        sc4::cISCPropertyHolder* holder = plant ? plant->AsPropertyHolder() : nullptr;
        if (!budget || !holder || !budget->GetBudgetItemForPurpose(holder, kPowerPlantBudgetProperty, item)) return entry;
        entry.tracked = true;
        entry.monthlyCost = item.cost;
        cSC4DepartmentBudget* department = budget->GetDepartmentBudget(static_cast<std::uint32_t>(item.departmentId));
        entry.hasExplicitFunding = department && department->IsLocallyFundedObjectInDepartment(holder, kPowerPlantBudgetProperty);
        entry.funding = !department ? 1.0f : entry.hasExplicitFunding ? item.localFunding : department->GetFundingPercentage();
        return entry;
    }
    float DepartmentFundingFraction() override {
        cSC4DepartmentBudget* department = Department();
        return department ? department->GetFundingPercentage() : 1.0f;
    }
    const sc4::cISCPropertyHolder* SimulatorExemplar() override { return NativeSimulatorExemplar(0xc911e35b); }
    void SetGenerationCost(std::int64_t cost) override {
        cSC4DepartmentBudget* department = Department();
        if (cSC4LineItem* item = department ? department->GetLineItem(1) : nullptr) item->SetFullExpenses(cost);
    }
    std::int64_t DepartmentExpense() override {
        cSC4DepartmentBudget* department = Department();
        cSC4LineItem* item = department ? department->GetLineItem(1) : nullptr;
        return item ? item->GetFullExpenses() : 0;   // PARTIAL: the generation line only (budget vslot 0x88 sums the department)
    }
    // @ i386 0x00183b7a (DoOccupantInserted's tail): the plant's budget item (0xca58e540) lives on a locally funded line item of its
    // department named by the building type (created with display flag 2 when missing); the plant is one of the department's locally
    // funded objects and the line counts it.
    void AddPlantToBudget(cISC4Occupant* plant) override {
        cSC4BudgetSimulator* budget = Budget();
        sc4::cISCPropertyHolder* holder = plant ? plant->AsPropertyHolder() : nullptr;
        BudgetItem item;
        if (!budget || !holder || !budget->GetBudgetItemForPurpose(holder, kPowerPlantBudgetProperty, item)) return;
        void* raw = nullptr;
        if (!plant->QueryInterface(kGZIID_cISC4BuildingOccupant, &raw) || !raw) return;
        auto* building = static_cast<cISC4BuildingOccupant*>(raw);
        cSC4DepartmentBudget* department = budget->GetDepartmentBudget(std::uint32_t(item.departmentId));
        if (item.lineItemId == 0) {
            item.lineItemId = std::int32_t(building->GetBuildingType());
            budget->ChangeBudgetItemLine(holder, int(item.purpose), std::uint32_t(item.lineItemId));
        }
        building->Release();
        if (item.lineItemId == 0 || !department) return;
        cSC4LineItem* line = department->GetLineItem(std::uint32_t(item.lineItemId));
        if (!line && (line = department->CreateLineItemForBuildingType(std::uint32_t(item.lineItemId), true))) line->SetDisplayFlag(2, true);
        department->AddLocallyFundedObject(holder, kPowerPlantBudgetProperty);
        if (!line) return;
        line->SetSecondaryInfoField(line->GetSecondaryInfoField() + 1);
        cSCPropertyHelp::SetPropertyValue(holder, kPlantExpenseProperty, item.cost);
    }
    // @ i386 0x00185bba: the department lets the plant go and its line counts one less.
    void RemovePlantFromBudget(cISC4Occupant* plant) override {
        cSC4BudgetSimulator* budget = Budget();
        sc4::cISCPropertyHolder* holder = plant ? plant->AsPropertyHolder() : nullptr;
        BudgetItem item;
        if (!budget || !holder || !budget->GetBudgetItemForPurpose(holder, kPowerPlantBudgetProperty, item)) return;
        cSC4DepartmentBudget* department = budget->GetDepartmentBudget(std::uint32_t(item.departmentId));
        cSC4LineItem* line = department ? department->GetLineItem(std::uint32_t(item.lineItemId)) : nullptr;
        if (!line) return;
        department->RemoveLocallyFundedObject(holder, kPowerPlantBudgetProperty);
        line->SetSecondaryInfoField(line->GetSecondaryInfoField() - 1);
    }
    // The plant's monthly expense (0x2a58d350 by its wear curve) into its budget item when it changed.
    void SetPlantExpense(cISC4Occupant* plant, std::int64_t expense) override {
        cSC4BudgetSimulator* budget = Budget();
        sc4::cISCPropertyHolder* holder = plant ? plant->AsPropertyHolder() : nullptr;
        BudgetItem item;
        if (!budget || !holder || !budget->GetBudgetItemForPurpose(holder, kPowerPlantBudgetProperty, item) || item.cost == expense) return;
        cSC4DepartmentBudget* department = budget->GetDepartmentBudget(std::uint32_t(item.departmentId));
        if (cSC4LineItem* line = department ? department->GetLineItem(std::uint32_t(item.lineItemId)) : nullptr) {
            line->AddToFullExpenses(expense - item.cost);
            budget->ChangeBudgetItemCost(holder, item.purpose, expense);
        }
    }
    void SetOccupantLotPowered(cISC4Occupant* building) override { if (cSC4Lot* lot = LotUnder(building)) lot->SetHasPower(true); }
    void SetLotPoweredAt(int x, int z) override { if (cSC4Lot* lot = LotAtCell(x, z)) lot->SetHasPower(true); }
    bool LotCentreIfStateOne(cIGZUnknown* object, int& x, int& z) override {
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kGZIID_PlainObject, &raw) || !raw) return false;
        const auto* lot = static_cast<const cSC4Lot*>(raw);
        if (lot->GetState() != 1) return false;
        std::int32_t rect[4] = {};
        lot->GetBoundingRect(rect);
        x = (rect[0] + rect[2]) / 2;
        z = (rect[1] + rect[3]) / 2;
        return true;
    }
    void DistressPlantLot(cISC4Occupant* plant) override { if (cSC4Lot* lot = LotUnder(plant)) lot->SetCondition(2); }
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
        if (city) neighbors->GetConnectionsOfTypeToCity(kPowerConnectionType, static_cast<const cISC4RegionalCity*>(city), connections);
        else neighbors->GetConnectionsOfType(kPowerConnectionType, connections);
        for (auto& connection : connections) if (connection) out.push_back(connection.get());
    }
    cISC4NeighborConnection* UnvisitedConnection(void* city) override {
        cSC4NeighborsSimulator* neighbors = Neighbors();
        return neighbors ? neighbors->FindUnvisitedConnection(kPowerConnectionType, static_cast<const cISC4RegionalCity*>(city)) : nullptr;
    }
    bool OtherSideActive(cISC4NeighborConnection* connection) override {
        cSC4NeighborsSimulator* neighbors = Neighbors();
        cISC4NeighborConnection* other = neighbors ? neighbors->FindConnectionOnOtherSide(connection) : nullptr;
        return other && other->GetIsActive();
    }
    void RefreshNeighborDeals() override { if (cSC4NeighborsSimulator* neighbors = Neighbors()) neighbors->MarkDealsDirty(); }
    void PostMessage(std::uint32_t type, std::uint32_t data, const PowerRect* rect) override {
        if (!SL::spMS2) return;
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(type);
        if (rect) {   // 0x8a07ae00: the block
            message->SetData1(std::uint64_t(rect->left));
            message->SetData2(std::uint64_t(rect->top));
            message->SetData3(std::uint64_t(rect->right));
            message->SetData4(std::uint64_t(rect->bottom));
        } else {
            message->SetData1(data);
        }
        SL::spMS2->MessagePost(message, false);
        message->Release();
    }
    void AddHistory(std::uint32_t id, std::int64_t value) override {
        if (cSC4HistoryWarehouse* history = History()) history->RecordHistoryData(id, static_cast<int>(value));
    }
    void CreateHistory(std::uint32_t id, std::uint32_t length) override {
        if (cSC4HistoryWarehouse* history = History()) history->SetHistoryDataQueueLength(id, length);
    }
    void AddAgent(cIGZMessageTarget2* target, int interval, const char* name, std::uint32_t flags) override { AddCitySimulatorAgent(target, interval, name, flags); }
    void RemoveAgent(cIGZMessageTarget2* target) override { RemoveCitySimulatorAgent(target); }
    void AddNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->AddNotification(target, message); }
    void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->RemoveNotification(target, message); }
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
    void WriteSupplyGrid(cIGZOStream& stream) override {
        SC4WriteGZSerializable(stream, supply_ ? static_cast<const cIGZUnknown*>(static_cast<cIGZSerializable*>(supply_)) : nullptr);
    }
    bool ReadSupplyGrid(cIGZIStream& stream) override {
        if (supply_) { supply_->Release(); supply_ = nullptr; }
        void* raw = nullptr;
        const bool ok = SC4ReadGZSerializable(stream, kGZIID_cIGZSerializableSimGrid, &raw, false);   // the serializable view
        auto* serializable = static_cast<cIGZSerializable*>(raw);
        supply_ = serializable ? dynamic_cast<cSC4SimGrid<std::uint8_t>*>(serializable) : nullptr;
        if (serializable && !supply_) serializable->Release();
        return ok;
    }
    void WriteOccupant(cIGZOStream& stream, cISC4Occupant* occupant) override { SC4WriteGZSerializable(stream, occupant); }
    bool ReadOccupant(cIGZIStream& stream, std::uint32_t iid, cISC4Occupant** out) override {
        *out = nullptr;
        void* raw = nullptr;
        const bool ok = SC4ReadGZSerializable(stream, iid, &raw, false);
        if (raw && iid != kGZIID_cISC4Occupant) {   // a pole reference: the occupant view of it
            auto* unknown = static_cast<cIGZUnknown*>(raw);
            void* occupant = nullptr;
            unknown->QueryInterface(kGZIID_cISC4Occupant, &occupant);
            unknown->Release();
            raw = occupant;
        }
        *out = static_cast<cISC4Occupant*>(raw);
        return ok;
    }
    // A city without a saved department (a new one) gets it here, as Init creates it (named, its generation line item, 120% cap).
    cSC4DepartmentBudget* Department() {
        cSC4BudgetSimulator* budget = Budget();
        if (!budget) return nullptr;
        cSC4DepartmentBudget* department = budget->GetDepartmentBudget(kPowerDepartment);
        if (!department && (department = budget->CreateDepartmentBudget(kPowerDepartment, kPowerDepartmentGroup))) {
            department->SetDepartmentName(0x6a3ff01c, 0x4a493980);
            department->SetMaxAllowedFundingPercentage(1.2f);
            if (cSC4LineItem* item = department->CreateLineItem(1, false)) item->SetName(0x6a3ff01c, 0x2a75f757);
        }
        return department;
    }
    void CityClosed() {
        Drop(held_);
        if (supply_) { supply_->Release(); supply_ = nullptr; }
    }
    // A new city: its supply grid (0x49b9e602, cells, 0x49d5bc86), when the file brought none.
    void EnsureSupplyGrid() {
        if (supply_ || CityCellsX() <= 0) return;
        supply_ = new cSC4SimGrid<std::uint8_t>();
        supply_->AddRef();
        supply_->Init();
        supply_->SetTractSize(1);
        supply_->SetInstanceID(0x49b9e602);
        supply_->SetTractValues(0);
    }

    // The supply grid's value at a cell (GetPoweredGrid, grid vslot 0x38): 0 not wired, 1 wired, 2 powered.
    cSC4SimGrid<std::uint8_t>* Supply() const { return supply_; }
    std::uint8_t SupplyAt(int x, int z) const { return supply_ && supply_->TractIsInBounds(x, z) ? supply_->GetTractValue(x, z) : 0; }

private:
    static void Drop(OccupantList& list) {
        if (list.empty()) return;
        if (cISC4OccupantManager* occupants = CityOccupants()) occupants->ReleaseOccupantList(list);
        else for (cISC4Occupant* occupant : list) occupant->Release();
        list.clear();
    }
    OccupantList held_;
    cSC4SimGrid<std::uint8_t>* supply_ = nullptr;
};

PowerHost g_power;

}  // namespace

sc4::IPowerHost* NativePowerHost() { return &g_power; }
// cSC4PowerSimulator::GetPoweredGrid (i386 vslot 0x90): the supply grid the host keeps for the simulator.
const sc4::cSC4SimGrid<std::uint8_t>* NativePoweredGrid() { return g_power.Supply(); }

sc4::cSC4PowerSimulator* NativePowerSimulator() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::PowerSimulator) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4PowerSimulator, &raw) || !raw) return nullptr;
    auto* simulator = static_cast<cSC4PowerSimulator*>(static_cast<cISC4PowerSimulator*>(raw));
    simulator->Release();
    return simulator->IsInitialized() ? simulator : nullptr;
}

bool NativeCellPowered(int x, int z) {
    cSC4PowerSimulator* simulator = NativePowerSimulator();
    return simulator ? simulator->IsPowered(x, z) : true;   // no simulator: every cell counts as powered (as before the port)
}

std::uint8_t NativePowerSupplyAt(int x, int z) { return NativePowerSimulator() ? g_power.SupplyAt(x, z) : 2; }

void NativePowerCityClosed() { g_power.CityClosed(); }
void NativePowerCityOpened() { g_power.EnsureSupplyGrid(); }

}  // namespace native
