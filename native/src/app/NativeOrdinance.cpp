// The ordinance simulator's services on the native application (sc4::IOrdinanceHost). Only the generated stub was installed:
// every ordinance's cost read the residential base as 0 (so a per-capita ordinance such as the smoke detector program cost
// nothing), the current year as 0, and exemplars and strings came back empty.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <functional>

#include "app/NativeCity.h"
#include "app/NativeUi.h"
#include "gz/cGZPersistResourceManager.h"
#include "gz/cGZPersistResourceKeyList.h"
#include "gz/cIGZMessage2.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZString.h"
#include "sc4/Exemplar.h"
#include "sc4/Budget.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/LandfillDeveloper.h"
#include "sc4/Ordinance.h"
#include "sc4/OrdinanceTypes.h"
#include "sc4/PoliceSimulator.h"
#include "sc4/PollutionSimulator.h"
#include "sc4/PowerSimulator.h"
#include "sc4/ResidentialSimulator.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/cSC4City.h"

namespace native {
namespace {
using namespace sc4;

template <class T> T* CityAs(CityComponent component) {
    cSC4City* city = HostCity();
    return city ? dynamic_cast<T*>(city->GetComponent(component)) : nullptr;
}

// The ordinance simulator as a simulator agent (ordinance.md 8.1: month level, flags 2): the simulator's begin (0x66956810) and
// month (0x66956816) messages go to cSC4OrdinanceSimulator::DoMessage (the port's class is not a message target itself).
class OrdinanceTarget final : public cIGZMessageTarget2 {
public:
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZMessageTarget2 && iid != 1) return false;
        *out = static_cast<cIGZMessageTarget2*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override { return refs_ > 1 ? --refs_ : 1; }   // static: lives as long as the process
    bool DoMessage(cIGZMessage2* message) override {
        if (!message) return true;
        cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(CityComponent::OrdinanceSimulator) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return true;
        auto* serializable = static_cast<cIGZSerializable*>(raw);
        if (serializable->GetGZCLSID() == kGZCLSID_cSC4OrdinanceSimulator)
            static_cast<cSC4OrdinanceSimulator*>(serializable)->DoMessage(message->GetType());
        serializable->Release();
        return true;
    }

private:
    std::uint32_t refs_ = 1;
};

// The exemplar keys of one resource type (the ordinance exemplars are type 0x6534284a; the simulator checks their property 0x10).
class TypeFilter final : public cIGZPersistResourceKeyFilter {
public:
    explicit TypeFilter(std::uint32_t type) : type_(type) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool IsKeyIncluded(const cGZPersistResourceKey& key) override { return key.type == type_; }

private:
    std::uint32_t type_;
};

OrdinanceTarget g_target;

class OrdinanceHost final : public IOrdinanceHost {
public:
    void ForEachOrdinanceKey(const std::function<void(const cGZPersistResourceKey&)>& callback) override {
        if (!SL::spRM) return;
        cIGZPersistResourceKeyList* list = nullptr;
        TypeFilter filter(0x6534284a);
        SL::spRM->GetAvailableResourceList(&list, &filter);
        if (!list) return;
        std::vector<cGZPersistResourceKey> keys;
        for (std::uint32_t i = 0; i < list->Size(); ++i) keys.push_back(*list->GetKey(i));
        list->Release();
        for (const cGZPersistResourceKey& key : keys) callback(key);
    }
    // GZCOM CreateInstance(classId, 0x8bd4fc74): the serializer's factory makes every ordinance class the city files hold.
    cSC4OrdinanceSimple* CreateOrdinance(std::uint32_t classId) override {
        cIGZSerializable* object = NativeCreateSerializable(classId);
        if (!object) return nullptr;
        void* raw = nullptr;
        cSC4OrdinanceSimple* ordinance = nullptr;
        if (object->QueryInterface(kGZIID_cISC4Ordinance, &raw) && raw) {
            ordinance = dynamic_cast<cSC4OrdinanceSimple*>(static_cast<cISC4Ordinance*>(raw));
            if (!ordinance) static_cast<cISC4Ordinance*>(raw)->Release();   // a class that is no standard ordinance
        }
        object->Release();
        return ordinance;   // the reference QueryInterface added
    }
    void AddSimulatorAgent(cSC4OrdinanceSimulator*) override { AddCitySimulatorAgent(&g_target, 6, "cSC4OrdinanceSimulator", 2); }
    void RemoveSimulatorAgent(cSC4OrdinanceSimulator*) override { RemoveCitySimulatorAgent(&g_target); }
    cISCResExemplar* LoadExemplar(const cGZPersistResourceKey& key) override {
        void* resource = nullptr;
        return SL::spRM && SL::spRM->GetResource(key, 0xa52160f5, &resource, 0, nullptr) && resource ? static_cast<cISCResExemplar*>(resource) : nullptr;
    }
    int CurrentYear() override {   // simulator vslot 0x38
        cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(CityComponent::Simulator) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return 0;
        auto* serializable = static_cast<cIGZSerializable*>(raw);
        auto* simulator = serializable->GetGZCLSID() == kGZCLSID_cSC4Simulator ? static_cast<cSC4Simulator*>(serializable) : nullptr;
        serializable->Release();
        int year = 0;
        if (simulator) simulator->GetSimDate(&year, nullptr, nullptr, nullptr, nullptr);
        return year;
    }
    // residential simulator vslot 0x80 (i386 +0x48: GetPopulation)
    int ResidentialIncomeBase() override {
        auto* residential = CityAs<cSC4ResidentialSimulator>(CityComponent::ResidentialSimulator);
        return residential ? residential->GetPopulation() : 0;
    }
    std::string LoadString(const cGZPersistResourceKey& key) override {
        cRZString text;
        return LoadLocalizedString(key.group, key.instance, text) ? std::string(text.ToChar()) : std::string();
    }
    void PostMessage(std::uint32_t type, std::uint32_t id, std::uint32_t flag) override {
        if (!SL::spMS2) return;
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(type);
        message->SetData1(id);
        message->SetData2(flag);
        SL::spMS2->MessagePost(message, false);
        message->Release();
    }
};

// What the 20 ordinance classes ask besides the standard ordinance (ordinance.md 3.6): budget departments and line items, the
// landfill, power plants of a type, police stations and the recycling effect.
class OrdinanceTypesHost final : public IOrdinanceSimHost {
public:
    bool BudgetLineItem(std::uint32_t dept, std::uint32_t item, std::int64_t& value) override {
        cSC4DepartmentBudget* department = Department(dept);
        cSC4LineItem* line = department ? department->GetLineItem(item) : nullptr;
        if (!line) return false;
        value = line->GetFullExpenses();
        return true;
    }
    bool BudgetDepartmentTotal(std::uint32_t dept, std::int64_t& value) override {
        cSC4DepartmentBudget* department = Department(dept);
        if (!department) return false;
        value = department->GetTotalExpenses();
        return true;
    }
    // cISC4LandfillDeveloper::GetLandfillUsage's two outputs in their order; the gas recovery ordinance reads the second (the
    // summed heap capacity of the connected landfill zones, landfill.md 4.6.5).
    bool LandfillCapacity(std::uint32_t& first, std::uint32_t& second) override {
        auto* landfill = CityAs<cSC4LandfillDeveloper>(CityComponent::LandfillDeveloper);
        if (!landfill) return false;
        landfill->GetLandfillUsage(first, second);
        return true;
    }
    int PowerPlantsOfType(int type) override {
        auto* power = CityAs<cSC4PowerSimulator>(CityComponent::PowerSimulator);
        return power ? static_cast<int>(power->GetPowerPlantCount(static_cast<std::uint32_t>(type))) : 0;
    }
    int PoliceStationCount() override {
        auto* police = CityAs<cSC4PoliceSimulator>(CityComponent::PoliceSimulator);
        return police ? static_cast<int>(police->GetPoliceStationCount()) : 0;
    }
    // pollution compiled slot 33: the recycling centers' effect as a fraction (pollution.md 4.6.3)
    float PollutionAtLevel() override {
        auto* pollution = CityAs<cSC4PollutionSimulator>(CityComponent::PollutionSimulator);
        return pollution ? pollution->GetRecyclingCenterEffect() : 0.0f;
    }

private:
    static cSC4DepartmentBudget* Department(std::uint32_t id) {
        auto* budget = CityAs<cSC4BudgetSimulator>(CityComponent::BudgetSimulator);
        return budget ? budget->GetDepartmentBudget(id) : nullptr;
    }
};

}  // namespace

void InstallOrdinanceHost() {
    static OrdinanceHost host;
    sc4::SetOrdinanceHost(&host);
    static OrdinanceTypesHost types;
    sc4::SetOrdinanceSimHost(&types);
}

}  // namespace native
