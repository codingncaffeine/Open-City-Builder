// The disaster menu in a native city: cSC4View3DWin::InvokeDisaster (Mac i386 0x00521612), the two tools' city services
// (sc4::IDisasterToolHost) and the cursor effects of every effect tool (sc4::IEffectToolHost over the effects manager).
#include "app/NativeDisasterTools.h"

#include <cstdio>
#include <cstdlib>

#include "app/NativeCity.h"
#include "app/NativeEffects.h"
#include "app/NativeZoneTool.h"
#include "gz/cGZPersistResourceManager.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZString.h"
#include "sc4/Exemplar.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/ServiceLocator.h"
#include "sc4/View3DWin.h"
#include "sc4/ViewInputControlDisaster.h"
#include "sc4/VisualEffect.h"
#include "sc4/cSC4City.h"

namespace native {
namespace {
using namespace sc4;

bool Trace() {
    static const bool on = std::getenv("SC4_DISASTERDEBUG") != nullptr;
    return on;
}

// A visual effect as the tools hold it.
class ToolEffect final : public IToolVisualEffect {
public:
    explicit ToolEffect(cSC4VisualEffect* effect) : effect_(effect) {}   // takes the caller's reference
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
    void SetSourceTransform(const cS3DTransform& transform) override { effect_->SetSourceTransform(transform); }
    bool SetVectorParams(std::uint32_t type, const cS3DVector3* values, int count) override { return effect_->SetVectorParams(type, values, count); }
    bool SetFloatParams(std::uint32_t type, const float* values, int count) override { return effect_->SetFloatParams(type, values, count); }
    cSC4VisualEffect* Effect() const { return effect_; }
private:
    cSC4VisualEffect* effect_;
    std::uint32_t refs_ = 1;
};

class ToolEffectsManager final : public IToolEffectsManager {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 2; }
    std::uint32_t Release() override { return 1; }
    bool HasEffect(const char* name) override { return HasNamedEffect(name); }
    bool CreateEffect(const char* name, IToolVisualEffect** out) override {
        cSC4VisualEffect* effect = CreateNamedEffect(name);
        if (Trace()) std::fprintf(stderr, "disaster tools: effect \"%s\" -> %s\n", name ? name : "", effect ? "made" : "none");
        if (!effect) return false;
        *out = new ToolEffect(effect);
        return true;
    }
    bool CreateEffect(std::uint32_t groupId, int type, IToolVisualEffect** out) override {
        cSC4VisualEffect* effect = CreateKeyedEffect(groupId, static_cast<std::uint32_t>(type));
        if (Trace()) std::fprintf(stderr, "disaster tools: effect group %08x type %d -> %s\n", groupId, type, effect ? "made" : "none");
        if (!effect) return false;
        *out = new ToolEffect(effect);
        return true;
    }
};

class EffectToolHost final : public IEffectToolHost {
public:
    IToolEffectsManager* EffectsManager() override { return &manager_; }
private:
    ToolEffectsManager manager_;
};

// The tool's effect as a disaster instance holds it.
class EffectFace final : public IDisasterEffectFace {
public:
    explicit EffectFace(cSC4VisualEffect* effect) : effect_(effect) { effect_->AddRef(); }
    void Retain() override { ++refs_; }
    void Drop() override {
        if (--refs_ > 0) return;
        effect_->Release();
        delete this;
    }
    void Start(int transition) override { effect_->Start(transition); }
    void Stop(int transition) override { effect_->Stop(transition); }
    bool IsPlaying() override { return effect_->IsStarted(); }
    void SetCompletionMessage(std::uint32_t message, std::uint32_t data) override { effect_->SetFinishNotifyMessage(message, data); }
    void SetIntParameter(int parameter, const void* data, int count) override {   // the float table (the "attract" switch 0xb)
        effect_->SetFloatParams(static_cast<std::uint32_t>(parameter), static_cast<const float*>(data), count);
    }
    void SetPointerParameter(int parameter, const void* data, int count) override {
        effect_->SetVoidParams(static_cast<std::uint32_t>(parameter), static_cast<void**>(const_cast<void*>(data)), count);
    }
private:
    cSC4VisualEffect* effect_;
    int refs_ = 0;
};

class DisasterToolHost final : public IDisasterToolHost {
public:
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        cellX = static_cast<int>(x / 16.0f);
        cellZ = static_cast<int>(z / 16.0f);
    }
    bool IgniteCell(int cellX, int cellZ, bool again, bool force, std::uint32_t value) override {
        const bool lit = NativeTorchIgnite(cellX, cellZ, again, force, value);
        if (Trace()) std::fprintf(stderr, "torch: cell %d,%d %s -> %s\n", cellX, cellZ, again ? "again" : "first", lit ? "lit" : "nothing lit");
        return lit;
    }
    void SetFireCause(std::uint32_t cause) override { NativeSetFireCause(cause); }
    void StartTick(cIGZMessageTarget2* target, std::uint32_t interval) override { StartToolTick(target, interval); }
    void StopTick(cIGZMessageTarget2* target, std::uint32_t) override { StopToolTick(target); }
    // The torch's sound events go to the sound system's listeners (cSC4AudioEventHandlerDisasterFire); nothing listens natively yet.
    void SendToolAudio(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) override {}
    bool HasDisasterManager(std::uint32_t id) override { return NativeHasDisasterManager(id); }
    void AttemptDisasterStart(std::uint32_t id, int x, int z) override {
        if (Trace()) std::fprintf(stderr, "disaster tools: manager %08x asked to start at %d,%d\n", id, x, z);
        NativeAttemptDisasterStart(id, x, z);
    }
    cIGZUnknown* StartDisasterEffect(std::uint32_t managerId, IToolVisualEffect* effect, const cS3DVector3& position, std::uint32_t id) override {
        auto* held = dynamic_cast<ToolEffect*>(effect);
        if (!held) return nullptr;
        const float p[3] = {position.x, position.y, position.z};
        cIGZUnknown* instance = NativeStartDisasterEffect(managerId, new EffectFace(held->Effect()), p, id);
        if (Trace()) std::fprintf(stderr, "disaster tools: effect disaster %08x at (%.0f, %.0f) -> %s\n", id, p[0], p[2], instance ? "started" : "not started");
        return instance;
    }
    void StopDisasterEffect(cIGZUnknown* instance) override { NativeStopDisasterEffect(instance); }
};

