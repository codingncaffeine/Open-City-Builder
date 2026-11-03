#include "sc4/Lot.h"

#include <algorithm>
#include <cmath>

#include "gz/StreamOps.h"
#include "sc4/BusinessFeature.h"
#include "sc4/CommonIIDs.h"
#include "sc4/DBSegment.h"

namespace sc4 {

namespace {
ILotHost* g_lotHost = nullptr;

template <typename Map>
const typename Map::mapped_type* FindExact(const Map& map, const typename Map::key_type& key) {
    auto it = map.find(key);
    return it == map.end() ? nullptr : &it->second;
}
}  // namespace

ILotHost* LotHost() { return g_lotHost; }
void SetLotHost(ILotHost* host) { g_lotHost = host; }

bool WriteLotReference(cIGZOStream& stream, const cSC4Lot* lot) {
    cIGZUnknown* serializable = lot && g_lotHost ? g_lotHost->LotSerializable(const_cast<cSC4Lot*>(lot)) : nullptr;
    return SC4WriteGZSerializable(stream, serializable);
}

cSC4Lot* ReadLotReference(cIGZIStream& stream) {
    void* raw = nullptr;
    SC4ReadGZSerializable(stream, kGZIID_cIGZSerializable, &raw, false);
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    if (!serializable) return nullptr;
    void* plain = nullptr;
    cSC4Lot* lot = serializable->GetGZCLSID() == kGZCLSID_cSC4Lot && serializable->QueryInterface(kGZIID_PlainObject, &plain)
                       ? static_cast<cSC4Lot*>(plain) : nullptr;
    serializable->Release();   // the stream's object table keeps the lot's serializable while the city loads
    return lot;
}

// ---------------------------------------------------------------------------------------------- trips

bool cSC4TripData::Write(cIGZOStream& stream) const {
    stream.SetUint32(2);
    stream << route;
    stream << routeReversed;
    stream.SetUint32(count);
    stream.SetUint8(type);
    stream.SetUint8(index);
    stream.SetUint8(flags);
    stream.SetUint8(kind);
    stream.SetSint16(sourceX);
    stream.SetSint16(sourceZ);
    stream.SetFloat32(distance);
    stream.SetUint32(extra);
    return !stream.GetError();
}

bool cSC4TripData::Read(cIGZIStream& stream) {
    std::uint32_t version = 0;
    stream.GetUint32(version);
    stream >> route;
    stream >> routeReversed;
    stream.GetUint32(count);
    stream.GetUint8(type);
    stream.GetUint8(index);
    stream.GetUint8(flags);
    stream.GetUint8(kind);
    stream.GetSint16(sourceX);
    stream.GetSint16(sourceZ);
    stream.GetFloat32(distance);
    if (version < 2) extra = 0; else stream.GetUint32(extra);
    return !stream.GetError();
}

// ---------------------------------------------------------------------------------------- construction

cSC4Lot::cSC4Lot(int x0, int z0, int sizeX, int sizeZ, int facing) {
    const std::uint8_t x1 = static_cast<std::uint8_t>(x0 - 1 + sizeX);
    const std::uint8_t z1 = static_cast<std::uint8_t>(z0 - 1 + sizeZ);
    rect_[0] = static_cast<std::uint8_t>(x0);
    rect_[1] = static_cast<std::uint8_t>(z0);
    rect_[2] = x1;
    rect_[3] = z1;
    facing_ = static_cast<std::uint8_t>(facing);
    std::uint8_t cx = x1, cz = static_cast<std::uint8_t>(z0);
    std::uint8_t sx = static_cast<std::uint8_t>(sizeX), sz = static_cast<std::uint8_t>(sizeZ);
    switch (facing & 0xff) {
    case 0: break;
    case 1: cz = z1; std::swap(sx, sz); break;
    case 2: cx = static_cast<std::uint8_t>(x0); cz = z1; break;
    case 3: cx = static_cast<std::uint8_t>(x0); std::swap(sx, sz); break;
    default: cx = 0; cz = 0; sx = sz = 0; break;       // size stays zero for an invalid facing
    }
    cellX_ = cx; cellZ_ = cz;
    sizeX_ = sx; sizeZ_ = sz;
    if (g_lotHost) zoneType_ = g_lotHost->ZoneTypeAt(cellX_, cellZ_);
}

// The copy keeps geometry, state, populations and the configuration but not the road-access cache,
// trips, unit or travel designate (used for message snapshots).
cSC4Lot::cSC4Lot(const cSC4Lot& o)
    : properties_(o.properties_) {
    std::copy(o.rect_, o.rect_ + 4, rect_);
    altitude_ = o.altitude_;
    slope_ = o.slope_;
    slopeAtConstruction_ = o.slopeAtConstruction_;
    savedConfigurationId_ = o.savedConfigurationId_;
    configuration_ = o.configuration_;
    if (configuration_) configuration_->AddRef();
    lastStateChange_ = o.lastStateChange_;
    buildingType_ = o.buildingType_;
    populations_ = o.populations_;
    travelPopulations_ = o.travelPopulations_;
    std::copy(o.jobs_, o.jobs_ + 3, jobs_);
    initialized_ = o.initialized_;
    flags_ = o.flags_;
    cellX_ = o.cellX_; cellZ_ = o.cellZ_;
    sizeX_ = o.sizeX_; sizeZ_ = o.sizeZ_;
    facing_ = o.facing_;
    state_ = o.state_;
    condition_ = o.condition_;
    zoneType_ = o.zoneType_;
    wealth_ = o.wealth_;
    vacateReason_ = o.vacateReason_;
    randomPercent_ = o.randomPercent_;
}

cSC4Lot::~cSC4Lot() {
    if (unit_) unit_->Release();
    if (travelDesignate_) travelDesignate_->Release();
    if (configuration_) configuration_->Release();
}

std::uint32_t cSC4Lot::Release() {
    if (refCount_ < 2) {
        Shutdown();
        delete this;
        return 0;
    }
    return --refCount_;
}

bool cSC4Lot::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != kGZIID_cISC4TrafficSource && iid != 1 && iid != kGZIID_cISC4Lot) return false;
    *out = this;
    AddRef();
    return true;
}

