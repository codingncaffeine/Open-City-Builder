// cSC4TractDeveloper — see TractDeveloper.h for the layout, the three COM views and the list of quirks.
//
// Every call outside the object goes through ITractDeveloperHost (one method per original call, with
// its slot); cSC4Lot and cSC4LotConfiguration are used directly through the recovered methods of
// Lot.h / LotConfiguration.h. The comments name the original vtable slot of every such call.
//
// The growth pass itself (Grow's candidate collection and build loop, TakeOverAbandoned, the three
// ListCandidateLots_* passes, Build/BuildFarm/PickBuilding, the compatibility predicates, FindNetworks
// and UpdatePopulation) is PARTIAL: it needs cISC4GrowthDeveloper's growth/decline lists and the
// occupant-manager occupant interface, neither of which is recovered. Grow still performs its whole
// per-pass setup (the part that reads the growth developer and fills the attempt record), then stops;
// the reason is repeated on every partial function below.

#include "sc4/TractDeveloper.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <unordered_set>

#include "sc4/BuildingZoneFit.h"
#include "sc4/GrowthDeveloper.h"
#include "sc4/LotConfiguration.h"
#include "sc4/LotConfigurationManager.h"
#include "sc4/Occupant.h"

namespace sc4 {

namespace {

ITractDeveloperHost* g_host = nullptr;
bool g_staticInitialized = false;          // mbStaticInitialized @ 0x100b7b... (set by the first Init)

// The score weights of ScoreCandidateLots: three three-element float tables at 0x100a18e80,
// 0x100a18e8c and 0x100a18e98, indexed by purpose - 1 (residential, commercial, industrial).
constexpr float kNetworkWeight0[3] = {10.0f, 0.0f, -5.0f};
constexpr float kNetworkWeight1[3] = {2.0f, -5.0f, -1.0f};
constexpr float kNetworkWeight2[3] = {1.0f, -2.0f, -1.0f};

constexpr std::uint32_t kTractMessageStyleTick = 0x66956817;   // DoMessage's only message
constexpr std::int32_t kStyleTickMessageId1 = 6;               // simulator registration ids
constexpr std::int32_t kStyleTickMessageId2 = 7;
constexpr const char* kSimulatorName = "cSC4TractDeveloper";
constexpr std::int32_t kDefaultYearsBetweenStyleChanges = 0x28;
constexpr std::int32_t kDaysBeforeDecline = 0x3b;              // Decline: 3b < days since last change

// The class-wide candidate pool (sPool / DAT_100b225a8 at 0x100b225a8, capacity at 0x100b225b0) and
// the static default CandidateLot that NewCandidateLot copies over the popped object.
std::vector<TractCandidateLot*>& CandidatePool() {
    static std::vector<TractCandidateLot*> pool;
    return pool;
}

const TractCandidateLot& CandidateTemplate() {
    static const TractCandidateLot tmpl;
    return tmpl;
}

}  // namespace

// The cLotSizesFilter of the original (vtable 0x100b00378, 0x10013c4ca): the pass caches one per
// {wealth, purpose} pair and hands it to the lot configuration manager, which keeps the configurations
// whose purpose (+0x88) and wealth (+0x80) bits include those values.
class cSC4TractDeveloper::LotSizesFilter final : public cISC4LotConfigurationFilter {
public:
    LotSizesFilter(int wealth, int purpose) : wealth_(wealth), purpose_(purpose) {}
    // cISC4LotConfigurationFilter::Matches (the original's IsLotConfigurationIncluded, 0x10013c4ca).
    bool Matches(cSC4LotConfiguration* configuration) override {
        if (!configuration) return false;
        if (!configuration->IsCompatibleWithPurposeType(purpose_)) return false;
        return configuration->IsCompatibleWithWealthType(wealth_);
    }
    int Wealth() const { return wealth_; }
    int Purpose() const { return purpose_; }

private:
    int wealth_ = 0;      // +0x08
    int purpose_ = 0;     // +0x0c
};

ITractDeveloperHost* TractDeveloperHost() { return g_host; }
void SetTractDeveloperHost(ITractDeveloperHost* host) { g_host = host; }

//==================================================================================================
// construction / COM
//==================================================================================================

cSC4TractDeveloper::cSC4TractDeveloper() {
    // +0x18 initialised byte, +0x1c reference count and the ten service pointers are zeroed; +0x74 is
    // an unused int; the tunable floats and the style flags are the defaults the original stores.
    initialized_ = false;
    refCount_ = 0;
    messageServer_ = nullptr;
    city_ = nullptr;
    buildingDevelopment_ = nullptr;
    landValue_ = nullptr;
    lotConfigurationManager_ = nullptr;
    lotDeveloper_ = nullptr;
    lotManager_ = nullptr;
    plumbing_ = nullptr;
    power_ = nullptr;
    traffic_ = nullptr;
    zoneManager_ = nullptr;
    reserved74_ = 0;
    capacityFactor_ = 1.5f;          // 0x3fc00000
    declineFactor_ = 0.0f;
    buildBias_ = 1.2f;               // 0x3f99999a
    capacityFactor2_ = 1.5f;
    existingCapacityWeight_ = 1.0f;
    abandonedCapacityWeight_ = 0.2f; // 0x3e4ccccd
    aggregationPenalty_ = 10.0f;     // 0x41200000
    subdivisionPenalty_ = 5.0f;      // 0x40a00000
    populationJitter_ = 0.2f;
    declineJitterLow_ = 0.01f;       // 0x3c23d70a
    declineJitterHigh_ = 0.1f;       // 0x3dcccccd
    allowTakeOver_ = true;
    yearsBetweenStyleChanges_ = 0;
    singleStyle_ = 1;                // +0x1c0 = 1 (IsUsingAllStylesAtOnce() == false)
    styleIndex_ = 0;
    styleYears_ = 0;
}

cSC4TractDeveloper::~cSC4TractDeveloper() {
    // The original's destructor releases the containers only; it never releases the services (the
    // caller must have run Shutdown). The pool and the cached filters are freed here so a test or a
    // shutdown-less caller does not leak them (the original leaks the filters, see the header).
    for (TractCandidateLot* candidate : candidates_) delete candidate;
    candidates_.clear();
    ClearCandidatePool();
    for (auto& entry : filterCache_) delete entry.second;
    filterCache_.clear();
}

bool cSC4TractDeveloper::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    if (iid == 1 || iid == kGZIID_cISC4TractDeveloper) {
        *out = static_cast<cISC4TractDeveloper*>(this);
    } else if (iid == kGZIID_cIGZMessageTarget2) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else if (iid == kGZIID_cISC4TractDeveloperSerializable) {
        *out = static_cast<cIGZSerializable*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

std::uint32_t cSC4TractDeveloper::AddRef() { return ++refCount_; }

std::uint32_t cSC4TractDeveloper::Release() {
    if (refCount_ < 2) {
        Shutdown();
        delete this;
        return 0;
    }
    return --refCount_;
}

//==================================================================================================
// Init / Shutdown / tunables
//==================================================================================================

bool cSC4TractDeveloper::Init() {
    if (initialized_) return true;
    if (!g_staticInitialized) g_staticInitialized = true;
    ReadSimulatorTunableValues();
    ITractDeveloperHost* host = g_host;
    if (!host) return false;
    if (!city_) { city_ = host->City(); if (!city_) return false; city_->AddRef(); }
    if (!buildingDevelopment_) {
        buildingDevelopment_ = host->BuildingDevelopment();
        if (!buildingDevelopment_) return false;
        buildingDevelopment_->AddRef();
    }
    if (!landValue_) {
        landValue_ = host->LandValue();
        if (!landValue_) return false;
        landValue_->AddRef();
    }
    if (!lotDeveloper_) {
        lotDeveloper_ = host->LotDeveloper();
        if (!lotDeveloper_) return false;
        lotDeveloper_->AddRef();
    }
    if (!lotConfigurationManager_) {
        lotConfigurationManager_ = host->LotConfigurationManager();
        if (!lotConfigurationManager_) return false;
        lotConfigurationManager_->AddRef();
    }
    if (!lotManager_) {
        lotManager_ = host->LotManager();
        if (!lotManager_) return false;
        lotManager_->AddRef();
    }
    if (!plumbing_) {
        plumbing_ = host->Plumbing();
        if (!plumbing_) return false;
        plumbing_->AddRef();
    }
    if (!power_) {
        power_ = host->Power();
        if (!power_) return false;
        power_->AddRef();
    }
    if (!traffic_) {
        traffic_ = host->Traffic();
        if (!traffic_) return false;
        traffic_->AddRef();
    }
    if (!zoneManager_) {
        zoneManager_ = host->ZoneManager();
        if (!zoneManager_) return false;
        zoneManager_->AddRef();
    }
    // SL::spSimulator->vslot 0x98(this + 8, 6/7, "cSC4TractDeveloper", 2): the two messages the
    // developer ticks on. The registration is skipped when there is no simulator at all.
    if (host->SimulatorPresent()) {
        host->RegisterSimulatorCallback(kStyleTickMessageId1, static_cast<cIGZMessageTarget2*>(this),
                                        kSimulatorName);
        host->RegisterSimulatorCallback(kStyleTickMessageId2, static_cast<cIGZMessageTarget2*>(this),
                                        kSimulatorName);
    }
    if (!messageServer_) {
        messageServer_ = host->MessageServer();
        if (!messageServer_) return false;
        messageServer_->AddRef();
    }
    initialized_ = true;
    return true;
}

void cSC4TractDeveloper::ReadSimulatorTunableValues() {
    ITractDeveloperHost* host = g_host;
    if (!host) return;
    TractTunables tunables;
    tunables.capacityFactor = capacityFactor_;
    tunables.declineFactor = declineFactor_;
    tunables.buildBias = buildBias_;
    tunables.capacityFactor2 = capacityFactor2_;
    tunables.existingCapacityWeight = existingCapacityWeight_;
    tunables.abandonedCapacityWeight = abandonedCapacityWeight_;
    tunables.aggregationPenalty = aggregationPenalty_;
    tunables.subdivisionPenalty = subdivisionPenalty_;
    tunables.populationJitter = populationJitter_;
    tunables.declineJitterLow = declineJitterLow_;
    tunables.declineJitterHigh = declineJitterHigh_;
    tunables.allowTakeOver = allowTakeOver_;
    host->ReadTunables(tunables);   // resource manager -> exemplar (0xe7e2c2db 6534284a / 0xe8da7677)
    capacityFactor_ = tunables.capacityFactor;
    declineFactor_ = tunables.declineFactor;
    buildBias_ = tunables.buildBias;
    capacityFactor2_ = tunables.capacityFactor2;
    existingCapacityWeight_ = tunables.existingCapacityWeight;
    abandonedCapacityWeight_ = tunables.abandonedCapacityWeight;
    aggregationPenalty_ = tunables.aggregationPenalty;
    subdivisionPenalty_ = tunables.subdivisionPenalty;
    populationJitter_ = tunables.populationJitter;
    declineJitterLow_ = tunables.declineJitterLow;
    declineJitterHigh_ = tunables.declineJitterHigh;
    allowTakeOver_ = tunables.allowTakeOver;
    tunableStyles_ = tunables.styles;                 // +0xa8
    if (activeStyles_.empty()) {                      // +0x1a0 empty
        styleIndex_ = 0;
        activeStyles_ = tunableStyles_;
    }
    if (activeStyles_.size() <= static_cast<std::size_t>(styleIndex_)) styleIndex_ = 0;
    if (yearsBetweenStyleChanges_ == 0) {             // +0xc0 == 0
        yearsBetweenStyleChanges_ = kDefaultYearsBetweenStyleChanges;
        if (tunables.hasYearsBetweenStyleChanges) yearsBetweenStyleChanges_ = tunables.yearsBetweenStyleChanges;
    }
}

bool cSC4TractDeveloper::Shutdown() {
    if (!initialized_) return true;
    initialized_ = false;
    if (messageServer_) {
        messageServer_->Release();
        messageServer_ = nullptr;
    }
    if (city_) {
        // SL::spCity->vslot 0x190->vslot 0xa8(this + 8): the developer registry forgets this developer.
        city_->RemoveDeveloper(static_cast<cIGZMessageTarget2*>(this));
        city_->Release();
        city_ = nullptr;
    }
    if (buildingDevelopment_) { buildingDevelopment_->Release(); buildingDevelopment_ = nullptr; }
    if (landValue_) { landValue_->Release(); landValue_ = nullptr; }
    if (lotDeveloper_) { lotDeveloper_->Release(); lotDeveloper_ = nullptr; }
    if (lotConfigurationManager_) {
        for (auto& entry : filterCache_) {
            if (g_host) g_host->UnregisterLotConfigurationFilter(entry.second);
            delete entry.second;      // cLotSizesFilter::Release returns 1 without deleting (quirk)
        }
        filterCache_.clear();
        lotConfigurationManager_->Release();
        lotConfigurationManager_ = nullptr;
    }
    if (lotManager_) { lotManager_->Release(); lotManager_ = nullptr; }
    if (plumbing_) { plumbing_->Release(); plumbing_ = nullptr; }
    if (power_) { power_->Release(); power_ = nullptr; }
    if (traffic_) { traffic_->Release(); traffic_ = nullptr; }
    if (zoneManager_) { zoneManager_->Release(); zoneManager_ = nullptr; }
    FreeGrowthDatabase();
    for (TractCandidateLot* candidate : candidates_) delete candidate;
    candidates_.clear();
    ClearCandidatePool();
    return true;
}

//==================================================================================================
// stream
//==================================================================================================

bool cSC4TractDeveloper::Write(cIGZOStream& stream) const {
    stream.SetUint16(3);                                              // version
    stream.SetSint32(styleIndex_);                                    // +0x1b8
    stream.SetSint32(styleYears_);                                    // +0x1bc
    stream.SetUint8(singleStyle_);                                    // +0x1c0 (written as stored)
    stream.SetUint32(static_cast<std::uint32_t>(activeStyles_.size()));
    for (std::uint32_t style : activeStyles_) stream.SetUint32(style);
    stream.SetSint32(yearsBetweenStyleChanges_);                      // +0xc0
    return !stream.GetError();
}

bool cSC4TractDeveloper::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    if (!stream.GetUint16(version)) return false;
    if (version >= 4) return false;   // a future version is rejected (0x100135c32 returns 0)
    if (!stream.GetSint32(styleIndex_)) return false;
    if (!stream.GetSint32(styleYears_)) return false;
    if (version < 2) {
        // Version 1 did not store the style set: the three default styles are installed and the object
        // is put in single-style mode (the original sets +0x1c0 = 1).
        singleStyle_ = 1;
        activeStyles_.resize(3);
        activeStyles_[0] = 0x2000;
        activeStyles_[1] = 0x2001;
        activeStyles_[2] = 0x2002;
    } else {
        std::uint8_t flag = 0;
        if (!stream.GetUint8(flag)) return false;
        singleStyle_ = flag != 0;
        std::uint32_t count = 0;
        if (!stream.GetUint32(count)) return false;
        activeStyles_.resize(count);   // quirk: the stored count is not validated
        for (std::uint32_t i = 0; i < count; ++i) {
            if (!stream.GetUint32(activeStyles_[i])) return false;
        }
    }
    if (version > 2) {
        if (!stream.GetSint32(yearsBetweenStyleChanges_)) return false;
    }
    return !stream.GetError();
}

bool cSC4TractDeveloper::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    if (message->GetType() != kTractMessageStyleTick) return true;
    const std::int32_t years = styleYears_;
    styleYears_ = years + 1;
    if (yearsBetweenStyleChanges_ <= years + 1) {
        styleYears_ = 0;
        const std::int32_t index = styleIndex_;
        styleIndex_ = index + 1;
        if (static_cast<std::uint64_t>(activeStyles_.size()) <= static_cast<std::uint64_t>(index) + 1u) {
            styleIndex_ = 0;
        }
    }
    return true;
}

