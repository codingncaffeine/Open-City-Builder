#include "sc4/AttractorEffect.h"

namespace sc4 {

namespace {
IAttractorHost* g_attractorHost = nullptr;
ISequenceHost* g_sequenceHost = nullptr;
}  // namespace

IAttractorHost* AttractorHost() { return g_attractorHost; }
void SetAttractorHost(IAttractorHost* host) { g_attractorHost = host; }
ISequenceHost* SequenceHost() { return g_sequenceHost; }
void SetSequenceHost(ISequenceHost* host) { g_sequenceHost = host; }

// ---- attractor ------------------------------------------------------------------------------------------------------------

cSC4AttractorEffect::cSC4AttractorEffect(cSC4AttractorDescription* description) : description_(description) {
    if (description_) description_->AddRef();
}

// Port guard: the original only drops its two references here; an attractor destroyed while started would leave its
// anchor in the occupant manager and its controllers on a dead occupant, so it is stopped first.
cSC4AttractorEffect::~cSC4AttractorEffect() {
    if (started_) Stop(1);
    if (g_attractorHost) {
        if (controller_) g_attractorHost->RemoveController(controller_);
        if (anchor_) g_attractorHost->DestroyAnchor(anchor_);
    }
    if (description_) description_->Release();
}

// A controller template that does not exist leaves the effect not started (its anchor stays until the effect goes).
void cSC4AttractorEffect::Start(int) {
    IAttractorHost* host = g_attractorHost;
    if (started_ || !host || !host->HasOccupantManager()) return;
    started_ = true;
    time_ = 0.0f;
    if (anchor_) host->DestroyAnchor(anchor_);   // port guard: one left by a start whose template was unknown
    anchor_ = host->CreateAnchor();
    if (!description_->flag) {
        if (controller_) host->RemoveController(controller_);
        controller_ = host->AddController(anchor_, description_->name.c_str());
        if (!controller_) started_ = false;
    } else {
        host->AddGroupControllers(description_->name.c_str(), anchor_);
    }
    const float xyz[3] = {position_.x, position_.y, position_.z};
    host->MoveAnchor(anchor_, xyz, true);
}

void cSC4AttractorEffect::Stop(int) {
    IAttractorHost* host = g_attractorHost;
    if (!started_ || !host) return;
    if (!description_->flag) {
        if (controller_) {
            host->RemoveController(controller_);
            controller_ = nullptr;
        }
    } else {
        host->RemoveGroupControllers(description_->name.c_str(), anchor_);
    }
    if (anchor_) {
        host->DestroyAnchor(anchor_);
        anchor_ = nullptr;
    }
    started_ = false;
}

void cSC4AttractorEffect::SetTransforms(const cS3DTransform& parent, const cS3DTransform& local) {
    position_ = ComposeTranslation(parent, local);
    if (!anchor_ || !g_attractorHost) return;
    const float xyz[3] = {position_.x, position_.y, position_.z};
    g_attractorHost->MoveAnchor(anchor_, xyz, false);
}

// ---- sequence -------------------------------------------------------------------------------------------------------------

cSC4SequenceEffect::cSC4SequenceEffect(cSC4SequenceDescription* description) : description_(description) {
    if (description_) {
        description_->AddRef();
        startTransition_ = (description_->value & kSequenceStartTransition) != 0 ? 1 : 0;
        itemStopTransition_ = (description_->value & kSequenceCutItems) != 0 ? 1 : 0;
    }
}

cSC4SequenceEffect::~cSC4SequenceEffect() {
    Stop(1);
    for (ISequenceItemEffect* effect : fading_) effect->Release();
    if (current_) current_->Release();
    if (description_) description_->Release();
}

void cSC4SequenceEffect::SetCurrent(ISequenceItemEffect* effect) {
    ISequenceItemEffect* old = current_;
    current_ = effect;
    if (old) old->Release();
}

// A start with a transition picks the sequence up where it was stopped; the first start and a plain one begin it again.
void cSC4SequenceEffect::Start(int transition) {
    if (description_->items.empty()) {
        started_ = false;
        return;
    }
    started_ = true;
    if (transition == 0 || fresh_) {
        elapsed_ = 0.0f;
        item_ = 0;
        nextTime_ = 0.0f;
    }
    fresh_ = false;
}

// A soft stop (0) leaves the effects that are fading out to finish; any other stops and drops them too.
void cSC4SequenceEffect::Stop(int transition) {
    if (current_) {
        current_->Stop(transition);
        SetCurrent(nullptr);
    }
    if (transition != 0 && !fading_.empty()) {
        for (ISequenceItemEffect* effect : fading_) {
            if (effect->IsStarted()) effect->Stop(transition);
            effect->Release();
        }
        fading_.clear();
    }
    started_ = false;
}

void cSC4SequenceEffect::SetTransforms(const cS3DTransform& parent, const cS3DTransform& local) {
    source_ = parent;
    effect_ = local;
}

void cSC4SequenceEffect::CullSoftStoppedEffects() {
    std::size_t kept = 0;
    for (std::size_t i = 0; i < fading_.size(); ++i) {
        if (fading_[i]->IsStarted()) fading_[kept++] = fading_[i];
        else fading_[i]->Release();
    }
    fading_.resize(kept);
}

void cSC4SequenceEffect::ApplyEffect(float elapsed) {
    if (!started_) {
        CullSoftStoppedEffects();
        return;
    }
    elapsed_ += elapsed;
    if (elapsed_ < nextTime_) return;
    if (0.0f < nextTime_) {                       // an item is over (the first item begins without one before it)
        ++item_;
        if (description_->items.size() <= item_) {
            if ((description_->value & kSequenceLoop) == 0) {
                Stop(itemStopTransition_);
                return;
            }
            item_ = 0;
        }
        if (current_ && current_->IsStarted()) {
            current_->Stop(itemStopTransition_);
            if (itemStopTransition_ == 0) {
                current_->AddRef();
                fading_.push_back(current_);
                CullSoftStoppedEffects();
            }
            SetCurrent(nullptr);
        }
    }
    const cSC4SequenceDescription::cItemRec& item = description_->items[item_];
    ISequenceHost* host = g_sequenceHost;
    nextTime_ += host ? static_cast<float>(host->RandomRange(static_cast<double>(item.range.x), static_cast<double>(item.range.y))) : item.range.x;
    if (item.name.empty() || !host) return;
    SetCurrent(nullptr);
    SetCurrent(host->CreateEffect(item.name.c_str()));
    if (!current_) return;
    current_->SetSourceTransform(source_);
    current_->SetEffectTransform(effect_);
    current_->Start(startTransition_);
}

}  // namespace sc4
