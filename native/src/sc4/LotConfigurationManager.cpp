#include "sc4/LotConfigurationManager.h"

#include <algorithm>
#include <set>

namespace sc4 {

namespace {
ILotConfigurationManagerHost* g_host = nullptr;
std::unordered_map<std::uint32_t, cSC4LotConfiguration*> g_configurations;      // mLotConfigurationMap
std::unordered_set<std::uint32_t> g_familyIds;                                  // mLotConfigurationFamilyIDSet
bool g_staticInitialized = false;
std::uint32_t g_staticInitDetail = 0;

void MaxFootprint(float out[4], float defaultSize) {
    out[0] = out[1] = defaultSize;
    out[2] = out[3] = 0.0f;
}
void Grow(float out[4], const SC4Footprint& f) {
    if (out[0] <= f.x && f.x != out[0]) out[0] = f.x;
    if (out[1] <= f.z && f.z != out[1]) out[1] = f.z;
}
}  // namespace

ILotConfigurationManagerHost* LotConfigurationManagerHost() { return g_host; }
void SetLotConfigurationManagerHost(ILotConfigurationManagerHost* host) { g_host = host; }

std::unordered_map<std::uint32_t, cSC4LotConfiguration*>& cSC4LotConfigurationManager::GetLotConfigurationMap() {
    return g_configurations;
}
std::unordered_set<std::uint32_t>& cSC4LotConfigurationManager::GetLotConfigurationFamilyIDSet() { return g_familyIds; }

cSC4LotConfigurationManager::~cSC4LotConfigurationManager() {
    Shutdown();
    for (auto& kv : filterCache_) delete kv.second;
}

std::uint32_t cSC4LotConfigurationManager::Release() {
    if (refCount_ < 2) {
        Shutdown();
        delete this;
        return 0;
    }
    return --refCount_;
}

bool cSC4LotConfigurationManager::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != 1 && iid != kGZIID_cISC4LotConfigurationManager && iid != 0xe4fda3d4) return false;
    *out = this;
    AddRef();
    return true;
}

// ---------------------------------------------------------------------------------- static registry

void cSC4LotConfigurationManager::LoadAllStandardLotConfigurations() {
    if (g_host)
        g_host->ForEachLotConfigurationKey([this](const cGZPersistResourceKey& key) {
            if (key.group == kLotConfigurationGroup) AddLotConfiguration(key);
        });
}

bool cSC4LotConfigurationManager::StaticInit(cSC4LotConfigurationManager* manager) {
    const std::uint32_t detail = g_host ? g_host->PreferencesDetail() : 0;
    if (!g_staticInitialized || g_staticInitDetail != detail) {
        if (g_staticInitialized && g_staticInitDetail != detail) StaticShutdown();
        g_staticInitialized = true;
        if (g_host)
            g_host->ForEachLotConfigurationKey([manager](const cGZPersistResourceKey& key) {
                if (key.group == kLotConfigurationGroup) manager->AddLotConfiguration(key);
            });
        g_staticInitDetail = detail;
    }
    return true;
}

bool cSC4LotConfigurationManager::StaticShutdown() {
    if (g_staticInitialized) {
        g_staticInitialized = false;
        if (!g_configurations.empty()) {
            for (auto& kv : g_configurations) kv.second->Release();
            g_configurations.clear();
        }
    }
    return true;
}

bool cSC4LotConfigurationManager::Init() {
    if (!initialized_) {
        initialized_ = true;
        StaticInit(this);
        haveBuildingSim_ = haveFloraSim_ = havePropManager_ = true;
        // (the original also subscribes to message 0x26d31ec1 on the message server here)
    }
    return true;
}

bool cSC4LotConfigurationManager::Shutdown() {
    if (initialized_) {
        haveBuildingSim_ = haveFloraSim_ = havePropManager_ = false;
        initialized_ = false;
    }
    return true;
}

bool cSC4LotConfigurationManager::Write(cIGZOStream& stream) const {
    stream.SetUint16(1);
    if (g_host) {
        g_host->WriteSerializable(stream, kGZCLSID_cSC4BuildingDevelopmentSimulator);
        g_host->WriteSerializable(stream, kGZCLSID_cSC4FloraSimulator);
        g_host->WriteSerializable(stream, kGZCLSID_cSC4PropManager);
    }
    return !stream.GetError();
}