//==================================================================================================
// styles
//==================================================================================================

std::uint32_t cSC4TractDeveloper::GetCurrentStyle() const {
    if (activeStyles_.empty()) return 0;
    return activeStyles_[static_cast<std::size_t>(styleIndex_)];
}

void cSC4TractDeveloper::SetActiveStyles(const std::vector<std::uint32_t>& styles) {
    const int index = styleIndex_;
    int newIndex = 0;
    if (static_cast<std::size_t>(index) < styles.size()) newIndex = index;
    for (std::size_t i = 0; i < styles.size(); ++i) {
        if (!activeStyles_.empty() && styles[i] == activeStyles_[static_cast<std::size_t>(index)]) {
            newIndex = static_cast<int>(i);
        }
    }
    activeStyles_ = styles;
    styleIndex_ = newIndex;
}

//==================================================================================================
// growth database bookkeeping
//==================================================================================================

void cSC4TractDeveloper::FreeBuildAttempt(BuildAttemptRecord& record) {
    for (CandidateLot* candidate : record.candidates) delete candidate;
    record.candidates.clear();
}

void cSC4TractDeveloper::FreeGrowthAttempt(GrowthAttemptRecord& record) {
    for (BuildAttemptRecord& attempt : record.attempts) FreeBuildAttempt(attempt);
    record.attempts.clear();
    record.histograms.clear();
}

void cSC4TractDeveloper::FreeGrowthDatabase() {
    for (auto& byType : growthDatabase_) {
        for (auto& keyed : byType.second) {
            FreeGrowthAttempt(keyed.second.grown);
            FreeGrowthAttempt(keyed.second.failed);
        }
    }
    growthDatabase_.clear();
}

void cSC4TractDeveloper::RecordBuildAttempt() {
    // vslot 0x1c8: the attempt database only exists in the cheat/debug build.
    if (!buildingDevelopment_ || !buildingDevelopment_->CheatCodesEnabled()) return;
    BuildAttemptRecord record;
    record.attempted = attempted_;                       // this[0x114]
    record.configurations = configurations_;             // +0x130 copy
    record.configurationData = reinterpret_cast<std::uint64_t>(configuration_);   // this+0x160
    record.configurationId = static_cast<std::int32_t>(buildingType_);   // this+0x168
    record.outcome0 = outcome0_;                         // this+0x16c
    record.outcome1 = outcome1_;                         // this+0x170
    record.outcome2 = outcome2_;                         // this+0x174
    record.outcomeCount = outcomeList_ ? static_cast<std::int32_t>(outcomeList_->size()) : 0;
    record.candidates.reserve(candidates_.size());
    for (CandidateLot* candidate : candidates_) {
        // The original deep-copies every candidate lot (0x100137018); the copies are freed by
        // FreeBuildAttempt.
        record.candidates.push_back(new CandidateLot(*candidate));
    }
    currentAttempt_.attempts.push_back(std::move(record));
}

void cSC4TractDeveloper::RecordGrowthAttempt() {
    if (!buildingDevelopment_ || !buildingDevelopment_->CheatCodesEnabled()) return;
    if (!growthDeveloper_) return;
    const std::uint32_t type = static_cast<std::uint32_t>(growthDeveloper_->GetDeveloperType());  // vslot 0x30
    GrowthRecord& record = growthDatabase_[type][std::pair<int, int>(tractX_, tractZ_)];
    // A built pass replaces the *grown* attempt, a pass that built nothing replaces the *failed* one;
    // the other record keeps its own last attempt (0x1001372b4 frees only the target's nodes).
    const bool built = builtCount_ >= 1;
    GrowthAttemptRecord& target = built ? record.grown : record.failed;
    FreeGrowthAttempt(target);
    if (&target != &currentAttempt_) target = currentAttempt_;
}

