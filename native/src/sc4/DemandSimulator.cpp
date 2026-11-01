#include "sc4/DemandSimulator.h"

#include <cstdio>
#include <cstdlib>

#include "sc4/DBSegment.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace sc4 {

// ---------------------------------------------------------------------------------------------------------
// static data
// ---------------------------------------------------------------------------------------------------------

const std::uint32_t kDemandCensusKeys[24] = {
    0x2111, 0x2112, 0x2113, 0x2114, 0x2121, 0x2122, 0x2123, 0x2124, 0x2131, 0x2132, 0x2133, 0x2134,
    0x1011, 0x1021, 0x1031, 0x3111, 0x3121, 0x3131, 0x3321, 0x3331, 0x4101, 0x4201, 0x4301, 0x4401,
};

const std::uint32_t kDemandSalaryKeys[11] = {
    0x2111, 0x2112, 0x2113, 0x2114, 0x2121, 0x2122, 0x2123, 0x2124, 0x2131, 0x2132, 0x2133,
};

const DemandChannel kDemandChannels[12] = {
    {0x21010, 0x2a67a399, 0xaabaaa9f, 0x2a651010, 0x1010, 0},
    {0x21020, 0xaa67a3a0, 0xaabaaaa0, 0x2a651020, 0x1020, 0},
    {0x21030, 0x8a67a3a4, 0xaabaaaa1, 0x2a651030, 0x1030, 0},
    {0x23110, 0x0a67a3a9, 0xaabaaaa2, 0x2a653110, 0x3110, 0},
    {0x23120, 0xaa67a3ad, 0xaabaaaa3, 0x2a653120, 0x3120, 0},
    {0x23130, 0xca67a3b2, 0xaabaaaa4, 0x2a653130, 0x3130, 0},
    {0x23320, 0xaa67a3b7, 0xaabaaaa5, 0x2a653320, 0x3320, 1},
    {0x23330, 0x2a67a3bc, 0xaabaaaa6, 0x2a653330, 0x3330, 1},
    {0x24100, 0xea67a3bf, 0xaabaaaa7, 0x2a654100, 0x4100, 2},
    {0x24200, 0x0a67a3c4, 0xaabaaaa8, 0x2a654200, 0x4200, 2},
    {0x24300, 0xea67a3c8, 0xaabaaaa9, 0x2a654300, 0x4300, 2},
    {0x24400, 0x8a67a3cc, 0xaabaaaaa, 0x2a654400, 0x4400, 2},
};

namespace {

struct DemandOrdinanceEffect {
    std::uint32_t id = 0;
    float value = 0.0f;
};

bool g_staticInitialized = false;
std::map<std::uint32_t, std::list<DemandAgentEffect>> g_agentEffectMap;
std::map<std::uint32_t, std::list<DemandAgentEffect>> g_connectionEffectMap;
std::map<std::uint32_t, std::list<DemandOrdinanceEffect>> g_ordinanceEffectMap;
std::uint32_t g_linkCount = 0;      // CreateDemandFromExemplar's function-local static
IDemandSimulatorHost* g_host = nullptr;

constexpr float kCensusLifeExpectancyStep = 50.1f;
constexpr float kComparisonEpsilon = 0.01f;
constexpr float kLog2 = 1.442695f;

// The original calls the cISC4OccupationManager's lot query with (x, z, 1); the third argument is a
// "return the lot even when unbuilt" style hint the port forwards to the host.
constexpr int kLotQueryArgument = 1;

}  // namespace

IDemandSimulatorHost* DemandSimulatorHost() { return g_host; }
void SetDemandSimulatorHost(IDemandSimulatorHost* host) { g_host = host; }

std::map<std::uint32_t, std::list<DemandAgentEffect>>& cSC4DemandSimulator::AgentEffectMap() {
    return g_agentEffectMap;
}
std::map<std::uint32_t, std::list<DemandAgentEffect>>& cSC4DemandSimulator::ConnectionEffectMap() {
    return g_connectionEffectMap;
}
void cSC4DemandSimulator::ResetStaticStateForTest() {
    g_staticInitialized = false;
    g_agentEffectMap.clear();
    g_connectionEffectMap.clear();
    g_ordinanceEffectMap.clear();
    g_linkCount = 0;
}

// ---------------------------------------------------------------------------------------------------------
// construction / lifetime
// ---------------------------------------------------------------------------------------------------------

cSC4DemandSimulator::cSC4DemandSimulator() : utilizationWeights_(13, 0.0f) {}

cSC4DemandSimulator::~cSC4DemandSimulator() {
    // The original's destructor frees neither the demand objects nor the two response curves (Shutdown does),
    // it only tears the containers down. Kept: a plain delete leaks whatever Shutdown did not release.
    delete census_;
    census_ = nullptr;
    utilizationWeights_.clear();
    connectedCities_.clear();
    regionalCities_.clear();
    links_.clear();
    demands_.clear();
    previousCensus_.clear();
    censusMap_.clear();
}

bool cSC4DemandSimulator::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    if (iid == 1 || iid == kGZIID_cISC4DemandSimulator) {
        *out = static_cast<cISC4DemandSimulator*>(this);
    } else if (iid == 0xe4fda3d4) {
        *out = static_cast<cIGZSerializable*>(this);
    } else if (iid == 0x452294aa) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

std::uint32_t cSC4DemandSimulator::Release() {
    if (refCount_ < 2) {
        Shutdown();
        delete this;
        return 0;
    }
    return --refCount_;
}

// ---------------------------------------------------------------------------------------------------------
// statics
// ---------------------------------------------------------------------------------------------------------

bool cSC4DemandSimulator::StaticInit(void*) {
    if (!g_staticInitialized) {
        g_staticInitialized = true;
        g_agentEffectMap.clear();
        g_connectionEffectMap.clear();
        g_ordinanceEffectMap.clear();
        if (IDemandSimulatorHost* h = DemandSimulatorHost()) h->AddShutdownCallback(&StaticShutdown, nullptr);
    }
    return true;
}

bool cSC4DemandSimulator::StaticShutdown(void*) {
    if (g_staticInitialized) {
        g_staticInitialized = false;
        g_agentEffectMap.clear();
        g_connectionEffectMap.clear();
        g_ordinanceEffectMap.clear();
    }
    return true;
}

void cSC4DemandSimulator::AddToAgentEffectMap(std::uint32_t buildingType, const cGZPersistResourceKey& key) {
    IDemandSimulatorHost* h = DemandSimulatorHost();
    if (!h) return;
    cISCPropertyHolder* holder = h->LoadExemplar(key);
    if (!holder) return;

    std::list<DemandAgentEffect> list;
    // Property 0x27812841 is a (id, value) u32 table that feeds the target's demand (the original puts the
    // value in the node's +0x18 slot); property 0x27812840 feeds the target's supply (+0x14).
    struct Group { std::uint32_t table, aux; bool demandSide; };
    const Group groups[2] = {{kDemandPropEffectDemand, kDemandPropEffectDemandAux, true},
                             {kDemandPropEffectSupply, kDemandPropEffectSupplyAux, false}};
    for (const Group& group : groups) {
        std::vector<std::uint32_t> values;
        if (!cSCPropertyHelp::GetPropertyValue(holder, group.table, values)) continue;
        std::vector<float> aux;
        const bool hasAux = cSCPropertyHelp::GetPropertyValue(holder, group.aux, aux);
        for (std::size_t i = 0; i + 1 < values.size(); i += 2) {
            DemandAgentEffect effect;
            effect.id = values[i];
            const std::size_t pairIndex = i / 2;
            const float value = (hasAux && pairIndex < aux.size()) ? aux[pairIndex]
                                                                   : static_cast<float>(values[i + 1]);
            if (group.demandSide) effect.demand = value; else effect.supply = value;
            list.push_back(effect);
        }
    }
    if (!list.empty()) g_agentEffectMap[buildingType] = std::move(list);
    h->ReleaseExemplar(holder);
}

