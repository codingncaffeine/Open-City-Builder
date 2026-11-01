#include "sc4/BuildingDevelopmentSimulator.h"
#include "sc4/BuildingZoneFit.h"
#include "sc4/DBSegment.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include "sc4/SCPropertyHelp.h"
#include "sc4/Occupant.h"

namespace sc4 {

namespace {
IBuildingDevelopmentSimHost* g_host = nullptr;
std::unordered_map<std::uint32_t, cGZPersistResourceKey> g_buildingKeys;       // mBuildingExemplarKeyMap
std::unordered_map<std::uint32_t, cRZString> g_buildingNames;                  // mBuildingNameMap
std::unordered_map<std::uint32_t, cRZString> g_exemplarNames;                  // mExemplarNameMap
std::unordered_map<std::uint32_t, BuildingProfile> g_profiles;                 // mBuildingProfileMap
std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> g_families;      // mBuildingFamilyMap
std::map<std::pair<std::int32_t, std::uint8_t>, float> g_zoneCapacity;         // mZoneCapacityMap
std::unordered_map<std::uint32_t, cGZPersistResourceKey> g_foundationKeys;     // mFoundationKeyMap
std::unordered_map<std::uint32_t, cGZPersistResourceKey> g_wallKeys;           // mLotRetainingWallKeyMap
bool g_staticInitialized = false;

// kPropertyIdForCategoryBit: occupant group ids -> bit in BuildingProfile::categoryFlags
constexpr std::uint32_t kCategoryGroupIds[31] = {
    0x1000, 0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1006, 0x1300, 0x1301, 0x1302, 0x1400,
    0x1401, 0x1402, 0x1500, 0x1501, 0x1502, 0x1503, 0x1504, 0x1505, 0x1506, 0x1507, 0x1508,
    0x1509, 0x150a, 0x150b, 0x2000, 0x2001, 0x2002, 0x3000, 0x3001, 0x3002};

void CopyKeyList(const std::unordered_map<std::uint32_t, cGZPersistResourceKey>& map, std::vector<std::uint32_t>& out) {
    out.clear();
    for (const auto& kv : map) out.push_back(kv.first);
}
}  // namespace

IBuildingDevelopmentSimHost* BuildingDevelopmentSimHost() { return g_host; }
void SetBuildingDevelopmentSimHost(IBuildingDevelopmentSimHost* host) { g_host = host; }

// ------------------------------------------------------------------------------------ type mapping

std::uint32_t cSC4BuildingDevelopmentSimulator::GetDeveloperType_Static(std::uint32_t purpose, std::int32_t wealth) {
    switch (purpose) {
    case 1: return static_cast<std::uint32_t>(wealth - 1) < 3 ? (wealth - 1) * 0x10 + 0x1010 : 0;
    case 2: return static_cast<std::uint32_t>(wealth - 1) < 3 ? (wealth - 1) * 0x10 + 0x3110 : 0;
    case 3: return wealth == 2 ? 0x3320 : (wealth == 3 ? 0x3330 : 0);
    case 5: return 0x4100;
    case 6: return 0x4200;
    case 7: return 0x4300;
    case 8: return 0x4400;
    default: return 0;
    }
}

std::uint32_t cSC4BuildingDevelopmentSimulator::GetWealth_Static(std::uint32_t developerType) {
    switch (developerType) {
    case 0x1010: case 0x3110: case 0x4100: return 1;
    case 0x1020: case 0x3120: case 0x3320: case 0x4200: case 0x4300: return 2;
    case 0x1030: case 0x3130: case 0x3330: case 0x4400: return 3;
    default: return 0;
    }
}

void cSC4BuildingDevelopmentSimulator::SetBuildingCategoryFlags(BuildingProfile& profile,
                                                                const cISCPropertyHolder& exemplar) {
    profile.categoryFlags = 0;
    std::vector<std::uint32_t> groups;
    if (!cSCPropertyHelp::GetPropertyValue(&exemplar, kBuildingPropGroups, groups)) return;
    for (std::uint32_t group : groups)
        for (int bit = 0; bit < 31; ++bit)
            if (kCategoryGroupIds[bit] == group) profile.categoryFlags |= 1u << bit;
}

std::uint8_t cSC4BuildingDevelopmentSimulator::GetBuildingMapKey(std::uint32_t buildingType) const {
    const BuildingProfile& p = GetBuildingProfile(buildingType);
    return GetBuildingMapKey(p.purpose, p.wealth);
}

// --------------------------------------------------------------------------------------- catalogue

void cSC4BuildingDevelopmentSimulator::LoadBuildingProperties(cISCPropertyHolder& exemplar, std::uint32_t type,
                                                              bool firstLoad) {
    BuildingProfile profile;
    cRZString exemplarName;
    if (!exemplar.GetProperty(0x20, exemplarName)) exemplarName.AssignFromCString("Building");
    cRZString buildingName;
    if (!(g_host && g_host->LoadLocalizedName(&exemplar, kBuildingPropName, buildingName))) {
        if (!exemplar.GetProperty(kBuildingPropCustomName, buildingName)) buildingName.AssignFromCString("Building");
    }

    std::uint8_t value = 0;
    if (cSCPropertyHelp::GetPropertyValue(&exemplar, kBuildingPropWealth, value)) {
        profile.wealth = value;
        if (value > 3) profile.wealth = 0;            // ("Building %s has a bad wealth property.")
    }
    value = 0;
    if (cSCPropertyHelp::GetPropertyValue(&exemplar, kBuildingPropPurpose, value)) profile.purpose = value;

    std::vector<std::uint32_t> pairs;
    if (cSCPropertyHelp::GetPropertyValue(&exemplar, kBuildingPropCapacities, pairs)) {
        std::map<std::uint32_t, std::uint16_t> capacities;
        for (std::size_t i = 0; i + 1 < pairs.size(); i += 2)
            capacities[pairs[i]] = static_cast<std::uint16_t>(pairs[i + 1]);
        profile.capacities.push_back(capacities);
        const std::uint32_t ownType = GetDeveloperType_Static(profile.purpose, static_cast<std::int32_t>(profile.wealth));
        profile.capacity = capacities[ownType];           // (operator[]: 0 when the own type is missing)
    }
    bool partial = false;
    if (cSCPropertyHelp::GetPropertyValue(&exemplar, kBuildingPropPartialCapacity, partial)) profile.allowPartialCapacity = partial;
    std::uint8_t stage = 0x1e;
    cSCPropertyHelp::GetPropertyValue(&exemplar, kBuildingPropGrowthStage, stage);
    profile.growthStage = stage;
    SetBuildingCategoryFlags(profile, exemplar);
    std::vector<float> size;
    if (cSCPropertyHelp::GetPropertyValue(&exemplar, kBuildingPropSize, size))
        for (std::size_t i = 0; i < size.size() && i < 3; ++i) profile.size[i] = size[i];
    std::int64_t cost = 0;
    if (!cSCPropertyHelp::GetPropertyValue(&exemplar, kBuildingPropCost, cost)) cost = 0;
    profile.cost = cost;
    profile.maintenance = g_host ? g_host->MaintenanceCost(&exemplar) : 0;

    if (!firstLoad) {
        g_buildingNames.erase(type);
        g_exemplarNames.erase(type);
        g_profiles.erase(type);
        for (auto& kv : g_families) {
            auto& members = kv.second;
            auto it = std::find(members.begin(), members.end(), type);
            if (it != members.end()) members.erase(it);
        }
    }
    g_buildingNames[type] = buildingName;
    g_exemplarNames[type] = exemplarName;
    g_profiles[type] = profile;

    std::vector<std::uint32_t> families;
    if (cSCPropertyHelp::GetPropertyValue(&exemplar, kBuildingPropFamilies, families)) {
        for (std::uint32_t family : families) {
            auto& members = g_families[family];
            if (std::find(members.begin(), members.end(), type) == members.end()) members.push_back(type);
        }
    }
}

bool cSC4BuildingDevelopmentSimulator::AddBuildingData(const cGZPersistResourceKey& key) {
    if (!g_host) return false;
    cISCPropertyHolder* exemplar = g_host->AcquireExemplar(key);
    if (!exemplar) return false;
    g_buildingKeys[key.instance] = key;
    LoadBuildingProperties(*exemplar, key.instance, true);
    g_host->ReleaseExemplar(exemplar);
    return true;
}

bool cSC4BuildingDevelopmentSimulator::AddFoundationData(const cGZPersistResourceKey& key) {
    if (!g_host) return false;
    if (cISCPropertyHolder* exemplar = g_host->AcquireExemplar(key)) {
        g_foundationKeys[key.instance] = key;
        g_host->ReleaseExemplar(exemplar);
    }
    return false;                      // (sic) always 0
}

bool cSC4BuildingDevelopmentSimulator::AddLotRetainingWallData(const cGZPersistResourceKey& key) {
    if (!g_host) return false;
    if (cISCPropertyHolder* exemplar = g_host->AcquireExemplar(key)) {
        g_wallKeys[key.instance] = key;
        g_host->ReleaseExemplar(exemplar);
    }
    return false;
}

bool cSC4BuildingDevelopmentSimulator::GrowthStageFitsInZone(int stage, std::uint32_t zoneType) {   // @ 0x100150a4c (BuildingZoneFit.cpp)
    return sc4::GrowthStageFitsInZone(stage, zoneType);
}

bool cSC4BuildingDevelopmentSimulator::BuildingFitsInZone(std::uint32_t buildingPurpose, std::uint32_t zoneType) {   // @ 0x100150a86
    return sc4::BuildingFitsInZone(buildingPurpose, zoneType);
}

bool cSC4BuildingDevelopmentSimulator::IsSuitableForConstruction(const ConstructionProfile& profile, ReasonForFailure& reason) const {   // @ 0x100150b0c
    IConstructionSuitabilityHost* city = suitability_;
    if (!city) {
        reason = kReasonNone;
        return false;
    }
    const int x = profile.x, z = profile.z;
    // the zone type is passed on as a signed byte widened to int, as the original does
    if (!BuildingFitsInZone(profile.purpose, static_cast<std::uint32_t>(static_cast<std::int32_t>(city->ZoneTypeAt(x, z))))) {
        reason = kReasonNone;
        return false;
    }
    IConstructionLot* lot = city->LotAt(x, z);
    if (!lot) {
        reason = kReasonNoLot;
        return false;
    }
    if (!cheatPower_ && !city->HasPowerAt(x, z)) {
        reason = kReasonNoPower;
        return false;
    }
    if (profile.minLandValue == 0 && profile.maxLandValue == 0) {
        const std::int32_t landValueClass = city->LandValueClassAt(x, z);
        if (profile.purpose - 7u >= 3u && landValueClass != profile.landValueClass) {
            reason = kReasonLandValueClass;
            return false;
        }
    } else {
        const std::uint8_t landValue = city->LandValueAt(x, z);
        if (landValue < profile.minLandValue || landValue > profile.maxLandValue) {
            reason = kReasonLandValueRange;
            return false;
        }
    }
    if (lot->ConstructionState() != 1) {
        reason = kReasonLotNotReady;
        return false;
    }
    return true;
}

void cSC4BuildingDevelopmentSimulator::InitStaticDataFromExemplar(const cGZPersistResourceKey& key, void*) {
    if (!g_host) return;
    cISCPropertyHolder* exemplar = g_host->AcquireExemplar(key);
    if (!exemplar) return;
    std::uint32_t exemplarType = 0;
    if (cSCPropertyHelp::GetPropertyValue(exemplar, 0x10, exemplarType)) {
        if (exemplarType == kExemplarTypeBuilding) AddBuildingData(key);
        else if (exemplarType == kExemplarTypeFoundation) AddFoundationData(key);
        else if (exemplarType == kExemplarTypeLotRetainingWall) AddLotRetainingWallData(key);
    }
    g_host->ReleaseExemplar(exemplar);
}

void cSC4BuildingDevelopmentSimulator::PostBuildingTypesUpdatedMessage() {
    if (g_host) g_host->PostBuildingTypesUpdated();
}

bool cSC4BuildingDevelopmentSimulator::StaticInit() {
    if (!g_staticInitialized) {
        g_staticInitialized = true;
        if (g_host) {
            g_host->ForEachExemplarKey([](const cGZPersistResourceKey& key) { InitStaticDataFromExemplar(key, nullptr); });
            PostBuildingTypesUpdatedMessage();
        }
        CalculateMaxCapacity();
    }
    return true;
}

bool cSC4BuildingDevelopmentSimulator::StaticShutdown() {
    if (g_staticInitialized) {
        g_staticInitialized = false;
        g_buildingKeys.clear();
        g_buildingNames.clear();
        g_exemplarNames.clear();
        g_profiles.clear();
        g_families.clear();
        g_zoneCapacity.clear();
        g_foundationKeys.clear();
        g_wallKeys.clear();
    }
    return true;
}

void cSC4BuildingDevelopmentSimulator::CalculateMaxCapacity() {
    if (!g_host) return;
    std::vector<IBuildingDevelopmentSimHost::LotConfigurationInfo> configurations;
    g_host->LotConfigurations(configurations);
    float cellX = 16.0f, cellZ = 16.0f;
    g_host->CityCellSize(cellX, cellZ);
    for (const auto& config : configurations) {
        const float area = static_cast<float>(config.sizeX * config.sizeZ);
        const float lotWidth = cellX * static_cast<float>(config.sizeX);
        const float lotDepth = cellZ * static_cast<float>(config.sizeZ);
        std::vector<std::uint32_t> types;
        for (std::uint32_t id : config.buildingIds) {
            auto family = g_families.find(id);
            if (family == g_families.end()) {
                if (std::find(types.begin(), types.end(), id) == types.end()) types.push_back(id);
            } else {
                for (std::uint32_t member : family->second)
                    if (std::find(types.begin(), types.end(), member) == types.end()) types.push_back(member);
            }
        }
        for (std::uint32_t type : types) {
            bool fits = true;
            auto key = g_buildingKeys.find(type);
            cISCPropertyHolder* exemplar = key != g_buildingKeys.end() ? g_host->AcquireExemplar(key->second) : nullptr;
            if (exemplar) {
                std::vector<float> size;
                if (cSCPropertyHelp::GetPropertyValue(exemplar, kBuildingPropSize, size) && size.size() >= 3) {
                    float width, depth;
                    if ((config.objectOrientation | 2) == 3) { width = size[2]; depth = size[0]; }
                    else { width = size[0]; depth = size[2]; }
                    const float minX = config.objectX - width * 0.5f;
                    const float maxX = width * 0.5f + config.objectX;
                    fits = (0.0f <= minX + 0.001f) && (0.0f <= (config.objectZ - depth * 0.5f) + 0.001f) &&
                           (maxX - 0.001f <= lotWidth) && (depth * 0.5f + config.objectZ - 0.001f <= lotDepth);
                }
                g_host->ReleaseExemplar(exemplar);
            }
            if (!fits) continue;
            auto profileIt = g_profiles.find(type);
            if (profileIt == g_profiles.end()) continue;
            const BuildingProfile& profile = profileIt->second;
            if (profile.purpose == 0) continue;
            float perCell = static_cast<float>(profile.capacity) / area + 0.5f;
            if (profile.capacity != 0 && perCell < 1.0f) perCell = 1.0f;
            const std::uint8_t mapKey = static_cast<std::uint8_t>(profile.wealth + 1 + profile.purpose * 4);
            for (int zone = 0; zone < 16; ++zone) {
                if (zone >= static_cast<int>(config.zoneCompatibility.size()) || !config.zoneCompatibility[zone]) continue;
                auto k = std::make_pair(static_cast<std::int32_t>(zone), mapKey);
                auto it = g_zoneCapacity.find(k);
                if (it == g_zoneCapacity.end() || it->second < perCell) g_zoneCapacity[k] = perCell;
            }
        }
    }
}

float cSC4BuildingDevelopmentSimulator::GetMaxCapacity(int zoneType, std::uint32_t purpose,
                                                       std::uint32_t wealth) const {
    auto it = g_zoneCapacity.find({zoneType, GetBuildingMapKey(purpose, wealth)});
    if (it == g_zoneCapacity.end()) return 0.0f;
    return static_cast<float>(static_cast<std::int32_t>(it->second));   // cvttss2si then cvtsi2ss
}

void cSC4BuildingDevelopmentSimulator::GetAllBuildingTypes(std::vector<std::uint32_t>& out) const { CopyKeyList(g_buildingKeys, out); }
void cSC4BuildingDevelopmentSimulator::GetAllBuildingFamilyTypes(std::vector<std::uint32_t>& out) const {
    out.clear();
    for (const auto& kv : g_families) out.push_back(kv.first);
}

bool cSC4BuildingDevelopmentSimulator::GetBuildingKeyFromType(std::uint32_t type, cGZPersistResourceKey& key) const {
    auto it = g_buildingKeys.find(type);
    if (it == g_buildingKeys.end()) return false;
    key = it->second;
    return true;
}

bool cSC4BuildingDevelopmentSimulator::IsBuildingTypeValid(std::uint32_t type) const { return g_buildingKeys.count(type) != 0; }

bool cSC4BuildingDevelopmentSimulator::IsBuildingFamilyTypeValid(std::uint32_t type) const {
    return g_families.count(type) != 0 || g_buildingKeys.count(type) != 0;
}

int cSC4BuildingDevelopmentSimulator::GetBuildingTypesByFamily(std::vector<std::uint32_t>& out, std::uint32_t family) const {
    auto it = g_families.find(family);
    if (it != g_families.end()) {
        if (&it->second != &out) out = it->second;
        return static_cast<int>(it->second.size());
    }
    if (g_buildingKeys.count(family) == 0) return 0;
    out.assign(1, family);
    return 0;                              // (sic) like the prop manager: fills one id but returns 0
}

bool cSC4BuildingDevelopmentSimulator::IsBuildingInFamily(std::uint32_t type, std::uint32_t family) const {
    if (type == family) return true;
    auto it = g_families.find(family);
    return it != g_families.end() && std::find(it->second.begin(), it->second.end(), type) != it->second.end();
}

void cSC4BuildingDevelopmentSimulator::GetAllFoundationTypes(std::vector<std::uint32_t>& out) const { CopyKeyList(g_foundationKeys, out); }
bool cSC4BuildingDevelopmentSimulator::GetFoundationKeyFromType(std::uint32_t type, cGZPersistResourceKey& key) const {
    auto it = g_foundationKeys.find(type);
    if (it == g_foundationKeys.end()) return false;
    key = it->second;
    return true;
}
void cSC4BuildingDevelopmentSimulator::GetAllLotRetainingWallTypes(std::vector<std::uint32_t>& out) const { CopyKeyList(g_wallKeys, out); }
bool cSC4BuildingDevelopmentSimulator::GetLotRetainingWallKeyFromType(std::uint32_t type, cGZPersistResourceKey& key) const {
    auto it = g_wallKeys.find(type);
    if (it == g_wallKeys.end()) return false;
    key = it->second;
    return true;
}

BuildingProfile& cSC4BuildingDevelopmentSimulator::GetBuildingProfile(std::uint32_t type) const { return g_profiles[type]; }
const BuildingProfile* cSC4BuildingDevelopmentSimulator::GetBuildingProfilePtr(std::uint32_t type) const {
    auto it = g_profiles.find(type);
    return it == g_profiles.end() ? nullptr : &it->second;
}
cRZString& cSC4BuildingDevelopmentSimulator::GetBuildingName(std::uint32_t type) const { return g_buildingNames[type]; }
cRZString& cSC4BuildingDevelopmentSimulator::GetExemplarName(std::uint32_t type) const { return g_exemplarNames[type]; }

// ---------------------------------------------------------------------------- construction resources

std::uint16_t cSC4BuildingDevelopmentSimulator::GetFreeConstructionResources() const {
    if (!resources_) return 0;
    const float free = resources_->Total() - resources_->Used();
    const std::int32_t value = static_cast<std::int32_t>(free);
    return static_cast<std::uint16_t>(value < 0 ? 0 : value);
}

std::uint16_t cSC4BuildingDevelopmentSimulator::GetTotalConstructionResources() const {
    return resources_ ? static_cast<std::uint16_t>(static_cast<std::int32_t>(resources_->Total())) : 0;
}

bool cSC4BuildingDevelopmentSimulator::ClaimConstructionResources(std::uint16_t amount) {
    if (!resources_) return false;
    const bool ok = resources_->Used() + static_cast<float>(amount) <= resources_->Total();
    if (ok) {
        resources_->Add(static_cast<float>(amount));
        resources_->Update();
    }
    return ok;
}

void cSC4BuildingDevelopmentSimulator::ReleaseConstructionResources(std::uint16_t amount) {
    if (!resources_) return;
    resources_->Add(-static_cast<float>(amount));
    resources_->Update();
}

std::int32_t cSC4BuildingDevelopmentSimulator::GetDayOfLastSuccessfulBuild(std::int32_t developerType) const {
    auto it = lastBuildDay_.find(developerType);
    return it == lastBuildDay_.end() ? 0 : it->second;
}

bool cSC4BuildingDevelopmentSimulator::IsTractUnmarked(std::uint32_t developerType, int x, int z) const {
    auto it = growthMarks_.find(developerType);
    return it == growthMarks_.end() || it->second.find({x, z}) == it->second.end();
}

// ------------------------------------------------------------------------------------- lifecycle

std::uint32_t cSC4BuildingDevelopmentSimulator::Release() {
    if (refCount_ < 2) {
        Shutdown();
        delete this;
        return 0;
    }
    return --refCount_;
}

bool cSC4BuildingDevelopmentSimulator::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != 1) return false;
    *out = this;
    AddRef();
    return true;
}