const GrowthRecord* cSC4TractDeveloper::GetLastSuccessfulBuild(
    int x, int z, const std::list<std::uint32_t>& developerTypes) const {
    if (!buildingDevelopment_ || !buildingDevelopment_->CheatCodesEnabled()) return nullptr;
    if (!g_host) return nullptr;
    const GrowthRecord* best = nullptr;
    std::uint32_t bestDay = 0;
    for (std::uint32_t type : developerTypes) {
        auto byType = growthDatabase_.find(type);
        if (byType == growthDatabase_.end()) continue;
        IGrowthDeveloperApi* developer = g_host->GrowthDeveloper(type);   // building dev vslot 0x108
        if (!developer) continue;
        ITractValueMap* map = developer->GetDesirabilityMap();            // vslot 0x68
        const int shift = map ? map->GetTractShift() : 0;                 // map vslot 0x58
        auto found = byType->second.find(std::pair<int, int>(x >> shift, z >> shift));
        if (found == byType->second.end()) continue;
        const std::uint32_t day = static_cast<std::uint32_t>(found->second.grown.day);
        if (bestDay < day) {
            bestDay = day;
            best = &found->second;
        }
    }
    return best;
}

const GrowthAttemptRecord* cSC4TractDeveloper::GetLastFailedBuild(int x, int z,
                                                                 std::uint32_t developerType) const {
    if (!buildingDevelopment_ || !buildingDevelopment_->CheatCodesEnabled()) return nullptr;
    if (!g_host) return nullptr;
    auto byType = growthDatabase_.find(developerType);
    if (byType == growthDatabase_.end()) return nullptr;
    IGrowthDeveloperApi* developer = g_host->GrowthDeveloper(developerType);
    if (!developer) return nullptr;
    ITractValueMap* map = developer->GetDesirabilityMap();
    const int shift = map ? map->GetTractShift() : 0;
    auto found = byType->second.find(std::pair<int, int>(x >> shift, z >> shift));
    if (found == byType->second.end()) return nullptr;
    if (found->second.failed.day == 0) return nullptr;   // this+0xa0 == 0 in the node
    return &found->second.failed;
}

//==================================================================================================
// the tract pass
//==================================================================================================

void cSC4TractDeveloper::ListExistingLots() {
    if (!growthDeveloper_ || !lotManager_ || !g_host) return;
    ITractValueMap* map = growthDeveloper_->GetDesirabilityMap();   // vslot 0x68
    if (!map) return;
    const int shift = map->GetTractShift();                        // map vslot 0x58
    // The original walks the tract's city cells with a cRZRandomIterator (see the header note); the
    // set it fills does not depend on the order, so the host supplies the iteration.
    g_host->ForEachCellInTract(tractX_, tractZ_, shift, [this](int x, int z) {
        cSC4Lot* lot = lotManager_->GetLot(x, z, false);            // lot manager vslot 0x28
        if (lot) existingLots_.insert(lot);
    });
}

void cSC4TractDeveloper::GetBoundingRect(CandidateLot* candidate) {
    if (!candidate) return;
    if (candidate->rectX0 >= 0 && candidate->rectZ0 >= 0 && candidate->rectX1 >= 0 &&
        candidate->rectZ1 >= 0) {
        return;
    }
    int z1 = 0;
    switch (candidate->facing) {
    case 0:
        candidate->rectX0 = (candidate->x + 1) - candidate->sizeX;
        candidate->rectX1 = candidate->x;
        candidate->rectZ0 = candidate->z;
        z1 = candidate->z - 1 + candidate->sizeZ;
        break;
    case 1:
        candidate->rectX0 = (candidate->x + 1) - candidate->sizeZ;
        candidate->rectX1 = candidate->x;
        candidate->rectZ0 = (candidate->z + 1) - candidate->sizeX;
        z1 = candidate->z;
        break;
    case 2:
        candidate->rectX0 = candidate->x;
        candidate->rectX1 = candidate->x - 1 + candidate->sizeX;
        candidate->rectZ0 = (candidate->z + 1) - candidate->sizeZ;
        z1 = candidate->z;
        break;
    case 3:
        candidate->rectX0 = candidate->x;
        candidate->rectX1 = candidate->x - 1 + candidate->sizeZ;
        candidate->rectZ0 = candidate->z;
        z1 = candidate->z - 1 + candidate->sizeX;
        break;
    case -1:
        // The facing was not set: take the lot's own footprint (lot vslots 0xb8/0xb0/0xa8/0x70).
        if (candidate->lot) {
            candidate->facing = candidate->lot->GetFacing();
            candidate->lot->GetSize(candidate->sizeX, candidate->sizeZ);
            candidate->lot->GetLocation(candidate->x, candidate->z);
            candidate->lot->GetBoundingRect(&candidate->rectX0);
        }
        return;
    default:
        return;
    }
    candidate->rectZ1 = z1;
}

int cSC4TractDeveloper::FindExistingCapacity(CandidateLot* candidate) {
    if (!candidate) return 0;
    if (candidate->existingCapacity >= 0 && candidate->abandonedCapacity >= 0) return 0;
    candidate->existingCapacity = 0;      // 8-byte store at +0x38
    candidate->abandonedCapacity = 0;
    candidate->maxGrowthStage = 0;        // 8-byte store at +0x40
    candidate->maxWealth = 0;
    const std::uint32_t developerType =
        growthDeveloper_ ? static_cast<std::uint32_t>(growthDeveloper_->GetDeveloperType()) : 0;

    auto accumulate = [this, candidate, developerType](cSC4Lot* lot) {
        if (!lot) return;
        // lot vslot 0x198: GetCapacity(developerType, ignoreState = true)
        const std::uint16_t capacity = lot->GetCapacity(developerType, true);
        // lot vslot 0x108: GetState(); 4 = abandoned
        if (lot->GetState() == kLotStateAbandoned) {
            candidate->abandonedCapacity += capacity;
        } else {
            candidate->existingCapacity += capacity;
        }
        // lot vslot 0x48: the lot configuration; its +0x58 growth stage. The port's lot holds the manager's reference, not the
        // configuration itself: the configuration comes from the manager by the reference's id (as ListCandidateLots_Existing does).
        cISC4LotConfigurationRef* reference = lot->GetLotConfiguration();
        const cSC4LotConfiguration* configuration =
            reference && lotConfigurationManager_ ? lotConfigurationManager_->GetLotConfiguration(reference->GetID()) : nullptr;
        const std::uint8_t stage = configuration ? configuration->GetGrowthStage() : 0;
        if (candidate->maxGrowthStage < static_cast<std::int32_t>(stage)) {
            candidate->maxGrowthStage = stage;
        }
        // lot vslot 0x170: GetBuildingType(false); building development vslot 0x138 profile.
        const std::uint32_t buildingType = lot->GetBuildingType(false);
        if (buildingType && buildingDevelopment_) {
            const BuildingProfile& profile = buildingDevelopment_->GetBuildingProfile(buildingType);
            // The original copies the profile's +0x04 (wealth, not capacity) into +0x44.
            if (candidate->maxWealth < static_cast<std::int32_t>(profile.wealth)) {
                candidate->maxWealth = static_cast<std::int32_t>(profile.wealth);
            }
        }
    };

    const std::int32_t kind = candidate->kind;
    if (static_cast<std::uint32_t>(kind - 1) > 1u) {
        if (kind == 3) {
            for (cSC4Lot* neighbour : candidate->neighbours) accumulate(neighbour);
            return 0;
        }
        if (kind != 4) return 0;
    }
    accumulate(candidate->lot);
    return 0;
}

int cSC4TractDeveloper::ScoreCandidateLots() {
    const std::int32_t purpose = growthDeveloper_ ? growthDeveloper_->GetPurpose() : 0;   // vslot 0x38
    float weight0 = 0, weight1 = 0, weight2 = 0;
    if (static_cast<std::uint32_t>(purpose - 1) < 3u) {
        weight0 = kNetworkWeight0[purpose - 1];
        weight1 = kNetworkWeight1[purpose - 1];
        weight2 = kNetworkWeight2[purpose - 1];
    }
    for (CandidateLot* candidate : candidates_) {
        candidate->score = 0;
        float score = 0;
        if (candidate->kind != 1) {
            FindExistingCapacity(candidate);
            score = static_cast<float>(candidate->abandonedCapacity) * abandonedCapacityWeight_ +
                    static_cast<float>(candidate->existingCapacity) * existingCapacityWeight_ +
                    candidate->score;
            candidate->score = score;
            if (candidate->kind == 4) {
                score += subdivisionPenalty_;
                candidate->score = score;
            } else if (candidate->kind == 3) {
                score += static_cast<float>(static_cast<std::int32_t>(candidate->neighbours.size()) - 1) *
                         aggregationPenalty_;
                candidate->score = score;
            }
        }
        if (weight0 != 0.0f || std::isnan(weight0)) {
            for (int i = 0; i < 4; ++i) {
                score += static_cast<float>(candidate->rails[static_cast<std::size_t>(i)]) * weight0 +
                         static_cast<float>(candidate->avenues[static_cast<std::size_t>(i)]) * weight1 +
                         static_cast<float>(candidate->roads[static_cast<std::size_t>(i)]) * weight2;
                candidate->score = score;
            }
        }
    }
    return 0;
}

bool cSC4TractDeveloper::CompareCandidateLots(const CandidateLot* a, const CandidateLot* b) {
    return a->score <= b->score && b->score != a->score;
}

