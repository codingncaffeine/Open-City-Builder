#include "sc4/PropManager.h"
#include "sc4/DBSegment.h"

#include <algorithm>
#include <cfloat>

#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {
IPropManagerHost* g_host = nullptr;
std::unordered_map<std::uint32_t, cGZPersistResourceKey> g_keys;                 // mPropExemplarKeyMap
std::unordered_map<std::uint32_t, PropProfile> g_profiles;                       // mPropProfileMap
std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> g_families;        // mPropFamilyMap
std::unordered_map<std::uint32_t, cRZString> g_names;                            // mPropNameMap
bool g_staticInitialized = false;

// The usage filter object of the original (+0x14 holds the usage a query accepts).
class cSC4PropUsageFilter final : public cISC4OccupantFilter {
public:
    explicit cSC4PropUsageFilter(std::uint32_t usage) : usage_(usage) {}
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1) return false;
        *out = this;
        return true;
    }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool IsOccupantIncluded(cISC4Occupant* occupant) override {
        auto* prop = dynamic_cast<cSC4PropOccupant*>(occupant);
        return prop && prop->GetPropUsage() == usage_;
    }
    bool IsOccupantTypeIncluded(std::uint32_t type) override { return type == kOccupantTypeProp; }
    bool IsPropertyHolderIncluded(cISCPropertyHolder*) override { return true; }

private:
    std::uint32_t usage_;
};

cISC4OccupantManager* Manager() {
    IPropHost* host = PropHost();
    return host ? host->OccupantManager() : nullptr;
}

bool CityCellsQuery(cISC4OccupantManager* manager, OccupantList& out, const int a[2], const int b[2],
                    std::uint32_t usage) {
    cSC4PropUsageFilter filter(usage);
    return manager->GetOccupantsByStandardCityCells(out, a, b, &filter, 0xffffffffu);
}

void ReleaseAndErase(OccupantList& list, OccupantList::iterator& it) {
    (*it)->Release();
    it = list.erase(it);
}
}  // namespace

IPropManagerHost* PropManagerHost() { return g_host; }
void SetPropManagerHost(IPropManagerHost* host) { g_host = host; }

// ------------------------------------------------------------------------------------ lifecycle

std::uint32_t cSC4PropManager::Release() {
    if (refCount_ < 2) {
        Shutdown();
        delete this;
        return 0;
    }
    return --refCount_;
}

bool cSC4PropManager::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != 1 && iid != kGZIID_cISC4PropManager && iid != 0xe4fda3d4 && iid != 0x452294aa) return false;
    *out = this;
    AddRef();
    return true;
}

bool cSC4PropManager::StaticInit(cSC4PropManager* manager) {
    if (!g_staticInitialized) {
        g_staticInitialized = true;
        if (g_host) {
            g_host->ForEachExemplarKey([manager](const cGZPersistResourceKey& key) { AddPropExemplarDataStatic(key, manager); });
            PostPropExemplarDataUpdatedMessage();
        }
    }
    return true;
}

void cSC4PropManager::AddPropExemplarDataStatic(const cGZPersistResourceKey& key, void* manager) {
    if (!g_host) return;
    cISCPropertyHolder* holder = g_host->AcquireExemplar(key);
    if (!holder) return;
    std::uint32_t exemplarType = 0;
    if (holder->GetProperty(0x10, exemplarType) && exemplarType == kPropExemplarType)
        static_cast<cSC4PropManager*>(manager)->AddPropExemplarData(key, holder);
    g_host->ReleaseExemplar(holder);
}

void cSC4PropManager::PostPropExemplarDataUpdatedMessage() {
    if (g_host) g_host->PostPropExemplarDataUpdated();
}

void cSC4PropManager::LoadAllStandardPropExemplarData() {
    if (g_host) g_host->ForEachExemplarKey([this](const cGZPersistResourceKey& key) { AddPropExemplarDataStatic(key, this); });
    PostPropExemplarDataUpdatedMessage();
}

