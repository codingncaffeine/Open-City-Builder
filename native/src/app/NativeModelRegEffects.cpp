// The effects that ride on an automaton's model in a native city: the services of cSC4ModelRegEffects (IRegEffectsHost): the
// model's registration points and the effects manager's effects by name.
#include <cstdio>
#include <cstdlib>

#include "app/NativeEffects.h"
#include "sc4/ModelRegEffects.h"
#include "sc4/VisualEffect.h"

namespace native {
const sc4::RegPointMap* NativeAutomatonModelRegPoints(sc4::IAutomatonModel* model);   // app/NativeAutomata.cpp

namespace {
using namespace sc4;

bool Trace() {
    static const bool on = std::getenv("SC4_REGEFFECTDEBUG") != nullptr;
    return on;
}

class RegEffect final : public IRegEffect {
public:
    explicit RegEffect(cSC4VisualEffect* effect) : effect_(effect) {}
    void AddRef() override { ++refs_; }
    void Release() override {
        if (--refs_ > 0) return;
        effect_->Release();
        delete this;
    }
    void Start(int transition) override { effect_->Start(transition); }
    void Stop(int transition) override { effect_->Stop(transition); }
    bool IsStarted() override { return effect_->IsStarted(); }
    void SetSourceTransform(const cS3DTransform& transform) override { effect_->SetSourceTransform(transform); }
    void SetVectorParams(std::uint32_t type, const cS3DVector3* values, int count) override { effect_->SetVectorParams(type, values, count); }
    bool CheckEffectFlag(std::uint32_t flag) override { return effect_->CheckEffectFlag(flag); }

private:
    cSC4VisualEffect* effect_;
    int refs_ = 1;
};

class RegEffectsHost final : public IRegEffectsHost {
public:
    const RegPointMap* RegPoints(IAutomatonModel* model) override { return NativeAutomatonModelRegPoints(model); }
    IRegEffect* CreateEffect(const char* name) override {
        cSC4VisualEffect* effect = CreateNamedEffect(name);
        if (Trace()) std::fprintf(stderr, "reg effects: effect '%s' -> %s\n", name, effect ? "made" : "none");
        return effect ? new RegEffect(effect) : nullptr;
    }
} g_regEffectsHost;

}  // namespace

sc4::IAutomatonEffects* NativeNewModelRegEffects() {
    if (!sc4::RegEffectsHost()) sc4::SetRegEffectsHost(&g_regEffectsHost);
    return new sc4::cSC4ModelRegEffects();
}

}  // namespace native
