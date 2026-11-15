// The effects a sequence component plays, in a native city: each item's effect comes from the effects manager by name. Its own
// file because the effect class's header and the disaster headers (NativeEffectComponents.cpp) declare the same interface.
#include <cstdio>
#include <cstdlib>

#include "app/NativeEffects.h"
#include "rz/cRZRandom.h"
#include "sc4/AttractorEffect.h"
#include "sc4/VisualEffect.h"

namespace native {
namespace {
using namespace sc4;

bool Trace() {
    static const bool on = std::getenv("SC4_FXCOMPDEBUG") != nullptr;
    return on;
}

class SequenceItem final : public ISequenceItemEffect {
public:
    explicit SequenceItem(cSC4VisualEffect* effect) : effect_(effect) {}   // takes the caller's reference
    void AddRef() override { ++refs_; }
    void Release() override {
        if (--refs_ != 0) return;
        effect_->Release();
        delete this;
    }
    void Start(int transition) override { effect_->Start(transition); }
    void Stop(int transition) override { effect_->Stop(transition); }
    bool IsStarted() override { return effect_->IsStarted(); }
    void SetEffectTransform(const cS3DTransform& transform) override { effect_->SetEffectTransform(transform); }
    void SetSourceTransform(const cS3DTransform& transform) override { effect_->SetSourceTransform(transform); }
private:
    cSC4VisualEffect* effect_;
    int refs_ = 1;
};

class SequenceHost final : public ISequenceHost {
public:
    ISequenceItemEffect* CreateEffect(const char* name) override {
        cSC4VisualEffect* effect = CreateNamedEffect(name);
        if (Trace()) std::fprintf(stderr, "sequence: item effect %s -> %s\n", name, effect ? "made" : "none");
        return effect ? new SequenceItem(effect) : nullptr;
    }
    double RandomRange(double low, double high) override { return random_.RandomDoubleRangeUniform(low, high); }
private:
    cRZRandom random_{0xffffffffu};
} g_sequenceHost;

}  // namespace

sc4::ISequenceHost* NativeSequenceHost() { return &g_sequenceHost; }

}  // namespace native