// 0x10014ce64. PARTIAL: the services the original AddRefs (+0x28..+0x68: the resource manager, city, occupant manager, zone,
// lot, land value and power simulators, terrain) are reached through the host and the injected interfaces; the building map grid
// (+0x80) and the lot / tract developers (+0xd8 / +0xd0, layer 3) are not attached here; the cheat codes are not registered (no
// cheat-code manager natively).
bool cSC4BuildingDevelopmentSimulator::Init() {
    if (initialized_) return true;
    initialized_ = true;
    StaticInit();
    if (!placement_ && g_host) placement_ = g_host->PlacementServices();
    if (!tractDeveloper_ && g_host) tractDeveloper_ = g_host->TractDeveloper();    // SL::spTractDeveloper (+0xd0)
    // every exemplar of type 0x6534284a whose ExemplarType (0x10) is 4 (a simulator) that names a developer type makes a developer
    if (g_host) g_host->ForEachExemplarKey([this](const cGZPersistResourceKey& key) { InitDynamicDataFromExemplar(key, this); });
    ReadSimulatorTunableValues();
    // the construction resources (+0xe0): a cSC4Demand with supply 100 and demand 0
    if (!resources_ && g_host) {
        resources_ = g_host->CreateConstructionResources();
        if (resources_) {
            resources_->Init();
            resources_->SetTotal(100.0f);
            resources_->SetUsed(0.0f);
        }
    } else if (resources_) {
        resources_->Init();
    }
    if (g_host && !messageTargetRegistered_) {
        g_host->RegisterMessageTarget(this);
        messageTargetRegistered_ = true;
    }
    return true;
}

