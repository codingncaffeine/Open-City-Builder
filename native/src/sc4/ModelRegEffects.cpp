#include "sc4/ModelRegEffects.h"

#include <cstring>
#include <strings.h>

#include "s3d/S3DMath.h"

namespace sc4 {

namespace {
IRegEffectsHost* g_host = nullptr;

void StopAndRelease(cSC4ModelRegEffects::tRegEffect& entry, int transition) {
    if (!entry.effect) return;
    entry.effect->Stop(transition);
    entry.effect->Release();
    entry.effect = nullptr;
}
}  // namespace

IRegEffectsHost* RegEffectsHost() { return g_host; }
void SetRegEffectsHost(IRegEffectsHost* host) { g_host = host; }

cSC4ModelRegEffects::~cSC4ModelRegEffects() {
    ClearEffectsList();
    if (model_) model_->Release();
}

// @ 0x0016055c
bool cSC4ModelRegEffects::Init() {
    if (model_) InitEffectsList();
    return true;
}

// @ 0x00160de0: every effect stops at once and is let go.
void cSC4ModelRegEffects::ClearEffectsList() {
    for (auto& entry : effects_) StopAndRelease(entry.second, 0);
    effects_.clear();
}

// @ 0x00160e3e
void cSC4ModelRegEffects::Shutdown() {
    ClearEffectsList();
    if (model_) model_->Release();
    model_ = nullptr;
}

// @ 0x0016057c: another model drops the effects of the old one.
void cSC4ModelRegEffects::SetModel(IAutomatonModel* model) {
    if (model_ == model) return;
    Shutdown();
    if (model) model->AddRef();
    model_ = model;
}

// @ 0x001605ae: the effect rides on the model's registration point of that name, or on the model's origin without one.
IRegEffect* cSC4ModelRegEffects::AddRegEffect(const std::string& point, const std::string& effect) {
    const RegPointFrames* frames = nullptr;
    if (model_ && g_host) {
        if (const RegPointMap* points = g_host->RegPoints(model_)) {
            auto found = points->find(point);
            if (found != points->end()) frames = &found->second;
        }
    }
    return AddRegEffect(point, effect, frames);
}

// @ 0x00160622: one effect per point: a point that has one already loses it first. An effect the effects manager does not
// know leaves no entry.
IRegEffect* cSC4ModelRegEffects::AddRegEffect(const std::string& point, const std::string& effect, const RegPointFrames* frames) {
    auto [entry, inserted] = effects_.try_emplace(point);
    if (!inserted) StopAndRelease(entry->second, 1);
    entry->second.effect = g_host ? g_host->CreateEffect(effect.c_str()) : nullptr;
    if (!entry->second.effect) {
        effects_.erase(entry);
        return nullptr;
    }
    entry->second.frames = frames;
    entry->second.effectName = effect;
    return entry->second.effect;
}

// cSC4Automaton::AddRegEffect @ 0x000bb4ee: the new effect starts.
bool cSC4ModelRegEffects::AddRegEffect(const cRZString& point, const cRZString& effect) {
    IRegEffect* made = AddRegEffect(std::string(point.ToChar()), std::string(effect.ToChar()));
    if (made) made->Start(1);
    return made != nullptr;
}

// @ 0x00160798
void* cSC4ModelRegEffects::FindRegEffect(const cRZString& point) {
    auto found = effects_.find(point.ToChar());
    return found == effects_.end() ? nullptr : found->second.effect;
}

// @ 0x001607c8
void cSC4ModelRegEffects::RemoveRegEffect(const cRZString& point) {
    auto found = effects_.find(point.ToChar());
    if (found == effects_.end()) return;
    StopAndRelease(found->second, 1);
    effects_.erase(found);
}

// @ 0x001609e6
void cSC4ModelRegEffects::StartAllEffects(int transition) {
    for (auto& entry : effects_)
        if (entry.second.effect && !entry.second.effect->IsStarted()) entry.second.effect->Start(transition);
}

// @ 0x00160a36
void cSC4ModelRegEffects::StopAllEffects(int transition) {
    for (auto& entry : effects_)
        if (entry.second.effect && entry.second.effect->IsStarted()) entry.second.effect->Stop(transition);
}

// cSC4Automaton::StartAllEffects @ 0x000c06c6: an effect runs unless it is one that starts off; an effect a player-drive key
// stands for runs only while its key is held (`keyed`: effect name, held).
void cSC4ModelRegEffects::StartAllEffectsByKeys(const std::vector<std::pair<std::string, bool>>& keyed) {
    for (auto& entry : effects_) {
        IRegEffect* effect = entry.second.effect;
        if (!effect) continue;
        bool on = !effect->CheckEffectFlag(kEffectFlagStartsOff);
        for (const auto& key : keyed)
            if (key.first == entry.second.effectName) on = key.second;
        if (!on) effect->Stop(0);
        else if (!effect->IsStarted()) effect->Start(1);
    }
}

// @ 0x00160a86: every effect of that name (any case) starts; when there is none and `create` is set, one is added on the
// point of the same name. True when something started.
bool cSC4ModelRegEffects::StartNamedEffects(const cRZString& effect, int transition, bool create) {
    bool any = false;
    for (auto& entry : effects_) {
        if (!entry.second.effect || strcasecmp(entry.second.effectName.c_str(), effect.ToChar()) != 0) continue;
        entry.second.effect->Start(transition);
        any = true;
    }
    if (any) return true;
    if (!create) return false;
    IRegEffect* made = AddRegEffect(std::string(effect.ToChar()), std::string(effect.ToChar()));
    if (!made) return false;
    made->Start(transition);
    return true;
}

// @ 0x00160b44
void cSC4ModelRegEffects::StopNamed(const cRZString& effect, int transition) {
    for (auto& entry : effects_) {
        IRegEffect* held = entry.second.effect;
        if (held && held->IsStarted() && strcasecmp(entry.second.effectName.c_str(), effect.ToChar()) == 0) held->Stop(transition);
    }
}

// @ 0x00160bbc
void cSC4ModelRegEffects::ToggleNamed(const cRZString& effect, int transition, bool create) {
    bool any = false;
    for (auto& entry : effects_) {
        IRegEffect* held = entry.second.effect;
        if (!held || strcasecmp(entry.second.effectName.c_str(), effect.ToChar()) != 0) continue;
        if (held->IsStarted()) held->Stop(transition);
        else held->Start(transition);
        any = true;
    }
    if (any || !create) return;
    if (IRegEffect* made = AddRegEffect(std::string(effect.ToChar()), std::string(effect.ToChar()))) made->Start(transition);
}

// @ 0x00160ca8: every effect of that name is made anew and started (an effect that cannot be made leaves its point empty).
void cSC4ModelRegEffects::RecreateNamed(const cRZString& effect, int transition, bool create) {
    if (!g_host) return;
    bool any = false;
    for (auto& entry : effects_) {
        if (!entry.second.effect || strcasecmp(entry.second.effectName.c_str(), effect.ToChar()) != 0) continue;
        entry.second.effect->Release();
        entry.second.effect = g_host->CreateEffect(effect.ToChar());
        if (entry.second.effect) entry.second.effect->Start(transition);
        any = true;
    }
    if (any || !create) return;
    if (IRegEffect* made = AddRegEffect(std::string(effect.ToChar()), std::string(effect.ToChar()))) made->Start(transition);
}

// @ 0x00160e6e: an effect for every registration point named "fx_<effect>" or "fx_<digits>_<effect>".
void cSC4ModelRegEffects::InitEffectsList() {
    const RegPointMap* points = model_ && g_host ? g_host->RegPoints(model_) : nullptr;
    if (!points) return;
    const std::size_t prefix = std::strlen(kRegEffectPrefix);
    for (const auto& point : *points) {
        if (point.first.compare(0, prefix, kRegEffectPrefix) != 0) continue;
        std::string effect = point.first.substr(prefix);
        if (!effect.empty() && effect[0] >= '0' && effect[0] <= '9') {
            const std::size_t underscore = effect.find('_');
            if (underscore != std::string::npos) effect = effect.substr(underscore + 1);
        }
        AddRegEffect(point.first, effect, &point.second);
    }
}

// The effect's place: the model's transform times the point's transform of the frame (the identity without a point).
void cSC4ModelRegEffects::Place(tRegEffect& entry, const cS3DTransform& model, int frame) {
    cS3DTransform local;
    if (entry.frames && frame >= 0 && static_cast<std::size_t>(frame) < entry.frames->size()) {
        const RegPointTransform& point = (*entry.frames)[static_cast<std::size_t>(frame)];
        nS3D::Rotate(local, cS3DVector3{point.axis[0], point.axis[1], point.axis[2]}, point.angle);
        local.translation[0] += point.translation[0];
        local.translation[1] += point.translation[1];
        local.translation[2] += point.translation[2];
        local.flags |= 1;
        ++local.changeCounter;
    }
    cS3DTransform world = model;
    nS3D::MultiplyTransform(world, local);
    entry.effect->SetSourceTransform(world);
}

// @ 0x0016102e
void cSC4ModelRegEffects::UpdatePlacement() {
    if (!model_ || effects_.empty()) return;
    cS3DTransform model;
    model_->GetTransform(model);
    const int frame = model_->GetFrame();
    for (auto& entry : effects_)
        if (entry.second.effect && entry.second.effect->IsStarted()) Place(entry.second, model, frame);
}

// @ 0x00161250: the same, and every effect learns how far the model moves in this update.
void cSC4ModelRegEffects::Update(const cS3DVector3& position, const cS3DVector3& direction, float dt) {
    (void)position;
    if (!model_ || effects_.empty()) return;
    cS3DTransform model;
    model_->GetTransform(model);
    const int frame = model_->GetFrame();
    for (auto& entry : effects_) {
        if (!entry.second.effect || !entry.second.effect->IsStarted()) continue;
        Place(entry.second, model, frame);
        const cS3DVector3 moved{dt * direction.x, dt * direction.y, dt * direction.z};
        entry.second.effect->SetVectorParams(kEffectParamVelocity, &moved, 1);
    }
}

}  // namespace sc4