void cSC4PropManager::DoPostViewFilterChange(PropViewFilterInformation*, PropViewFilterInformation* newFilter) {
    IPropViewFilter* incoming = newFilter->filter;
    if (incoming == viewFilter_) return;
    if (incoming) incoming->AddRef();
    if (viewFilter_) viewFilter_->Release();
    viewFilter_ = incoming;
}

bool cSC4PropManager::StaticShutdown() {
    if (g_staticInitialized) {
        g_staticInitialized = false;
        g_keys.clear();
    }
    return true;
}

bool cSC4PropManager::Init() {
    if (!initialized_) {
        initialized_ = true;
        StaticInit(this);
        // (the original also registers the TextDecal command and subscribes to messages 0xa6b79602,
        //  0x06cdb65b, 0x26d31ec1 and 0x0beaf1a2 on the message server here)
    }
    return true;
}

bool cSC4PropManager::Shutdown() {
    if (initialized_) {
        initialized_ = false;
        usageFilter_ = 0;
    }
    return true;
}

bool cSC4PropManager::DoMessage(std::uint32_t) { return true; }

bool cSC4PropManager::Write(cIGZOStream& stream) const {
    stream.SetUint16(1);
    SC4WriteGZSerializable(stream, nullptr);   // the tool the manager keeps (not modelled)
    SC4WriteGZSerializable(stream, nullptr);   // the lot manager
    return !stream.GetError();
}

bool cSC4PropManager::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 1) return false;
    // the two objects the manager keeps (a tool and the lot manager) are references of the record: consumed here, the port keeps neither
    for (std::uint32_t iid : {0xc6bb8334u, 0x681905bdu}) {
        void* raw = nullptr;
        SC4ReadGZSerializable(stream, iid, &raw, false);
        if (raw) static_cast<cIGZUnknown*>(raw)->Release();
    }
    return !stream.GetError();
}

// ------------------------------------------------------------------------------------ exemplar data

bool cSC4PropManager::AddPropExemplarData(const cGZPersistResourceKey& key, cISCPropertyHolder* exemplar) {
    if (!exemplar) return false;
    g_keys[key.instance] = key;
    LoadPropProperties(exemplar, key.instance, true);
    return false;          // (sic) always returns 0
}

void cSC4PropManager::LoadPropProperties(cISCPropertyHolder* holder, std::uint32_t type, bool firstLoad) {
    PropProfile profile;
    profile.propClass = kPropClassDefault;
    {
        std::vector<float> values;
        if (cSCPropertyHelp::GetPropertyValue(holder, kPropPropertySize, values))
            for (std::size_t i = 0; i < values.size() && i < 3; ++i) profile.size[i] = values[i];
    }
    std::uint32_t value = 0;
    if (holder->GetProperty(kPropPropertyClass, value)) profile.propClass = value;
    value = 0;
    holder->GetProperty(kPropPropertyA, value);
    profile.valueA = value;
    value = 0;
    holder->GetProperty(kPropPropertyC, value);
    profile.valueC = value;
    {
        std::uint32_t flag = 0;
        holder->GetProperty(kPropPropertyFlag, flag);
        profile.flag = static_cast<std::uint8_t>(flag);
    }
    cRZString name;
    holder->GetProperty(0x20, name);
    if (!firstLoad) {
        g_profiles.erase(type);
        for (auto& kv : g_families) {
            auto& members = kv.second;
            auto it = std::find(members.begin(), members.end(), type);
            if (it != members.end()) members.erase(it);
        }
    }
    g_profiles[type] = profile;
    g_names[type] = name;
    std::vector<std::uint32_t> families;
    if (cSCPropertyHelp::GetPropertyValue(holder, kPropPropertyFamilies, families)) {
        for (std::uint32_t family : families) {
            auto& members = g_families[family];
            if (std::find(members.begin(), members.end(), type) == members.end()) members.push_back(type);
        }
    }
}

void cSC4PropManager::GetAllPropTypes(std::vector<std::uint32_t>& out) const {
    out.clear();
    for (const auto& kv : g_keys) out.push_back(kv.first);
}

void cSC4PropManager::GetAllPropFamilyTypes(std::vector<std::uint32_t>& out) const {
    out.clear();
    for (const auto& kv : g_families) out.push_back(kv.first);
}