bool cSC4BuildingDevelopmentSimulator::Shutdown() {
    if (!initialized_) return true;
    initialized_ = false;
    if (g_host && messageTargetRegistered_) {
        g_host->UnregisterMessageTarget(this);
        messageTargetRegistered_ = false;
    }
    for (cIGZUnknown* occupant : civicOccupants_) if (occupant) occupant->Release();
    civicOccupants_.clear();
    stage_ = 0;
    idleCursorValid_ = false;
    return true;
}

// 0x10014d666: a simulator exemplar (ExemplarType 4) makes or re-initialises the developer it names (property 0x21).
void cSC4BuildingDevelopmentSimulator::InitDynamicDataFromExemplar(const cGZPersistResourceKey& key, void* context) {
    auto* self = static_cast<cSC4BuildingDevelopmentSimulator*>(context);
    if (!self || !g_host) return;
    cISCPropertyHolder* exemplar = g_host->AcquireExemplar(key);
    if (!exemplar) return;
    std::uint32_t exemplarType = 0;
    if (cSCPropertyHelp::GetPropertyValue(exemplar, 0x10, exemplarType) && exemplarType == 4) self->CreateDeveloper(exemplar);
    g_host->ReleaseExemplar(exemplar);
}

// 0x10014d71e: the construction-resource bounds (0x47e2c380 = {min, max}, defaults 100 / 2000), resources per unit of RCI
// demand (0x47e2c381, 0.5), the developer weight bonus (0x47e2c382, 10) and the stage population multipliers (0x47e2c383).
void cSC4BuildingDevelopmentSimulator::ReadSimulatorTunableValues() {
    minResources_ = 100;
    maxResources_ = 2000;
    resourcesPerDemand_ = 0.5f;
    std::uint32_t bonus = 10;
    cISCPropertyHolder* tunables = g_host ? g_host->AcquireExemplar(cGZPersistResourceKey{0x6534284a, 0xe7e2c2db, 0xe8da7677}) : nullptr;
    if (tunables) {
        std::vector<std::uint32_t> bounds;
        if (cSCPropertyHelp::GetPropertyValue(tunables, 0x47e2c380, bounds) && bounds.size() >= 2) {
            minResources_ = bounds[0];
            maxResources_ = bounds[1];
        }
        cSCPropertyHelp::GetPropertyValue(tunables, 0x47e2c381, resourcesPerDemand_);
        cSCPropertyHelp::GetPropertyValue(tunables, 0x47e2c382, bonus);
        cSCPropertyHelp::GetPropertyValue(tunables, 0x47e2c383, stageMultipliers_);
        g_host->ReleaseExemplar(tunables);
    }
    developerWeightBonus_ = bonus;
}

