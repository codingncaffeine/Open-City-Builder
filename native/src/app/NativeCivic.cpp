// The civic building simulator's services on the native application (sc4/CivicBuildingSimulator.h ICivicHost): the budget
// simulator its departments live in, the game data registry its Lua landmark count is published through, the simulator's
// agents, the building development simulator's building keys, the lot a state-change message or an occupant belongs to, and the
// automata controller manager that gives a landmark its occupant group's controllers.
#include <cstdint>

#include "app/NativeCity.h"
#include "app/NativeGameData.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/AutomataControllerManager.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/BuildingDevelopmentSimulator.h"
#include "sc4/CivicBuildingSimulator.h"
#include "sc4/Lot.h"
#include "sc4/Occupant.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/CommonIIDs.h"
#include "sc4/cSC4City.h"

namespace native {
bool AddCitySimulatorAgent(cIGZMessageTarget2* target, int interval, const char* name, std::uint32_t flags);
void RemoveCitySimulatorAgent(cIGZMessageTarget2* target);

namespace {
using namespace sc4;

template <class T> T* CityAs(CityComponent component) {
    cSC4City* city = HostCity();
    return city ? dynamic_cast<T*>(city->GetComponent(component)) : nullptr;
}

// A lot as the civic simulator reads it: its building (lot vslot 0x180) and its condition (vslots 0x118 / 0x120). The simulator
// uses the lot at once and keeps nothing, so each question rebinds one adapter.
class CivicLot final : public ICivicLot {
public:
    ICivicLot* Bind(cSC4Lot* lot) {
        lot_ = lot;
        return lot ? this : nullptr;
    }
    cISC4BuildingOccupant* GetBuilding() override { return static_cast<cISC4BuildingOccupant*>(lot_->GetBuilding()); }
    std::uint32_t GetCondition() const override { return lot_->GetCondition(); }
    void SetCondition(std::uint32_t condition) override { lot_->SetCondition(condition); }

private:
    cSC4Lot* lot_ = nullptr;
};

class CivicHost final : public ICivicHost {
public:
    cSC4BudgetSimulator* BudgetSimulator() override {
        return CityAs<cSC4BudgetSimulator>(CityComponent::BudgetSimulator);
    }
    cISC4GameDataRegistry* GameDataRegistry() override { return NativeGameDataRegistry(); }
    bool AddSimulatorAgent(cIGZMessageTarget2* target, int interval, const char* name, std::uint32_t flags) override {
        return AddCitySimulatorAgent(target, interval, name, flags);
    }
    void RemoveSimulatorAgent(cIGZMessageTarget2* target) override { RemoveCitySimulatorAgent(target); }
    bool GetBuildingKeyFromType(std::uint32_t buildingType, cGZPersistResourceKey& key) override {
        auto* development = CityAs<cSC4BuildingDevelopmentSimulator>(CityComponent::BuildingDevelopmentSimulator);
        return development && development->GetBuildingKeyFromType(buildingType, key);
    }
    // The lot state change (0xe8bda2e7) carries the lot as its object: a plain view of the lot (NativeZoneDeveloperHost.cpp).
    ICivicLot* MessageLot(cIGZMessage2* message) override {
        void* raw = nullptr;
        if (!message || !message->QueryInterface(kGZIID_cIGZMessage2Standard, &raw) || !raw) return nullptr;
        auto* standard = static_cast<cIGZMessage2Standard*>(raw);
        cIGZUnknown* object = standard->GetIGZUnknown();
        standard->Release();
        void* plain = nullptr;
        if (!object || !object->QueryInterface(kGZIID_PlainObject, &plain) || !plain) return nullptr;
        return messageLot_.Bind(static_cast<cSC4Lot*>(plain));
    }
    ICivicLot* LotForOccupant(cISC4Occupant* occupant) override {
        cSC4ZoneDeveloper* lots = CityAs<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper);   // the lot manager (vslot 0x70)
        return occupantLot_.Bind(lots && occupant ? lots->GetOccupantLot(occupant) : nullptr);
    }
    bool AddControllersFromOccupantGroup(const cIGZString& group, cISC4Occupant* occupant) override {
        auto* controllers = CityAs<cSC4AutomataControllerManager>(CityComponent::AutomataControllerManager);
        return controllers && controllers->AddControllersFromOccupantGroup(group, occupant, nullptr);
    }

private:
    CivicLot messageLot_, occupantLot_;
};

}  // namespace

void InstallCivicBuildingHost() {
    static CivicHost host;
    sc4::SetCivicBuildingHost(&host);
}

}  // namespace native