void cSC4DemandSimulator::InitDynamicDataFromExemplar(const cGZPersistResourceKey& key, void* context) {
    auto* self = static_cast<cSC4DemandSimulator*>(context);
    IDemandSimulatorHost* h = self ? self->host() : DemandSimulatorHost();
    if (!h) return;
    cISCPropertyHolder* holder = h->LoadExemplar(key);
    if (!holder) return;
    std::uint32_t exemplarType = 0;
    const bool hasType = cSCPropertyHelp::GetPropertyValue(holder, 0x10, exemplarType);
    if (hasType && exemplarType == kExemplarTypeDemand) self->CreateDemandFromExemplar(key.instance, holder);
    h->ReleaseExemplar(holder);
}

// ---------------------------------------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------------------------------------

bool cSC4DemandSimulator::Init() {
    if (initialized_) return true;
    for (auto& [demand, object] : pendingDemandObjects_) {   // the loaded demands, now that every record is read
        void* plain = nullptr;
        if (object->QueryInterface(kGZIID_PlainObject, &plain) && plain) demand->CopyStateFrom(*static_cast<cSC4Demand*>(plain));
        object->Release();
    }
    pendingDemandObjects_.clear();
    StaticInit(this);
    IDemandSimulatorHost* h = host();
    if (!h) return true;

    std::vector<cGZPersistResourceKey> keys;
    h->ExemplarKeysOfType(kDemandTuningType, keys);
    for (const cGZPersistResourceKey& key : keys) InitDynamicDataFromExemplar(key, this);

    if (!census_) {
        census_ = new CensusInfo();
        int cx = 0, cz = 0;
        h->CityCellCount(cx, cz);
        census_->xBase = 0;
        census_->zBase = 0;
        census_->maxX = cx - 1;
        census_->maxZ = cz - 1;
        if (cx < 1) census_->maxX = 0;
        if (cz < 1) census_->maxZ = 0;
    }
    state_ = readFromStream_ ? 0 : 3;
    if (!readFromStream_) {
        value130_ = 0.0f;
        value128_ = 0.0f;
        value12c_ = 0.0f;
        ZeroCensusInfo(previousCensus_);
    }

    ReadSimulatorTunableValues();
    ResetRegionalDemands(true);
    for (const DemandChannel& channel : kDemandChannels) h->HistoryQueueLength(channel.dailyHistory, 1);

    if (!initialized_) {
        h->AddNotification(static_cast<cIGZMessageTarget2*>(this), kDemandMessageLotStateChanged);
        h->AddNotification(static_cast<cIGZMessageTarget2*>(this), kDemandMessageConnectionCreated);
        h->AddNotification(static_cast<cIGZMessageTarget2*>(this), kDemandMessageConnectionDeleted);
        h->AddNotification(static_cast<cIGZMessageTarget2*>(this), kDemandMessageIndexLinks);
        h->AddNotification(static_cast<cIGZMessageTarget2*>(this), kDemandMessageReadTunables);
    }
    static const int kIntervals[4] = {0, 4, 6, 10};
    for (int interval : kIntervals) h->AddSimulatorAgent(static_cast<cIGZMessageTarget2*>(this), interval);

    initialized_ = true;
    DoDemandIndexLinks();
    if (std::getenv("SC4_DEMANDDEBUG")) std::fprintf(stderr, "demand: init %zu exemplar keys, %zu demands\n", keys.size(), demands_.size());
    if (std::getenv("SC4_DEMANDDEBUG")) {
        int unset = 0;
        for (const auto& [id, d] : demands_) if (d && d->QueryActiveDemandMax() > 1e30f) ++unset;
        std::fprintf(stderr, "demand: %d demands without an active maximum\n", unset);
        for (std::uint32_t id : {0x21010u, 0x21020u, 0x21030u, 0x23110u, 0x24200u, 0x1010u, 0x11010u}) {
            auto it = demands_.find(id);
            if (it == demands_.end() || !it->second) { std::fprintf(stderr, "demand: %05x missing\n", id); continue; }
            std::fprintf(stderr, "demand: %05x min %g max %g active %g supply %g demand %g\n", id, it->second->QueryActiveDemandMin(), it->second->QueryActiveDemandMax(),
                         it->second->QueryActiveDemandValue(), it->second->QuerySupplyValue(), it->second->QueryDemandValue());
        }
    }
    return true;
}

bool cSC4DemandSimulator::Shutdown() {
    for (auto& entry : pendingDemandObjects_) entry.second->Release();   // read but never initialised
    pendingDemandObjects_.clear();
    if (!initialized_) return true;
    initialized_ = false;
    IDemandSimulatorHost* h = host();
    if (h) {
        h->RemoveSimulatorAgent(static_cast<cIGZMessageTarget2*>(this));
        h->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kDemandMessageLotStateChanged);
        h->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kDemandMessageConnectionCreated);
        h->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kDemandMessageConnectionDeleted);
        h->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kDemandMessageIndexLinks);
        h->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kDemandMessageReadTunables);
    }
    for (auto& entry : demands_) {
        cSC4Demand* demand = entry.second;
        if (!demand) continue;
        demand->Shutdown();
        delete demand;
    }
    demands_.clear();
    links_.clear();
    delete census_;
    census_ = nullptr;
    censusMap_.clear();
    previousCensus_.clear();
    // Quirk kept: the original frees +0x140 and then clears +0x138, so +0x140 stays dangling.
    delete taxCurve_;
    taxCurve_ = nullptr;
    delete economyCurve_;
    taxCurve_ = nullptr;
    connectedCities_.clear();
    regionalCities_.clear();
    return true;
}

// ---------------------------------------------------------------------------------------------------------
// tunables and exemplars
// ---------------------------------------------------------------------------------------------------------

void cSC4DemandSimulator::ReadSimulatorTunableValues() {
    IDemandSimulatorHost* h = host();
    if (!h) return;

    std::vector<std::uint32_t> buildingTypes;
    h->AllBuildingTypes(buildingTypes);
    for (std::uint32_t type : buildingTypes) {
        cGZPersistResourceKey key;
        if (h->BuildingKeyFromType(type, key)) AddToAgentEffectMap(type, key);
    }

    cGZPersistResourceKey key{kDemandTuningType, kDemandTuningGroup, kDemandTuningInstance};
    cISCPropertyHolder* holder = h->LoadExemplar(key);
    if (!holder) return;

    auto loadCurve = [&](cSC4ResponseCurve*& slot, std::uint32_t property) {
        std::vector<float> values;
        if (!cSCPropertyHelp::GetPropertyValue(holder, property, values)) {
            delete slot;
            slot = nullptr;
            return;
        }
        if (!slot) slot = new cSC4ResponseCurve();
        slot->SetResponseCurveData(values.data(), values.size());
    };
    loadCurve(taxCurve_, kDemandTunePropTaxCurve);
    loadCurve(economyCurve_, kDemandTunePropEconomyCurve);

    historyDays_ = 0x5a;
    cSCPropertyHelp::GetPropertyValue(holder, kDemandTunePropHistoryDays, historyDays_);

    std::vector<float> weights;
    if (cSCPropertyHelp::GetPropertyValue(holder, kDemandTunePropUtilization, weights)) {
        for (std::size_t i = 0; i < 13; ++i) utilizationWeights_[i] = i < weights.size() ? weights[i] : 0.0f;
    } else {
        for (float& value : utilizationWeights_) value = 0.0f;
    }

    airportFactor_ = 1.0f;
    cSCPropertyHelp::GetPropertyValue(holder, kDemandTunePropAirportFactor, airportFactor_);
    seaportFactor_ = 2.0f;
    cSCPropertyHelp::GetPropertyValue(holder, kDemandTunePropSeaportFactor, seaportFactor_);
    connectionFactor_ = 20.0f;
    cSCPropertyHelp::GetPropertyValue(holder, kDemandTunePropConnectionFactor, connectionFactor_);
    trafficFactor_ = 0.1f;
    cSCPropertyHelp::GetPropertyValue(holder, kDemandTunePropTrafficFactor, trafficFactor_);
    seaConnectionFactor_ = 20.0f;
    cSCPropertyHelp::GetPropertyValue(holder, kDemandTunePropSeaConnectionFactor, seaConnectionFactor_);

    std::vector<std::int32_t> salaryWeights;
    if (cSCPropertyHelp::GetPropertyValue(holder, kDemandTunePropSalaryWeights, salaryWeights)) {
        for (std::size_t i = 0; i < 12; ++i)
            salaryWeights_[i] = i < salaryWeights.size() ? salaryWeights[i] : 0;
    } else {
        for (std::int32_t& value : salaryWeights_) value = 100;
    }
    h->ReleaseExemplar(holder);
}