bool cSC4LotConfigurationManager::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 1) return false;
    if (g_host) {
        g_host->ReadSerializable(stream, kGZCLSID_cSC4BuildingDevelopmentSimulator);
        g_host->ReadSerializable(stream, kGZCLSID_cSC4FloraSimulator);
        g_host->ReadSerializable(stream, kGZCLSID_cSC4PropManager);
    }
    return !stream.GetError();
}

bool cSC4LotConfigurationManager::DoMessage(std::uint32_t messageType) {
    if (messageType == kMessageLotConfigurationsInit)
        for (auto& kv : g_configurations) InitBuildingInformation(kv.second);
    return true;
}

// ------------------------------------------------------------------------------------ building info

void cSC4LotConfigurationManager::InitBuildingInformation(cSC4LotConfiguration* configuration) const {
    if (!g_host) return;
    std::set<std::uint32_t> styles;
    std::uint16_t minCapacity = 0xffff, maxCapacity = 0;
    bool flag = false;
    if (cSC4LotConfigurationObject* building = configuration->GetPrimaryBuildingConfigurationObject()) {
        for (std::uint32_t family : building->extra) {
            std::vector<std::uint32_t> types;
            g_host->BuildingsInFamily(family, types);
            if (types.empty() && g_host->IsBuildingType(family)) types.push_back(family);
            for (std::uint32_t type : types) {
                std::vector<std::uint32_t> groups;
                bool notRegular = false;
                std::uint16_t capacity = 0;
                g_host->BuildingInformation(type, groups, notRegular, capacity);
                styles.insert(groups.begin(), groups.end());
                if (!notRegular) flag = true;                       // (sic) cleared only by the previous value
                minCapacity = std::min(minCapacity, capacity);
                maxCapacity = std::max(maxCapacity, capacity);
            }
        }
    }
    if (maxCapacity < minCapacity) minCapacity = maxCapacity = 0;
    configuration->SetBuildingInformation(minCapacity, maxCapacity,
                                          std::vector<std::uint32_t>(styles.begin(), styles.end()), flag);
}

// ---------------------------------------------------------------------------------------- registry

void cSC4LotConfigurationManager::FlushCachedLotConfigurationSearches() {
    for (auto& kv : filterCache_) {
        delete kv.second;
        kv.second = nullptr;
    }
}

bool cSC4LotConfigurationManager::ReadLotConfiguration(const cGZPersistResourceKey& key,
                                                       cSC4LotConfiguration*& out) const {
    out = new cSC4LotConfiguration();
    out->AddRef();
    const bool ok = out->Load(key, g_host ? g_host->PreferencesDetail() : 0);
    if (!ok) out->Release();
    else out->Init();
    return ok;
}

bool cSC4LotConfigurationManager::AddLotConfiguration(cSC4LotConfiguration* configuration, bool add) {
    const std::uint32_t id = configuration->GetID();
    auto it = g_configurations.find(id);
    if (it != g_configurations.end()) {
        if (add) {
            it->second->Release();
            configuration->AddRef();
            it->second = configuration;
            return true;
        }
        FlushCachedLotConfigurationSearches();
        it->second->Release();   // the reference the table took when it was added
        g_configurations.erase(it);
        return true;
    }
    if (!add) return true;
    configuration->AddRef();
    for (std::uint32_t family : configuration->GetFamilyIDArray()) g_familyIds.insert(family);
    FlushCachedLotConfigurationSearches();
    g_configurations.emplace(id, configuration);
    return true;
}

bool cSC4LotConfigurationManager::AddLotConfiguration(const cGZPersistResourceKey& key) {
    cSC4LotConfiguration* configuration = nullptr;
    if (!ReadLotConfiguration(key, configuration)) return false;
    const bool ok = AddLotConfiguration(configuration, true);
    configuration->Release();
    return ok;
}

bool cSC4LotConfigurationManager::RemoveLotConfiguration(std::uint32_t id) {
    auto it = g_configurations.find(id);
    return it != g_configurations.end() && AddLotConfiguration(it->second, false);
}