bool cSC4Lot::Init() {
    if (!initialized_) {
        jobs_[0] = jobs_[1] = jobs_[2] = 0;
        float center[3];
        GetCenter(center);
        SetAltitude(g_lotHost ? g_lotHost->TerrainAltitude(center[0], center[2]) : 0.0f);
        initialized_ = true;
    }
    if (randomPercent_ == 0xff) randomPercent_ = static_cast<std::uint8_t>(g_lotHost ? g_lotHost->RandomPercent() : 0);
    if (configuration_) {
        configuration_->Release();
        configuration_ = nullptr;
    }
    if (savedConfigurationId_ != 0 && g_lotHost) {
        cISC4LotConfigurationRef* cfg = g_lotHost->LotConfigurationById(savedConfigurationId_);
        if (cfg) cfg->AddRef();
        configuration_ = cfg;
    }
    savedConfigurationId_ = 0;
    return true;
}

bool cSC4Lot::Shutdown() {
    if (initialized_) {
        initialized_ = false;
        populations_.clear();
        travelPopulations_.clear();
        if (configuration_) { configuration_->Release(); configuration_ = nullptr; }
        if (travelDesignate_) { travelDesignate_->Release(); travelDesignate_ = nullptr; }
        if (unit_) unit_->Remove(this);   // the unit clears the lot's unit

    }
    return true;
}

void cSC4Lot::PostCityInit() {
    const std::uint8_t saved = flags_;
    flags_ = 0;
    SetFlags(saved);
}

void cSC4Lot::SetFlags(int flags) {
    const std::uint8_t old = flags_;
    flags_ = static_cast<std::uint8_t>(flags);
    if (((old ^ flags) & 1) == 0 || !g_lotHost) return;
    const bool on = (flags_ & 1) != 0;
    for (int x = rect_[0]; x <= rect_[2]; ++x)
        for (int z = rect_[1]; z <= rect_[3]; ++z) g_lotHost->SetLotHighlight(x, z, on, altitude_);
}

void cSC4Lot::GetBoundingRect(std::int32_t rect[4]) const {
    for (int i = 0; i < 4; ++i) rect[i] = rect_[i];
}

void cSC4Lot::GetSourceCells(std::int32_t out[4]) const { GetBoundingRect(out); }

void cSC4Lot::GetCenter(float* xyz) const {
    xyz[0] = (static_cast<float>(rect_[2] * 0x10 + 0x10) + static_cast<float>(static_cast<unsigned>(rect_[0]) << 4)) * 0.5f;
    xyz[1] = altitude_;
    xyz[2] = (static_cast<float>(rect_[3] * 0x10 + 0x10) + static_cast<float>(static_cast<unsigned>(rect_[1]) << 4)) * 0.5f;
}

void cSC4Lot::SetAltitude(float altitude) {
    if (std::fabs(altitude - altitude_) > 0.001) {
        altitude_ = altitude;
        slope_ = slopeAtConstruction_ = g_lotHost ? g_lotHost->SlopeOf(*this) : 0.0f;
    }
}

void cSC4Lot::UpdateSlope() { slope_ = g_lotHost ? g_lotHost->SlopeOf(*this) : 0.0f; }

std::int32_t cSC4Lot::GetDaysSinceLastStateChange() const {
    if (lastStateChange_ == 0 || !g_lotHost) return 0;
    return g_lotHost->SimulatorDay() - static_cast<std::int32_t>(lastStateChange_);
}

void cSC4Lot::UpdateZoneType() { if (g_lotHost) zoneType_ = g_lotHost->ZoneTypeAt(cellX_, cellZ_); }