EffectToolHost g_effectHost;
DisasterToolHost g_toolHost;

}  // namespace

void InstallDisasterTools() {
    sc4::SetEffectToolHost(&g_effectHost);
    sc4::SetDisasterToolHost(&g_toolHost);
}

// The torch for its own id; any other id names a disaster exemplar {0x6534284a, 0x29b6c670, id}: an effect tool carrying the
// exemplar's named effect, or the effects of its effect group, with the exemplar's disaster manager.
void InvokeDisasterTool(sc4::cSC4View3DWin& view, std::uint32_t id) {
    if (id == kDisasterTorchId) {
        auto* torch = new cSC4ViewInputControlTorch();
        torch->AddRef();
        view.SetCurrentViewInputControl(torch, 2);
        torch->Release();
        if (Trace()) std::fprintf(stderr, "disaster tools: the torch\n");
        return;
    }
    const cGZPersistResourceKey key{0x6534284a, 0x29b6c670, id};
    cIGZUnknown* raw = nullptr;
    if (!SL::spRM || !SL::spRM->GetResource(key, 0xa52160f5, reinterpret_cast<void**>(&raw), 0, nullptr) || !raw) {
        if (Trace()) std::fprintf(stderr, "disaster tools: no exemplar for %08x\n", id);
        return;
    }
    auto* exemplar = static_cast<cISCResExemplar*>(raw);
    const cISCPropertyHolder* properties = exemplar->AsISCPropertyHolder();
    std::uint32_t managerId = 0, group = 0;
    cRZString name;
    cSCPropertyHelp::GetPropertyValue(properties, kDisasterPropertyManager, managerId);
    const bool named = cSCPropertyHelp::GetPropertyValue(properties, kDisasterPropertyEffectName, name);
    const bool grouped = cSCPropertyHelp::GetPropertyValue(properties, kDisasterPropertyEffectGroup, group);
    if (Trace())
        std::fprintf(stderr, "disaster tools: %08x: effect \"%s\" group %08x (%s) manager %08x\n", id, named ? name.ToChar() : "", group,
                     named ? "named" : grouped ? "group" : "neither", managerId);
    if (named || grouped) {
        bool pick = false;
        const bool hasPick = cSCPropertyHelp::GetPropertyValue(properties, kDisasterPropertyEffectGroup, pick);
        auto* tool = new cSC4ViewInputControlEffectDisaster(hasPick, pick);
        tool->AddRef();
        view.SetCurrentViewInputControl(tool, 2);
        if (named) tool->SetEffectName(name.ToChar());
        else tool->SetEffectGroupID(group);
        if (managerId != 0) tool->SetDisasterManager(managerId);
        tool->Release();
    }
    exemplar->Release();
}