CandidateLot* cSC4TractDeveloper::NewCandidateLot() {
    // The pool starts empty: ten candidates are allocated at once (0x100138eae).
    if (CandidatePool().empty()) {
        for (int i = 0; i < 10; ++i) CandidatePool().push_back(new CandidateLot());
    }
    CandidateLot* candidate = CandidatePool().back();
    CandidatePool().pop_back();
    *candidate = CandidateTemplate();   // copy the static default over the popped object
    return candidate;
}

void cSC4TractDeveloper::DeleteCandidateLot(CandidateLot* candidate) {
    if (!candidate) return;
    CandidatePool().push_back(candidate);
}

void cSC4TractDeveloper::ClearCandidateList(std::vector<CandidateLot*>& list) {
    while (!list.empty()) {
        CandidatePool().push_back(list.back());
        list.pop_back();
    }
}

std::size_t cSC4TractDeveloper::CandidatePoolSize() const { return CandidatePool().size(); }

void cSC4TractDeveloper::ClearCandidatePool() {
    for (CandidateLot* candidate : CandidatePool()) delete candidate;
    CandidatePool().clear();
}

//==================================================================================================
// the configuration pass
//==================================================================================================

int cSC4TractDeveloper::IsLotConfigurationSuitable(cSC4LotConfiguration* configuration) {
    int outcome = kTractOutcomeNone;
    if (!configuration || !growthDeveloper_ || !buildingDevelopment_) return outcome;
    // cISC4LotConfiguration vslot 0x88: purpose bits; vslot 0x80: wealth bits.
    if (!configuration->IsCompatibleWithPurposeType(growthDeveloper_->GetPurpose())) {
        outcome = kTractOutcomePurposeMismatch;
    } else if (!configuration->IsCompatibleWithWealthType(growthDeveloper_->GetWealth())) {
        outcome = kTractOutcomeWealthMismatch;
    } else {
        const std::int32_t growthStage = configuration->GetGrowthStage();   // vslot 0x58
        if (static_cast<std::int32_t>(profileFlag_) < growthStage) {
            outcome = kTractOutcomeGrowthStageTooHigh;
        } else {
            const std::int32_t purpose = growthDeveloper_->GetPurpose();
            bool styleChecked = false;
            bool styleOk = true;
            if (static_cast<std::uint32_t>(purpose - 1) < 3u && !activeStyles_.empty()) {
                styleChecked = true;
                if (singleStyle_ == 0) {
                    // "use all styles at once": any active style is enough (vslot 0x90).
                    styleOk = false;
                    for (std::uint32_t style : activeStyles_) {
                        if (configuration->IsCompatibleWithStyle(style)) { styleOk = true; break; }
                    }
                } else {
                    const std::size_t index = static_cast<std::size_t>(styleIndex_);
                    styleOk = index < activeStyles_.size() &&
                              configuration->IsCompatibleWithStyle(activeStyles_[index]);
                }
            }
            if (styleChecked && !styleOk) {
                outcome = kTractOutcomeStyleMismatch;
            } else if (!configuration->GetPrimaryBuildingConfigurationObject()) {
                outcome = kTractOutcomeNone;                    // vslot 0xc0 == null: outcome 0
            } else {
                const std::uint32_t minCapacity =
                    growthStage != 1 ? configuration->GetMinBuildingCapacity() : 0;   // vslot 0x60
                const std::uint32_t maxCapacity = configuration->GetMaxBuildingCapacity();  // vslot 0x68
                if (attempted_ == 0 && static_cast<std::int32_t>(maxCapacity) < maximumBuildAttempts_) {
                    outcome = kTractOutcomeCapacityTooLow;
                } else if (capacity_ + secondAttemptLimit_ < static_cast<std::int32_t>(minCapacity)) {
                    outcome = kTractOutcomeCapacityTooHigh;
                } else {
                    outcome = kTractOutcomeSuccess;
                }
            }
        }
    }
    // The original indexes the 13x28 table directly; the port ignores a call made before Grow sized it.
    if (currentAttempt_.histograms.size() > kHistogramSuitable &&
        currentAttempt_.histograms[kHistogramSuitable].size() > static_cast<std::size_t>(outcome)) {
        ++currentAttempt_.histograms[kHistogramSuitable][static_cast<std::size_t>(outcome)];
    }
    return outcome;
}

int cSC4TractDeveloper::ListPossibleLotConfigurations() {
    if (!growthDeveloper_ || !g_host) return kTractOutcomeNone;
    const int wealth = growthDeveloper_->GetWealth();      // vslot 0x40
    const int purpose = growthDeveloper_->GetPurpose();    // vslot 0x38
    const std::pair<int, int> key(wealth, purpose);
    auto entry = filterCache_.find(key);
    if (entry == filterCache_.end()) {
        auto* filter = new LotSizesFilter(wealth, purpose);
        entry = filterCache_.emplace(key, filter).first;
        g_host->RegisterLotConfigurationFilter(filter);   // manager vslot 0x90
    }
    std::vector<cSC4LotConfiguration*> configurations;
    const int count = g_host->LotConfigurationsByFilter(configurations, entry->second);  // vslot 0x48
    int best = 0;
    if (count == 0) {
        best = kTractOutcomeNoConfiguration;
    } else if (configurations.empty()) {
        best = kTractOutcomeNone;
    } else {
        for (cSC4LotConfiguration* configuration : configurations) {
            const int outcome = IsLotConfigurationSuitable(configuration);
            if (currentAttempt_.histograms.size() > kHistogramConfigurations &&
                currentAttempt_.histograms[kHistogramConfigurations].size() >
                    static_cast<std::size_t>(outcome)) {
                ++currentAttempt_.histograms[kHistogramConfigurations][static_cast<std::size_t>(outcome)];
            }
            if (best < outcome) best = outcome;
            if (outcome == kTractOutcomeSuccess) configurations_.insert(configuration);
            configuration->Release();   // the manager's reference; the set keeps the bare pointer
        }
    }
    return best;
}

const cISC4LotConfigurationFilter* cSC4TractDeveloper::CachedFilter(int wealth, int purpose) const {
    auto entry = filterCache_.find(std::pair<int, int>(wealth, purpose));
    return entry == filterCache_.end() ? nullptr : entry->second;
}

//==================================================================================================
// Grow / Decline / UpdatePopulation
//==================================================================================================

int cSC4TractDeveloper::Grow(const GrowthProfile& profile, std::list<int>& outcomes, bool& declined) {
    IGrowthDeveloperApi* developer = profile.developer;
    if (!developer || !g_host || !buildingDevelopment_) {
        declined = false;
        return 0;
    }
    tractX_ = profile.tractX;
    tractZ_ = profile.tractZ;
    profileFlag_ = profile.growthStage;
    growthDeveloper_ = developer;
    developer->GetConstructionInfo(construction_);                     // vslot 0x48 -> this+0xdc
    ITractValueMap* capacityMap = developer->GetCapacityMap();         // vslot 0x70
    capacity_ = capacityMap ? capacityMap->GetCellValue(tractX_, tractZ_) : 0;   // map vslot 0xb0
    ITractDemandApi* demand = developer->GetSourceDemand();            // vslot 0x90
    const float demandValue = demand ? demand->GetValue() : 0.0f;      // demand vslot 0x60
    int attempts = 1;
    if (demandValue > 0.0f) attempts = static_cast<int>(demandValue);
    maximumBuildAttempts_ = attempts;
    secondAttemptLimit_ = static_cast<std::int32_t>(static_cast<float>(attempts) * capacityFactor_);
    builtCount_ = 0;                                                   // 8-byte store at +0x10c
    configuration_ = nullptr;                                          // 8-byte store at +0x160
    outcomeList_ = &outcomes;                                          // +0x178
    noGrowth_ = 0;                                                     // +0x180
    const std::int32_t day = g_host->SimulatorDay();                   // cRZDate of simulator vslot 0x30
    currentAttempt_.day = day;                                         // +0x1e8
    currentAttempt_.developer = developer;                             // +0x1f0
    currentAttempt_.tractX = tractX_;                                  // +0x1f8
    currentAttempt_.tractZ = tractZ_;                                  // +0x1fc
    ITractValueMap* populationMap = developer->GetPopulationMap();     // vslot 0x78
    currentAttempt_.population = populationMap ? populationMap->GetCellValue(tractX_, tractZ_) : 0;
    currentAttempt_.reserved24 = 0;
    currentAttempt_.capacity = capacity_;                              // +0x208
    ITractValueMap* maxCapacityMap = developer->GetMaxCapacityMap();   // vslot 0x80
    currentAttempt_.maxCapacity = maxCapacityMap ? maxCapacityMap->GetCellValue(tractX_, tractZ_) : 0;
    ITractValueMap* desirabilityMap = developer->GetDesirabilityMap(); // vslot 0x68
    currentAttempt_.desirability =
        desirabilityMap ? desirabilityMap->GetCellValue(tractX_, tractZ_) : 0;
    currentAttempt_.maximumBuilds = maximumBuildAttempts_;             // +0x214
    currentAttempt_.objectId = buildingDevelopment_->GetFreeConstructionResources();   // building dev vslot 0x180
    currentAttempt_.attempts.clear();                                  // +0x220
    currentAttempt_.histograms.assign(kHistogramBucketCount, std::vector<std::uint32_t>(kTractOutcomeSlots, 0));

    // The passes (0x100136234..0x1001365fc). At most n*n + 1 of them (n = the desirability map's tract size). Each starts clean,
    // lists the tract's lots and tries, in order: taking over an abandoned building (which ends Grow when it succeeds), then, when
    // configurations qualify, the three candidate searches (subdivision not for agriculture); the candidates are scored, sorted
    // and built in order. Every outcome goes on the caller's list and into the attempt record. A pass that ends without a
    // decision is followed by another only after the first pass (`firstPass`), or when it ended in success (27).
    const std::int32_t purpose = developer->GetPurpose();              // vslot 0x38
    ITractValueMap* tractMap = developer->GetDesirabilityMap();        // vslot 0x68
    int passesLeft = (tractMap ? tractMap->GetTractSize() : 0);        // map vslot 0x50
    passesLeft = passesLeft * passesLeft + 1;
    bool firstPass = true;
    bool done = false;
    do {
        const bool carried = done;                                     // local_40: the previous pass's verdict (always false here)
        existingLots_.clear();
        configurations_.clear();
        ClearCandidateList(candidates_);
        attempted_ = firstPass ? 0 : 1;                                // +0x114
        buildingType_ = 0;                                             // +0x168 / +0x16c (one 8-byte store)
        outcome0_ = 0;
        outcome1_ = -1;                                                // +0x170 / +0x174 (one 8-byte store of -1)
        outcome2_ = -1;
        lotOutcomes_.clear();                                          // +0x188
        ListExistingLots();

        int outcome = 0;
        bool verdict = carried;                                        // what a pass that runs out of candidates hands on
        const int takeOver = TakeOverAbandoned();
        if (takeOver > 0) outcome = takeOver;
        if (takeOver == kTractOutcomeSuccess) {
            outcomes.push_back(kTractOutcomeSuccess);
            RecordBuildAttempt();
            break;
        }
        if (ListPossibleLotConfigurations() == kTractOutcomeSuccess) {
            outcome = std::max(outcome, ListCandidateLots_Existing());
            outcome = std::max(outcome, ListCandidateLots_Aggregation());
            if (purpose != 5) outcome = std::max(outcome, ListCandidateLots_Subdivision());
            if (candidates_.empty()) {
                if (outcome == kTractOutcomeSuccess) outcome = kTractOutcomeNoCandidates;
                outcomes.push_back(outcome);
                currentAttempt_.histograms[0][static_cast<std::size_t>(outcome)] += 1;
                RecordBuildAttempt();
            } else {
                ScoreCandidateLots();
                std::sort(candidates_.begin(), candidates_.end(), CompareCandidateLots);
                for (CandidateLot* candidate : candidates_) {
                    const int result = developer->GetPurpose() == 5 ? BuildFarm(candidate) : Build(candidate);
                    outcome = result;
                    outcomes.push_back(result);
                    RecordBuildAttempt();
                    if (result == kTractOutcomeBuildFailed) {      // the pass is over; a second one only after the first
                        verdict = attempted_ != 0;
                        break;
                    }
                    if (maximumBuildAttempts_ <= builtCount_) {    // grew as much as the demand asks
                        verdict = true;
                        break;
                    }
                    if (result == kTractOutcomeSuccess) {          // agriculture and industry stop at one building
                        verdict = static_cast<std::uint32_t>(developer->GetPurpose() - 5) <= 3 ? true : carried;
                        break;
                    }
                }
            }
        }
        if (passesLeft < 1) break;
        done = (outcome == kTractOutcomeSuccess || firstPass) ? verdict : true;
        --passesLeft;
        firstPass = false;
    } while (!done);
    RecordGrowthAttempt();
    declined = noGrowth_ != 0;                                         // +0x180
    return builtCount_;
}