bool cSC4PropManager::GetPropKeyFromType(std::uint32_t type, cGZPersistResourceKey& key) const {
    auto it = g_keys.find(type);
    if (it == g_keys.end()) return false;
    key = it->second;
    return true;
}

bool cSC4PropManager::IsPropTypeValid(std::uint32_t type) const { return g_keys.count(type) != 0; }

bool cSC4PropManager::IsPropFamilyTypeValid(std::uint32_t type) const {
    return g_families.count(type) != 0 || g_keys.count(type) != 0;
}

int cSC4PropManager::GetPropTypesByFamily(std::vector<std::uint32_t>& out, std::uint32_t family) const {
    auto it = g_families.find(family);
    if (it != g_families.end()) {
        if (&it->second != &out) out = it->second;
        return static_cast<int>(it->second.size());
    }
    if (g_keys.count(family) == 0) return 0;
    out.assign(1, family);          // a plain prop type is its own one-member family
    return 0;                       // (sic) the original returns 0 here although it fills one id
}

bool cSC4PropManager::IsPropInFamily(std::uint32_t type, std::uint32_t family) const {
    if (type == family) return true;
    auto it = g_families.find(family);
    if (it == g_families.end()) return false;
    return std::find(it->second.begin(), it->second.end(), type) != it->second.end();
}

cRZString& cSC4PropManager::GetPropName(std::uint32_t type) const { return g_names[type]; }
PropProfile& cSC4PropManager::GetPropProfile(std::uint32_t type) const { return g_profiles[type]; }

const PropProfile* cSC4PropManager::GetPropProfilePtr(std::uint32_t type) const {
    auto it = g_profiles.find(type);
    return it == g_profiles.end() ? nullptr : &it->second;
}

bool cSC4PropManager::LoadPropProperties(cSC4PropOccupant* prop, std::uint32_t type) {
    if (type == 0) type = prop->GetPropType();
    auto it = g_keys.find(type);
    if (it == g_keys.end() || !g_host) return true;
    cISCPropertyHolder* exemplar = g_host->AcquireExemplar(it->second);
    if (!exemplar) return true;
    g_host->SetDefaultExemplar(prop, exemplar);   // @ i386 0x0012eb1c: holder QI 0x0ac2b5f7, vslot 0xc
    // bounding box: origin to the exemplar's size (a default box when the property is missing)
    float lo[3] = {0, 0, 0}, hi[3] = {1, 1, 1};
    std::vector<float> size;
    if (cSCPropertyHelp::GetPropertyValue(exemplar, kPropPropertySize, size))
        for (int i = 0; i < 3 && i < static_cast<int>(size.size()); ++i) hi[i] = size[i];
    prop->SetBoundingBox(lo, hi);
    g_host->ReleaseExemplar(exemplar);
    return true;
}

// ------------------------------------------------------------------------------------------ creation

bool cSC4PropManager::CreateProp(std::uint32_t type, cSC4PropOccupant*& out) {
    if (!IsPropTypeValid(type)) return false;
    std::uint32_t propClass = GetPropProfile(type).propClass;
    if (propClass == 0) propClass = kPropClassDefault;
    out = g_host ? g_host->CreateRegisteredProp(propClass) : nullptr;
    if (!out) {
        if (propClass == kPropClassVisualEffect && g_host) out = g_host->CreateVisualEffectProp();
        else if (propClass == kPropClassTerrainDecal && g_host) out = g_host->CreateTerrainDecalProp();
        if (!out) out = new cSC4PropOccupant();
        out->AddRef();
    }
    out->SetPropType(type);
    LoadPropProperties(out, type);
    return true;
}

