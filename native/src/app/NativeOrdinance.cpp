// The ordinance simulator's services on the native application (sc4::IOrdinanceHost). Only the generated stub was installed:
// every ordinance's cost read the residential base as 0 (so a per-capita ordinance such as the smoke detector program cost
// nothing), the current year as 0, and exemplars and strings came back empty.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "app/NativeCity.h"
#include "app/NativeUi.h"
#include "gz/cGZPersistResourceManager.h"
#include "gz/cIGZMessage2.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZString.h"
#include "sc4/Exemplar.h"
#include "sc4/Ordinance.h"
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

// The simulator's begin (0x66956810) and monthly (0x66956816) messages for the ordinance simulator (cSC4OrdinanceSimulator::
// DoMessage): nothing delivered them natively, so its monthly income and expense totals (which the budget reads) never moved.
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

class OrdinanceHost final : public IOrdinanceHost {
public:
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

}  // namespace

void InstallOrdinanceHost() {
    static OrdinanceHost host;
    sc4::SetOrdinanceHost(&host);
    static OrdinanceTarget target;
    static bool subscribed = false;
    if (!subscribed && SL::spMS2) {
        SL::spMS2->AddNotification(&target, 0x66956810);
        SL::spMS2->AddNotification(&target, 0x66956816);
        subscribed = true;
    }
}

}  // namespace native