// 0x10014e84c. PARTIAL: the building-changed (0xe8bda2e7 -> DoBuildingChanged), terrain-changed (0x278e6003 ->
// DoTerrainChanged) and PlaceBuilding command (0xb99ae81) branches need functions that are not recovered.
bool cSC4BuildingDevelopmentSimulator::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    const std::uint32_t type = message->GetType();
    switch (type) {
        case 0x99ef1142:                                     // an occupant inserted: civic buildings (families 0x150a / 0x150b)
        case 0x99ef1143: {                                   // ... removed
            auto* occupant = static_cast<cISC4Occupant*>(standard->GetVoid1());
            if (!occupant) break;
            cISCPropertyHolder* properties = occupant->AsPropertyHolder();                 // vslot 0x30
            if (properties && properties->HasProperty(0x29244db)) break;                  // vslot 0x18
            if (!occupant->IsOccupantGroup(0x150a) && !occupant->IsOccupantGroup(0x150b)) break;   // vslot 0xb0
            if (type == 0x99ef1142) {
                occupant->AddRef();
                civicOccupants_.push_front(occupant);
            } else {
                for (auto it = civicOccupants_.begin(); it != civicOccupants_.end(); ++it)
                    if (*it == occupant) { civicOccupants_.erase(it); occupant->Release(); break; }
            }
            break;
        }
        case 0x230e27ac: {                                   // cheat codes
            const std::uint64_t cheat = standard->GetData1();
            if (cheat == 0x8dad89a) cheatPower_ = true;
            else if (cheat == 0x6ab6a2d7) cheatCoffee_ = !cheatCoffee_;
            else if (cheat == 0x491865e1) cheatPlumbing_ = true;
            break;
        }
        case 0x66956814:                                     // a day
            DoIdle();
            DoDailySimulation();
            break;
        case 0x6695681a:                                     // idle time
            DoIdle();
            break;
        case 0x66956810:                                     // the month's two messages
        case 0x66956816:
            DoMonthlySimulation();
            break;
        default:
            break;
    }
    return true;
}