void cSC4DemandSimulator::CreateDemandFromExemplar(std::uint32_t exemplarId, cISCPropertyHolder* holder) {
    if (!holder) return;
    std::uint32_t interfaceId = 0x0797a4e8;
    cSCPropertyHelp::GetPropertyValue(holder, kDemandPropInterfaceId, interfaceId);
    std::uint32_t id = exemplarId;
    cSCPropertyHelp::GetPropertyValue(holder, kDemandPropId, id);

    auto existingIt = demands_.find(id);
    const bool wasNew = existingIt == demands_.end();
    cSC4Demand* demand = nullptr;
    if (!wasNew) {
        demand = existingIt->second;
        if (demand) demand->Init();
    } else if (CreateNewDemand(id, interfaceId, &demand)) {
        // The original releases the reference CreateNewDemand's QueryInterface added.
    }
    if (!demand) return;

    if (wasNew || !readFromStream_) {
        std::int32_t value = 0;
        if (cSCPropertyHelp::GetPropertyValue(holder, kDemandPropSupply, value)) demand->SetSupplyValue(static_cast<float>(value));
        if (cSCPropertyHelp::GetPropertyValue(holder, kDemandPropDemand, value)) demand->SetDemandValue(static_cast<float>(value));
    }
    if (std::getenv("SC4_DEMANDDEBUG") && (id == 0x21010 || id == 0x1010)) {
        std::uint32_t u = 0;
        float f = 0;
        std::fprintf(stderr, "demand: create %05x new %d read %d: demand prop int32 %d uint32 %d float %d -> demand %g\n", id, wasNew ? 1 : 0, readFromStream_ ? 1 : 0,
                     cSCPropertyHelp::GetPropertyValue(holder, kDemandPropDemand, *reinterpret_cast<std::int32_t*>(&u)) ? 1 : 0,
                     cSCPropertyHelp::GetPropertyValue(holder, kDemandPropDemand, u) ? 1 : 0, cSCPropertyHelp::GetPropertyValue(holder, kDemandPropDemand, f) ? 1 : 0,
                     demand->QueryDemandValue());
    }
    {
        std::int32_t value = 0;
        if (cSCPropertyHelp::GetPropertyValue(holder, kDemandPropMin, value)) demand->SetActiveDemandMin(static_cast<float>(value));
        if (cSCPropertyHelp::GetPropertyValue(holder, kDemandPropMax, value)) demand->SetActiveDemandMax(static_cast<float>(value));
    }

    auto addPairLinks = [&](std::uint32_t property, std::uint32_t type, std::uint32_t base) {
        std::vector<std::uint32_t> values;
        if (!cSCPropertyHelp::GetPropertyValue(holder, property, values)) return;
        for (std::size_t i = 0; i + 1 < values.size(); i += 2) {
            DemandEconomyLink link;
            link.id = id;
            link.linkId = g_linkCount + base;
            link.ownerId = id;
            link.targetId = values[i];
            link.type = type;
            link.percent = static_cast<float>(values[i + 1]) * 0.01f;
            links_.emplace(id, link);
            ++g_linkCount;
        }
    };
    addPairLinks(kDemandPropLinkLo, kDemandPropLinkLo, 10000);
    addPairLinks(kDemandPropLinkSupply, kDemandPropLinkSupply, 20000);
    addPairLinks(kDemandPropLinkHi, kDemandPropLinkHi, 30000);

    auto addIdLinks = [&](std::uint32_t property, std::uint32_t type, std::uint32_t base) {
        std::vector<std::uint32_t> values;
        if (!cSCPropertyHelp::GetPropertyValue(holder, property, values)) return;
        for (std::uint32_t target : values) {
            DemandEconomyLink link;
            link.id = id;
            link.linkId = g_linkCount + base;
            link.ownerId = id;
            link.targetId = target;
            link.type = type;
            link.percent = 1.0f;
            links_.emplace(id, link);
            ++g_linkCount;
        }
    };
    addIdLinks(kDemandPropLinkCap, kDemandPropLinkCap, 40000);
    addIdLinks(kDemandPropLinkSum, kDemandPropLinkSum, 50000);

    std::uint8_t regionUse = 0;
    cSCPropertyHelp::GetPropertyValue(holder, kDemandPropRegionUse, regionUse);
    demand->SetRegionUse(regionUse);
}

bool cSC4DemandSimulator::CreateNewDemand(std::uint32_t id, std::uint32_t iid, cSC4Demand** out) {
    (void)iid;   // the port knows a single cISC4Demand implementation (cSC4Demand)
    if (!out) return false;
    cSC4Demand* demand = new cSC4Demand(this);
    demand->SetId(id);
    auto it = demands_.find(id);
    if (it != demands_.end()) {
        cSC4Demand* old = it->second;
        if (old) {
            old->Shutdown();
            delete old;
        }
    }
    demands_[id] = demand;
    *out = demand;
    return true;
}

// ---------------------------------------------------------------------------------------------------------
// demand map queries
// ---------------------------------------------------------------------------------------------------------

cSC4Demand* cSC4DemandSimulator::DemandAt(std::uint32_t key) {
    auto it = demands_.find(key);
    return it == demands_.end() ? nullptr : it->second;
}

cSC4Demand* cSC4DemandSimulator::GetDemand(std::uint32_t id, std::uint32_t keyOffset) {
    if (!initialized_) Init();
    return DemandAt(id + keyOffset);
}

bool cSC4DemandSimulator::AddDemandToSimulator(cSC4Demand* demand) {
    if (!demand) return true;
    const std::uint32_t id = demand->GetId();
    auto it = demands_.find(id);
    if (it != demands_.end()) {
        cSC4Demand* old = it->second;
        if (old) {
            old->Shutdown();
            delete old;
        }
        it->second = demand;
    } else {
        demands_[id] = demand;
    }
    return true;
}

void cSC4DemandSimulator::ZeroCensusInfo(std::map<std::uint32_t, float>& census) {
    for (std::uint32_t key : kDemandCensusKeys) census[key] = 0.0f;
}

// ---------------------------------------------------------------------------------------------------------
// the RCI index links
// ---------------------------------------------------------------------------------------------------------

