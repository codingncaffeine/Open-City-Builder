// The fire disaster's visual effects in a native city: the flames of cSC4FireOccupant (FireServices) and the burn-down and scorch
// effects of the fire instance. Apart from NativeDisasters.cpp because the effect and the disaster headers each declare
// cISC4VisualEffect.
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "app/NativeCitySystems.h"
#include "app/NativeEffects.h"
#include "sc4/FireOccupant.h"
#include "sc4/VisualEffect.h"

namespace native {
int NativeCurrentViewLevel();   // app/NativeCitySystems.cpp
bool NativeFireSkip(const char* what);

namespace {
using namespace sc4;


class FlameEffect final : public IFlameEffect {
public:
    explicit FlameEffect(cSC4VisualEffect* effect) : effect_(effect) {}
    bool QueryInterface(std::uint32_t, void** out) override { *out = nullptr; return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (refs_ < 2) {
            if (effect_) effect_->Release();
            delete this;
            return 0;
        }
        return --refs_;
    }
    void Start(int flags) override { effect_->Start(flags); }
    void Stop(int flags) override { effect_->Stop(flags); }
    bool IsRunning() override { return effect_->IsStarted(); }
    void SetTransform(const cS3DTransform& transform) override { effect_->SetSourceTransform(transform); }
private:
    cSC4VisualEffect* effect_;
    std::uint32_t refs_ = 0;
};

}  // namespace

void InstallFireServices() {
    FireServices& services = GetFireServices();
    services.createEffect = [](const char* name) -> IFlameEffect* {
        if (NativeFireSkip("flames")) return nullptr;
        cSC4VisualEffect* effect = CreateNamedEffect(name);
        if (!effect) return nullptr;
        auto* flame = new FlameEffect(effect);
        flame->AddRef();
        return flame;
    };
    services.viewZoom = [] { return NativeCurrentViewLevel(); };
}


// effects manager vslot 0x18 (by name), effect vslot 0x24 SetModelFromOccupant, vslot 0xc Start(0)
bool NativeFireSkip(const char* what) {   // dev switch SC4_FIRESKIP=burnfx,flames,condition (bisecting)
    const char* list = std::getenv("SC4_FIRESKIP");
    return list && std::strstr(list, what) != nullptr;
}
void NativeStartEffectOnOccupant(const char* name, sc4::cISC4Occupant* occupant) {
    if (NativeFireSkip("burnfx")) return;
    sc4::cSC4VisualEffect* effect = CreateNamedEffect(name);
    if (!effect) return;
    if (effect->SetModelFromOccupant(occupant)) effect->Start(0);
    effect->Release();
}

// The scorch mark: an identity transform moved to the position (translation flag set), SetSourceTransform (vslot 0x20), Start(0).
void NativeStartEffectAt(const char* name, const float position[3]) {
    sc4::cSC4VisualEffect* effect = CreateNamedEffect(name);
    if (!effect) return;
    cS3DTransform transform;
    transform.translation[0] = position[0];
    transform.translation[1] = position[1];
    transform.translation[2] = position[2];
    transform.flags |= 0x101;
    effect->SetSourceTransform(transform);
    effect->Start(0);
    effect->Release();
}

}  // namespace native