int cSC4TractDeveloper::Decline(const GrowthProfile& profile) {
    IGrowthDeveloperApi* developer = profile.developer;
    if (!developer || !g_host || !lotManager_ || !buildingDevelopment_ || !lotDeveloper_) return 0;
    ITractValueMap* map = developer->GetDesirabilityMap();   // vslot 0x68
    if (!map) return 0;
    const int shift = map->GetTractShift();                  // map vslot 0x58
    const std::int32_t purpose = developer->GetPurpose();    // vslot 0x38
    const std::int32_t wealth = developer->GetWealth();      // vslot 0x40
    const std::uint32_t developerType = static_cast<std::uint32_t>(developer->GetDeveloperType());  // 0x30
    int total = 0;
    g_host->ForEachCellInTract(profile.tractX, profile.tractZ, shift, [&](int x, int z) {
        cSC4Lot* lot = lotManager_->GetLot(x, z, false);     // lot manager vslot 0x28
        if (!lot) return;
        if (lot->GetState() != kLotStateLivable) return;      // lot vslot 0x108 == 3
        if (lot->GetDaysSinceLastStateChange() <= kDaysBeforeDecline) return;   // lot vslot 0x140
        const std::uint32_t buildingType = lot->GetBuildingType(false);         // lot vslot 0x170
        if (!buildingType) return;
        const BuildingProfile& building = buildingDevelopment_->GetBuildingProfile(buildingType);  // 0x138
        const std::int32_t lotWealth = lot->GetOccupantWealth();               // lot vslot 0x148
        if (static_cast<std::int32_t>(building.purpose) != purpose) return;
        if (lotWealth != wealth) return;
        // lot vslot 0x198: GetCapacity(developerType, ignoreState = false)
        const std::uint16_t capacity = lot->GetCapacity(developerType, false);
        total += capacity;
        lotDeveloper_->AbandonBuilding(lot, 6);   // lot developer vslot 0x40, VacateReason 6
    });
    return total;
}

void cSC4TractDeveloper::UpdatePopulation(const GrowthProfile& profile) {
    // PARTIAL (0x10013beb6): the original lists every lot of the tract with
    // cISC4LotManager::GetLotsInCellRect (lot manager vslot 0x38), asks each lot's building occupant
    // for its type and capacity (cISC4BuildingOccupant slots 0x130/0x148/0x1a0) and rescales the
    // population of the matching developer type with cRZRandom seeds derived from the tract
    // coordinates. That needs the occupant-manager/occupant interfaces of the unrecovered building
    // occupant path, so this port leaves the population untouched.
    (void)profile;
}

//==================================================================================================
// The candidate search and the build (the path an empty zoned lot takes to a building)
//==================================================================================================

// Every lot of the tract that IsLotSuitable passes, paired with the first suitable configuration of its own footprint that fits
// the zone's growth stage, the lot's slope and IsLotConfigurationCompatible, becomes a kind-2 candidate. The best outcome
// seen is returned; a tract with a lot that is neither unsuitable for its zone (6) nor abandoned (3) is marked "could grow"
// (noGrowth_ is the original's +0x180 byte, which this sets).
int cSC4TractDeveloper::ListCandidateLots_Existing() {   // @ 0x1001377a0
    int best = 0;
    for (cSC4Lot* lot : existingLots_) {
        const int suitable = IsLotSuitable(lot, true);
        const int state = lot->GetState();                                         // lot vslot 0x108
        if (suitable != 6 && state != 3) noGrowth_ = 1;
        int x, z;
        lot->GetLocation(x, z);                                                    // lot vslot 0xa8
        if (suitable != kTractOutcomeSuccess) {
            if (best <= suitable) best = suitable;
            continue;
        }
        std::uint8_t sizeX, sizeZ;
        lot->GetSize(sizeX, sizeZ);                                                // lot vslot 0xb0
        const float slope = lot->GetSlope();                                       // lot vslot 0x90
        for (cSC4LotConfiguration* configuration : configurations_) {
            std::uint32_t configurationX, configurationZ;
            configuration->GetSize(configurationX, configurationZ);                // vslot 0x38
            if (configurationX != sizeX || configurationZ != sizeZ) {
                ++currentAttempt_.histograms[kHistogramExisting][kTractOutcomeNoConfiguration];
                if (best <= 4) best = kTractOutcomeNoConfiguration;
                continue;
            }
            const std::uint8_t stage = configuration->GetGrowthStage();            // vslot 0x58
            if (!GrowthStageFitsInZone(stage, static_cast<std::uint32_t>(zoneManager_->ZoneTypeAt(x, z)))) {
                ++currentAttempt_.histograms[kHistogramExisting][6];
                if (best <= 5) best = 6;
                continue;
            }
            const float minimum = configuration->GetMinimumSlope();                // vslot 0x48
            const float maximum = configuration->GetMaximumSlope();                // vslot 0x40
            if (slope < minimum || maximum < slope) {
                ++currentAttempt_.histograms[kHistogramExisting][10];
                if (best <= 9) best = 10;
                continue;
            }
            CandidateLot* candidate = NewCandidateLot();
            candidate->kind = 2;
            candidate->lot = lot;
            lot->GetLocation(candidate->x, candidate->z);
            candidate->sizeX = sizeX;
            candidate->sizeZ = sizeZ;
            candidate->facing = lot->GetFacing();                                  // lot vslot 0xb8
            FindNetworks(candidate);
            const int compatible = IsLotConfigurationCompatible(configuration, candidate, false);
            if (best <= compatible) best = compatible;
            if (compatible == kTractOutcomeSuccess) {
                candidates_.push_back(candidate);
                ++currentAttempt_.histograms[kHistogramExisting][kTractOutcomeSuccess];
                break;
            }
            DeleteCandidateLot(candidate);
        }
    }
    return best;
}

// Picks a building for the candidate, claims its capacity from the construction resources and starts it: an existing lot is
// cleared first (DemolishLots, flags 0x800) and its old capacity no longer counts; a subdivision hands its capacity back and
// subdivides instead of constructing.
int cSC4TractDeveloper::Build(CandidateLot* candidate) {   // @ 0x1001388c4
    cSC4Lot* lot = nullptr;
    int result = PickBuilding(candidate);
    if (result != kTractOutcomeSuccess) return result;
    const std::uint16_t capacity = buildingDevelopment_->GetBuildingProfile(buildingType_).capacity;   // vslot 0x138, +8
    if (!buildingDevelopment_->ClaimConstructionResources(capacity)) {                                 // vslot 0x190
        ++currentAttempt_.histograms[kHistogramBuild][kTractOutcomeBuildFailed];
        return kTractOutcomeBuildFailed;
    }
    if (candidate->kind == 4) {
        buildingDevelopment_->ReleaseConstructionResources(capacity);                                   // vslot 0x198
        PerformSubdivision(candidate);
        return kTractOutcomeSuccess;
    }
    if (candidate->kind == 3) {
        PerformAggregation(candidate, lot);
    } else if (candidate->kind == 2) {
        lot = candidate->lot;
        lot->AddRef();
        lotOutcomes_.erase(lot);
        std::list<cRZAutoRefCount<cSC4Lot>> lots;
        lots.push_back(cRZAutoRefCount<cSC4Lot>(lot));
        g_host->DemolishLots(lots, 0x800);
        builtCount_ -= candidate->existingCapacity;
    }
    if (!lot) {
        ++currentAttempt_.histograms[kHistogramBuild][0];
        return 0;
    }
    lotDeveloper_->StartConstruction(lot, configuration_, buildingType_, true);                        // vslot 0x30
    builtCount_ += capacity;
    lot->Release();
    return kTractOutcomeSuccess;
}