void cSC4DemandSimulator::DoDemandIndexLinks() {
    bool changed = true;
    while (changed) {
        if (demands_.empty()) return;
        changed = false;
        for (auto& entry : demands_) {
            cSC4Demand* owner = entry.second;
            if (!owner) continue;
            const std::uint32_t key = entry.first;

            float sumSupply = 0.0f;
            float sumDemand = 0.0f;
            bool hasSumLinks = false;
            auto range = links_.equal_range(key);
            for (auto it = range.first; it != range.second; ++it) {
                const DemandEconomyLink& link = it->second;
                cSC4Demand* target = DemandAt(link.targetId);
                if (!target) continue;
                switch (link.type) {
                case 0x47bb3f50: {
                    // accumulate the *targets'* supply/demand, then force them onto the owner below
                    sumSupply += target->QuerySupplyValue();
                    sumDemand += target->QueryDemandValue();
                    hasSumLinks = true;
                    break;
                }
                case 0x47bb3f30: {
                    float value = owner->QueryNewSupply() * link.percent;
                    if (std::fabs(value) > kComparisonEpsilon) {
                        target->AddToDemandValue(value);
                        changed = true;
                    }
                    break;
                }
                case 0x47bb3f31: {
                    float value = owner->QueryNewSupply() * link.percent;
                    if (std::fabs(value) > kComparisonEpsilon) {
                        target->AddToSupplyValue(value);
                        changed = true;
                    }
                    break;
                }
                case 0x47bb3f32: {
                    float value = owner->QueryNewDemand() * link.percent;
                    if (std::fabs(value) > kComparisonEpsilon) {
                        target->AddToDemandValue(value);
                        changed = true;
                    }
                    break;
                }
                case 0x47bb3f41: {
                    const float supply = owner->QuerySupplyValue();
                    const float requested = owner->QueryDemandValue();
                    float cap = supply;
                    if (cap < kComparisonEpsilon) cap = kComparisonEpsilon;
                    float used = requested;
                    if (used < 0.0f) used = 0.0f;
                    const float ratio = used < cap ? used / cap : 1.0f;
                    target->SetDemandCap(ratio);
                    break;
                }
                default:
                    break;
                }
            }
            owner->EndOfCycle();
            if (hasSumLinks) {
                sumSupply -= owner->QuerySupplyValue();
                sumDemand -= owner->QueryDemandValue();
                if (std::fabs(sumSupply) > kComparisonEpsilon) {
                    owner->AddToSupplyValue(sumSupply);
                    changed = true;
                }
                if (std::fabs(sumDemand) > kComparisonEpsilon) {
                    owner->AddToDemandValue(sumDemand);
                    changed = true;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------------
// the tax / economy modifiers
// ---------------------------------------------------------------------------------------------------------

float cSC4DemandSimulator::GetNeutralTaxRate() {
    cSC4Demand* residential = DemandAt(0x1001);
    cSC4Demand* commercial = DemandAt(0x3001);
    cSC4Demand* industrial = DemandAt(0x4001);
    const float r = residential ? residential->QuerySupplyValue() : 0.0f;
    const float c = commercial ? commercial->QuerySupplyValue() : 0.0f;
    const float i = industrial ? industrial->QuerySupplyValue() : 0.0f;
    if (!taxCurve_) return 0.0f;
    return taxCurve_->GetOutputValue((c + r) + i);
}

void cSC4DemandSimulator::UpdateTaxAndEconomyModifiers() {
    IDemandSimulatorHost* h = host();
    if (!h) return;
    const float neutralTax = GetNeutralTaxRate();
    const std::uint32_t today = h->CurrentDayNumber();
    const std::uint32_t fromDay = today - historyDays_;

    for (const DemandChannel& channel : kDemandChannels) {
        float minimum = 0.0f, maximum = 0.0f, average = 0.0f;
        h->HistoryMinMaxAverage(channel.taxHistory, fromDay, today, minimum, maximum, average);
        float taxBase = 0.1f;
        if (0.1f <= neutralTax) taxBase = neutralTax;
        const float curveValue = economyCurve_ ? economyCurve_->GetOutputValue((maximum * 0.1f) / taxBase) : 0.0f;
        cSC4Demand* demand = DemandAt(channel.demandId);
        if (!demand) continue;
        const float ordinance = h->OrdinanceFactor(channel.ordinanceProp, 0);
        demand->SetTaxModifier(curveValue + ordinance + -1.0f);
        float economy = h->NationDemandModifier(channel.purpose);
        if (channel.extraEconomy == 1) economy = value128_ + economy;
        else if (channel.extraEconomy == 2) economy = value12c_ + economy;
        demand->SetEconomyModifier(economy);
    }
}

// ---------------------------------------------------------------------------------------------------------
// the census
// ---------------------------------------------------------------------------------------------------------

void cSC4DemandSimulator::PerformCensus(int x, int z) {
    IDemandSimulatorHost* h = host();
    if (!h) return;
    IDemandLot* lot = h->LotAt(x, z, kLotQueryArgument);
    if (!lot) return;
    if (!lot->HasCapacity()) return;
    const std::uint32_t buildingType = lot->GetBuildingType(true);
    if (!buildingType) return;
    const DemandBuildingProfile* profile = h->BuildingProfile(buildingType);
    if (!profile) return;
    const std::uint32_t purpose = profile->purpose;
    if (purpose > 8) return;

    if ((0x1ecu >> purpose) & 1u) {          // purposes 2, 3, 5, 6, 7, 8
        const std::int32_t wealth = lot->GetOccupantWealth();
        const std::uint32_t developerType = h->DeveloperType(purpose, wealth);
        IDemandDeveloper* developer = h->Developer(developerType);
        if (!developer) return;
        const std::int16_t raw = developer->DesirabilityAt(static_cast<unsigned>(x), static_cast<unsigned>(z));
        float ratio = static_cast<float>(raw) / 100.0f;
        if (ratio < 0.0f) ratio = 0.0f;
        if (ratio > 1.0f) ratio = 1.0f;
        float value = static_cast<float>(lot->GetCapacity(developerType, false)) * ratio;
        censusMap_[developerType + 1] += value;
    } else if (purpose == 1) {
        const std::int32_t wealth = lot->GetOccupantWealth();
        const std::uint32_t developerType = h->DeveloperType(purpose, wealth);
        IDemandDeveloper* developer = h->Developer(developerType);
        if (!developer) return;
        const std::int16_t raw = developer->DesirabilityAt(static_cast<unsigned>(x), static_cast<unsigned>(z));
        float ratio = static_cast<float>(raw) / 100.0f;
        if (ratio < 0.0f) ratio = 0.0f;
        if (ratio > 1.0f) ratio = 1.0f;
        float value = static_cast<float>(lot->GetCapacity(developerType, false)) * ratio;
        value = h->CellWorkforcePercent(static_cast<unsigned>(x), static_cast<unsigned>(z)) * value;
        const float lifeExpectancy = h->CellEQ(static_cast<unsigned>(x), static_cast<unsigned>(z));
        const std::uint32_t byteValue = static_cast<std::uint32_t>(static_cast<std::int32_t>(lifeExpectancy)) & 0xffu;
        const float bucketFloat = static_cast<float>(byteValue) / kCensusLifeExpectancyStep + 1.0f;
        int bucket = static_cast<int>(bucketFloat);
        if (bucket < 1) bucket = 1;
        if (bucket > 4) bucket = 4;
        censusMap_[(static_cast<std::uint32_t>(wealth) << 4) + 0x2100u + static_cast<std::uint32_t>(bucket)] += value;
        censusMap_[(static_cast<std::uint32_t>(wealth) << 4) + 0x1001u] += value;
    }
}

void cSC4DemandSimulator::ApplyCensusDeltas() {
    if (censusMap_.empty()) return;
    auto previous = previousCensus_.begin();
    for (auto it = censusMap_.begin(); it != censusMap_.end(); ++it, ++previous) {
        const float value = it->second;
        const float previousValue = previous != previousCensus_.end() ? previous->second : 0.0f;
        const float delta = value - previousValue;
        if (delta != 0.0f || std::isnan(delta)) {
            cSC4Demand* demand = DemandAt(it->first);
            if (demand) {
                const std::int32_t regionUse = demand->GetRegionUse();
                if (regionUse == 2) demand->AddToDemandValue(delta);
                else if (regionUse == 1) demand->AddToSupplyValue(delta);
                previousCensus_[it->first] = value;
            }
        }
    }
}

void cSC4DemandSimulator::RecordChannelHistory() {
    IDemandSimulatorHost* h = host();
    if (!h) return;
    struct JobHistory { std::uint32_t demandKey, series; };
    static const JobHistory kResidentialJobs[4] = {
        {0x1001, 0x2a4e2003}, {0x1011, 0x4a5e9485}, {0x1021, 0xea5e948a}, {0x1031, 0x2a5e948e},
    };
    static const JobHistory kCommercialJobs[5] = {
        {0x3111, 0x8a5e950e}, {0x3121, 0x0a5e9515}, {0x3131, 0x8a5e954a}, {0x3321, 0x8a5e9552}, {0x3331, 0xea5e9557},
    };
    static const JobHistory kIndustrialJobs[4] = {
        {0x4101, 0x2a5e95a1}, {0x4201, 0x6a5e95a7}, {0x4301, 0x0a5e95ac}, {0x4401, 0xaa5e95b1},
    };
    for (const JobHistory& entry : kResidentialJobs) {
        cSC4Demand* demand = DemandAt(entry.demandKey);
        if (demand) h->HistoryRecord(entry.series, static_cast<std::int32_t>(demand->QueryActiveDemandValue()));
    }
    std::int32_t job0 = static_cast<std::int32_t>(GetJobsBySensus(kCommercialJobs[0].demandKey));
    h->HistoryRecord(kCommercialJobs[0].series, job0);
    std::int32_t job1 = static_cast<std::int32_t>(GetJobsBySensus(kCommercialJobs[1].demandKey));
    h->HistoryRecord(kCommercialJobs[1].series, job1);
    std::int32_t job2 = static_cast<std::int32_t>(GetJobsBySensus(kCommercialJobs[2].demandKey));
    h->HistoryRecord(kCommercialJobs[2].series, job2);
    std::int32_t commercialTotal = job1 + job0 + job2;
    std::int32_t job3 = static_cast<std::int32_t>(GetJobsBySensus(kCommercialJobs[3].demandKey));
    h->HistoryRecord(kCommercialJobs[3].series, job3);
    std::int32_t job4 = static_cast<std::int32_t>(GetJobsBySensus(kCommercialJobs[4].demandKey));
    h->HistoryRecord(kCommercialJobs[4].series, job4);
    commercialTotal += job3;
    commercialTotal += job4;
    h->HistoryRecord(0x0a4e2056, commercialTotal);

    std::int32_t ind0 = static_cast<std::int32_t>(GetJobsBySensus(kIndustrialJobs[0].demandKey));
    h->HistoryRecord(kIndustrialJobs[0].series, ind0);
    std::int32_t ind1 = static_cast<std::int32_t>(GetJobsBySensus(kIndustrialJobs[1].demandKey));
    h->HistoryRecord(kIndustrialJobs[1].series, ind1);
    std::int32_t ind2 = static_cast<std::int32_t>(GetJobsBySensus(kIndustrialJobs[2].demandKey));
    h->HistoryRecord(kIndustrialJobs[2].series, ind2);
    std::int32_t ind3 = static_cast<std::int32_t>(GetJobsBySensus(kIndustrialJobs[3].demandKey));
    h->HistoryRecord(kIndustrialJobs[3].series, ind3);
    std::int32_t industrialTotal = ind0 + ind1 + ind2 + ind3;
    h->HistoryRecord(0x4a4e206b, industrialTotal);

    cSC4Demand* workforce = DemandAt(0x2000);
    const float workforceValue = workforce ? workforce->QueryDemandValue() : 0.0f;
    h->HistoryRecord(kDemandHistoryWorkforce, static_cast<std::int32_t>(workforceValue));
    float employment = workforceValue - static_cast<float>(commercialTotal);
    employment -= static_cast<float>(industrialTotal);
    if (employment < 0.0f) employment = 0.0f;
    h->HistoryRecord(kDemandHistoryEmployment, static_cast<std::int32_t>(employment));

    // salary average: weighted integer sum divided by the unsigned value sum (the original divides unsigned)
    std::uint64_t valueSum = 0;
    std::uint64_t weightedSum = 0;
    for (int i = 0; i < 11; ++i) {
        cSC4Demand* demand = DemandAt(kDemandSalaryKeys[i]);
        const std::int32_t value = demand ? static_cast<std::int32_t>(demand->QueryActiveDemandValue()) : 0;
        valueSum += static_cast<std::uint64_t>(static_cast<std::int64_t>(value));
        const std::int32_t product = value * salaryWeights_[i];
        weightedSum += static_cast<std::uint64_t>(static_cast<std::uint32_t>(product));
    }
    const std::uint64_t average = valueSum == 0 ? 0 : weightedSum / valueSum;
    h->HistoryRecord(kDemandHistorySalary, static_cast<std::int32_t>(average & 0xffffffffu));
}

void cSC4DemandSimulator::DoIdle(std::uint32_t elapsedMs) {
    IDemandSimulatorHost* h = host();
    if (!h) return;
    switch (state_) {
    case 0:
        UpdateTaxAndEconomyModifiers();
        ZeroCensusInfo(censusMap_);
        if (census_) {
            census_->x = 0;
            census_->z = 0;
            census_->state = 0;
            if (census_->xBase > 0 || census_->zBase > 0 || census_->maxX < 0 || census_->maxZ < 0)
                census_->state = 2;
        }
        state_ = 1;
        return;
    case 2:
        RecordChannelHistory();
        state_ = 3;
        return;
    case 3:
        return;
    default:
        break;
    }

    // state 1: walk every city cell within the elapsed-millisecond budget
    const std::uint64_t deadline =
        h->NowCycles() + static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<float>(static_cast<int>(elapsedMs)) *
                                                                              h->MillisecondsToCycles()));
    bool timedOut = false;
    int limit = 0x400;
    CensusInfo* census = census_;
    if (!census) {
        state_ = 2;
        return;
    }
    for (;;) {
        limit += 0x400;
        if ((limit & 0x3ff) == 0 && h->NowCycles() > deadline) {
            timedOut = true;
            break;
        }
        if (timedOut) break;
        int x = 0;
        if (census->state == 0) {
            census->state = 1;
            x = census->x;
        } else if (census->state == 2) {
            break;
        } else {
            x = census->x + census->step;
            census->x = x;
            if (census->maxX < x) {
                x = census->xBase;
                census->x = x;
                const int z = census->z + census->step;
                census->z = z;
                if (census->maxZ < z) break;
            }
        }
        PerformCensus(x, census->z);
    }
    census->state = 2;
    ApplyCensusDeltas();
    state_ = 2;
}

// ---------------------------------------------------------------------------------------------------------
// daily / monthly simulation
// ---------------------------------------------------------------------------------------------------------

void cSC4DemandSimulator::DoDailySimulation() {
    DoDemandIndexLinks();
    IDemandSimulatorHost* h = host();
    if (!h) return;
    for (const DemandChannel& channel : kDemandChannels) {
        cSC4Demand* demand = DemandAt(channel.demandId);
        if (!demand) continue;
        h->HistoryRecord(channel.dailyHistory, static_cast<std::int32_t>(demand->QueryActiveDemandValue()));
    }
}

void cSC4DemandSimulator::DoMonthlySimulation() {
    EvaluateConnectionUtilization();
    ExportDemand();
    if (state_ == 3) state_ = 0;
    if (std::getenv("SC4_DEMANDDEBUG")) {
        int shown = 0, nonzero = 0;
        for (const auto& [id, d] : demands_) {
            if (!d || (d->QuerySupplyValue() == 0.0f && d->QueryDemandValue() == 0.0f && d->QueryActiveDemandValue() == 0.0f)) continue;
            ++nonzero;
            if (shown++ < 25) std::fprintf(stderr, "demand: month %05x supply %g demand %g active %g [%g..%g] econ %g\n", id, d->QuerySupplyValue(),
                                           d->QueryDemandValue(), d->QueryActiveDemandValue(), d->QueryActiveDemandMin(), d->QueryActiveDemandMax(), d->QueryEconomyModifier());
        }
        std::fprintf(stderr, "demand: month: %d of %zu demands non-zero, state %d\n", nonzero, demands_.size(), state_);
    }
}

void cSC4DemandSimulator::SimulatorBegin() {
    for (auto& entry : demands_)
        if (entry.second) entry.second->SimulationBegin();
}

void cSC4DemandSimulator::ResetRegionalDemands(bool reset) {
    IDemandSimulatorHost* h = host();
    if (!h) return;
    std::list<IDemandRegionCity*> allCities;
    h->AllConnectedCities(allCities);
    h->ConnectedCities(connectedCities_);
    IDemandRegionCity* current = h->CurrentRegionCity();
    for (auto& entry : demands_) {
        cSC4Demand* demand = entry.second;
        if (!demand) continue;
        const std::int32_t regionUse = demand->GetRegionUse();
        if (regionUse != 1 && regionUse != 2) continue;
        const std::uint32_t id = demand->GetId();
        auto it = demands_.find(id + 0x10000u);
        if (it == demands_.end() || !it->second) continue;
        cSC4Demand* other = it->second;
        float sumLocal = 0.0f;
        float sumAll = 0.0f;
        for (IDemandRegionCity* city : connectedCities_)
            if (city != current) sumLocal += static_cast<float>(city->GetExtrapolatedPopulation(id));
        for (IDemandRegionCity* city : allCities)
            if (city != current) sumAll += static_cast<float>(city->GetExtrapolatedPopulation(id));
        float value = sumAll - sumLocal;
        if (regionUse == 1) {
            if (reset) value = sumAll - (other->QuerySupplyValue() + other->QueryNewSupply());
            other->AddToSupplyValue(value);
        } else {
            if (reset) value = sumAll - (other->QueryDemandValue() + other->QueryNewDemand());
            other->AddToDemandValue(value);
        }
    }
    regionalCities_.swap(allCities);
}

// ---------------------------------------------------------------------------------------------------------
// connection utilization
// ---------------------------------------------------------------------------------------------------------

void cSC4DemandSimulator::EvaluateConnectionUtilization() {
    IDemandSimulatorHost* h = host();
    if (!h) return;
    const float old130 = value130_;
    const float old134 = value134_;
    value130_ = 0.0f;
    value134_ = 0.0f;

    std::vector<IDemandConnection*> connections;
    h->AllConnections(connections);
    for (IDemandConnection* connection : connections) {
        const std::uint32_t type = connection->GetType();
        if (type < 0xd && ((0x150eu >> type) & 1u))
            value134_ += static_cast<float>(h->TrafficArrived(connection->PropertyHolder(), 3));
    }
    value134_ = connectionFactor_ * value134_;

    std::list<IDemandRegionCity*> connectedCities;
    h->ConnectedCities(connectedCities);   // the original asks for the list and drops it

    std::set<const void*> setA;
    std::vector<IDemandConnection*> list;
    h->ConnectionsOfType(3, list);
    for (IDemandConnection* connection : list) setA.insert(connection->CityOnOtherSide());
    h->ConnectionsOfType(0xc, list);
    for (IDemandConnection* connection : list) setA.insert(connection->CityOnOtherSide());

    auto collect = [&](std::uint32_t type, const std::set<const void*>& excluded) {
        std::set<const void*> result;
        std::vector<IDemandConnection*> ofType;
        h->ConnectionsOfType(type, ofType);
        for (IDemandConnection* connection : ofType) {
            const void* city = connection->CityOnOtherSide();
            if (!excluded.count(city)) result.insert(city);
        }
        return result;
    };

    const std::set<const void*> setB = collect(8, setA);
    const std::set<const void*> setC = collect(2, [&] {
        std::set<const void*> merged = setA;
        merged.insert(setB.begin(), setB.end());
        return merged;
    }());
    const std::set<const void*> excludedD = [&] {
        std::set<const void*> merged = setA;
        merged.insert(setB.begin(), setB.end());
        merged.insert(setC.begin(), setC.end());
        return merged;
    }();
    const std::set<const void*> setD = collect(0xa, excludedD);

    std::set<const void*> setE;
    h->ConnectionsOfType(1, list);
    for (IDemandConnection* connection : list) setE.insert(connection->CityOnOtherSide());

    std::set<const void*> setF;
    for (std::uint32_t type : {7u, 0xbu, 6u}) {
        h->ConnectionsOfType(type, list);
        for (IDemandConnection* connection : list) setF.insert(connection->CityOnOtherSide());
    }

    const float countA = static_cast<float>(std::max(1, static_cast<int>(setA.size())));
    const float countB = static_cast<float>(std::max(1, static_cast<int>(setB.size())));
    const float countC = static_cast<float>(std::max(1, static_cast<int>(setC.size())));
    const float countD = static_cast<float>(std::max(1, static_cast<int>(setD.size())));
    const float countE = static_cast<float>(std::max(1, static_cast<int>(setE.size())));
    const float countF = static_cast<float>(std::max(1, static_cast<int>(setF.size())));

    float sum = std::log(countA) * utilizationWeights_[2];
    sum = std::log(countB) * utilizationWeights_[6] + sum;
    sum = std::log(countC) * utilizationWeights_[0] + sum;
    sum = std::log(countD) * utilizationWeights_[11] + sum;
    sum = std::log(countE) * utilizationWeights_[1] + sum;
    value130_ = (std::log(countF) * utilizationWeights_[7] + sum) * kLog2 + value130_;

    const float airportTrips = static_cast<float>(h->AirportTrips());
    value130_ = airportFactor_ * airportTrips + value130_;
    value134_ = airportTrips * trafficFactor_ + value134_;
    const float seaportTrips = static_cast<float>(h->SeaportTrips());
    value134_ = seaportFactor_ * seaportTrips + value134_;
    value130_ = seaportTrips * seaConnectionFactor_ + value130_;

    cSC4Demand* utilizationSupply = DemandAt(0x4800);
    cSC4Demand* utilizationDemand = DemandAt(0x4900);
    cSC4Demand* utilizationOther = DemandAt(0x3800);
    if (utilizationSupply) utilizationSupply->AddToSupplyValue(value134_ - old134);
    if (utilizationDemand) utilizationDemand->AddToSupplyValue(value134_ - old134);
    if (utilizationOther) utilizationOther->AddToSupplyValue(value130_ - old130);
}

// ---------------------------------------------------------------------------------------------------------
// regional export
// ---------------------------------------------------------------------------------------------------------

void cSC4DemandSimulator::ExtrapolateResidentialCensus(IDemandRegionCity* city, std::uint32_t demandType, float amount) {
    if (city == nullptr) return;
    if (demandType < 0x1010 || demandType > 0x1030) return;
    if ((static_cast<std::uint32_t>(demandType - 0x1010) & 0x1fu) != 0) return;   // 0x1010 / 0x1020 / 0x1030
    const int population = city->GetPopulation(demandType);
    const int nextPopulation = city->GetPopulation(demandType + 1);
    float ratio = 0.8f;
    if (population > 0) ratio = static_cast<float>(nextPopulation) / static_cast<float>(population);
    float scaled = ratio * city->WorkforcePercentage() * amount;

    const std::uint32_t key1 = demandType + 0x1101u;
    const std::uint32_t key2 = demandType + 0x1102u;
    const std::uint32_t key3 = demandType + 0x1103u;
    const std::uint32_t key4 = demandType + 0x1104u;
    int value1 = city->GetPopulation(key1);
    int value2 = city->GetPopulation(key2);
    int value3 = city->GetPopulation(key3);
    int value4 = city->GetPopulation(key4);
    int total = value2 + value1 + value3 + value4;
    const std::uint32_t addOn = total == 0 ? 1u : 0u;
    total += static_cast<int>(addOn);
    value1 += static_cast<int>(addOn);
    const float totalFloat = static_cast<float>(total);
    const float share1 = (static_cast<float>(value1) * scaled) / totalFloat;
    const float share2 = (static_cast<float>(value2) * scaled) / totalFloat;
    const float share3 = (static_cast<float>(value3) * scaled) / totalFloat;
    const float share4 = (static_cast<float>(value4) * scaled) / totalFloat;

    if (cSC4Demand* d = DemandAt(demandType + 0x11101u)) d->AddToSupplyValue(share1);
    if (cSC4Demand* d = DemandAt(demandType + 0x11102u)) d->AddToSupplyValue(share2);
    if (cSC4Demand* d = DemandAt(demandType + 0x11103u)) d->AddToSupplyValue(share3);
    if (cSC4Demand* d = DemandAt(demandType + 0x11104u)) d->AddToSupplyValue(share4);
    city->ExtrapolateGrowth(key1, share1);
    city->ExtrapolateGrowth(key2, share2);
    city->ExtrapolateGrowth(key3, share3);
    city->ExtrapolateGrowth(key4, share4);
}

void cSC4DemandSimulator::ExtrapolateBusinessCensus(IDemandRegionCity* city, std::uint32_t demandType, float amount) {
    if (city == nullptr) return;
    bool known = false;
    if (demandType < 0x3330) {
        if (demandType >= 0x3110 && demandType <= 0x3130 &&
            (((static_cast<std::uint32_t>(demandType - 0x3110) & 0x1fu) == 0) ||
             ((static_cast<std::uint32_t>(demandType - 0x3110) & 0x1fu) == 0x10) ||
             ((static_cast<std::uint32_t>(demandType - 0x3110) & 0x1fu) == 0x20)))
            known = true;
        if (demandType == 0x3320) known = true;
    } else if (demandType < 0x4200) {
        if (demandType == 0x3330 || demandType == 0x4100) known = true;
    } else if (demandType == 0x4200 || demandType == 0x4400 || demandType == 0x4300) {
        known = true;
    }
    if (!known) return;

    const std::uint32_t next = demandType + 1;
    const int population = city->GetPopulation(demandType);
    const int nextPopulation = city->GetPopulation(next);
    float ratio = 0.8f;
    if (population > 0) ratio = static_cast<float>(nextPopulation) / static_cast<float>(population);
    float scaled = ratio * amount;

    if (cSC4Demand* d = DemandAt(demandType + 0x10001u)) d->AddToSupplyValue(scaled);
    city->ExtrapolateGrowth(next, scaled);

    float jobs[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    CalculateJobsPerUnitOfDemand(jobs, next, 0);
    float commercial = jobs[1] * scaled;
    float industrial = jobs[2] * scaled;
    float other = scaled * jobs[3];
    if (cSC4Demand* d = DemandAt(0x12010u)) d->AddToDemandValue(commercial);
    city->ExtrapolateGrowth(0x2010u, commercial);
    if (cSC4Demand* d = DemandAt(0x12020u)) d->AddToDemandValue(industrial);
    city->ExtrapolateGrowth(0x2020u, industrial);
    if (cSC4Demand* d = DemandAt(0x12030u)) d->AddToDemandValue(other);
    city->ExtrapolateGrowth(0x2030u, other);
}

void cSC4DemandSimulator::ExportDemand() {
    IDemandSimulatorHost* h = host();
    if (!h) return;
    IDemandRegionCity* region = h->CurrentRegionCity();
    if (region) {
        int x = 0, z = 0;
        int width = 0, height = 0;
        region->GetPosition(x, z);
        region->GetCitySize(width, height);
    }
    std::vector<IDemandRegionCity*> cities(connectedCities_.begin(), connectedCities_.end());

    std::map<const IDemandRegionCity*, float> taxRates;
    std::vector<IDemandDeveloper*> developers;
    h->Developers(developers);

    std::int32_t residentialTotal = 0;
    std::int32_t commercialTotal = 0;
    std::int32_t industrialTotal = 0;
    const std::uint32_t today = h->CurrentDayNumber();

    for (IDemandDeveloper* developer : developers) {
        if (!developer) continue;
        const std::uint32_t developerType = developer->GetDeveloperType();
        const std::int32_t lastBuild = h->DayOfLastSuccessfulBuild(developerType);
        const std::uint32_t elapsed = today - static_cast<std::uint32_t>(lastBuild);
        if (elapsed < 0x42u) continue;
        cSC4Demand* source = developer->GetSourceDemand();
        if (!source) continue;
        float remaining = source->QueryDemandValue() - source->QuerySupplyValue();
        if (!(remaining >= 10.0f)) continue;

        taxRates.clear();
        for (IDemandRegionCity* city : cities) {
            if (!city) continue;
            int x = 0, z = 0, width = 0, height = 0;
            city->GetPosition(x, z);
            city->GetCitySize(width, height);
            const float tax = city->GetTaxRate(developerType);
            taxRates[city] = tax * 3.0f;
        }
        std::vector<IDemandRegionCity*> sorted = cities;
        std::sort(sorted.begin(), sorted.end(), [&taxRates](IDemandRegionCity* a, IDemandRegionCity* b) {
            return taxRates[a] < taxRates[b];
        });

        for (IDemandRegionCity* city : sorted) {
            if (!city) continue;
            int allowed = city->GetAllowableExtrapolation(developerType);
            if (allowed > 1000) allowed = 1000;
            const int amount = static_cast<float>(allowed) <= remaining ? allowed : static_cast<int>(remaining);
            if (amount > 9) {
                remaining = remaining - static_cast<float>(amount);
                city->ExtrapolateGrowth(developerType, static_cast<float>(amount));
                if (cSC4Demand* d = DemandAt(developerType + 0x10000u))
                    d->AddToSupplyValue(static_cast<float>(amount));
                switch (developer->GetType()) {
                case 1: residentialTotal += amount; break;
                case 2: commercialTotal += amount; break;
                case 3: industrialTotal += amount; break;
                default: break;
                }
                ExtrapolateResidentialCensus(city, developerType, static_cast<float>(amount));
                ExtrapolateBusinessCensus(city, developerType, static_cast<float>(amount));
            }
            if (!(remaining >= 10.0f)) break;
        }
    }
    h->HistoryRecord(kDemandHistoryResidential, residentialTotal);
    h->HistoryRecord(kDemandHistoryCommercial, commercialTotal);
    h->HistoryRecord(kDemandHistoryIndustrial, industrialTotal);
}

// ---------------------------------------------------------------------------------------------------------
// per-cell jobs / area effects
// ---------------------------------------------------------------------------------------------------------

void cSC4DemandSimulator::CalculateJobsPerUnitOfDemand(float* out, std::uint32_t demandId, std::uint32_t kind) {
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = 0.0f;
    IDemandSimulatorHost* h = host();
    if (!h) return;
    cGZPersistResourceKey key{0x6534284a, 0xc7bb4816, demandId + kind};
    cISCPropertyHolder* holder = h->LoadExemplar(key);
    if (!holder) return;
    std::vector<std::uint32_t> values;
    if (cSCPropertyHelp::GetPropertyValue(holder, kDemandJobsProperty, values)) {
        for (std::size_t i = 0; i + 1 < values.size(); i += 2) {
            const std::uint32_t property = values[i];
            if (property == kind + 0x2010u) out[1] = static_cast<float>(values[i + 1]) / 100.0f;
            else if (property == kind + 0x2020u) out[2] = static_cast<float>(values[i + 1]) / 100.0f;
            else if (property == kind + 0x2030u) out[3] = static_cast<float>(values[i + 1]) / 100.0f;
        }
    }
    h->ReleaseExemplar(holder);
}

std::int64_t cSC4DemandSimulator::GetJobsBySensus(std::uint32_t demandKey) {
    if (demandKey >= 0x1011 && demandKey <= 0x1031 && ((demandKey - 0x1011) % 0x10) == 0) return 0;
    float jobs[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    CalculateJobsPerUnitOfDemand(jobs, demandKey, 0);
    const float perUnit = jobs[1] + jobs[2] + jobs[3];
    cSC4Demand* demand = DemandAt(demandKey);
    if (!demand) return 0;
    return static_cast<std::int64_t>(demand->QueryActiveDemandValue() * perUnit);
}

void cSC4DemandSimulator::AddAgentEffect(std::uint32_t type, bool add, const SC4Percentage& scale) {
    auto it = g_agentEffectMap.find(type);
    if (it == g_agentEffectMap.end()) return;
    for (const DemandAgentEffect& effect : it->second) {
        cSC4Demand* demand = DemandAt(effect.id);
        if (!demand) continue;
        float value = effect.supply;
        if (value != 0.0f || std::isnan(value)) {
            value *= scale.value;
            if (!add) value = -value;
            demand->AddToSupplyValue(value);
        }
        value = effect.demand;
        if (value != 0.0f || std::isnan(value)) {
            value *= scale.value;
            if (!add) value = -value;
            demand->AddToDemandValue(value);
        }
    }
}

void cSC4DemandSimulator::UpdateOccupantEffects(std::uint32_t occupantType, const SC4Percentage& oldFunding,
                                                const SC4Percentage& newFunding) {
    if (oldFunding.value == newFunding.value) return;
    AddAgentEffect(occupantType, false, oldFunding);
    AddAgentEffect(occupantType, true, newFunding);
}

void cSC4DemandSimulator::UpdateCapacity(std::uint32_t buildingType, std::uint32_t wealth, bool add) {
    IDemandSimulatorHost* h = host();
    if (!h) return;
    const DemandBuildingProfile* profile = h->BuildingProfile(buildingType);
    if (!profile || profile->purpose == 0) return;
    const std::uint32_t developerType = h->DeveloperType(profile->purpose, static_cast<std::int32_t>(wealth));
    std::int32_t total = 0;
    for (const auto& capacities : profile->capacities) {
        auto it = capacities.find(developerType);
        if (it != capacities.end()) total += static_cast<std::int32_t>(it->second);
    }
    cSC4Demand* demand = DemandAt(developerType);
    if (!demand) return;
    const std::int32_t delta = add ? total : -total;
    demand->AddToSupplyValue(static_cast<float>(delta));
}

void cSC4DemandSimulator::GetLocalPopulationSummary(std::map<std::uint32_t, int>& out) const {
    out.clear();
    for (const auto& entry : demands_) {
        cSC4Demand* demand = entry.second;
        if (!demand) continue;
        const std::int32_t regionUse = demand->GetRegionUse();
        if (regionUse == 2) out[entry.first] = static_cast<int>(demand->QueryDemandValue());
        else if (regionUse == 1) out[entry.first] = static_cast<int>(demand->QuerySupplyValue());
    }
}

// ---------------------------------------------------------------------------------------------------------
// messages
// ---------------------------------------------------------------------------------------------------------

bool cSC4DemandSimulator::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    const std::uint32_t type = message->GetType();
    if (std::getenv("SC4_DEMANDDEBUG")) {
        static std::map<std::uint32_t, int> seen;
        const int n = ++seen[type];
        if (n <= 3 || n % 50 == 0) std::fprintf(stderr, "demand: message %08x #%d\n", type, n);
    }
    auto* payload = static_cast<cIGZMessage2Standard*>(message);
    switch (type) {
    case kDemandMessageInterval0:
        SimulatorBegin();
        break;
    case kDemandMessageDay:
        DoIdle(kDemandDefaultDayInterval);
        DoDailySimulation();
        break;
    case kDemandMessageMonth:
        DoMonthlySimulation();
        break;
    case kDemandMessageIdleTime:
        DoIdle(static_cast<std::uint32_t>(payload->GetData3()));
        break;
    case kDemandMessageConnectionCreated:
    case kDemandMessageConnectionDeleted:
        // The original reads all three payload words and discards them.
        (void)payload->GetData1();
        (void)payload->GetData2();
        (void)payload->GetData3();
        ResetRegionalDemands(false);
        break;
    case kDemandMessageReadTunables:
        ReadSimulatorTunableValues();
        break;
    case kDemandMessageIndexLinks:
        DoDemandIndexLinks();
        DoDemandIndexLinks();
        break;
    case kDemandMessageLotStateChanged: {
        cIGZUnknown* unknown = payload->GetIGZUnknown();
        if (!unknown) break;
        auto* lot = static_cast<IDemandLotState*>(unknown);
        const std::uint64_t oldState = payload->GetData1();
        const std::uint64_t newState = payload->GetData2();
        std::uint32_t buildingType = static_cast<std::uint32_t>(payload->GetData3());
        (void)payload->GetData4();
        const bool oldOccupied = (oldState & 0xfeu) == 2u;
        const bool newOccupied = (newState & 0xfeu) == 2u;
        if (oldOccupied == newOccupied) break;
        if (buildingType == 0) {
            buildingType = lot->GetBuildingType(true);
            if (buildingType == 0) break;
        }
        bool ignore = false;
        IDemandOccupant* building = lot->GetBuilding();
        if (building) {
            cISCPropertyHolder* holder = building->AsPropertyHolder();
            if (holder) cSCPropertyHelp::GetPropertyValue(holder, kDemandPropBuildingIgnoreCapacity, ignore);
            if (ignore) break;
        }
        AddAgentEffect(buildingType, newOccupied, SC4Percentage(1.0f));
        UpdateCapacity(buildingType, static_cast<std::uint32_t>(oldState & 0xfeu), newOccupied);
        break;
    }
    default:
        break;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------
// persistence
// ---------------------------------------------------------------------------------------------------------

namespace {
// The serializable view of one demand in a city file (class 0x8990c12d): cSC4Demand is plain C++ in the port and owned
// by the simulator's map, so the view only borrows it; the serializer's references keep the view alive through the save.
class DemandSerializable final : public cIGZSerializable {
public:
    explicit DemandSerializable(const cSC4Demand* demand) : demand_(const_cast<cSC4Demand*>(demand)) {}
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cIGZUnknown || iid == kGZIID_cIGZSerializable) { *out = static_cast<cIGZSerializable*>(this); AddRef(); return true; }
        if (iid == kGZIID_PlainObject) { *out = demand_; return true; }
        *out = nullptr;
        return false;
    }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override { if (--refs_ == 0) { delete this; return 0; } return refs_; }
    bool Write(cIGZOStream& stream) const override { return demand_->Write(stream); }
    bool Read(cIGZIStream& stream) override { return demand_->Read(stream); }
    std::uint32_t GetGZCLSID() const override { return demand_->GetGZCLSID(); }
private:
    cSC4Demand* demand_;
    std::uint32_t refs_ = 0;
};
}  // namespace

bool cSC4DemandSimulator::Write(cIGZOStream& stream) const {
    if (stream.GetError()) return false;
    stream.SetUint16(2);
    stream.SetFloat32(value128_);
    stream.SetFloat32(value12c_);
    stream.SetFloat32(value130_);
    stream.SetFloat32(value134_);
    stream.SetUint32(static_cast<std::uint32_t>(demands_.size()));
    void* dbStream = nullptr;
    const bool references = stream.QueryInterface(kGZIID_cISC4DBSegmentOStream, &dbStream) && dbStream;
    if (dbStream) static_cast<cIGZUnknown*>(dbStream)->Release();
    for (const auto& entry : demands_) {
        stream.SetUint32(entry.first);
        if (!entry.second) return false;
        if (references) {   // a city file: each demand is its own object, referenced here (as Read reads it)
            cRZAutoRefCount<cIGZSerializable> view(new DemandSerializable(entry.second));
            if (!SC4WriteGZSerializable(stream, view.get())) return false;
        } else if (!entry.second->Write(stream)) {
            return false;
        }
    }
    stream.SetUint32(static_cast<std::uint32_t>(previousCensus_.size()));
    for (const auto& entry : previousCensus_) {
        stream.SetUint32(entry.first);
        stream.SetFloat32(entry.second);
    }
    return !stream.GetError();
}

bool cSC4DemandSimulator::Read(cIGZIStream& stream) {
    if (stream.GetError()) return false;
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version >= 3) return false;
    readFromStream_ = true;
    if (version != 2) {
        std::uint32_t legacy = 0;
        stream.GetUint32(legacy);
        stream.GetUint32(legacy);
        stream.GetUint32(legacy);
        stream.GetUint32(legacy);
    }
    stream.GetFloat32(value128_);
    stream.GetFloat32(value12c_);
    stream.GetFloat32(value130_);
    stream.GetFloat32(value134_);
    std::uint32_t demandCount = 0;
    stream.GetUint32(demandCount);
    // In a city file every demand is its own object of class 0x8990c12d and the simulator stores a reference to it (id + class id);
    // plain streams (the unit tests) carry the demand inline.
    void* dbStream = nullptr;
    const bool references = stream.QueryInterface(kGZIID_cISC4DBSegmentIStream, &dbStream) && dbStream;
    if (dbStream) static_cast<cIGZUnknown*>(dbStream)->Release();
    for (std::uint32_t i = 0; i < demandCount; ++i) {
        std::uint32_t id = 0;
        stream.GetUint32(id);
        auto* demand = new cSC4Demand(this);
        if (references) {
            void* raw = nullptr;
            if (!SC4ReadGZSerializable(stream, kGZIID_cIGZSerializable, &raw, true)) {
                delete demand;
                return false;
            }
            // The referenced object's own record may come later in the file (the original keeps the object itself, so it
            // sees the values whenever they arrive); the state is copied once the city is loaded, in Init.
            auto* object = static_cast<cIGZSerializable*>(raw);
            if (object) pendingDemandObjects_.emplace_back(demand, object);   // keeps the reference SC4ReadGZSerializable gave
        } else if (!demand->Read(stream)) {
            delete demand;
            return false;
        }
        demands_[id] = demand;
    }
    std::uint32_t censusCount = 0;
    stream.GetUint32(censusCount);
    for (std::uint32_t i = 0; i < censusCount; ++i) {
        std::uint32_t key = 0;
        float value = 0.0f;
        stream.GetUint32(key);
        stream.GetFloat32(value);
        previousCensus_[key] = value;
    }
    return !stream.GetError();
}

}  // namespace sc4