std::int64_t cSC4Lot::GetLandValue() const {
    std::int64_t total = 0;
    if (!g_lotHost) return 0;
    for (int a = rect_[1]; a <= rect_[3]; ++a)
        for (int b = rect_[0]; b <= rect_[2]; ++b) total += g_lotHost->LandValueAt(a, b);
    return total;
}

// ------------------------------------------------------------------------------------------ building

std::uint32_t cSC4Lot::GetBuildingType(bool fromConfiguration) const {
    if (buildingType_ == 0 && fromConfiguration && configuration_ && state_ != kLotStateDemolishable)
        return configuration_->GetID();
    return buildingType_;
}

cIGZUnknown* cSC4Lot::GetBuilding() const {
    if (buildingType_ == 0 || !g_lotHost) return nullptr;
    return g_lotHost->FindBuildingOccupant(*this);
}

void cSC4Lot::GetLotOccupants(std::vector<cIGZUnknown*>& out) const {
    if (g_lotHost) g_lotHost->GetLotOccupants(*this, out);
}

void cSC4Lot::SetLotConfiguration(cISC4LotConfigurationRef* configuration) {
    cISC4LotConfigurationRef* old = configuration_;
    if (old == configuration) return;
    if (configuration) configuration->AddRef();
    configuration_ = configuration;
    if (old) old->Release();
}

void cSC4Lot::SetUnit(cISC4BusinessUnit* unit) {
    if (unit_ == unit) return;
    if (unit_) unit_->Release();
    unit_ = unit;
    if (unit) unit->AddRef();
}

void cSC4Lot::SetTravelDesignate(cSC4Lot* designate) {
    cSC4Lot* old = travelDesignate_;
    if (old == designate) return;
    if (designate) designate->AddRef();
    travelDesignate_ = designate;
    if (old) old->Release();
}

std::uint32_t cSC4Lot::DeveloperTypeFor(std::uint32_t purpose, std::int32_t wealth) const {
    return g_lotHost ? g_lotHost->DeveloperType(purpose, wealth) : 0;
}

// -------------------------------------------------------------------------------------------- state

void cSC4Lot::SetState(std::uint32_t state, std::uint32_t wealthArg, std::uint8_t vacateReason) {
    const std::uint8_t old = state_;
    if (old == state || !g_lotHost) {
        if (old == state) return;
    }
    state_ = static_cast<std::uint8_t>(state);
    ILotHost* host = g_lotHost;
    if (host) lastStateChange_ = static_cast<std::uint32_t>(host->SimulatorDay());

    if (old == kLotStateOccupied && buildingType_ != 0 && host) {
        const LotBuildingProfile* profile = host->BuildingProfile(buildingType_);
        if (profile && profile->purpose != 0) {
            host->OnDeveloperLotAbandoned(static_cast<std::uint16_t>(profile->capacity));
            if (state_ == kLotStateLivable) {
                const std::uint32_t devType = host->DeveloperType(profile->purpose, profile->wealth);
                SetPopulation(devType, GetCapacity(devType, true));
            }
        }
    }
    if (state_ == kLotStateAbandoned) {
        vacateReason_ = vacateReason;
    } else if (state_ == kLotStateOccupied) {
        const std::uint32_t type = GetBuildingType(true);
        const LotBuildingProfile* profile = type && host ? host->BuildingProfile(type) : nullptr;
        if (profile) {
            populations_.clear();
            for (const auto& m : profile->capacities) {
                populations_.emplace_back();
                for (const auto& kv : m) populations_.back()[kv.first] = 0;
            }
        }
    }
    if ((state_ & 0xfe) != 2)
        for (auto& m : populations_)
            for (auto& kv : m) kv.second = 0;

    const std::uint8_t oldWealth = wealth_;
    UpdatePrimaryOccupantWealth();
    const bool sameCategory = ((state_ & 0xfe) == 2) == ((old & 0xfe) == 2);
    std::uint32_t reported = oldWealth;
    if (sameCategory) {
        reported = wealth_;
    } else if ((state_ & 0xfe) == 2) {
        reported = wealth_;
        if (wealthArg != 0) {
            wealth_ = static_cast<std::uint8_t>(wealthArg);
            reported = wealthArg;
        }
    }
    UpdateNeedsPower();
    if (host) {
        cSC4Lot snapshot(*this);
        host->PostLotMessage(kMessageLotStateChanged, snapshot, old, state_, buildingType_,
                             static_cast<std::int32_t>(reported & 0xff));
    }
}

void cSC4Lot::SetCondition(std::uint32_t condition) {
    const std::uint8_t old = condition_;
    if (old == condition) return;
    condition_ = static_cast<std::uint8_t>(condition);
    if (state_ == kLotStateAbandoned && g_lotHost) lastStateChange_ = static_cast<std::uint32_t>(g_lotHost->SimulatorDay());
    if (g_lotHost) {
        cSC4Lot snapshot(*this);
        g_lotHost->PostLotMessage(kMessageLotConditionChanged, snapshot, old, condition_, buildingType_, 0);
    }
}