// 0x10014ecba: the stage machine the month starts. 1/2: wait until every developer finished its map update (one may need
// several idle slices); 3: plan the growth (only in the planned mode, which nothing sets); 4: plan the decline; back to 0.
void cSC4BuildingDevelopmentSimulator::DoIdle() {
    switch (stage_) {
        case 1:
            idleCursorValid_ = !developers_.empty();
            if (idleCursorValid_) idleCursor_ = developers_.begin()->first;
            stage_ = 2;
            [[fallthrough]];
        case 2: {
            auto it = idleCursorValid_ ? developers_.find(idleCursor_) : developers_.end();
            for (; it != developers_.end(); ++it) {
                idleCursor_ = it->first;
                idleCursorValid_ = true;
                if (it->second && !it->second->IsMapUpdateFinished()) return;   // vslot 0xe0
            }
            idleCursorValid_ = false;
            stage_ = 3;
            [[fallthrough]];
        }
        case 3:
            stage_ = 4;   // PlanGrowth only when +0x1c8 (planned growth) is set
            return;
        case 4:
            PlanDecline();
            stage_ = 0;
            return;
        default:
            return;
    }
}

// 0x10014fb24, the "random pass": within a 3 ms slice, a developer is drawn at random among those whose active demand is above
// 5 (weight = the demand + +0x1a8); its first growth tract this developer has not marked yet is marked and handed to the tract
// developer (vslot 0x28); a build records the day. When every tract of the list is marked, the marks of that developer are
// cleared and the list starts over. The drawn developer leaves the candidates whatever happens. Kept: a pass where no developer
// has demand above 5 only ends when the slice expires (the original spins on its candidate map); the port stops at once.
void cSC4BuildingDevelopmentSimulator::Grow() {
    if (!g_host) return;
    std::map<std::uint32_t, IGrowthDeveloperRef*> candidates;
    for (const auto& entry : developers_) if (entry.second) candidates.emplace(entry.first, entry.second);
    const std::uint64_t slice = g_host->BeginGrowSlice();
    while (!candidates.empty() && !g_host->GrowSliceExpired(slice)) {
        std::uint32_t total = 0;
        for (const auto& entry : candidates) {
            const int demand = static_cast<int>(entry.second->ActiveDemand());              // vslot 0x90 -> 0x60
            if (demand > 5) total += static_cast<std::uint32_t>(demand) + developerWeightBonus_;
        }
        if (total == 0) break;
        std::int32_t pick = static_cast<std::int32_t>(g_host->RandomBelow(total));
        for (auto it = candidates.begin(); it != candidates.end(); ++it) {
            IGrowthDeveloperRef* developer = it->second;
            const int demand = static_cast<int>(developer->ActiveDemand());
            if (demand <= 5) continue;
            const std::int32_t weight = demand + static_cast<std::int32_t>(developerWeightBonus_);
            const bool chosen = pick < weight;
            pick -= weight;
            if (!chosen) continue;
            const std::uint32_t type = developer->SubType();                               // vslot 0x30
            if (developer->ListsValid()) {                                                  // vslot 0x50
                const std::vector<GrowthTract> list = developer->GrowthList();              // vslot 0x58
                auto tract = std::find_if(list.begin(), list.end(), [&](const GrowthTract& t) { return IsTractUnmarked(type, t.x, t.z); });
                if (tract == list.end()) {                    // every tract tried: forget the marks and start over
                    growthMarks_[type].clear();
                    tract = list.begin();
                }
                if (tract != list.end()) {
                    MarkTract(type, tract->x, tract->z);
                    if (tractDeveloper_ && tractDeveloper_->Grow(developer, tract->x, tract->z, static_cast<std::uint8_t>(tract->style)) > 0)
                        lastBuildDay_[static_cast<std::int32_t>(type)] = g_host->SimulatorDay();
                }
            }
            candidates.erase(it);
            break;
        }
    }
}