bool cSC4PropManager::CreateGenericProp(const cGZPersistResourceKey& key, cSC4PropOccupant*& out) {
    if (!g_host) return false;
    cISCPropertyHolder* exemplar = g_host->AcquireExemplar(key);
    if (!exemplar) return false;
    std::uint32_t propClass = kPropClassDefault;
    exemplar->GetProperty(kPropPropertyClass, propClass);
    out = g_host->CreateRegisteredProp(propClass);
    if (!out) {
        if (propClass == kPropClassVisualEffect) out = g_host->CreateVisualEffectProp();
        else if (propClass == kPropClassTerrainDecal) out = g_host->CreateTerrainDecalProp();
        if (!out) out = new cSC4PropOccupant();
        out->AddRef();
    }
    float lo[3] = {0, 0, 0}, hi[3] = {1, 1, 1};
    std::vector<float> size;
    if (cSCPropertyHelp::GetPropertyValue(exemplar, kPropPropertySize, size))
        for (int i = 0; i < 3 && i < static_cast<int>(size.size()); ++i) hi[i] = size[i];
    out->SetBoundingBox(lo, hi);
    g_host->ReleaseExemplar(exemplar);
    return true;
}

// -------------------------------------------------------------------------------------- usage lists

void cSC4PropManager::SetPropUsage(cISC4Occupant* occupant, std::uint32_t usage) {
    if (auto* prop = dynamic_cast<cSC4PropOccupant*>(occupant)) prop->SetPropUsage(static_cast<std::uint8_t>(usage));
}

bool cSC4PropManager::InitProp(cSC4PropOccupant* prop) {
    cISC4OccupantManager* manager = Manager();
    if (!manager) return false;
    if (prop->IsFlagSet(1)) return true;
    if (!prop->Init()) return false;
    return manager->InsertOccupant(prop, 0);
}

bool cSC4PropManager::GetCityProps(OccupantList& out, const int rect[4]) {
    cISC4OccupantManager* manager = Manager();
    if (!manager) return false;
    const int a[2] = {rect[0], rect[1]}, b[2] = {rect[2], rect[3]};
    return CityCellsQuery(manager, out, a, b, 1);
}

bool cSC4PropManager::GetCityProps(OccupantList& out, const float rect[4]) {
    cISC4OccupantManager* manager = Manager();
    if (!manager) return false;
    const float lo[3] = {rect[0], -FLT_MAX, rect[1]};
    const float hi[3] = {rect[2], FLT_MAX, rect[3]};
    cSC4PropUsageFilter filter(1);
    return manager->GetOccupantsByBBox(out, lo, hi, &filter, 0xffffffffu);
}

cSC4PropOccupant* cSC4PropManager::AddPropWithUsage(std::uint32_t type, const float* position, int orientation,
                                                    std::uint32_t usage) {
    cSC4PropOccupant* prop = nullptr;
    if (!CreateProp(type, prop)) return nullptr;
    prop->SetPosition(position);
    prop->SetOrientation(orientation);
    cISC4OccupantManager* manager = Manager();
    if (prop->Init() && manager && manager->InsertOccupant(prop, 0)) {
        prop->SetPropUsage(static_cast<std::uint8_t>(usage));
        return prop;
    }
    prop->Release();
    return nullptr;
}

bool cSC4PropManager::AddCityProp(std::uint32_t type, const float* position, int orientation) {
    cSC4PropOccupant* prop = AddPropWithUsage(type, position, orientation, 1);
    if (!prop) return false;
    prop->Release();
    return true;
}

bool cSC4PropManager::AddCityProp(cISC4Occupant* occupant) {
    if (!Manager()) return false;
    if (occupant->IsFlagSet(1)) return true;
    if (!occupant->Init()) return false;
    if (!Manager()->InsertOccupant(occupant, 0)) return false;
    SetPropUsage(occupant, 1);
    return true;
}

bool cSC4PropManager::AddStreetProp(std::uint32_t type, const float* position, int orientation) {
    cSC4PropOccupant* prop = AddPropWithUsage(type, position, orientation, 4);
    if (!prop) return false;
    prop->Release();
    return true;
}

cSC4PropOccupant* cSC4PropManager::AddLotProp(std::uint32_t type, const float* position, int orientation) {
    return AddPropWithUsage(type, position, orientation, 2);
}

bool cSC4PropManager::AddLotProp(cSC4Lot*, cISC4Occupant* occupant) {
    if (!Manager()) return false;
    if (occupant->IsFlagSet(1)) return true;
    if (!occupant->Init()) return false;
    if (!Manager()->InsertOccupant(occupant, 0)) return false;
    SetPropUsage(occupant, 2);
    return true;
}