void cSC4Lot::UpdatePrimaryOccupantWealth() {
    const std::uint32_t type = GetBuildingType(false);
    if (type == 0) { wealth_ = 0; return; }
    if (!g_lotHost) return;
    const LotBuildingProfile* profile = g_lotHost->BuildingProfile(type);
    if (!profile || profile->purpose == 0) return;
    std::map<std::uint32_t, float> occupancy;
    GetOccupancyByType(occupancy);
    bool keep = false;
    if (wealth_ != 0)
        keep = occupancy.count(DeveloperTypeFor(profile->purpose, wealth_)) != 0;
    if (!keep) wealth_ = static_cast<std::uint8_t>(profile->wealth);
    if (profile->wealth > 0) {
        float best = 0.0f;
        for (int level = profile->wealth; level >= 1; --level) {
            auto it = occupancy.find(DeveloperTypeFor(profile->purpose, level));
            if (it != occupancy.end() && best < it->second) {
                wealth_ = static_cast<std::uint8_t>(level);
                best = it->second;
            }
        }
    }
}

void cSC4Lot::UpdateNeedsPower() {
    bool needs = false;
    if (static_cast<std::uint8_t>(state_ - 3) < 2 && g_lotHost) {
        const std::uint32_t type = GetBuildingType(false);
        if (type != 0) {
            cIGZUnknown* building = GetBuilding();
            const LotBuildingProfile* profile = g_lotHost->BuildingProfile(type);
            bool bVar1 = false;
            bool decided = false;
            if (!profile || profile->purpose == 0) {
                bVar1 = true;
            } else {
                bool available = false;
                const bool exceeded = g_lotHost->PowerRequirementExceeded(*this, *profile, available);
                if (!available) decided = true;           // no sim record / configuration -> false
                else bVar1 = exceeded;
            }
            if (!decided && building != nullptr && bVar1) {
                std::int32_t value = 0;
                if (!g_lotHost->ExemplarProperty(*this, 0x27812852, value)) bVar1 = true;
                else if (value != 0) bVar1 = false;
                if (g_lotHost->ExemplarProperty(*this, 0x27812854, value) && value != 0) needs = bVar1;
                else needs = false;
            } else if (!decided && building == nullptr) {
                needs = bVar1;                            // quirk: no building, requirement stays as computed
            }
            // (when bVar1 is false the original skips straight to the result, which is false)
        }
    }
    SetFlagBit(kLotFlagNeedsPower, needs);
}

// ---------------------------------------------------------------------------------------- populations

std::uint16_t cSC4Lot::GetPopulation(std::uint32_t developerType) const {
    std::uint16_t total = 0;
    for (const auto& m : populations_)
        if (const auto* v = FindExact(m, developerType)) total = static_cast<std::uint16_t>(total + *v);
    return total;
}

std::uint16_t cSC4Lot::GetTravelPopulation(std::uint32_t developerType) const {
    const auto* v = FindExact(travelPopulations_, developerType);
    return v ? *v : 0;
}

std::uint16_t cSC4Lot::GetCapacityForWealth(std::uint32_t wealth) const {
    const std::uint32_t type = GetBuildingType(true);
    if (type == 0 || !g_lotHost) return 0;
    const LotBuildingProfile* profile = g_lotHost->BuildingProfile(type);
    if (!profile) return 0;
    return GetCapacity(g_lotHost->DeveloperType(profile->purpose, static_cast<std::int32_t>(wealth)), false);
}

std::uint16_t cSC4Lot::GetCapacity(std::uint32_t developerType, bool ignoreState) const {
    if ((ignoreState || (state_ & 0xfe) == 2) && g_lotHost) {
        const std::uint32_t type = GetBuildingType(true);
        if (type != 0) {
            const LotBuildingProfile* profile = g_lotHost->BuildingProfile(type);
            if (profile)
                for (const auto& m : profile->capacities)
                    if (const auto* v = FindExact(m, developerType)) return *v;
        }
    }
    return 0;
}

std::uint16_t cSC4Lot::GetAvailableCapacity(std::uint32_t developerType, bool includeAbandoned) const {
    if (!g_lotHost) return 0;
    if (!includeAbandoned) { if (state_ != kLotStateLivable) return 0; }
    else if (static_cast<std::uint8_t>(state_ - 3) > 1) return 0;
    const std::uint32_t type = GetBuildingType(true);
    const LotBuildingProfile* profile = g_lotHost->BuildingProfile(type);
    if (!profile) return 0;
    for (std::size_t i = 0; i < profile->capacities.size(); ++i) {
        const auto& caps = profile->capacities[i];
        const auto* own = FindExact(caps, developerType);
        if (!own) continue;
        const float capacity = static_cast<float>(*own);
        float fraction = 1.0f;
        for (const auto& kv : caps) {
            if (kv.first == developerType || i >= populations_.size()) continue;
            if (const auto* pop = FindExact(populations_[i], kv.first))
                fraction -= static_cast<float>(*pop) / static_cast<float>(kv.second);
        }
        if (fraction == 1.0f) return static_cast<std::uint16_t>(static_cast<std::uint32_t>(capacity) & 0xffff);
        if (profile->allowPartialCapacity)
            return static_cast<std::uint16_t>(static_cast<std::uint32_t>(std::lrint(fraction * capacity)) & 0xffff);
        return 0;
    }
    return 0;
}