// Whether a lot can be rebuilt by this developer. With useCache the per-lot outcome map answers repeat questions and records
// the answer. The checks, in order: the zone fits the developer's building purpose (else 6) and, for a cached question about
// a livable lot, it has been in that state 180 days (else 19); the land value range of the construction profile (22); the
// lot's state (4); the lot manager's CanCreateLot on its footprint (7 / 8 pass, 0..3 map to 0 / 4 / 6 / 3); an occupied lot
// must not be a landmark-type (5) or same-purpose agriculture (15) building and, for a cached question, may only be taken
// over by a richer or (when take-overs are off) equal-wealth developer of the same purpose (16); not historical (18); its
// growth stage not past the tract's (17); then road access: 27 or 21.
int cSC4TractDeveloper::IsLotSuitable(cSC4Lot* lot, bool useCache) {   // @ 0x100138b10
    if (useCache) {
        auto found = lotOutcomes_.find(lot);
        if (found != lotOutcomes_.end()) return found->second;
    }
    int x, z;
    lot->GetLocation(x, z);                                                        // lot vslot 0xa8
    ConstructionProfile info;
    growthDeveloper_->GetConstructionInfo(info);                                   // vslot 0x48
    const char zone = static_cast<char>(zoneManager_->ZoneTypeAt(x, z));           // zone manager vslot 0x60
    const bool fits = BuildingFitsInZone(info.purpose, static_cast<std::uint32_t>(static_cast<int>(zone)));
    int result = 6;
    const BuildingProfile* profile = nullptr;
    do {
        if (fits && useCache && lot->IsInLivableState()) {                         // lot vslot 0x128
            result = 0x13;
            if (lot->GetDaysSinceLastStateChange() < 180) break;                   // lot vslot 0x140
        } else if (!fits) {
            break;
        }
        if (info.flag10 != 0 || info.flag11 != 0) {
            const std::uint8_t value = g_host->LandValueAt(x, z);                  // land value vslot 0x30
            if (value < info.flag10 || info.flag11 < value) {
                outcome0_ = value;
                result = 0x16;
                break;
            }
        }
        const int state = lot->GetState();                                         // lot vslot 0x108
        result = 4;
        if (state == 2 || state > 4) break;
        std::int32_t rect[4];
        lot->GetBoundingRect(rect);                                                // lot vslot 0x70
        static constexpr int kCanCreateOutcome[4] = {0, 4, 6, 3};                  // 0x100a18df0
        const int canCreate = lotManager_->CanCreateLot(rect[0], rect[1], rect[2] + 1 - rect[0], rect[3] + 1 - rect[1],
                                                        false, false, nullptr);    // lot manager vslot 0xa0
        if (static_cast<unsigned>(canCreate - 7) > 1) {
            result = static_cast<unsigned>(canCreate) < 4 ? kCanCreateOutcome[canCreate] : 0;
            break;
        }
        if (lot->HasCapacity()) {                                                  // lot vslot 0x130
            const std::uint32_t buildingType = lot->GetBuildingType(false);       // lot vslot 0x170
            if (buildingType == 0) break;
            profile = &buildingDevelopment_->GetBuildingProfile(buildingType);     // vslot 0x138
            result = 0xf;
            if (profile->purpose == 5 || (profile->purpose == 7 && construction_.value12 == 6)) break;
            if (useCache) {
                const std::int32_t wealth = std::bit_cast<std::int32_t>(construction_.value8);
                const std::int32_t lotWealth = static_cast<std::int32_t>(profile->wealth);
                result = 0x10;
                if (allowTakeOver_ ? lotWealth > wealth : lotWealth != wealth) break;
                if (lotWealth >= wealth && static_cast<std::uint32_t>(profile->purpose) != construction_.value12) break;
            }
            result = 0x12;
            if (lot->IsHistorical()) break;                                        // lot vslot 0x158
            cISC4LotConfigurationRef* reference = lot->GetLotConfiguration();      // lot vslot 0x48
            const cSC4LotConfiguration* configuration =
                reference && lotConfigurationManager_ ? lotConfigurationManager_->GetLotConfiguration(reference->GetID()) : nullptr;
            if (configuration) {
                const int stage = configuration->GetGrowthStage();                 // vslot 0x58
                const bool fitsStage = static_cast<std::int32_t>(profile->wealth) != std::bit_cast<std::int32_t>(construction_.value8)
                                           ? stage <= profileFlag_
                                           : stage < profileFlag_;
                result = 0x11;
                if (!fitsStage) break;
            }
        }
        const int purpose = growthDeveloper_->GetPurpose();                        // vslot 0x38
        bool access;
        if (static_cast<unsigned>(purpose - 6) < 3) {
            access = lot->HasOrganicRoadAccess(static_cast<std::uint32_t>(purpose));   // lot vslot 0xe8
        } else if (purpose == 1) {
            access = g_host->HasTripLengthAt(x, z);                                // traffic vslot 0x68 -> map vslot 0x38
        } else {
            access = lot->HasRoadAccess();                                         // lot vslot 0xe0
        }
        result = access ? kTractOutcomeSuccess : 0x15;
    } while (false);
    if (useCache) lotOutcomes_[lot] = result;
    ++currentAttempt_.histograms[kHistogramLotSuitable][static_cast<std::size_t>(result)];
    return result;
}

// Chooses the configuration and building for a candidate. The style is the anchor style (0x3000) where the lot developer
// needs an anchor building, else one drawn from the developer's style percentages (0 = any). Among the suitable
// configurations of that style that IsLotConfigurationCompatible accepts, one is drawn at random (configuration_); the
// capacity window [outcome1_, outcome2_] follows from what is still to build; then one building of the configuration's
// primary building families that IsBuildingCompatible accepts is drawn (buildingType_), the monopoly-piece alternatives (2)
// standing in only when nothing else fits and the developer's monopoly use is 1.
int cSC4TractDeveloper::PickBuilding(CandidateLot* candidate) {   // @ 0x1001392fa
    FindExistingCapacity(candidate);
    const std::int32_t purpose = growthDeveloper_->GetPurpose();                   // vslot 0x38
    std::uint32_t style = 0x3000;
    if (!buildingDevelopment_->NeedsAnchorBuilding(&candidate->rectX0, purpose)) {
        const std::map<std::uint32_t, SC4Percentage>& percentages = growthDeveloper_->GetStylePercentages();   // vslot 0xd0
        style = 0;
        if (!percentages.empty()) {
            std::uint32_t draw = g_host->RandomBelow(100);
            for (const auto& entry : percentages) {
                const std::uint32_t share = static_cast<std::uint32_t>(std::lrintf(entry.second.value * 100.0f));
                if (draw < share) {
                    style = entry.first;
                    break;
                }
                draw -= share;
            }
        }
    }
    std::set<cSC4LotConfiguration*> chosen;
    for (cSC4LotConfiguration* configuration : configurations_) {
        if (style != 0 && !configuration->IsCompatibleWithStyle(style)) {          // vslot 0x90
            ++currentAttempt_.histograms[kHistogramStyle][kTractOutcomeStyleMismatch];
            continue;
        }
        if (IsLotConfigurationCompatible(configuration, candidate, candidate->needsRoad != 0) == kTractOutcomeSuccess)
            chosen.insert(configuration);
    }
    if (chosen.empty()) return 0;
    std::uint32_t pick = g_host->RandomBelow(static_cast<std::uint32_t>(chosen.size()));
    configuration_ = nullptr;
    for (cSC4LotConfiguration* configuration : chosen) {
        if (pick == 0) {
            configuration_ = configuration;
            break;
        }
        --pick;
    }

    const std::int32_t existing = candidate->existingCapacity;
    std::int32_t minimum;
    if (!attempted_) {
        minimum = maximumBuildAttempts_ - builtCount_ + existing;
        if (candidate->kind == 4) {
            minimum += reserved110_;
            if (minimum < existing) minimum = existing;
            minimum /= candidate->ratio;
        } else if (minimum < existing) {
            minimum = existing;
        }
    } else {
        minimum = existing + 1;
        if (minimum < existing) minimum = existing;
        if (candidate->kind == 4) minimum /= candidate->ratio;
    }
    const std::int32_t limit = secondAttemptLimit_ - builtCount_ + existing;
    outcome1_ = minimum;
    outcome2_ = minimum < limit ? limit : minimum + 1;

    int result = 0;
    std::unordered_set<std::uint32_t> compatible, alternatives;
    cSC4LotConfigurationObject* building = configuration_ ? configuration_->GetPrimaryBuildingConfigurationObject() : nullptr;   // vslot 0xc0
    if (building) {
        for (std::uint32_t family : building->extra) {
            std::unordered_set<std::uint32_t> types;
            buildingDevelopment_->GetBuildingTypesByFamily(types, family);         // vslot 0x88
            for (std::uint32_t type : types) {
                if (style != 0) {
                    cGZPersistResourceKey key{};
                    buildingDevelopment_->GetBuildingKeyFromType(type, key);       // vslot 0x68
                    if (!cSC4Occupant::IsOccupantGroup(key, style)) {
                        ++currentAttempt_.histograms[kHistogramStyle][kTractOutcomeStyleMismatch];
                        continue;
                    }
                }
                result = IsBuildingCompatible(type, candidate);
                if (result == 2) {
                    if (growthDeveloper_->GetMonopolyUse() == 1) alternatives.insert(type);   // vslot 0xc8
                } else if (result == kTractOutcomeSuccess) {
                    compatible.insert(type);
                }
            }
        }
    }
    if (compatible.empty()) {
        if (alternatives.empty()) return result;
        compatible = alternatives;
    }
    std::uint32_t draw = g_host->RandomBelow(static_cast<std::uint32_t>(compatible.size()));
    buildingType_ = 0;
    for (std::uint32_t type : compatible) {
        if (draw == 0) {
            buildingType_ = type;
            break;
        }
        --draw;
    }
    return kTractOutcomeSuccess;
}