cSC4LotConfiguration* cSC4LotConfigurationManager::GetLotConfiguration(std::uint32_t id) const {
    auto it = g_configurations.find(id);
    return it == g_configurations.end() ? nullptr : it->second;
}

void cSC4LotConfigurationManager::RegisterLotConfigurationFilter(cISC4LotConfigurationFilter* filter) {
    filterCache_.emplace(filter, nullptr);
}

void cSC4LotConfigurationManager::UnregisterLotConfigurationFilter(cISC4LotConfigurationFilter* filter) {
    auto it = filterCache_.find(filter);
    if (it == filterCache_.end()) return;
    delete it->second;
    filterCache_.erase(it);
}

int cSC4LotConfigurationManager::GetLotConfigurationsByFilter(ConfigurationSet& out,
                                                              cISC4LotConfigurationFilter* filter) {
    if (filter) {
        auto it = filterCache_.find(filter);
        if (it != filterCache_.end()) {
            if (!it->second) {
                auto* cached = new std::vector<cSC4LotConfiguration*>();
                for (auto& kv : g_configurations)
                    if (filter->Matches(kv.second)) cached->push_back(kv.second);
                std::sort(cached->begin(), cached->end());
                it->second = cached;
            }
            for (cSC4LotConfiguration* c : *it->second) c->AddRef();
            for (cSC4LotConfiguration* c : *it->second) out.insert(c);
            return static_cast<int>(it->second->size());
        }
    }
    int count = 0;
    for (auto& kv : g_configurations) {
        if (filter && !filter->Matches(kv.second)) continue;
        kv.second->AddRef();
        out.insert(kv.second);
        ++count;
    }
    return count;
}

int cSC4LotConfigurationManager::GetLotConfigurationsByFamily(ConfigurationSet& out, std::uint32_t family) {
    int count = 0;
    for (auto& kv : g_configurations) {
        if (!kv.second->BelongsToFamily(family)) continue;
        kv.second->AddRef();
        out.insert(kv.second);
        ++count;
    }
    return count;
}

int cSC4LotConfigurationManager::GetLotConfigurationsBySize(ConfigurationSet& out, std::uint32_t x,
                                                            std::uint32_t z) {
    int count = 0;
    for (auto& kv : g_configurations) {
        std::uint32_t sx, sz;
        kv.second->GetSize(sx, sz);
        if (sx != x || sz != z) continue;
        kv.second->AddRef();
        out.insert(kv.second);
        ++count;
    }
    return count;
}

void cSC4LotConfigurationManager::ClearLotConfigurationSet(ConfigurationSet& set) {
    for (cSC4LotConfiguration* c : set) c->Release();
    set.clear();
}

int cSC4LotConfigurationManager::GetLotConfigurationIDsByFilter(IdSet& out, cISC4LotConfigurationFilter* filter) {
    int count = 0;
    for (auto& kv : g_configurations) {
        if (filter && !filter->Matches(kv.second)) continue;
        out.insert(kv.second->GetID());
        ++count;
    }
    return count;
}

int cSC4LotConfigurationManager::GetLotConfigurationIDsByFamily(IdSet& out, std::uint32_t family) {
    int count = 0;
    for (auto& kv : g_configurations) {
        if (!kv.second->BelongsToFamily(family)) continue;
        out.insert(kv.second->GetID());
        ++count;
    }
    return count;
}

int cSC4LotConfigurationManager::GetLotConfigurationIDsBySize(IdSet& out, std::uint32_t x, std::uint32_t z) {
    int count = 0;
    for (auto& kv : g_configurations) {
        std::uint32_t sx, sz;
        kv.second->GetSize(sx, sz);
        if (sx != x || sz != z) continue;
        out.insert(kv.second->GetID());
        ++count;
    }
    return count;
}

bool cSC4LotConfigurationManager::LotConfigurationHasBuildingFamily(cSC4LotConfiguration* configuration,
                                                                    std::uint32_t family,
                                                                    cSC4LotConfigurationObject** object) {
    if (object) *object = nullptr;
    if (!configuration || family == 0) return false;
    for (auto& o : configuration->GetLotConfigurationObjectArray()) {
        if (o.type != 0) continue;
        if (std::find(o.extra.begin(), o.extra.end(), family) == o.extra.end()) continue;
        if (object) *object = &o;
        return true;
    }
    return false;
}