bool cSC4Lot::SetPopulation(std::uint32_t developerType, std::uint16_t population) {
    const std::uint16_t available = GetAvailableCapacity(developerType, false);
    if (available < population) population = available;
    if (!g_lotHost) return false;
    const LotBuildingProfile* profile = g_lotHost->BuildingProfile(GetBuildingType(true));
    if (!profile) return false;
    for (std::size_t i = 0; i < profile->capacities.size(); ++i) {
        if (profile->capacities[i].count(developerType) == 0) continue;
        if (i >= populations_.size()) populations_.resize(i + 1);
        populations_[i][developerType] = population;
        UpdatePrimaryOccupantWealth();
        return true;
    }
    return false;
}

float cSC4Lot::GetOccupancy() const {
    const std::uint32_t type = GetBuildingType(true);
    if (type == 0 || !g_lotHost) return 0.0f;
    const LotBuildingProfile* profile = g_lotHost->BuildingProfile(type);
    if (!profile || profile->capacities.empty()) return 0.0f;
    float weighted = 0.0f;
    int totalCapacity = 0;
    for (std::size_t i = 0; i < profile->capacities.size(); ++i) {
        float fraction = 0.0f;
        int capacity = 0, count = 0;
        for (const auto& kv : profile->capacities[i]) {
            std::uint16_t pop = 0;
            if (i < populations_.size())
                if (const auto* p = FindExact(populations_[i], kv.first)) pop = *p;
            fraction += static_cast<float>(pop) / static_cast<float>(kv.second);
            capacity += kv.second;
            ++count;
        }
        const int perLevel = count ? capacity / count : 0;
        totalCapacity += perLevel;
        weighted = static_cast<float>(static_cast<int>(weighted + static_cast<float>(perLevel) * fraction));
    }
    return totalCapacity > 0 ? weighted / static_cast<float>(totalCapacity) : 0.0f;
}

void cSC4Lot::GetOccupancyByType(std::map<std::uint32_t, float>& out) const {
    out.clear();
    const std::uint32_t type = GetBuildingType(true);
    if (!g_lotHost) return;
    const LotBuildingProfile* profile = g_lotHost->BuildingProfile(type);
    if (!profile || profile->capacities.empty()) return;
    std::vector<int> average(profile->capacities.size(), 0);
    int sum = 0;
    for (std::size_t i = 0; i < profile->capacities.size(); ++i) {
        int count = 0;
        for (const auto& kv : profile->capacities[i]) {
            average[i] += kv.second;
            ++count;
        }
        average[i] = count ? average[i] / count : 0;
        sum += average[i];
    }
    if (sum <= 0) return;
    for (std::size_t i = 0; i < profile->capacities.size(); ++i)
        for (const auto& kv : profile->capacities[i]) {
            std::uint16_t pop = 0;
            if (i < populations_.size())
                if (const auto* p = FindExact(populations_[i], kv.first)) pop = *p;
            const float share = (static_cast<float>(average[i]) / static_cast<float>(sum)) *
                                (static_cast<float>(pop) / static_cast<float>(kv.second));
            out[kv.first] += share;
        }
}

// ------------------------------------------------------------------------------------------ travel

std::int32_t cSC4Lot::GetTravelJobs(int wealth) const {
    switch (wealth) {
    case 1: return static_cast<std::int32_t>(jobs_[0]);
    case 2: return static_cast<std::int32_t>(jobs_[1]);
    case 3: return static_cast<std::int32_t>(jobs_[2]);
    default: return 0;
    }
}

void cSC4Lot::ResetTravelPopulation() {
    travelPopulations_.clear();
    jobs_[0] = jobs_[1] = jobs_[2] = 0;
    UpdateJobs();
}

void cSC4Lot::UpdateJobs() {
    ILotHost* host = g_lotHost;
    if (!host) return;
    for (int kind = 0; kind < 3; ++kind) {
        const std::int32_t wanted = static_cast<std::int32_t>(std::nearbyint(jobs_[kind]));
        std::int32_t current = host->TrafficJobs(*this, 0);       // the original always reads kind 0
        const std::int32_t diff = wanted - current;
        if (diff == 0) continue;
        if (current <= wanted) current = wanted;
        if (0.1f < static_cast<float>(std::abs(diff)) / static_cast<float>(current))
            host->SetTrafficJobs(*this, kind, wanted);
    }
}