void cSC4PropManager::RemoveOccupant(cISC4Occupant* occupant) {
    if (occupant->IsFlagSet(2)) {
        cISC43DPlaceableObject* placeable = occupant->GetPlaceableObject();
        if (placeable) {
            placeable->AddRef();
            placeable->SetVisibility(false);
        }
        occupant->ClearFlag(2);
        if (placeable) placeable->Release();
    }
    if (cISC4OccupantManager* manager = Manager())
        if (occupant->IsFlagSet(1)) manager->RemoveOccupant(occupant, false, 0);
    occupant->Shutdown();
}

void cSC4PropManager::RemoveAndReleaseOccupants(OccupantList& list) {
    for (cISC4Occupant* occupant : list) {
        RemoveOccupant(occupant);
        occupant->Release();
    }
    list.clear();
}

bool cSC4PropManager::RemoveCityProps(const int rect[4]) {
    OccupantList list;
    if (GetCityProps(list, rect)) RemoveAndReleaseOccupants(list);
    return true;
}

bool cSC4PropManager::RemoveCityProps(const float rect[4]) {
    OccupantList list;
    if (GetCityProps(list, rect)) RemoveAndReleaseOccupants(list);
    return true;
}

bool cSC4PropManager::RemovePropA(cISC4Occupant* occupant) {
    RemoveOccupant(occupant);
    return true;
}

bool cSC4PropManager::GetStreetProps(OccupantList& out, int x, int z) {
    cISC4OccupantManager* manager = Manager();
    if (!manager) return false;
    const int a[2] = {x, z}, b[2] = {x, z};
    if (CityCellsQuery(manager, out, a, b, 4) && g_host) {
        for (auto it = out.begin(); it != out.end();) {
            float position[3];
            (*it)->GetPosition(position);
            int cx = 0, cz = 0;
            g_host->PositionToCityCell(position[0], position[2], cx, cz);
            if (cx == x && cz == z) ++it;
            else ReleaseAndErase(out, it);
        }
    }
    return false;                      // (sic) the original always returns 0
}

bool cSC4PropManager::GetStreetProps(OccupantList& out, const int rect[4]) {
    cISC4OccupantManager* manager = Manager();
    if (!manager) return false;
    const int a[2] = {rect[0], rect[1]}, b[2] = {rect[2], rect[3]};
    return CityCellsQuery(manager, out, a, b, 4);
}

void cSC4PropManager::RemoveStreetPropsFromTile(int x, int z) {
    OccupantList list;
    GetStreetProps(list, x, z);
    RemoveAndReleaseOccupants(list);
}

bool cSC4PropManager::GetLotProps(OccupantList& out, cSC4Lot* lot) {
    cISC4OccupantManager* manager = Manager();
    if (!lot || !manager || !g_host) return false;
    std::int32_t rect[4];
    lot->GetBoundingRect(rect);
    const int a[2] = {rect[0], rect[1]}, b[2] = {rect[2], rect[3]};
    if (!CityCellsQuery(manager, out, a, b, 2)) return false;
    float minX, minZ, maxX, maxZ;
    g_host->CityCellToPosition(rect[0], rect[1], minX, minZ);
    g_host->CityCellToPosition(rect[2] + 1, rect[3] + 1, maxX, maxZ);
    for (auto it = out.begin(); it != out.end();) {
        float position[3];
        (*it)->GetPosition(position);
        if (position[2] > maxZ || position[0] < minX || position[2] < minZ || position[0] > maxX)
            ReleaseAndErase(out, it);
        else
            ++it;
    }
    return true;
}

bool cSC4PropManager::GetLotProps(OccupantList& out, int x, int z) {
    if (!g_host) return false;
    cSC4Lot* lot = g_host->LotAtCell(x, z);
    return lot && GetLotProps(out, lot);
}

bool cSC4PropManager::RemoveLotProps(cSC4Lot* lot) {
    OccupantList list;
    if (GetLotProps(list, lot)) RemoveAndReleaseOccupants(list);
    return true;
}

bool cSC4PropManager::RemoveLotProps(int x, int z) {
    OccupantList list;
    if (GetLotProps(list, x, z)) RemoveAndReleaseOccupants(list);
    return true;
}