bool cSC4LotConfigurationManager::LotConfigurationHasBuildingType(cSC4LotConfiguration* configuration,
                                                                  std::uint32_t type,
                                                                  cSC4LotConfigurationObject** object) {
    if (object) *object = nullptr;
    if (!configuration || type == 0 || !g_host) return false;
    for (auto& o : configuration->GetLotConfigurationObjectArray()) {
        if (o.type != 0) continue;
        if (std::find(o.extra.begin(), o.extra.end(), type) != o.extra.end()) {
            if (object) *object = &o;
            return true;
        }
        for (std::uint32_t family : o.extra) {
            if (g_host->BuildingMatchesFamily(type, family)) {
                if (object) *object = &o;
                return true;
            }
        }
    }
    return false;
}

// --------------------------------------------------------------------------------- footprints / fix

void cSC4LotConfigurationManager::GetMaxFootprintForBuildingFamilies(const std::vector<std::uint32_t>& families,
                                                                     float out[4]) const {
    MaxFootprint(out, 4.0f);
    if (!g_host) return;
    for (std::uint32_t id : families) {
        SC4Footprint f;
        if (g_host->IsBuildingType(id) && g_host->BuildingFootprint(id, f)) {
            Grow(out, f);
            continue;
        }
        std::vector<std::uint32_t> members;
        if (g_host->BuildingsInFamily(id, members))
            for (std::uint32_t m : members)
                if (g_host->BuildingFootprint(m, f)) Grow(out, f);
    }
}

void cSC4LotConfigurationManager::GetMaxFootprintForFloraFamilies(const std::vector<std::uint32_t>& families,
                                                                  float out[4]) const {
    MaxFootprint(out, 2.0f);
    if (!g_host) return;
    for (std::uint32_t id : families) {
        SC4Footprint f;
        if (g_host->FloraFootprint(id, f)) {
            Grow(out, f);
            continue;
        }
        std::unordered_set<std::uint32_t> members;
        if (g_host->FloraInFamily(id, members))
            for (std::uint32_t m : members)
                if (g_host->FloraFootprint(m, f)) Grow(out, f);
    }
}

void cSC4LotConfigurationManager::GetMaxFootprintForPropFamilies(const std::vector<std::uint32_t>& families,
                                                                 float out[4]) const {
    MaxFootprint(out, 2.0f);
    if (!g_host) return;
    for (std::uint32_t id : families) {
        SC4Footprint f;
        if (g_host->PropFootprint(id, f)) {
            Grow(out, f);
            continue;
        }
        std::vector<std::uint32_t> members;
        if (g_host->PropsInFamily(id, members))
            for (std::uint32_t m : members)
                if (g_host->PropFootprint(m, f)) Grow(out, f);
    }
}

void cSC4LotConfigurationManager::GetObjectIDArray(const cSC4LotConfigurationObject* object,
                                                   std::vector<std::uint32_t>& out) const {
    out.clear();
    if (!object || !g_host) return;
    for (std::uint32_t id : object->extra) {
        switch (object->type) {
        case 4:
            if (g_host->IsFloraType(id)) out.push_back(id);
            else {
                std::unordered_set<std::uint32_t> members;
                g_host->FloraInFamily(id, members);
                out.insert(out.end(), members.begin(), members.end());
            }
            break;
        case 1:
            if (g_host->IsPropType(id)) out.push_back(id);
            else {
                std::vector<std::uint32_t> members;
                g_host->PropsInFamily(id, members);
                out.insert(out.end(), members.begin(), members.end());
            }
            break;
        case 0:
            if (g_host->IsBuildingType(id)) out.push_back(id);
            else g_host->BuildingsInFamily(id, out);
            break;
        default: break;
        }
    }
}