void cSC4Lot::AddTravelPopulation(const cSC4Lot& other) {
    for (const auto& m : other.populations_)
        for (const auto& kv : m) {
            std::uint16_t& slot = travelPopulations_[kv.first];
            slot = static_cast<std::uint16_t>(kv.second + slot);
        }
    float jobs[4];
    other.GetJobs(jobs);
    jobs_[0] += jobs[1];
    jobs_[1] += jobs[2];
    jobs_[2] += jobs[3];
    UpdateJobs();
}

void cSC4Lot::GetJobs(float* jobs) const {
    jobs[1] = jobs[2] = jobs[3] = 0;
    if (!g_lotHost) return;
    const std::uint32_t type = GetBuildingType(true);
    if (type != 0) {
        const LotBuildingProfile* profile = g_lotHost->BuildingProfile(type);
        if (profile && profile->purpose != 0) {
            const std::uint32_t dev = g_lotHost->DeveloperType(profile->purpose, wealth_);
            const float pop = static_cast<float>(GetPopulation(dev));
            g_lotHost->JobsPerOccupant(jobs, static_cast<int>(dev) + 1);
            jobs[1] *= pop; jobs[2] *= pop; jobs[3] *= pop;
            return;
        }
    }
    if (GetBuilding()) g_lotHost->ExemplarJobs(*this, jobs);
}

bool cSC4Lot::HasJobAccess() const {
    if (!g_lotHost) return false;
    for (int kind = 0; kind < 2; ++kind)
        if (g_lotHost->TrafficHasJobAccess(*this, kind)) return true;
    return false;
}

void cSC4Lot::UpdateTrafficVolume() {
    ILotHost* host = g_lotHost;
    if (!host) return;
    const int zoneClass = zoneType_;
    std::uint16_t result[3] = {0, 0, 0};
    std::uint8_t kind = 0;
    std::uint32_t total = 0;
    bool havePlan = false;
    if (static_cast<unsigned>(zoneClass - 7) < 3) {
        if (!HasRoadAccess() || travelDesignate_) return;
        kind = 3;
        if (IsInLivableState()) {
            const float volume = host->TravelVolumeFactor();
            const float sum = static_cast<float>(GetTravelPopulation(0x4100) + GetTravelPopulation(0x4200) +
                                                 GetTravelPopulation(0x4300) + GetTravelPopulation(0x4400)) * volume;
            total = static_cast<std::uint32_t>(std::ceil(sum));
            result[2] = static_cast<std::uint16_t>(total);
            havePlan = true;
        }
    } else if (static_cast<unsigned>(zoneClass - 1) < 3) {
        if (IsInLivableState()) {
            const float factor = host->ResidentialTravelFactor(cellX_, cellZ_);
            std::uint16_t pop = GetTravelPopulation(0x1010);
            std::uint8_t sourceKind = 1;
            std::uint8_t trafficKind = 0;
            if (pop == 0) {
                pop = GetTravelPopulation(0x1020);
                if (pop != 0) { sourceKind = 2; trafficKind = 1; }
                else {
                    pop = GetTravelPopulation(0x1030);
                    if (pop != 0) { sourceKind = 3; trafficKind = 2; }
                    else { sourceKind = 0; }
                }
            }
            if (pop != 0) {
                std::uint32_t amount = static_cast<std::uint32_t>(std::floor(factor * static_cast<float>(pop)));
                if (amount == 0) amount = 1;
                total = amount & 0xffff;
                float dist[3] = {0, 0, 0};
                host->TravelDistribution(sourceKind, dist);
                if ((amount & 0xfffe) < 0x32) {
                    std::uint32_t acc = 1;
                    for (int i = 0; i < 3; ++i) {
                        acc = (acc & 0xff) + static_cast<std::uint32_t>(std::lrint(dist[i] * 100.0f));
                        if (randomPercent_ < (acc & 0xff)) { result[i] = static_cast<std::uint16_t>(total); break; }
                    }
                } else {
                    for (int i = 0; i < 3; ++i)
                        result[i] = static_cast<std::uint16_t>(std::lrint(dist[i] * static_cast<float>(amount & 0xffff)));
                }
                kind = trafficKind;
                havePlan = true;
            } else {
                kind = 0;
            }
        } else {
            kind = 0;
        }
    } else {
        return;
    }

    if (havePlan && static_cast<std::int16_t>(total) != 0) {
        for (auto it = trips_.begin(); it != trips_.end();) it = (it->flags & 2) ? trips_.erase(it) : std::next(it);
        for (int idx = 0; idx < 3; ++idx) {
            const std::uint32_t target = result[idx];
            std::uint32_t count = 0;
            for (auto it = trips_.begin(); it != trips_.end();) {
                if (it->type == kind) {
                    if (it->index == idx) count += it->count;
                    ++it;
                } else {
                    it = trips_.erase(it);
                }
            }
            if (count < target) {
                std::uint32_t need = (target - count) & 0xffff;
                bool matched = false;
                for (auto& trip : trips_) {
                    if (static_cast<std::uint16_t>(need) == 0) break;
                    if (trip.index == idx) {
                        matched = true;
                        if ((trip.kind & 0xfe) == 4) { trip.count += need; need = 0; }
                    }
                }
                if (static_cast<std::uint16_t>(need) != 0) {
                    cSC4TripData trip;
                    trip.count = need;
                    trip.type = kind;
                    trip.index = static_cast<std::uint8_t>(idx);
                    trip.kind = matched ? 4 : 0;
                    trips_.push_back(std::move(trip));
                }
            } else if (count > target) {
                std::uint32_t need = (count - target) & 0xffff;
                for (auto it = trips_.begin(); it != trips_.end() && static_cast<std::uint16_t>(need) != 0;) {
                    if (it->index == idx && it->kind == 4) {
                        if (it->count <= need) { need -= it->count; it = trips_.erase(it); continue; }
                        it->count -= need;
                        need = 0;
                        break;
                    }
                    ++it;
                }
                for (auto it = trips_.begin(); it != trips_.end() && static_cast<std::uint16_t>(need) != 0;) {
                    if (it->index == idx) {
                        if (need <= it->count && it->count - need != 0) { it->count -= need; need = 0; break; }
                        need -= it->count;
                        it = trips_.erase(it);
                        continue;
                    }
                    ++it;
                }
            }
        }
        return;
    }
    if (trips_.empty() || !(trips_.front().flags & 2)) {
        trips_.clear();
        cSC4TripData placeholder;
        placeholder.count = 1;
        placeholder.type = kind;
        placeholder.flags = 2;
        trips_.push_front(std::move(placeholder));
    }
}