// Counts the networks on the four sides of the candidate's footprint (one cell outside it, inside the city): the counters
// are CandidateLot's streets / roads / avenues / rails, one entry per edge.
void cSC4TractDeveloper::FindNetworks(CandidateLot* candidate) {   // @ 0x10013a5f2
    ITractNetworkManagerApi* network = g_host->NetworkManager();                   // SL::spCity vslot 0x140
    GetBoundingRect(candidate);
    std::int32_t rect[4];
    if (candidate->kind == 4) {
        candidate->lot->GetBoundingRect(rect);                                     // lot vslot 0x70
    } else {
        rect[0] = candidate->rectX0;
        rect[1] = candidate->rectZ0;
        rect[2] = candidate->rectX1;
        rect[3] = candidate->rectZ1;
    }
    auto count = [candidate](ITractNetworkCellApi* cell, std::size_t edge) {
        if (!cell) return;
        if (cell->HasNetworkType(3) || cell->HasNetworkType(0xb)) ++candidate->streets[edge];   // cell vslot 0xb0
        if (cell->HasNetworkType(0) || cell->HasNetworkType(10)) ++candidate->roads[edge];
        if (cell->HasNetworkType(6)) ++candidate->avenues[edge];
        if (cell->HasNetworkType(1)) ++candidate->rails[edge];
    };
    if (!network) return;
    if (rect[1] > 0) {
        for (int x = rect[0]; x <= rect[2]; ++x) count(network->GetNetworkAt(x, rect[1] - 1, 0xc4b, 1), 0);   // vslot 0x50
    }
    if (rect[2] < city_->CellCountX() - 1) {                                       // city vslot 0x2e8
        for (int z = rect[1]; z <= rect[3]; ++z) count(network->GetNetworkAt(rect[2] + 1, z, 0xc4b, 1), 1);
    }
    if (rect[3] < city_->CellCountZ() - 1) {                                       // city vslot 0x2f0
        for (int x = rect[0]; x <= rect[2]; ++x) count(network->GetNetworkAt(x, rect[3] + 1, 0xc4b, 1), 2);
    }
    if (rect[0] > 0) {
        for (int z = rect[1]; z <= rect[3]; ++z) count(network->GetNetworkAt(rect[0] - 1, z, 0xc4b, 1), 3);
    }
}

// Whether a configuration fits a candidate: its footprint (else 0), the zone (6), the growth stage against the candidate's
// tallest existing building, stricter for another wealth (12), the slope (10), unless the caps cheat is on the power (26),
// water (25), fire (23) and park (24) caps of the growth stage, then the required roads: a configuration that needs a road
// on some side is accepted only where TestForCornerLot finds one (and marks the candidate), and a candidate that needs a
// road accepts only such configurations (21). Agriculture-free developers of type 3 skip the road test.
int cSC4TractDeveloper::IsLotConfigurationCompatible(cSC4LotConfiguration* configuration, CandidateLot* candidate,
                                                     bool needsRoad) {   // @ 0x10013aa5c
    const int x = candidate->x;
    const int z = candidate->z;
    FindExistingCapacity(candidate);
    std::uint32_t sizeX, sizeZ;
    configuration->GetSize(sizeX, sizeZ);                                          // vslot 0x38
    if (sizeX != candidate->sizeX || sizeZ != candidate->sizeZ) {
        ++currentAttempt_.histograms[kHistogramCompatible][kTractOutcomeNoConfiguration];
        return 0;
    }
    if (!configuration->IsCompatibleWithZoneType(zoneManager_->ZoneTypeAt(x, z))) {   // vslot 0x78
        ++currentAttempt_.histograms[kHistogramCompatible][6];
        return 6;
    }
    const std::uint8_t stage = configuration->GetGrowthStage();                    // vslot 0x58
    const bool sameWealth = configuration->IsCompatibleWithWealthType(candidate->maxWealth);   // vslot 0x80
    if (!(sameWealth ? candidate->maxGrowthStage < stage : candidate->maxGrowthStage <= stage)) {
        ++currentAttempt_.histograms[kHistogramCompatible][0xc];
        return 0xc;
    }
    const float slope = candidate->lot ? candidate->lot->GetSlope()                // lot vslot 0x90
                                       : lotDeveloper_->GetAreaSlope(&candidate->rectX0, false);   // lot developer vslot 0xe8
    const float minimum = configuration->GetMinimumSlope();                        // vslot 0x48
    const float maximum = configuration->GetMaximumSlope();                        // vslot 0x40
    if (slope < minimum || maximum < slope) {
        ++currentAttempt_.histograms[kHistogramCompatible][10];
        return 10;
    }
    if (!buildingDevelopment_->CheatIgnoreCaps()) {                                // vslot 0x1c0
        if (growthDeveloper_->GetPowerSupplyCap() < stage && !buildingDevelopment_->CheatIgnorePower() &&
            g_host->PoweredAt(x, z) < 2) {
            ++currentAttempt_.histograms[kHistogramCompatible][0x1a];
            return 0x1a;
        }
        if (growthDeveloper_->GetWaterSupplyCap() < stage && !buildingDevelopment_->CheatIgnorePlumbing() &&
            !g_host->IsCellWatered(x, z)) {
            ++currentAttempt_.histograms[kHistogramCompatible][0x19];
            return 0x19;
        }
        if (growthDeveloper_->GetFireCoverageCap() < stage && !g_host->FireProtectedAt(x, z)) {
            ++currentAttempt_.histograms[kHistogramCompatible][0x17];
            return 0x17;
        }
        if (growthDeveloper_->GetParkCap() < stage && g_host->ParkEffectAt(x, z) == 0) {
            ++currentAttempt_.histograms[kHistogramCompatible][0x18];
            return 0x18;
        }
    }
    if (growthDeveloper_->GetType() != 3) {                                        // vslot 0x28
        std::vector<bool> required;
        configuration->GetRequiredRoads(required);                                 // vslot 0xa8
        bool anyRequired = false;
        for (std::size_t edge = 0; edge < 3; ++edge) {
            if (required[edge]) {
                anyRequired = true;
                break;
            }
        }
        if (!anyRequired && needsRoad) {
            ++currentAttempt_.histograms[kHistogramCompatible][0x15];
            return 0x15;
        }
        if (!TestForCornerLot(configuration, candidate)) {
            ++currentAttempt_.histograms[kHistogramCompatible][0x15];
            return 0x15;
        }
        if (anyRequired) candidate->needsRoad = 1;
    }
    ++currentAttempt_.histograms[kHistogramCompatible][kTractOutcomeSuccess];
    return kTractOutcomeSuccess;
}

// Every side the configuration needs a road on must have a street, road or avenue next to it (the side index turns with the
// candidate's facing).
bool cSC4TractDeveloper::TestForCornerLot(cSC4LotConfiguration* configuration, CandidateLot* candidate) {   // @ 0x10013b318
    std::vector<bool> required;
    configuration->GetRequiredRoads(required);                                     // vslot 0xa8
    for (int side = 0; side < 4; ++side) {
        if (!required[static_cast<std::size_t>(side)]) continue;
        int edge = side + candidate->facing + 1;
        while (edge < 0) edge += 4;
        while (edge > 3) edge -= 4;
        if (candidate->avenues[static_cast<std::size_t>(edge)] == 0 && candidate->roads[static_cast<std::size_t>(edge)] == 0 &&
            candidate->streets[static_cast<std::size_t>(edge)] == 0) {
            return false;
        }
    }
    return true;
}

// Whether a building type suits the developer and the capacity window: purpose (7), wealth (8), the current style for
// residential / commercial purposes (9), capacity below the window (11) or above it (13), and the monopoly-piece rule: a piece
// (property 0x2781283f) is 2 for a developer whose monopoly use is 0 or 1, a non-piece 2 for one whose use is 2.
int cSC4TractDeveloper::IsBuildingCompatible(std::uint32_t buildingType, CandidateLot* candidate) {   // @ 0x10013b436
    (void)candidate;
    const BuildingProfile& profile = buildingDevelopment_->GetBuildingProfile(buildingType);   // vslot 0x138
    if (static_cast<std::int32_t>(profile.purpose) != growthDeveloper_->GetPurpose()) {        // vslot 0x38
        ++currentAttempt_.histograms[kHistogramBuilding][kTractOutcomePurposeMismatch];
        return kTractOutcomePurposeMismatch;
    }
    if (static_cast<std::int32_t>(profile.wealth) != growthDeveloper_->GetWealth()) {          // vslot 0x40
        ++currentAttempt_.histograms[kHistogramBuilding][kTractOutcomeWealthMismatch];
        return kTractOutcomeWealthMismatch;
    }
    if (!activeStyles_.empty() && profile.purpose - 1 < 3) {
        const std::uint32_t style = activeStyles_[static_cast<std::size_t>(styleIndex_)];
        cGZPersistResourceKey key{};
        buildingDevelopment_->GetBuildingKeyFromType(buildingType, key);         // vslot 0x68
        if (!cSC4Occupant::IsOccupantGroup(key, style)) {
            ++currentAttempt_.histograms[kHistogramBuilding][kTractOutcomeStyleMismatch];
            return kTractOutcomeStyleMismatch;
        }
    }
    if (static_cast<int>(profile.capacity) < outcome1_) {
        ++currentAttempt_.histograms[kHistogramBuilding][kTractOutcomeCapacityTooLow];
        return kTractOutcomeCapacityTooLow;
    }
    if (outcome2_ < static_cast<int>(profile.capacity)) {
        ++currentAttempt_.histograms[kHistogramBuilding][kTractOutcomeCapacityTooHigh];
        return kTractOutcomeCapacityTooHigh;
    }
    bool monopolyPiece = false;
    cGZPersistResourceKey key{};
    buildingDevelopment_->GetBuildingKeyFromType(buildingType, key);
    g_host->BuildingExemplarBool(key, 0x2781283f, monopolyPiece);
    const std::uint32_t use = growthDeveloper_->GetMonopolyUse();                  // vslot 0xc8
    int result = kTractOutcomeSuccess;
    if (use < 2 ? monopolyPiece : (use == 2 && !monopolyPiece)) result = 2;
    ++currentAttempt_.histograms[kHistogramBuilding][static_cast<std::size_t>(result)];
    return result;
}