bool cSC4LotConfigurationManager::CreateGenericLotConfiguration(cSC4LotConfiguration*& out,
                                                                std::uint32_t buildingType) const {
    out = nullptr;
    if (!g_host || !g_host->BuildingTypeExists(buildingType)) return false;
    float rect[4];
    GetMaxFootprintForBuildingFamilies({buildingType}, rect);
    const std::uint8_t sizeX = static_cast<std::uint8_t>(static_cast<int>(rect[0] * 0.0625f) + 1);
    const std::uint8_t sizeZ = static_cast<std::uint8_t>(static_cast<int>(rect[1] * 0.0625f) + 1);
    cSC4LotConfigurationObject object;
    object.type = 0;
    const float cx = static_cast<float>((sizeX) * 0x10) * 0.5f;
    const float cz = static_cast<float>((sizeZ) * 0x10) * 0.5f;
    object.x = cx;
    object.z = cz;
    object.maxX = cx + rect[0] * 0.5f;
    object.maxZ = cz + rect[1] * 0.5f;
    object.minX = cx - rect[0] * 0.5f;
    object.minZ = cz - rect[1] * 0.5f;
    object.extra.push_back(buildingType);
    out = new cSC4LotConfiguration();
    out->AddRef();
    out->MakeGeneric(g_host->NewGuid(), sizeX, sizeZ, object);
    return true;
}

int cSC4LotConfigurationManager::FixLotConfiguration(cSC4LotConfiguration* configuration) const {
    if (!g_host) return 0;
    std::uint32_t sx, sz;
    configuration->GetSize(sx, sz);
    auto& objects = configuration->GetLotConfigurationObjectArray();
    int result = 0;
    const float limitX = static_cast<float>(sx << 4) + -0.05f + 0.001f;
    const float limitZ = static_cast<float>(sz << 4) + -0.05f + 0.001f;
    for (std::size_t i = 0; i < objects.size();) {
        cSC4LotConfigurationObject& o = objects[i];
        const std::uint8_t type = o.type;
        if (type >= 5 || ((0x13u >> type) & 1) == 0) { ++i; continue; }
        for (std::size_t j = 0; j < o.extra.size();) {
            bool valid = true;
            if (type == 0) valid = g_host->BuildingTypeExists(o.extra[j]);
            else if (type == 1) valid = g_host->PropTypeExists(o.extra[j]);
            else if (type == 4) valid = g_host->FloraTypeExists(o.extra[j]);
            if (!valid) {
                o.extra.erase(o.extra.begin() + j);
                if (result == 0) result = 1;
            } else {
                ++j;
            }
        }
        if (o.extra.empty()) {
            objects.erase(objects.begin() + i);
            if (result == 0) result = 2;
            continue;
        }
        const float width = o.maxX - o.minX;
        const float depth = o.maxZ - o.minZ;
        float rect[4] = {1.0f, 1.0f, 0.0f, 0.0f};
        if (type == 4) GetMaxFootprintForFloraFamilies(o.extra, rect);
        else if (type == 1) GetMaxFootprintForPropFamilies(o.extra, rect);
        else GetMaxFootprintForBuildingFamilies(o.extra, rect);
        float footX = rect[0], footZ = rect[1];
        if ((o.orientation | 2) == 3) std::swap(footX, footZ);
        if (width - footX == 0.0f && depth - footZ == 0.0f) {
            // footprint already matches
        } else {
            o.minX = o.x - footX * 0.5f;
            o.minZ = o.z - footZ * 0.5f;
            o.maxX = footX * 0.5f + o.x;
            o.maxZ = footZ * 0.5f + o.z;
            if (result == 0) result = 3;
        }
        if (o.minX < 0.049000002f || (o.minZ <= 0.049000002f && o.minZ != 0.049000002f) || limitX < o.maxX) {
            if (result == 0) result = 4;
        } else if (result == 0 && limitZ < o.maxZ) {
            result = 4;
        }
        if (o.x < o.minX || o.maxX < o.x || o.z < o.minZ || (o.maxZ <= o.z && o.maxZ != o.z)) {
            o.x = (o.minX + o.maxX) * 0.5f;
            o.z = (o.minZ + o.maxZ) * 0.5f;
            if (result == 0) result = 5;
        }
        ++i;
    }
    auto& walls = configuration->GetRetainingWallIDArray();
    for (std::size_t i = 0; i < walls.size(); ++i) {
        if (!g_host->RetainingWallExists(walls[i])) {
            walls.erase(walls.begin() + i);
            return result != 0 ? result : 6;
        }
    }
    return result;
}

}  // namespace sc4