// 0x10014edcc: grow (the planned mode would execute its plan instead), then the decline plan.
void cSC4BuildingDevelopmentSimulator::DoDailySimulation() {
    Grow();
    ExecuteDeclinePlan();
}

// 0x10014ee44: once the previous month's stages are done, the construction resources become the R + C + I demand (channels
// 0x1001 / 0x3001 / 0x4001, their supply) times +0x1a4, clamped to [+0x19c, +0x1a0]; every developer starts its map update.
void cSC4BuildingDevelopmentSimulator::DoMonthlySimulation() {
    if (stage_ != 0) return;
    const float demand = g_host ? g_host->DemandChannelSupply(0x1001) + g_host->DemandChannelSupply(0x3001) + g_host->DemandChannelSupply(0x4001) : 0.0f;
    std::uint32_t total = static_cast<std::uint32_t>(demand * resourcesPerDemand_);
    if (total < minResources_) total = minResources_;
    if (maxResources_ < total) total = maxResources_;
    if (resources_) resources_->SetTotal(static_cast<float>(static_cast<int>(total)));   // vslot 0x90
    for (auto& entry : developers_) if (entry.second) entry.second->StartMapUpdate();   // vslot 0xd8
    stage_ = 1;
}

bool cSC4BuildingDevelopmentSimulator::Write(cIGZOStream& stream) const {
    if (stream.GetError()) return false;
    stream.SetUint16(4);
    void* dbStream = nullptr;
    if (fromCityFile_ && stream.QueryInterface(kGZIID_cISC4DBSegmentOStream, &dbStream) && dbStream) {
        // the city-file layout ReadCityRecord reads: the file's own tables around the port's growth marks and flags
        static_cast<cIGZUnknown*>(dbStream)->Release();
        fileHead_.Replay(stream);
        stream.SetUint32(static_cast<std::uint32_t>(growthMarks_.size()));
        for (const auto& mark : growthMarks_) {
            stream.SetUint32(mark.first);
            stream.SetUint32(static_cast<std::uint32_t>(mark.second.size()));
            for (const auto& cell : mark.second) {
                stream.SetSint32(cell.first);
                stream.SetSint32(cell.second);
            }
        }
        fileTail_.Replay(stream);
        stream.SetUint8(cheatPlumbing_);
        stream.SetUint8(cheatPower_);
        stream.SetUint8(cheatCaps_);
        return !stream.GetError();
    }
    stream.SetUint32(static_cast<std::uint32_t>(growthMarks_.size()));
    for (const auto& mark : growthMarks_) {
        stream.SetUint32(mark.first);
        stream.SetUint32(static_cast<std::uint32_t>(mark.second.size()));
        for (const auto& cell : mark.second) {
            stream.SetSint32(cell.first);
            stream.SetSint32(cell.second);
        }
    }
    stream.SetUint32(static_cast<std::uint32_t>(lastBuildDay_.size()));
    for (const auto& kv : lastBuildDay_) {
        stream.SetSint32(kv.first);
        stream.SetSint32(kv.second);
    }
    stream.SetUint8(cheatPlumbing_);
    stream.SetUint8(cheatPower_);
    stream.SetUint8(cheatCaps_);
    return !stream.GetError();
}

