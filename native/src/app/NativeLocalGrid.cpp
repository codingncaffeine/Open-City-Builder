// The cursor grid's services on the native application (sc4/LocalGridHelper.h ILocalGridHost): the effects manager the
// "local_grid" effect comes from (SL::spEffectsManager vslot 0x30) and the terrain's cell size (SL::spTerrain vslot 0x30). The zone,
// network and lot placement tools show the grid around the pointer through it.
#include "app/NativeEffects.h"
#include "sc4/LocalGridHelper.h"
#include "sc4/VisualEffect.h"
#include "sc4/cSTETerrain.h"

namespace native {
namespace {
using namespace sc4;

// The created visual effect as the helper calls it; the adapter holds the effect's one reference.
class GridEffect final : public ILocalGridEffect {
public:
    explicit GridEffect(cSC4VisualEffect* effect) : effect_(effect) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (refs_ > 1) return --refs_;
        effect_->Release();
        delete this;
        return 0;
    }
    void Start(int transition) override { effect_->Start(transition); }
    void Stop(int transition) override { effect_->Stop(transition); }
    bool IsStarted() override { return effect_->IsStarted(); }
    void SetEffectTransform(const cS3DTransform& transform) override { effect_->SetEffectTransform(transform); }

private:
    cSC4VisualEffect* effect_;
    std::uint32_t refs_ = 1;
};

class GridEffects final : public ILocalGridEffects {
public:
    ILocalGridEffect* Create(const char* name) override {
        cSC4VisualEffect* effect = CreateNamedEffect(name);   // one reference, or null when the effects files have no such effect
        return effect ? new GridEffect(effect) : nullptr;
    }
};

class GridTerrain final : public ILocalGridTerrain {
public:
    float CellSize() override { return cSTETerrain::CellWidth(); }
};

class LocalGridHost final : public ILocalGridHost {
public:
    ILocalGridEffects* Effects() override { return &effects_; }
    ILocalGridTerrain* Terrain() override { return &terrain_; }

private:
    GridEffects effects_;
    GridTerrain terrain_;
};

}  // namespace

void InstallLocalGridHost() {
    static LocalGridHost host;
    sc4::SetLocalGridHost(&host);
}

}  // namespace native
