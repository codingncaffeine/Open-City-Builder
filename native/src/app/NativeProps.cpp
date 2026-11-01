// The prop manager and the props on the native application. Replaces the generated neutral IPropManagerHost / IPropHost, under
// which the prop catalogue was empty (no prop type was valid) and a new prop had no occupant manager to go into: the prop
// exemplars come from the resource manager, props go into the city's occupant manager and read the city's clock and date.
//
// PARTIAL (named): the registered prop classes and terrain-decal props (a plain cSC4PropOccupant stands in, as the manager does
// when a class is not registered), the prop-data-updated message, the city cell conversions the street props use
// (the city's own) and text decals.
#include <map>

#include "app/NativeCity.h"
#include "app/NativeProps.h"
#include "gz/cGZPersistResourceManager.h"
#include "gz/cIGZPersistResourceKeyList.h"
#include "rz/cRZRandom.h"
#include "sc4/CommonIIDs.h"
#include "sc4/Exemplar.h"
#include "sc4/OccupantManager.h"
#include "sc4/PropManager.h"
#include "sc4/PropOccupant.h"
#include "sc4/PropOccupantVisualEffect.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/cSC424HourClock.h"
#include "sc4/cSC4City.h"

namespace native {
namespace {
using namespace sc4;

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

cIGZUnknown* Component(CityComponent c) {
    cSC4City* city = HostCity();
    return city ? city->GetComponent(c) : nullptr;
}
template <class T> T* Plain(CityComponent c) {
    cIGZUnknown* object = Component(c);
    void* raw = nullptr;
    return object && object->QueryInterface(kGZIID_PlainObject, &raw) ? static_cast<T*>(raw) : nullptr;
}

class PropManagerHost final : public IPropManagerHost {
public:
    void ForEachExemplarKey(const std::function<void(const cGZPersistResourceKey&)>& fn) override {
        if (!SL::spRM) return;
        cIGZPersistResourceKeyList* list = nullptr;
        TypeFilter filter(0x6534284a);
        SL::spRM->GetAvailableResourceList(&list, &filter);
        if (!list) return;
        std::vector<cGZPersistResourceKey> keys;
        for (std::uint32_t i = 0; i < list->Size(); ++i) keys.push_back(*list->GetKey(i));
        list->Release();
        for (const cGZPersistResourceKey& key : keys) fn(key);
    }
    cISCPropertyHolder* AcquireExemplar(const cGZPersistResourceKey& key) override {
        if (!SL::spRM) return nullptr;
        void* raw = nullptr;
        if (!SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) || !raw) return nullptr;
        auto* exemplar = static_cast<cISCResExemplar*>(raw);
        cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
        held_[holder] = exemplar;
        return holder;
    }
    void ReleaseExemplar(cISCPropertyHolder* holder) override {
        auto it = held_.find(holder);
        if (it == held_.end()) return;
        it->second->Release();
        held_.erase(it);
    }
    cSC4PropOccupant* CreateRegisteredProp(std::uint32_t) override { return nullptr; }   // PARTIAL: no registered prop classes
    cSC4PropOccupant* CreateTerrainDecalProp() override { return nullptr; }               // PARTIAL
    cSC4PropOccupant* CreateVisualEffectProp() override {                                  // prop class 0x2977aa49 (one reference)
        auto* prop = new cSC4PropOccupantVisualEffect();
        prop->AddRef();
        return prop->AsPropOccupant();
    }
    void PostPropExemplarDataUpdated() override {}                                        // PARTIAL: message 0x896fdc70
    void PositionToCityCell(float x, float z, int& cx, int& cz) override {
        if (cSC4City* city = HostCity()) city->PositionToCell(x, z, cx, cz);
        else { cx = static_cast<int>(x / 16.0f); cz = static_cast<int>(z / 16.0f); }
    }
    void CityCellToPosition(int cx, int cz, float& x, float& z) override {
        if (cSC4City* city = HostCity()) city->CellCornerToPosition(cx, cz, x, z);
        else { x = static_cast<float>(cx) * 16.0f; z = static_cast<float>(cz) * 16.0f; }
    }
    cSC4Lot* LotAtCell(int, int) override { return nullptr; }   // PARTIAL: lot manager vslot 0x28 (street props only)
    void CreateTextDecal(const std::vector<std::uint32_t>&) override {}   // PARTIAL: message 0xbeaf1a2
private:
    std::map<cISCPropertyHolder*, cISCResExemplar*> held_;
} g_managerHost;

class PropHost final : public IPropHost {
public:
    int ClockHour() override { cSC424HourClock* c = Plain<cSC424HourClock>(CityComponent::Clock24Hour); return c ? static_cast<int>(c->GetHour()) : 12; }
    int ClockMinute() override { cSC424HourClock* c = Plain<cSC424HourClock>(CityComponent::Clock24Hour); return c ? static_cast<int>(c->GetMinute()) : 0; }
    void SimulatorDate(cGZDate& out) override {
        cIGZUnknown* object = Component(CityComponent::Simulator);
        void* raw = nullptr;
        if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return;
        auto* serializable = static_cast<cIGZSerializable*>(raw);
        if (serializable->GetGZCLSID() == kGZCLSID_cSC4Simulator) out = static_cast<cSC4Simulator*>(serializable)->GetSimDate();
        serializable->Release();
    }
    std::uint32_t RandomPercent() override { static cRZRandom random(0x9a0u); return random.RandomUint32Uniform(100); }
    cISC4OccupantManager* OccupantManager() override {
        cIGZUnknown* object = Component(CityComponent::OccupantManager);
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
        auto* manager = static_cast<cISC4OccupantManager*>(raw);
        manager->Release();   // the city keeps the manager
        return manager;
    }
} g_propHost;

}  // namespace

void InstallPropHosts() {
    sc4::SetPropManagerHost(&g_managerHost);
    sc4::SetPropHost(&g_propHost);
}

}  // namespace native