// 0x10014e064: the record of a city file (version 4): the growth grid and two objects as references, the per-zone developers, the lists
// of occupants and developer types and the growth marks, the growth profiles, then the three cheat flags. References and the tables
// the port does not model are consumed; the growth marks and the flags are kept. A stream that is not a city-file segment stream (the
// unit tests) carries the port's own reduced format, read below.
bool cSC4BuildingDevelopmentSimulator::ReadCityRecord(cIGZIStream& stream) {
    constexpr std::uint32_t kMaxCount = 1u << 22;
    if (stream.GetError()) return false;
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 4) return false;
    const auto skipReference = [&stream](std::uint32_t iid) {
        void* raw = nullptr;
        SC4ReadGZSerializable(stream, iid, &raw, false);
        if (raw) static_cast<cIGZUnknown*>(raw)->Release();
    };
    (void)skipReference;
    std::uint32_t count = 0;
    std::uint8_t u8 = 0;
    cRecordTape& head = fileHead_;   // what the port does not model is kept as read (RecordTape.h) and saved back
    head.Clear();
    fileTail_.Clear();
    head.Ref(stream);
    head.Ref(stream);
    count = head.U32(stream);                                  // the developers by key
    if (count > kMaxCount) return false;
    for (std::uint32_t i = 0; i < count && !stream.GetError(); ++i) { head.U32(stream); head.Ref(stream); }
    count = head.U32(stream);                                  // occupants
    if (count > kMaxCount) return false;
    for (std::uint32_t i = 0; i < count && !stream.GetError(); ++i) head.Ref(stream);
    count = head.U32(stream);                                  // developer type -> byte
    if (count > kMaxCount) return false;
    for (std::uint32_t i = 0; i < count && !stream.GetError(); ++i) { head.U32(stream); head.U8(stream); }
    growthMarks_.clear();
    stream.GetUint32(count);                                   // growth marks: key -> set of cells
    if (count > kMaxCount) return false;
    for (std::uint32_t i = 0; i < count && !stream.GetError(); ++i) {
        std::uint32_t key = 0, size = 0;
        stream.GetUint32(key);
        stream.GetUint32(size);
        if (size > kMaxCount) return false;
        auto& cells = growthMarks_[key];
        for (std::uint32_t j = 0; j < size && !stream.GetError(); ++j) {
            std::int32_t x = 0, z = 0;
            stream.GetSint32(x);
            stream.GetSint32(z);
            cells.insert({x, z});
        }
    }
    count = fileTail_.U32(stream);                             // growth profiles: developer, two ints, a byte -> count
    if (count > kMaxCount) return false;
    for (std::uint32_t i = 0; i < count && !stream.GetError(); ++i) {
        fileTail_.Ref(stream);
        fileTail_.S32(stream); fileTail_.S32(stream); fileTail_.U8(stream); fileTail_.U32(stream);
    }
    fromCityFile_ = true;
    stream.GetUint8(u8); cheatPlumbing_ = u8 != 0;
    stream.GetUint8(u8); cheatPower_ = u8 != 0;
    stream.GetUint8(u8); cheatCaps_ = u8 != 0;
    return !stream.GetError();
}

bool cSC4BuildingDevelopmentSimulator::Read(cIGZIStream& stream) {
    void* dbStream = nullptr;
    if (stream.QueryInterface(kGZIID_cISC4DBSegmentIStream, &dbStream) && dbStream) {
        static_cast<cIGZUnknown*>(dbStream)->Release();
        return ReadCityRecord(stream);
    }
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 4) return false;
    growthMarks_.clear();
    std::uint32_t count = 0;
    stream.GetUint32(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t key = 0, size = 0;
        stream.GetUint32(key);
        stream.GetUint32(size);
        auto& cells = growthMarks_[key];
        for (std::uint32_t j = 0; j < size; ++j) {
            std::int32_t x = 0, z = 0;
            stream.GetSint32(x);
            stream.GetSint32(z);
            cells.insert({x, z});
        }
    }
    lastBuildDay_.clear();
    stream.GetUint32(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::int32_t type = 0, day = 0;
        stream.GetSint32(type);
        stream.GetSint32(day);
        lastBuildDay_[type] = day;
    }
    std::uint8_t b = 0;
    stream.GetUint8(b); cheatPlumbing_ = b != 0;
    stream.GetUint8(b); cheatPower_ = b != 0;
    stream.GetUint8(b); cheatCaps_ = b != 0;
    return !stream.GetError();
}

// ------------------------------------------------------------------------------------------ placement

bool cSC4BuildingDevelopmentSimulator::LocationIsOKForNewBuilding(std::uint32_t type, float x, float z) {
    if (!IsBuildingTypeValid(type) || !placement_) return false;
    int cellX = 0, cellZ = 0;
    placement_->PositionToCell(x, z, cellX, cellZ);
    return !placement_->OccupantAtCell(cellX, cellZ, 0x278128a0);
}

void cSC4BuildingDevelopmentSimulator::GetStandardCityCellArea(const float rect[4], int out[4]) const {
    for (int i = 0; i < 4; ++i) out[i] = static_cast<int>(rect[i] * 0.0625f);
    if (out[0] < 0) out[0] = 0;
    if (out[1] < 0) out[1] = 0;
    if (cityCellsX_ <= static_cast<int>(rect[2] * 0.0625f)) out[2] = cityCellsX_ - 1;
    if (cityCellsZ_ <= static_cast<int>(rect[3] * 0.0625f)) out[3] = cityCellsZ_ - 1;
}

bool cSC4BuildingDevelopmentSimulator::AddNewBuildingOccupant(IPlacedBuilding* building) {
    return placement_ ? placement_->AddOccupant(building, false) : false;
}

IPlacedBuilding* cSC4BuildingDevelopmentSimulator::AddNewBuildingOccupant(std::uint32_t type, float* position, int rotation) {
    if (!placement_ || !IsBuildingTypeValid(type)) return nullptr;
    if (!LocationIsOKForNewBuilding(type, position[0], position[2])) return nullptr;
    IPlacedBuilding* building = placement_->CreateBuilding();
    building->SetBuildingType(type);
    placement_->LoadBuildingProperties(*building, type);
    float minimum[3], maximum[3];
    building->GetBounds(minimum, maximum);
    if ((static_cast<unsigned>(rotation) | 2u) == 3) {                  // a quarter turn swaps the footprint's x and z
        std::swap(minimum[0], minimum[2]);
        std::swap(maximum[0], maximum[2]);
    }
    building->SetBounds(minimum, maximum);
    const float halfX = (maximum[0] - minimum[0]) * 0.5f;
    const float halfZ = (maximum[2] - minimum[2]) * 0.5f;
    float area[4] = {(position[0] - halfX) + 0.5f, (position[2] - halfZ) + 0.5f,
                     (halfX + position[0]) + -0.5f, (halfZ + position[2]) + -0.5f};
    int cells[4];
    GetStandardCityCellArea(area, cells);
    if (position[1] <= 0.0f && position[1] != 0.0f) {                   // a negative height asks for the surface under the footprint
        const float footprint[4] = {minimum[0], minimum[2], maximum[0], maximum[2]};
        position[1] = placement_->SurfaceHeightForFootprint(footprint);
    }
    building->SetPosition(position);
    building->SetRotation(rotation);
    building->FinishPlacement();
    const bool added = AddNewBuildingOccupant(building);
    placement_->ReleaseBuilding(building);
    return added ? building : nullptr;
}

