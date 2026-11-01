// The lot configuration manager and the lot configurations on the native application (replace the generated neutral stubs, under
// which no lot configuration was ever loaded: the tract developer found no lot to build and plopping had nothing to offer).
// The configurations come from the lot configuration exemplars (type 0x6534284a, group 0xa8fbd372) of the resource manager; the
// building, family and retaining-wall checks from the building development simulator's catalogue, the prop checks from the prop
// manager.
//
// Neutral (each named): the flora checks (the flora simulator is not wired natively: lot flora is skipped), the detail preference
// (the native preferences blob is all defaults: 0), saving configurations (the lot editor's path).
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <vector>

#include "app/NativeBuildingDevelopment.h"
#include "app/NativeCity.h"
#include "gz/cGZPersistResourceManager.h"
#include "gz/cIGZPersistResourceKeyList.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/CommonIIDs.h"
#include "sc4/DBSegment.h"
#include "sc4/Exemplar.h"
#include "sc4/LotConfiguration.h"
#include "sc4/LotConfigurationManager.h"
#include "sc4/PropManager.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/ServiceLocator.h"
#include "sc4/cSC4City.h"

namespace native {
namespace {
using namespace sc4;

class KeyFilter final : public cIGZPersistResourceKeyFilter {
public:
    KeyFilter(std::uint32_t type, std::uint32_t group) : type_(type), group_(group) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool IsKeyIncluded(const cGZPersistResourceKey& key) override { return key.type == type_ && key.group == group_; }
private:
    std::uint32_t type_, group_;
};

// Exemplars from the resource manager, one reference held per acquired holder.
class Exemplars {
public:
    cISCPropertyHolder* Acquire(const cGZPersistResourceKey& key) {
        if (!SL::spRM) return nullptr;
        void* raw = nullptr;
        if (!SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) || !raw) return nullptr;
        auto* exemplar = static_cast<cISCResExemplar*>(raw);
        cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
        held_[holder] = exemplar;
        return holder;
    }
    void Release(cISCPropertyHolder* holder) {
        auto it = held_.find(holder);
        if (it == held_.end()) return;
        it->second->Release();
        held_.erase(it);
    }
private:
    std::map<cISCPropertyHolder*, cISCResExemplar*> held_;
} g_exemplars;

cSC4PropManager* Props() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(sc4::CityComponent::PropManager) : nullptr;
    void* raw = nullptr;
    return object && object->QueryInterface(kGZIID_PlainObject, &raw) ? static_cast<cSC4PropManager*>(raw) : nullptr;
}

std::uint32_t NewId() {
    static std::mt19937 random(0x10c0);
    return random() | 1u;
}

