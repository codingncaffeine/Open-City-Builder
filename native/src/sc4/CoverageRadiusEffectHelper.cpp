#include "sc4/CoverageRadiusEffectHelper.h"

#include "gz/StreamOps.h"
#include "sc4/Occupant.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {
namespace {
ICoverageRadiusHost* g_host = nullptr;

// The ring's transform (UpdateEffectForOccupant): the lot centre, a uniform scale of twice the radius; the change counter goes
// to 2 and the flags say translation, plus uniform scale unless the scale is exactly 1.
cS3DTransform RingTransform(const float center[3], float radius) {
    cS3DTransform transform;
    transform.translation[0] = center[0];
    transform.translation[1] = center[1];
    transform.translation[2] = center[2];
    transform.scale = radius + radius;
    transform.flags = transform.scale != 1.0f ? static_cast<std::uint8_t>(transform.flags | 3)
                                              : static_cast<std::uint8_t>((transform.flags & 0xfd) | 1);
    transform.changeCounter = static_cast<std::uint8_t>(transform.changeCounter + 2);
    return transform;
}
}  // namespace

ICoverageRadiusHost* CoverageRadiusHost() { return g_host; }
void SetCoverageRadiusHost(ICoverageRadiusHost* host) { g_host = host; }

cSC4CoverageRadiusEffectHelper::~cSC4CoverageRadiusEffectHelper() {
    for (auto& entry : rings_) delete entry.second.effect;
}

std::uint32_t cSC4CoverageRadiusEffectHelper::GetCategoriesForOccupant(cISC4Occupant* occupant) {
    ICoverageRadiusHost* host = g_host;
    if (!host || !occupant) return 0;
    cISCPropertyHolder* holder = occupant->AsPropertyHolder();
    std::uint32_t categories = host->IsPoliceStation(occupant) ? 1u : 0u;
    if (host->IsFireStation(occupant)) categories |= 2;
    std::uint32_t value = 0;
    if (holder && cSCPropertyHelp::GetPropertyValue(holder, 0x091b42a2, value)) categories |= 4;
    if (holder && cSCPropertyHelp::GetPropertyValue(holder, 0x691b42b3, value)) categories |= 8;
    return categories;
}

void cSC4CoverageRadiusEffectHelper::UpdateEffectForOccupant(cISC4Occupant* occupant, float radius, const char* effectName) {
    ICoverageRadiusHost* host = g_host;
    float center[3] = {0.0f, 0.0f, 0.0f};
    if (!host || !host->LotCenter(occupant, center)) return;
    auto found = rings_.find(occupant);
    if (found == rings_.end()) {
        ICoverageRingEffect* effect = host->CreateEffect(effectName);
        if (!effect) return;
        effect->SetEffectTransform(RingTransform(center, radius));
        effect->Start(0);
        RingEffectState& state = rings_[occupant];
        state.seen = true;
        state.radius = radius;
        state.effect = effect;
        return;
    }
    RingEffectState& state = found->second;
    // as compiled: a changed radius moves the ring but the stored radius stays the first one
    if (radius != state.radius && state.effect) state.effect->SetEffectTransform(RingTransform(center, radius));
    state.seen = true;
}

void cSC4CoverageRadiusEffectHelper::StopAndClear() {
    for (auto& entry : rings_) {
        ICoverageRingEffect* effect = entry.second.effect;
        if (effect && effect->IsStarted()) effect->Stop(0);
        delete effect;
    }
    rings_.clear();
}

void cSC4CoverageRadiusEffectHelper::SetCategories(std::uint32_t categories) {
    categories_ = categories;
    StopAndClear();
}

void cSC4CoverageRadiusEffectHelper::Shutdown() { StopAndClear(); }

void cSC4CoverageRadiusEffectHelper::Init() { RecalcCoverage(); }

void cSC4CoverageRadiusEffectHelper::RecalcCoverage() {
    for (auto& entry : rings_) entry.second.seen = false;
    if (ICoverageRadiusHost* host = g_host) {
        // police and fire at half their capacity, health and education at 70 %; a refused radius is 32 metres
        static const struct { std::uint32_t bit; float share; const char* effect; } kCategories[4] = {
            {1, 0.5f, "PlopMode_Police_Existing"}, {2, 0.5f, "PlopMode_Fire_Existing"},
            {4, 0.7f, "PlopMode_Health_Existing"}, {8, 0.7f, "PlopMode_Education_Existing"}};
        for (int category = 0; category < 4; ++category) {
            if ((categories_ & kCategories[category].bit) == 0) continue;
            std::vector<ICoverageRadiusHost::Building> buildings;
            if (!host->Buildings(category, kCategories[category].share, buildings)) continue;
            for (const auto& building : buildings) {
                UpdateEffectForOccupant(building.occupant, building.hasRadius ? building.radius : 32.0f, kCategories[category].effect);
                building.occupant->Release();
            }
        }
    }
    // the rings no category touched this time are stopped and dropped (their entries stay, without an effect)
    for (auto& entry : rings_) {
        RingEffectState& state = entry.second;
        if (state.seen) continue;
        if (state.effect && state.effect->IsStarted()) state.effect->Stop(0);
        delete state.effect;
        state.effect = nullptr;
    }
}

}  // namespace sc4