void cSC4BuildingDevelopmentSimulator::AddNewBuildingOccupant(std::uint32_t type, int x, int z, int rotation) {
    if (!placement_) return;
    float position[3];
    placement_->CellToPosition(x, z, position[0], position[2]);
    position[1] = placement_->TerrainHeight(position[0], position[2]);
    AddNewBuildingOccupant(type, position, rotation);
}

void cSC4BuildingDevelopmentSimulator::PlopBuilding(const ConstructionProfile& profile) {
    AddNewBuildingOccupant(profile.buildingType, profile.x, profile.z, profile.rotation);
}

bool cSC4BuildingDevelopmentSimulator::AddNewFoundationOccupant(IPlacedOccupant* foundation) {
    return placement_ ? placement_->AddOccupant(foundation, false) : false;
}

std::int64_t cSC4BuildingDevelopmentSimulator::GetCostOfOccupantDemolition(IPlacedBuilding* building, std::uint32_t) const {
    std::int64_t cost = 0;
    cSCPropertyHelp::GetPropertyValue(building->PropertyHolder(), 0x99afacd, cost);
    return cost;
}

bool cSC4BuildingDevelopmentSimulator::LoadFoundationProperties(IPlacedFoundation* foundation, std::uint32_t type) {
    cISCPropertyHolder* properties = foundation->PropertyHolder();
    if (type == 0) type = foundation->FoundationType();
    auto it = g_foundationKeys.find(type);
    if (it == g_foundationKeys.end() || !g_host) return false;
    cISCPropertyHolder* exemplar = g_host->AcquireExemplar(it->second);
    if (!exemplar) return false;
    foundation->ApplyExemplar(exemplar);
    float height = 0.0f;
    if (cSCPropertyHelp::GetPropertyValue(properties, 0x88fcc3e3, height)) foundation->SetHeight(height);
    g_host->ReleaseExemplar(exemplar);
    return true;
}

bool cSC4BuildingDevelopmentSimulator::AddNewFoundationOccupant(std::uint32_t type, int style, float* position,
                                                                float width, float depth, int rotation, float baseHeight) {
    if (!placement_) return false;
    IPlacedFoundation* foundation = placement_->CreateFoundation();
    foundation->SetFoundationType(type);
    bool added = false;
    if (LoadFoundationProperties(foundation, type)) {
        const float area[4] = {(position[0] - width * 0.5f) + 0.5f, (position[2] - depth * 0.5f) + 0.5f,
                               width * 0.5f + position[0] + -0.5f, depth * 0.5f + position[2] + -0.5f};
        int cells[4];
        GetStandardCityCellArea(area, cells);
        float top = position[1];
        if (top < 0.0f) {
            top = placement_->FoundationSurfaceHeight(cells, 0);
            position[1] = top;
        }
        float bottom = baseHeight;
        if (bottom < 0.0f) {
            bottom = placement_->FoundationSurfaceHeight(cells, 1);
            top = position[1];
        }
        const float difference = top - bottom;
        const float size[3] = {width, 0.5f <= difference ? difference : 0.5f, depth};
        foundation->SetPosition(position);
        foundation->SetSize(size);
        foundation->SetStyle(style);
        foundation->SetRotation(rotation);
        foundation->FinishPlacement();
        added = AddNewFoundationOccupant(foundation);
    }
    placement_->ReleaseFoundation(foundation);
    return added;
}

void cSC4BuildingDevelopmentSimulator::DemolishFoundationOccupant(IPlacedOccupant* foundation, std::uint32_t) {
    if (placement_) placement_->RemoveOccupant(foundation, true, false);
}

int cSC4BuildingDevelopmentSimulator::GetBuildingTypesByFilter(std::unordered_set<std::uint32_t>& out,
                                                                IBuildingTypeFilter* filter) const {
    int accepted = 0;
    for (const auto& kv : g_buildingKeys) {
        if (filter && !filter->Accepts(kv.first)) continue;
        out.insert(kv.first);
        ++accepted;
    }
    return accepted;
}

// ------------------------------------------------------------------------------------------ developers

IGrowthDeveloperRef* cSC4BuildingDevelopmentSimulator::GetDeveloper(std::uint32_t developerType) const {
    auto it = developers_.find(developerType);
    return it == developers_.end() ? nullptr : it->second;
}

IGrowthDeveloperRef* cSC4BuildingDevelopmentSimulator::GetDeveloper(std::uint32_t purpose, std::int32_t wealth) const {
    return GetDeveloper(GetDeveloperType(purpose, wealth));
}

void cSC4BuildingDevelopmentSimulator::CreateDeveloper(cISCPropertyHolder* properties) {
    std::uint32_t developerType = 0;
    cSCPropertyHelp::GetPropertyValue(properties, 0x21, developerType);
    if (developerType == 0) return;
    if (IGrowthDeveloperRef* existing = GetDeveloper(developerType)) {
        existing->Init(properties);
    } else if (placement_) {
        IGrowthDeveloperRef* developer = placement_->CreateGrowthDeveloper();
        developer->Init(properties);
        developers_[developerType] = developer;
    }
    lastBuildDay_[static_cast<std::int32_t>(developerType)] = g_host ? g_host->SimulatorDay() : 0;
}

void cSC4BuildingDevelopmentSimulator::ExecuteDeclinePlan() {
    const std::uint32_t today = static_cast<std::uint32_t>(g_host ? g_host->SimulatorDay() : 0);
    for (auto it = declinePlan_.begin(); it != declinePlan_.end();) {
        if (today < it->day) { ++it; continue; }                         // not due yet
        if (static_cast<int>(it->source->Condition()) < -0x13) {
            TractOutcome outcome;
            outcome.source = it->source;
            outcome.a = it->a;
            outcome.b = it->b;
            outcome.flag = it->flag;
            const int result = tractDeveloper_ ? tractDeveloper_->ExecuteOutcome(outcome) : 0;
            it = declinePlan_.erase(it);
            if (result > 0) return;                                      // one successful decline per call
        } else {
            it = declinePlan_.erase(it);
        }
    }
}

}  // namespace sc4