// ------------------------------------------------------------------------------------- road access

bool cSC4Lot::CalculateFerryTerminalRoadAccess() const {
    return g_lotHost && g_lotHost->FerryTerminalRoadAccess(*this);
}

bool cSC4Lot::CalculateRoadAccess() const {
    if (g_lotHost && g_lotHost->IsFerryTerminal(*this)) return CalculateFerryTerminalRoadAccess();
    int first = 0, last = 3;
    if (static_cast<unsigned>(zoneType_ - 1) < 6) first = last = facing_;
    bool access = false;
    for (int rotation = first; rotation <= last && !access; ++rotation)
        access = g_lotHost && g_lotHost->RoadAccessCells(*this, rotation);
    roadAccess_[0] = access;
    return access;
}

bool cSC4Lot::CalculateOrganicRoadAccess(std::uint32_t purpose) const {
    const std::int32_t key = static_cast<std::int32_t>(purpose);
    if (HasOrganicRoadAccess(0)) {
        roadAccess_[key] = true;
        return true;
    }
    roadAccess_[key] = false;
    bool found = false;
    if (g_lotHost) {
        std::vector<cSC4Lot*> neighbors;
        for (int direction : {1, 2, 4, 8}) g_lotHost->NeighborLots(*this, direction, neighbors);
        for (cSC4Lot* neighbor : neighbors) {
            if (found) continue;
            if (static_cast<unsigned>(neighbor->GetState() - 2) < 3) {
                const LotBuildingProfile* profile = g_lotHost->BuildingProfile(neighbor->GetBuildingType(true));
                // the neighbour's own organic access: it chains field to field (this lot's entry is already false, so a loop ends)
                if (profile && profile->purpose == purpose) found = neighbor->HasOrganicRoadAccess(purpose);
            }
        }
    }
    roadAccess_[key] = found;
    return found;
}

bool cSC4Lot::HasOrganicRoadAccess(std::uint32_t purpose) const {
    bool plain;
    auto cached = roadAccess_.find(0);
    plain = cached != roadAccess_.end() ? cached->second : CalculateRoadAccess();
    if (plain) return true;
    if (purpose == 0) return false;
    auto it = roadAccess_.find(static_cast<std::int32_t>(purpose));
    if (it != roadAccess_.end()) return it->second;
    return CalculateOrganicRoadAccess(purpose);
}

void cSC4Lot::UpdateRoadAccess() {
    CalculateRoadAccess();
    std::vector<std::int32_t> keys;
    for (const auto& kv : roadAccess_) if (kv.first != 0) keys.push_back(kv.first);
    for (std::int32_t key : keys) CalculateOrganicRoadAccess(static_cast<std::uint32_t>(key));
}

// ----------------------------------------------------------------------------------- persistence