void* NativeCreateEffectAtPoint(const char* name, const float xyz[3]) {
    cSC4VisualEffect* effect = CreateNamedEffect(name);
    if (Trace()) std::fprintf(stderr, "disaster effect \"%s\" at (%.0f, %.0f, %.0f) -> %s\n", name, xyz[0], xyz[1], xyz[2], effect ? "made" : "none");
    if (!effect) return nullptr;
    NativeMoveEffectTo(effect, xyz);
    effect->Start(0);
    return effect;
}

void NativeMoveEffectTo(void* handle, const float xyz[3]) {
    auto* effect = static_cast<cSC4VisualEffect*>(handle);
    if (!effect) return;
    cS3DTransform transform;
    transform.translation[0] = xyz[0];
    transform.translation[1] = xyz[1];
    transform.translation[2] = xyz[2];
    transform.flags |= 1;
    transform.changeCounter = static_cast<std::uint8_t>(transform.changeCounter + 1);
    effect->SetSourceTransform(transform);
}

void NativeReleaseEffectHandle(void* handle) {
    if (auto* effect = static_cast<cSC4VisualEffect*>(handle)) effect->Release();
}

void NativeDisasterMenuReport() {
    if (!SL::spRM) { std::printf("disasters: no resource manager\n"); return; }
    struct Filter final : cIGZPersistResourceKeyFilter {
        bool QueryInterface(std::uint32_t, void**) override { return false; }
        std::uint32_t AddRef() override { return 1; }
        std::uint32_t Release() override { return 1; }
        bool IsKeyIncluded(const cGZPersistResourceKey& key) override { return key.type == 0x6534284a && key.group == 0x29b6c670; }
    } filter;
    cIGZPersistResourceKeyList* list = nullptr;
    SL::spRM->GetAvailableResourceList(&list, &filter);
    if (!list) { std::printf("disasters: no exemplars\n"); return; }
    std::printf("disasters: torch %08x (the fire tool)\n", kDisasterTorchId);
    for (std::uint32_t i = 0; i < list->Size(); ++i) {
        const cGZPersistResourceKey key = *list->GetKey(i);
        cIGZUnknown* raw = nullptr;
        if (!SL::spRM->GetResource(key, 0xa52160f5, reinterpret_cast<void**>(&raw), 0, nullptr) || !raw) continue;
        auto* exemplar = static_cast<cISCResExemplar*>(raw);
        const cISCPropertyHolder* properties = exemplar->AsISCPropertyHolder();
        std::uint32_t managerId = 0, group = 0;
        cRZString name, title;
        cSCPropertyHelp::GetPropertyValue(properties, kDisasterPropertyManager, managerId);
        const bool named = cSCPropertyHelp::GetPropertyValue(properties, kDisasterPropertyEffectName, name);
        const bool grouped = cSCPropertyHelp::GetPropertyValue(properties, kDisasterPropertyEffectGroup, group);
        cSCPropertyHelp::GetPropertyValue(properties, 0x00000020u, title);   // the exemplar's name
        std::printf("disasters: %08x \"%s\": %s%s%s group %08x manager %08x%s\n", key.instance, title.ToChar(), named ? "effect \"" : "", named ? name.ToChar() : "",
                    named ? "\"" : "", group, managerId, named || grouped ? "" : " (not a menu disaster)");
        exemplar->Release();
    }
    list->Release();
}

}  // namespace native