// ---------------------------------------------------------------------------------- building props

bool cSC4PropManager::GetBuildingProps(OccupantList& out, cISC4Occupant* building) {
    cISCPropertyHolder* holder = building->AsPropertyHolder();
    cSC4BuildingPropList* props = nullptr;
    if (!holder || !holder->GetProperty(kPropertyBuildingProps, 1, reinterpret_cast<void**>(&props))) return false;
    for (cISC4Occupant* prop : props->props) {
        prop->AddRef();
        out.push_back(prop);
    }
    props->Release();
    return true;
}

bool cSC4PropManager::GetBuildingProps(OccupantList& out, int x, int z) {
    cISC4OccupantManager* manager = Manager();
    cISC4Occupant* building = nullptr;
    if (!manager || !manager->GetFirstOccupant(building, x, z, kOccupantTypeBuilding)) return false;
    const bool ok = GetBuildingProps(out, building);
    building->Release();
    return ok;
}

bool cSC4PropManager::AddBuildingProp(cISC4Occupant* building, cISC4Occupant* prop) {
    cISC4OccupantManager* manager = Manager();
    if (!manager) return false;
    if (!prop->IsFlagSet(1)) {
        if (!prop->Init()) return false;
        if (!manager->InsertOccupant(prop, 0)) return false;
        SetPropUsage(prop, 3);
    }
    cISCPropertyHolder* holder = building->AsPropertyHolder();
    if (!holder) return true;
    cSC4BuildingPropList* list = nullptr;
    if (!holder->GetProperty(kPropertyBuildingProps, 1, reinterpret_cast<void**>(&list))) {
        list = new cSC4BuildingPropList();
        list->props.push_back(prop);
        holder->AddProperty(kPropertyBuildingProps, static_cast<cIGZUnknown*>(list), false);
        list->Release();
    } else {
        if (std::find(list->props.begin(), list->props.end(), prop) == list->props.end()) list->props.push_back(prop);
        list->Release();
    }
    SetPropUsage(prop, 3);
    return true;
}

bool cSC4PropManager::RemoveBuildingProps(cISC4Occupant* building) {
    cISCPropertyHolder* holder = building->AsPropertyHolder();
    cSC4BuildingPropList* list = nullptr;
    if (!holder || !holder->GetProperty(kPropertyBuildingProps, 1, reinterpret_cast<void**>(&list))) return false;
    cISC4OccupantManager* manager = Manager();
    for (cISC4Occupant* prop : list->props) {
        if (manager && prop->IsFlagSet(1)) manager->RemoveOccupant(prop, false, 0);
        prop->Shutdown();
    }
    list->props.clear();
    list->Release();
    holder->RemoveProperty(kPropertyBuildingProps, false);
    return true;
}

bool cSC4PropManager::RemoveBuildingProps(int x, int z) {
    cISC4OccupantManager* manager = Manager();
    cISC4Occupant* building = nullptr;
    if (!manager || !manager->GetFirstOccupant(building, x, z, kOccupantTypeBuilding)) return false;
    const bool ok = RemoveBuildingProps(building);
    building->Release();
    return ok;
}

bool cSC4PropManager::RemoveBuildingProp(cISC4Occupant* building, cISC4Occupant* prop) {
    cISCPropertyHolder* holder = building->AsPropertyHolder();
    if (!holder) return false;
    cSC4BuildingPropList* list = nullptr;
    if (!holder->GetProperty(kPropertyBuildingProps, 1, reinterpret_cast<void**>(&list))) return false;
    bool removed = false;
    auto it = std::find(list->props.begin(), list->props.end(), prop);
    if (it != list->props.end()) {
        cISC4OccupantManager* manager = Manager();
        if (manager && prop->IsFlagSet(1)) manager->RemoveOccupant(prop, false, 0);
        prop->Shutdown();
        list->props.erase(it);
        removed = true;
    }
    const bool empty = list->props.empty();
    list->Release();
    if (empty) holder->RemoveProperty(kPropertyBuildingProps, false);
    return removed;
}

}  // namespace sc4