class ManagerHost final : public ILotConfigurationManagerHost {
public:
    std::uint8_t PreferencesDetail() override { return 0; }
    void ForEachLotConfigurationKey(const std::function<void(const cGZPersistResourceKey&)>& fn) override {
        if (!SL::spRM) return;
        // The manager initialises before the building development simulator; its building checks need the catalogue loaded.
        NativeEnsureBuildingCatalogue();
        cIGZPersistResourceKeyList* list = nullptr;
        KeyFilter filter(0x6534284a, kLotConfigurationGroup);
        SL::spRM->GetAvailableResourceList(&list, &filter);
        if (!list) return;
        std::vector<cGZPersistResourceKey> keys;
        for (std::uint32_t i = 0; i < list->Size(); ++i) keys.push_back(*list->GetKey(i));
        list->Release();
        for (const cGZPersistResourceKey& key : keys) fn(key);
        if (std::getenv("SC4_SIMDEBUG")) std::fprintf(stderr, "lot configurations: %zu exemplar keys\n", keys.size());
    }
    bool IsBuildingType(std::uint32_t id) override { return NativeIsBuildingType(id); }
    std::size_t loaded = 0;
    bool BuildingTypeExists(std::uint32_t id) override { return NativeIsBuildingType(id) || NativeIsBuildingFamily(id); }
    bool BuildingFootprint(std::uint32_t id, SC4Footprint& out) override {   // the profile's size (+0x30 x, +0x38 z)
        const BuildingProfile* profile = NativeBuildingProfile(id);
        if (!profile) return false;
        out.x = profile->size[0];
        out.z = profile->size[2];
        return true;
    }
    bool BuildingsInFamily(std::uint32_t family, std::vector<std::uint32_t>& out) override {
        NativeBuildingsInFamily(family, out);
        return !out.empty();
    }
    bool BuildingMatchesFamily(std::uint32_t type, std::uint32_t family) override { return NativeIsBuildingInFamily(type, family); }
    bool RetainingWallExists(std::uint32_t id) override { return NativeRetainingWallExists(id); }
    // the building exemplar's occupant groups (0xaa1dd396), its flag 0x2781283f and the profile's capacity
    bool BuildingInformation(std::uint32_t type, std::vector<std::uint32_t>& groups, bool& notRegular, std::uint16_t& capacity) override {
        groups.clear();
        notRegular = false;
        capacity = 0;
        cGZPersistResourceKey key;
        if (!NativeBuildingKeyFromType(type, key)) return false;
        if (cISCPropertyHolder* exemplar = g_exemplars.Acquire(key)) {
            cSCPropertyHelp::GetPropertyValue(exemplar, 0xaa1dd396, groups);
            cSCPropertyHelp::GetPropertyValue(exemplar, 0x2781283f, notRegular);
            g_exemplars.Release(exemplar);
        }
        if (const BuildingProfile* profile = NativeBuildingProfile(type)) capacity = profile->capacity;
        return true;
    }
    bool IsPropType(std::uint32_t id) override { cSC4PropManager* p = Props(); return p && p->IsPropTypeValid(id); }
    bool PropTypeExists(std::uint32_t id) override { cSC4PropManager* p = Props(); return p && (p->IsPropTypeValid(id) || p->IsPropFamilyTypeValid(id)); }
    bool PropFootprint(std::uint32_t id, SC4Footprint& out) override {
        cSC4PropManager* p = Props();
        if (!p || !p->IsPropTypeValid(id)) return false;
        const PropProfile& profile = p->GetPropProfile(id);
        out.x = profile.size[0];
        out.z = profile.size[2];
        return true;
    }
    bool PropsInFamily(std::uint32_t family, std::vector<std::uint32_t>& out) override {
        out.clear();
        cSC4PropManager* p = Props();
        return p && p->GetPropTypesByFamily(out, family) > 0;
    }
    bool IsFloraType(std::uint32_t) override { return false; }
    bool FloraTypeExists(std::uint32_t) override { return false; }
    bool FloraFootprint(std::uint32_t, SC4Footprint&) override { return false; }
    bool FloraInFamily(std::uint32_t, std::unordered_set<std::uint32_t>&) override { return false; }
    std::uint32_t NewGuid() override { return NewId(); }
    // the manager's record names the city's building development simulator, flora simulator and prop manager (by the
    // interfaces the city keeps them as); the references are written as the city's components so their ids match
    void WriteSerializable(cIGZOStream& stream, std::uint32_t iid) override {
        cSC4City* city = HostCity();
        const sc4::CityComponent component = iid == 0x07813eae ? sc4::CityComponent::BuildingDevelopmentSimulator
                                           : iid == 0x06c66177 ? sc4::CityComponent::FloraSimulator
                                                               : sc4::CityComponent::PropManager;
        SC4WriteGZSerializable(stream, city ? city->GetComponent(component) : nullptr);
    }
    bool ReadSerializable(cIGZIStream& stream, std::uint32_t clsid) override {
        void* out = nullptr;
        const bool ok = SC4ReadGZSerializable(stream, clsid, &out, false);
        if (out) static_cast<cIGZUnknown*>(out)->Release();
        return ok;
    }
} g_manager;

class ConfigurationHost final : public ILotConfigurationHost {
public:
    cISCPropertyHolder* AcquireExemplar(const cGZPersistResourceKey& key) override { return g_exemplars.Acquire(key); }
    void ReleaseExemplar(cISCPropertyHolder* holder) override { g_exemplars.Release(holder); }
    cISCPropertyHolder* CreateExemplar(const cGZPersistResourceKey&) override { return nullptr; }   // the lot editor's save path
    std::uint32_t NextObjectId() override { return NewId(); }
    bool StoreExemplar(cISCPropertyHolder*, const cGZPersistResourceKey&, void*) override { return false; }
    void* SegmentFor(const cGZPersistResourceKey&) override { return nullptr; }   // version-1 exemplars are not upgraded in place
} g_configuration;

// cSC4LotConfigurationManager::Init subscribes the manager to the city-init message 0x26d31ec1 (0x100403b5a); its DoMessage then
// gives every configuration its building styles, capacities and flag (InitBuildingInformation), which the tract developer's
// suitability and style tests read. The port's manager is a plain class, so this target subscribes on its behalf.
class CityInitTarget final : public cIGZMessageTarget2 {
public:
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZMessageTarget2 && iid != 1) return false;
        *out = static_cast<cIGZMessageTarget2*>(this);
        return true;
    }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool DoMessage(cIGZMessage2* message) override {
        if (!message || message->GetType() != sc4::kMessageLotConfigurationsInit) return true;
        cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(sc4::CityComponent::LotConfigurationManager) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kGZIID_PlainObject, &raw) || !raw) return true;
        NativeEnsureBuildingCatalogue();
        static_cast<cSC4LotConfigurationManager*>(raw)->DoMessage(message->GetType());
        return true;
    }
} g_cityInit;

}  // namespace

void InstallLotConfigurationHosts() {
    sc4::SetLotConfigurationManagerHost(&g_manager);
    sc4::SetLotConfigurationHost(&g_configuration);
    static bool subscribed = false;
    if (!subscribed && SL::spMS2) subscribed = SL::spMS2->AddNotification(&g_cityInit, sc4::kMessageLotConfigurationsInit);
}

}  // namespace native