bool cSC4Lot::Write(cIGZOStream& s) const {
    s.SetUint16(8);
    s.SetUint32(configuration_ ? configuration_->GetID() : 0);
    s.SetUint8(flags_);
    for (int i = 0; i < 4; ++i) s.SetUint8(rect_[i]);
    s.SetUint8(cellX_);
    s.SetUint8(cellZ_);
    s.SetFloat32(altitude_);
    s.SetFloat32(slopeAtConstruction_);
    s.SetFloat32(slope_);
    s.SetUint8(sizeX_);
    s.SetUint8(sizeZ_);
    s.SetUint8(facing_);
    s.SetUint8(state_);
    s.SetUint8(condition_);
    s.SetUint8(zoneType_);
    s.SetUint8(wealth_);
    s.SetUint32(lastStateChange_);
    s.SetUint32(buildingType_);
    s.SetUint8(vacateReason_);
    if (g_lotHost) {
        g_lotHost->WriteSerializable(s, unit_);
        WriteLotReference(s, travelDesignate_);
    }
    s.SetUint8(static_cast<std::uint8_t>(populations_.size()));
    for (const auto& m : populations_) {
        s.SetUint8(static_cast<std::uint8_t>(m.size()));
        for (const auto& kv : m) {
            s.SetUint32(kv.first);
            s.SetUint16(kv.second);
        }
    }
    s.SetUint8(static_cast<std::uint8_t>(travelPopulations_.size()));
    for (const auto& kv : travelPopulations_) {
        s.SetUint32(kv.first);
        s.SetUint16(kv.second);
    }
    for (float j : jobs_) s.SetFloat32(j);
    properties_.Write(s);
    s.SetUint32(static_cast<std::uint32_t>(trips_.size()));
    for (const auto& trip : trips_)
        if (!trip.Write(s)) return false;
    s.SetUint8(randomPercent_);
    return !s.GetError();
}

bool cSC4Lot::Read(cIGZIStream& s) {
    initialized_ = true;
    std::uint16_t version = 0;
    s.GetUint16(version);
    if (version >= 9) return false;
    s.GetUint32(savedConfigurationId_);
    s.GetUint8(flags_);
    auto readByteField = [&](std::uint8_t& out) {
        if (version < 6) { std::int32_t v = 0; s.GetSint32(v); out = static_cast<std::uint8_t>(v); }
        else s.GetUint8(out);
    };
    if (version < 6) {
        std::int32_t a = 0, b = 0, c = 0, d = 0;
        s.GetSint32(a); s.GetSint32(b); s.GetSint32(c); s.GetSint32(d);
        rect_[0] = static_cast<std::uint8_t>(a);
        rect_[2] = static_cast<std::uint8_t>(c);
        rect_[1] = static_cast<std::uint8_t>(b);
        rect_[3] = static_cast<std::uint8_t>(d);
    } else {
        for (int i = 0; i < 4; ++i) s.GetUint8(rect_[i]);
    }
    readByteField(cellX_);
    readByteField(cellZ_);
    s.GetFloat32(altitude_);
    s.GetFloat32(slopeAtConstruction_);
    s.GetFloat32(slope_);
    readByteField(sizeX_);
    readByteField(sizeZ_);
    s.GetUint8(facing_);
    s.GetUint8(state_);
    s.GetUint8(condition_);
    s.GetUint8(zoneType_);
    s.GetUint8(wealth_);
    if (version < 6) { bool locked = false; s >> locked; SetVerticesAreLocked(locked); }
    s.GetUint32(lastStateChange_);
    s.GetUint32(buildingType_);
    if (version < 6) {
        bool needsPower = false, hasPower = false, historical = false;
        s >> needsPower; SetFlagBit(kLotFlagNeedsPower, needsPower);
        s >> hasPower; SetHasPower(hasPower);
        s >> historical; SetHistorical(historical);
    }
    s.GetUint8(vacateReason_);
    if (g_lotHost) {
        if (unit_) { unit_->Release(); unit_ = nullptr; }
        unit_ = static_cast<cISC4BusinessUnit*>(g_lotHost->ReadSerializable(s, kGZIID_cISC4BusinessUnit));   // one reference
        SetTravelDesignate(ReadLotReference(s));
    }
    std::uint8_t count = 0;
    s.GetUint8(count);
    populations_.assign(count, {});
    for (std::uint8_t i = 0; i < count; ++i) {
        std::uint8_t n = 0;
        s.GetUint8(n);
        for (std::uint8_t j = 0; j < n; ++j) {
            std::uint32_t key = 0;
            std::uint16_t value = 0;
            s.GetUint32(key);
            s.GetUint16(value);
            populations_[i][key] = value;
        }
    }
    std::uint8_t n = 0;
    s.GetUint8(n);
    for (std::uint8_t j = 0; j < n; ++j) {
        std::uint32_t key = 0;
        std::uint16_t value = 0;
        s.GetUint32(key);
        s.GetUint16(value);
        travelPopulations_[key] = value;
    }
    for (float& j : jobs_) s.GetFloat32(j);
    properties_.Read(s);
    if (version > 6) {
        std::uint32_t trips = 0;
        s.GetUint32(trips);
        for (std::uint32_t i = 0; i < trips; ++i) {
            cSC4TripData trip;
            if (!trip.Read(s)) return false;
            trips_.push_back(std::move(trip));
        }
        if (version > 7) s.GetUint8(randomPercent_);
    }
    return !s.GetError();
}

}  // namespace sc4