//==================================================================================================
// Taking over abandoned buildings (@ 0x1001369ae, Mac i386 0x000e9d9c)
//==================================================================================================

// Before any building is planned, the tract's abandoned lots, and (when take-overs are allowed) livable lots whose occupants
// are poorer than this developer in a building rich enough for it, are offered to the developer: the lot's zone must be of the
// developer's type, its building must have room for the developer (else 15) and IsLotSuitable must answer 27 or 18. Unless the
// caps cheat is on, a lot whose growth stage is above the building's developer's power / water caps needs power (else 26) and
// water (25), and above this developer's fire / park caps fire protection (23) and a park effect (24). When the candidates'
// capacity passes what is still to grow (scaled by the +0x7c tunable) the best scored ones are reoccupied (a livable one is
// abandoned first, reason 5) until the reoccupied capacity passes the +0x80-scaled remainder of the second limit: 27. Otherwise
// the worst outcome seen is returned (1 when nothing was looked at).
int cSC4TractDeveloper::TakeOverAbandoned() {
    if (!growthDeveloper_ || !buildingDevelopment_ || !lotDeveloper_ || !g_host) return 1;
    const std::int32_t type = growthDeveloper_->GetType();                                     // vslot 0x28
    const std::uint32_t developerType = static_cast<std::uint32_t>(growthDeveloper_->GetDeveloperType());   // vslot 0x30
    const std::int32_t built = builtCount_;
    const std::int32_t maximumBuilds = maximumBuildAttempts_;
    const std::int32_t secondLimit = secondAttemptLimit_;
    std::vector<std::uint32_t>& histogram = currentAttempt_.histograms[kHistogramTakeOver];
    auto record = [&histogram](int outcome, int& worst) {
        ++histogram[static_cast<std::size_t>(outcome)];
        if (worst < outcome) worst = outcome;
    };
    int total = 0;
    int worst = 1;
    for (cSC4Lot* lot : existingLots_) {
        const int state = lot->GetState();                                                     // lot vslot 0x108
        const std::int32_t occupantWealth = lot->GetOccupantWealth();                          // lot vslot 0x148
        const std::uint32_t buildingType = lot->GetBuildingType(false);                        // lot vslot 0x170
        const std::int32_t buildingWealth =
            buildingType ? static_cast<std::int32_t>(buildingDevelopment_->GetBuildingProfile(buildingType).wealth) : 0;
        const std::int32_t wealth = growthDeveloper_->GetWealth();                             // vslot 0x40
        if (!(state == kLotStateAbandoned ||
              (allowTakeOver_ && state == kLotStateLivable && occupantWealth < wealth && wealth <= buildingWealth))) continue;
        if (cSC4GrowthDeveloper::ConvertZoneToType(lot->GetZoneType()) != type) continue;     // lot vslot 0xf8
        int outcome;
        if (lot->GetCapacity(developerType, true) == 0) {                                      // lot vslot 0x198
            ++histogram[0xf];
            outcome = 0xf;
        } else {
            outcome = IsLotSuitable(lot, false);
            if (outcome == kTractOutcomeSuccess || outcome == 0x12) {
                CandidateLot* candidate = NewCandidateLot();
                candidate->kind = 1;
                candidate->lot = lot;
                lot->GetLocation(candidate->x, candidate->z);                                 // lot vslot 0xa8
                lot->GetSize(candidate->sizeX, candidate->sizeZ);                              // lot vslot 0xb0
                candidate->facing = lot->GetFacing();                                          // lot vslot 0xb8
                FindExistingCapacity(candidate);
                if (candidate->abandonedCapacity == 0) {
                    candidate->abandonedCapacity = lot->GetAvailableCapacity(developerType, true);   // lot vslot 0x1a0
                    if (candidate->abandonedCapacity == 0 && state == kLotStateLivable)
                        candidate->abandonedCapacity = lot->GetCapacity(developerType, true);
                }
                bool keep = candidate->abandonedCapacity >= 1;
                if (keep && !buildingDevelopment_->CheatIgnoreCaps()) {                        // BDS vslot 0x1c0
                    cISC4LotConfigurationRef* reference = lot->GetLotConfiguration();          // lot vslot 0x48
                    const cSC4LotConfiguration* configuration =
                        reference && lotConfigurationManager_ ? lotConfigurationManager_->GetLotConfiguration(reference->GetID())
                                                              : nullptr;
                    if (configuration) {
                        const std::uint8_t stage = configuration->GetGrowthStage();           // vslot 0x58
                        const BuildingProfile& building = buildingDevelopment_->GetBuildingProfile(lot->GetBuildingType(false));
                        // BDS vslot 0x100 GetDeveloper(purpose, wealth): the developer the building was grown for
                        IGrowthDeveloperApi* owner = buildingDevelopment_->GetDeveloper(building.purpose,
                                                                                        static_cast<std::int32_t>(building.wealth));
                        const int x = candidate->x, z = candidate->z;
                        bool serviced = true;
                        if (owner && owner->GetPowerSupplyCap() < stage && !buildingDevelopment_->CheatIgnorePower() &&
                            g_host->PoweredAt(x, z) <= 1) {                                    // power vslot 0x120 -> grid 0x38
                            record(0x1a, worst);
                            serviced = false;
                        }
                        if (owner && owner->GetWaterSupplyCap() < stage && !buildingDevelopment_->CheatIgnorePlumbing() &&
                            !g_host->IsCellWatered(x, z)) {                                    // plumbing vslot 0x58
                            record(0x19, worst);
                            serviced = false;
                        }
                        if (growthDeveloper_->GetFireCoverageCap() < stage && !g_host->FireProtectedAt(x, z)) {
                            record(0x17, worst);                                               // fire protection vslot 0x28 -> 0x38
                            serviced = false;
                        }
                        if (growthDeveloper_->GetParkCap() < stage && g_host->ParkEffectAt(x, z) == 0) {
                            record(0x18, worst);                                               // aura vslot 0x70 -> grid 0x38
                            serviced = false;
                        }
                        keep = serviced;
                    }
                }
                if (keep) {
                    candidates_.push_back(candidate);
                    total += candidate->abandonedCapacity;
                } else {
                    DeleteCandidateLot(candidate);
                }
                continue;
            }
        }
        if (worst < outcome) worst = outcome;
    }
    if (!candidates_.empty()) {
        if (static_cast<int>(static_cast<float>(maximumBuilds - built) * declineFactor_) < total) {
            ScoreCandidateLots();
            std::sort(candidates_.begin(), candidates_.end(), CompareCandidateLots);   // SortCandidateLots
            int reoccupied = 0;
            for (CandidateLot* candidate : candidates_) {
                cSC4Lot* lot = candidate->lot;
                if (lot->GetState() == kLotStateLivable) lotDeveloper_->AbandonBuilding(lot, 5);   // lot developer vslot 0x40
                lotDeveloper_->ReoccupyBuilding(lot, developerType);                                // vslot 0x48
                builtCount_ += candidate->abandonedCapacity;
                reoccupied += candidate->abandonedCapacity;
                if (static_cast<int>(static_cast<float>(secondLimit - built) * buildBias_) < reoccupied) break;
            }
            ClearCandidateList(candidates_);
            ++histogram[kTractOutcomeSuccess];
            return kTractOutcomeSuccess;
        }
        ClearCandidateList(candidates_);
    }
    if (worst == 1) ++histogram[1];
    return worst;
}

//==================================================================================================
// PARTIAL: the other growth decisions (see the file comment)
//==================================================================================================

int cSC4TractDeveloper::ListCandidateLots_Aggregation() { return 0; }   // PARTIAL (0x100137b04)
int cSC4TractDeveloper::ListCandidateLots_Subdivision() { return 0; }   // PARTIAL (0x100137f44)

int cSC4TractDeveloper::BuildFarm(CandidateLot* candidate) {     // PARTIAL (0x10013845e)
    (void)candidate;
    return 0;
}

void cSC4TractDeveloper::PerformAggregation(CandidateLot* candidate, cSC4Lot*& out) {  // PARTIAL 0x100139dd2
    (void)candidate;
    out = nullptr;
}

void cSC4TractDeveloper::PerformSubdivision(CandidateLot* candidate) {   // PARTIAL (0x10013a04e)
    (void)candidate;
}

bool cSC4TractDeveloper::IsAggregationLegal(CandidateLot* candidate) {   // PARTIAL (0x10013ae4a)
    (void)candidate;
    return false;
}

}  // namespace sc4
