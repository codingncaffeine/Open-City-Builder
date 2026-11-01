// cSC4EffectsManager — clean C++20 port of the original class (see EffectsManager.h for the layout, the seam and the
// quirks). Every function below carries its original address; functions whose body could not be completed are marked
// PARTIAL / SKIPPED in the same comment and in notes/ledger_pending/cSC4EffectsManager.txt.
#include "sc4/EffectsManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "rz/PortMemoryStreams.h"
#include "rz/RZStringUtils.h"
#include "s3d/S3DMath.h"
#include "sc4/EffectsResource.h"
#include "gz/CommandParameterSet.h"
#include "sc4/GameDataRegistry.h"

namespace sc4 {

namespace {

IEffectsManagerHost* g_effectsManagerHost = nullptr;
cSC4EffectsManager* g_effectsManager = nullptr;

// The camera parameters' LOD distance curve sampled at `zoom` (the inlined body of EndCollection 0x1005e0dea and
// DoZoomChanged 0x1005ddf00): t = (count - 1) * zoom * 0.25, index = floor(t), linear interpolation by the fraction,
// count == 1 returns the single key. The original indexes the curve unchecked (an out-of-range zoom reads past the
// vector); the port clamps to the last key.
float SampleLodCurve(const std::vector<float>& keys, float zoom) {
    if (keys.empty()) return 0.0f;
    if (keys.size() == 1) return keys[0];
    const int last = static_cast<int>(keys.size()) - 1;
    const float t = static_cast<float>(last) * zoom * 0.25f;
    int index = static_cast<int>(std::floor(t));
    float fraction = t - static_cast<float>(index);
    if (index < 0) {
        index = 0;
        fraction = 0.0f;
    }
    if (index >= last) {
        index = last;
        fraction = 0.0f;
    }
    float value = keys[static_cast<std::size_t>(index)];
    if (fraction > 0.0f) value += fraction * (keys[static_cast<std::size_t>(index) + 1] - value);
    return value;
}

}  // namespace

IEffectsManagerHost* EffectsManagerHost() { return g_effectsManagerHost; }
void SetEffectsManagerHost(IEffectsManagerHost* host) { g_effectsManagerHost = host; }
cSC4EffectsManager* EffectsManager() { return g_effectsManager; }
void SetEffectsManager(cSC4EffectsManager* manager) { g_effectsManager = manager; }

// @ 0x1005dad3e. The original constructs cSC4EffectMaps at +0x18 and cSC4EffectsCollection at +0xd0, assigns the six
// vtables, and initialises the registry / pool / flash / tint / snowline fields; the port's member initialisers do the
// same (see the field map in the header).
cSC4EffectsManager::cSC4EffectsManager() : host_(EffectsManagerHost()) {}

void cSC4EffectsManager::ManagerLink::AddRef() { owner_->AddRef(); }
void cSC4EffectsManager::ManagerLink::Release() { owner_->Release(); }
void cSC4EffectsManager::ManagerLink::StartAncilliary(cSC4VisualEffect* effect) { owner_->StartAncilliary(effect); }
void cSC4EffectsManager::ManagerLink::StopAncilliary(cSC4VisualEffect* effect) { owner_->StopAncilliary(effect); }

// @ 0x1005db26c (D2; the complete-object alias is 0x1005db97a and the deleting variant 0x1005db9b2). The original only
// tears the sub-objects down: it releases the vertex-buffer extension, the renderer, the weather simulator, the effect
// resource and the parser and lets the containers release the effects. It does NOT release the effect maps at +0x1060
// nor the atmospheric / water effects at +0x1050 / +0x1058 (Shutdown does), and it does not stop the timer.
cSC4EffectsManager::~cSC4EffectsManager() {
    if (vertexBufferExtension_) {
        vertexBufferExtension_->Release();
        vertexBufferExtension_ = nullptr;
    }
    if (renderer_) {
        renderer_->Release();
        renderer_ = nullptr;
    }
    if (weatherSim_) {
        weatherSim_->Release();
        weatherSim_ = nullptr;
    }
    if (effectsResource_) {
        effectsResource_->Release();
        effectsResource_ = nullptr;
    }
    if (effectsParser_) {
        effectsParser_->Release();
        effectsParser_ = nullptr;
    }
    if (g_effectsManager == this) g_effectsManager = nullptr;
}

// @ 0x1005dd616. Interface ids: 1 / 0xc97cf5cd -> the manager, 0x452294aa -> the +0x10 interface (same object here),
// 0xc97cf5ce -> the collection at +0xd0, 0xa5eaa1a -> the effect maps at +0x1060 (forwarded, which also AddRefs them).
bool cSC4EffectsManager::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    if (iid == kGZIID_cIGZUnknown || iid == kGZIID_cISC4EffectsManager ||
        iid == kGZIID_cISC4EffectsManagerAlt) {
        *out = this;
        AddRef();
        return true;
    }
    if (iid == kGZIID_cISC4EffectsCollection) {
        *out = &collection_;
        AddRef();
        return true;
    }
    if (iid == kGZIID_cISC4EffectMaps) {
        if (!effectMaps_) return false;
        return effectMaps_->QueryInterface(iid, out);
    }
    return false;
}

// @ 0x1005dd6ce: only bumps the counter (the original does not return the value; GZCOM ignores it).
std::uint32_t cSC4EffectsManager::AddRef() { return ++refCount_; }

// @ 0x1005dd746: a quirk — Release never decrements anything and always reports success. The GZCOM framework therefore
// destroys the manager through FinalRelease, not through the last Release.
std::uint32_t cSC4EffectsManager::Release() { return 1; }

// @ 0x1005dd78e: vslot 0x20 (Shutdown) followed by vslot 0x60 (the deleting destructor).
void cSC4EffectsManager::FinalRelease() {
    Shutdown();
    delete this;
}

// @ 0x1005dba0e (PARTIAL: LoadAllEffects / ParseEffectsFiles and every service call are host seams; see the pending
// ledger). The order of the original is kept: guard, thread id, the two progress messages, the map extents, the
// renderer / weather / vertex-buffer references, the flash materials, the effect maps, the resource + parser, the
// effect files, the 11 message handlers, the cheats, the tick callback, the ATM registration and the game-data
// functions.
bool cSC4EffectsManager::Init() {
    if (initialised_ != 0) return true;
    initialised_ = 1;
    if (!host_) return true;

    threadId_ = host_->CurrentThreadId();
    host_->SendProgressMessage(1, "");
    host_->SendProgressMessage(2, "");
    tickCount_ = 0;
    elapsedSeconds_ = 0.0f;

    const float halfCell = host_->TerrainCellSize() * 0.5f;
    halfExtentX_ = static_cast<float>(host_->TerrainCellsX()) * halfCell;
    halfExtentZ_ = static_cast<float>(host_->TerrainCellsZ()) * halfCell;

    if (cIGZUnknown* renderer = host_->Renderer()) {
        if (renderer_ != renderer) {
            if (renderer_) renderer_->Release();
            renderer_ = renderer;
            host_->AttachRenderable(this);                       // renderer vslot 0x100(this, 5, 0x76c)
        } else {
            renderer->Release();
        }
    }
    weatherSim_ = host_->WeatherSim();
    vertexBufferExtension_ = host_->VertexBufferExtension();     // QI 0x9cd86f9, one reference
    CreateFlashMaterials();

    effectMaps_ = host_->CreateEffectMaps();
    if (effectMaps_) effectMaps_->Init();
    effectsResource_ = host_->CreateEffectsResource();           // already Init'ed by the host
    effectsParser_ = host_->CreateEffectsParser();
    host_->SendProgressMessage(2, "");
    host_->LoadEffectsFiles();
    LoadAllEffects();
    for (std::uint32_t id : kEffectsMessageIds) host_->RegisterMessageHandler(id);
    host_->AddCheatHandler(this);                                // SL::spCCM vslot 0x38(this, 0)
    host_->RegisterCheatCode(kCheatEffectCode, "Effect");
    if (host_->CheatsEnabled()) {
        host_->RegisterCheatCode(kCheatSnowCode, "Snow");
        host_->RegisterCheatCode(kCheatHeapCheckCode, "HeapCheck");
    }
    tickTimer_.SetCallbackFunction(&EffectsTickCallback, this);
    host_->AddTickNotification(this);                            // SL::ATM vslot 0x28(this, 0xc0)
    host_->RegisterGameDataFunctions();
    host_->SendProgressMessage(1, "");
    return true;
}

// @ 0x1005dcd62 (PARTIAL: the effect-resource / parser teardown is host work and the decal + three particle-family
// pools are not modelled because their classes are unrecovered; the visual-effect and particles pools, the collection
// tables, the registries and every service reference are torn down in the original's order).
bool cSC4EffectsManager::Shutdown() {
    if (initialised_ == 0) return true;
    initialised_ = 0;
    if (host_) host_->UnregisterGameDataFunctions();

    if (atmosphericEffect_) {
        atmosphericEffect_->Stop(1);
        atmosphericEffect_->Release();
        atmosphericEffect_ = nullptr;
    }
    if (waterEffect_) {
        waterEffect_->Stop(1);
        waterEffect_->Release();
        waterEffect_ = nullptr;
    }
    if (host_) host_->RemoveTickNotification(this);
    tickTimer_.Stop();
    if (host_) {
        host_->RemoveCheatHandler(this);
        host_->UnregisterCheatCode(kCheatEffectCode);
        if (host_->CheatsEnabled()) {
            host_->UnregisterCheatCode(kCheatSnowCode);
            host_->UnregisterCheatCode(kCheatHeapCheckCode);
        }
        for (std::uint32_t id : kEffectsMessageIds) host_->UnregisterMessageHandler(id);
        for (const cSC4MessageTriggerDescription& trigger : collection_.MessageTriggers())
            host_->UnregisterMessageHandler(trigger.message);
    }
    effectBindingsByKey_.clear();
    messageTriggers_.clear();
    if (effectMaps_) {
        effectMaps_->Shutdown();
        effectMaps_->Release();
        effectMaps_ = nullptr;
    }
    if (effectsParser_) {
        effectsParser_->Release();
        effectsParser_ = nullptr;
    }
    if (effectsResource_) {
        if (host_) host_->ShutdownEffectsResource(effectsResource_);
        effectsResource_->Release();
        effectsResource_ = nullptr;
    }
    for (cRZAutoRefCount<cSC4VisualEffect>& effect : visualEffects_)
        if (effect) effect->Shutdown();
    visualEffects_.clear();
    newVisualEffectSlots_.clear();
    activeVisualEffectSlots_.clear();
    freeVisualEffectSlots_.clear();
    for (cRZAutoRefCount<cSC4ParticlesEffect>& effect : particlesEffects_)
        if (effect) effect->Shutdown();
    particlesEffects_.clear();
    newParticlesEffectSlots_.clear();
    freeParticlesEffectSlots_.clear();
    // Quirk: the description tables are only partly cleared (the light table, the camera parameters, the effect
    // bindings and the message triggers survive, as they do in the original).
    collection_.ClearForEffectManagerShutdown();
    if (vertexBufferExtension_) {
        vertexBufferExtension_->Release();
        vertexBufferExtension_ = nullptr;
    }
    if (renderer_) {
        if (host_) host_->DetachRenderable(this);                // renderer vslot 0x108(this)
        renderer_->Release();
        renderer_ = nullptr;
    }
    if (weatherSim_) {
        weatherSim_->Release();
        weatherSim_ = nullptr;
    }
    effectsActive_ = false;
    return true;
}

// @ 0x1005e324a: parse the files the file parser at +0xeb0 was given. The original is a two-branch tail: when
// ParseFiles() answers true it clears the 3D view's error report (SetErrorReportString(nullptr)) and posts
// kEffectsParsedMessage through the message server; when it answers false it hands the parser's ErrorString() to the
// view instead. It does not null-check the parser (Init always creates it); the port returns early for a host that
// supplied none.
void cSC4EffectsManager::ParseEffectsFiles() {
    if (!host_ || !effectsResource_) return;
    if (host_->EffectsResourceParseFiles()) {
        host_->SetView3DErrorReportString(nullptr);
        host_->PostMessage(kEffectsParsedMessage);
        return;
    }
    host_->SetView3DErrorReportString(host_->EffectsResourceErrorString());
}

// @ 0x1005dcb24: the game-data registry's "create_effect" command. `handle` is the manager as registered with the
// registry: the entry id comes from GetEntryID(handle) and the manager back out of its GetEntryContext. The four
// parameters are the effect name (GetValString) and the source position's x, y and z (GetValFloat32 each); the effect is
// created by name, given an identity-with-translation source transform (change counter 1, flag 1 = translation) and
// started with transition 0. Parameter 1 of the out set receives the success flag (a Bool variant), and the one
// reference the creation returned is released. The command itself always succeeds (the original returns true even
// when no manager is reachable; it does not null-check in / out / registry, the port returns / answers false instead).
bool cSC4EffectsManager::sCreateEffect(void* handle, cIGZCommandParameterSet* in, cIGZCommandParameterSet* out,
                                       cISC4GameDataRegistry* registry) {
    cSC4EffectsManager* manager = nullptr;
    if (registry) {
        const EntryId entry = registry->GetEntryID(handle);
        manager = static_cast<cSC4EffectsManager*>(registry->GetEntryContext(entry));
    }
    cSC4VisualEffect* effect = nullptr;
    bool created = false;
    if (manager && in && out) {
        cRZString name;
        cS3DVector3 position;
        (*in)[0].GetValString(name);
        (*in)[1].GetValFloat32(position.x);
        (*in)[2].GetValFloat32(position.y);
        (*in)[3].GetValFloat32(position.z);
        if (manager->CreateVisualEffect(name.ToChar(), &effect)) {
            cS3DTransform source;
            source.changeCounter = 1;
            source.flags = 1;                       // kTransformTranslation (0x1002387cf / 0x1005dcc94 write 0x101)
            source.translation[0] = position.x;
            source.translation[1] = position.y;
            source.translation[2] = position.z;
            effect->SetSourceTransform(source);     // vslot 0x40
            effect->Start(0);                       // vslot 0x18
            created = true;
        }
    }
    if (out) {
        cRZVariant result;
        result.SetValBool(created);
        out->SetParameter(1, result);
    }
    if (effect) effect->Release();
    return true;
}

// @ 0x1005dc2a2: two packed render states — lit-ish (blend, source 4 / dest 1) and dark-ish (source 8 / dest 5).
void cSC4EffectsManager::CreateFlashMaterials() {
    flashStateLit_.PackRenderState(true, false, false, false, true, 0, false, -1, 4, 1, -1, -1.0f, -1, -1);
    flashStateDark_.PackRenderState(true, false, false, false, true, 0, false, -1, 8, 5, -1, -1.0f, -1, -1);
}

// The shared lowercase scratch at +0x2d8. The original calls __tolower with the sign-extended char; the port goes
// through unsigned char (identical for the ASCII effect names the resources use).
std::string cSC4EffectsManager::LowercaseName(const char* name) const {
    scratchName_.clear();
    if (name) {
        for (const char* p = name; *p; ++p)
            scratchName_.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
    }
    return scratchName_;
}

// The description selection of CreateVisualEffect(name): the highest version that is > 0 and <= maxEffectVersion_;
// ties keep the first entry the hash range yields.
int cSC4EffectsManager::ResolveDescriptionByName(const std::string& lowercaseName) const {
    const auto& names = collection_.EffectNames();
    const auto range = names.equal_range(lowercaseName);
    const auto& effects = collection_.Effects();
    int best = -1;
    std::uint32_t bestVersion = 0;
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second < 0 || static_cast<std::size_t>(it->second) >= effects.size()) continue;
        const std::uint32_t version = effects[static_cast<std::size_t>(it->second)].version;
        if (bestVersion < version && version <= maxEffectVersion_) {
            best = it->second;
            bestVersion = version;
        }
    }
    return best;
}

// The description selection of CreateVisualEffect(group,type): the binding's effect index when its version is in
// (0, maxEffectVersion_], -1 otherwise (the original still creates a live effect with no description then).
int cSC4EffectsManager::ResolveDescriptionByKey(std::uint32_t group, std::uint32_t type) const {
    const cSC4EffectsCollection::cEffectKey key{group, type};
    const auto it = effectBindingsByKey_.find(key);
    if (it == effectBindingsByKey_.end()) return -1;
    if (it->second < 0 || static_cast<std::size_t>(it->second) >= collection_.Effects().size()) return -1;
    const std::uint32_t version = collection_.Effects()[static_cast<std::size_t>(it->second)].version;
    if (version == 0 || version > maxEffectVersion_) return -1;
    return it->second;
}

// @ 0x1005e06ce: unknown / empty names are false; the lookup lowercases into the shared scratch and counts the
// multimap entries.
bool cSC4EffectsManager::HasVisualEffect(const char* name) const {
    if (!name || !*name) return false;
    const std::string lower = LowercaseName(name);
    return collection_.EffectNames().count(lower) != 0;
}

// @ 0x1005e075c (vslot 0x30). One reference is returned to the caller.
bool cSC4EffectsManager::CreateVisualEffect(const char* name, cSC4VisualEffect** out) {
    if (!name) return false;
    if (!out) return false;
    if (!*name) return false;
    const std::string lower = LowercaseName(name);
    const auto range = collection_.EffectNames().equal_range(lower);
    if (range.first == range.second) return false;   // the original repeats the lookup here and always fails
    const int index = ResolveDescriptionByName(lower);
    cSC4VisualEffect* effect = NewVisualEffect();
    effect->SetName(lower);
    effect->SetSlot(index);
    if (index >= 0) {
        effect->SetDescription(collection_.Effects()[static_cast<std::size_t>(index)]);
        ActivateNewVisualEffects();
    }
    *out = effect;
    effect->AddRef();
    return true;
}

// @ 0x1005e0c08 (vslot 0x38). A known binding always produces an effect, even when its description version is out of
// range (the slot then stays -1); an unknown binding fails.
bool cSC4EffectsManager::CreateVisualEffect(std::uint32_t group, std::uint32_t type, cSC4VisualEffect** out) {
    if (!out) return false;
    const cSC4EffectsCollection::cEffectKey key{group, type};
    if (effectBindingsByKey_.find(key) == effectBindingsByKey_.end()) return false;
    const int index = ResolveDescriptionByKey(group, type);
    cSC4VisualEffect* effect = NewVisualEffect();
    effect->SetGroup(group);
    effect->SetType(type);
    effect->SetSlot(index);
    if (index >= 0) {
        effect->SetDescription(collection_.Effects()[static_cast<std::size_t>(index)]);
        ActivateNewVisualEffects();
    }
    *out = effect;
    effect->AddRef();
    return true;
}

// @ 0x1005e099c: the pool grows with `new cSC4VisualEffect` (which is handed the manager as its IEffectsManagerLink)
// or reuses the index at the back of the free list. The returned object carries the pool's reference only.
cSC4VisualEffect* cSC4EffectsManager::NewVisualEffect() {
    if (freeVisualEffectSlots_.empty()) {
        auto* effect = new cSC4VisualEffect();
        effect->SetManager(&managerLink_);
        const int index = static_cast<int>(visualEffects_.size());
        newVisualEffectSlots_.push_back(index);
        visualEffects_.emplace_back(effect);
        return effect;
    }
    const int index = freeVisualEffectSlots_.back();
    freeVisualEffectSlots_.pop_back();
    newVisualEffectSlots_.push_back(index);
    return visualEffects_[static_cast<std::size_t>(index)].get();
}

// @ 0x1005e0b2e: append the new slots to the active list (at most ten rounds), then update the effects that were just
// appended. It does nothing while KillOrphanedEffects' guard is set.
void cSC4EffectsManager::ActivateNewVisualEffects() {
    if (killing_ || newVisualEffectSlots_.empty()) return;
    int guard = 10;
    while (!newVisualEffectSlots_.empty()) {
        if (guard == 0) return;
        const std::size_t first = activeVisualEffectSlots_.size();
        activeVisualEffectSlots_.insert(activeVisualEffectSlots_.end(), newVisualEffectSlots_.begin(),
                                        newVisualEffectSlots_.end());
        newVisualEffectSlots_.clear();
        if (first == activeVisualEffectSlots_.size()) return;
        --guard;
        for (std::size_t i = first; i < activeVisualEffectSlots_.size(); ++i)   // forced: a new effect builds all its components
            UpdateVisualEffect(visualEffects_[static_cast<std::size_t>(activeVisualEffectSlots_[i])].get(), true);
    }
}

// @ 0x1005de2b6: only while the manager is "active" (the 0xab96b05f message sets +0x2f1).
void cSC4EffectsManager::UpdateActiveVisualEffects() {
    if (!effectsActive_) return;
    for (int index : activeVisualEffectSlots_)
        UpdateVisualEffect(visualEffects_[static_cast<std::size_t>(index)].get(), false);
    ActivateNewVisualEffects();
}

// @ 0x1005e0102: count the running effects (not the kept ones — quirk), recycle the stopped effects nobody else holds,
// then retire the finished particle systems and activate the new effects.
void cSC4EffectsManager::DeactivateUnusedVisualEffects() {
    activeVisualEffectCount_ = 0;
    killing_ = true;
    bool changed = false;
    std::vector<int> kept;
    kept.reserve(activeVisualEffectSlots_.size());
    for (int index : activeVisualEffectSlots_) {
        auto* effect = visualEffects_[static_cast<std::size_t>(index)].get();
        if (!effect->CheckIfStopped()) {
            ++activeVisualEffectCount_;
            kept.push_back(index);
            continue;
        }
        if (effect->RefCount() != 1) {
            kept.push_back(index);
            continue;
        }
        effect->ClearForReuse();
        freeVisualEffectSlots_.push_back(index);
        changed = true;
    }
    activeVisualEffectSlots_.swap(kept);
    if (changed) {
        std::vector<int> stillRunning;
        stillRunning.reserve(newParticlesEffectSlots_.size());
        for (int index : newParticlesEffectSlots_) {
            auto* effect = particlesEffects_[static_cast<std::size_t>(index)].get();
            if (effect->RefCount() == 1 && !effect->IsStarted())
                freeParticlesEffectSlots_.push_back(index);
            else
                stillRunning.push_back(index);
        }
        newParticlesEffectSlots_.swap(stillRunning);
        stillRunning.clear();
        for (int index : newModelEffectSlots_) {
            auto* effect = modelEffects_[static_cast<std::size_t>(index)].get();
            if (effect->RefCount() == 1 && !effect->IsStarted())
                freeModelEffectSlots_.push_back(index);
            else
                stillRunning.push_back(index);
        }
        newModelEffectSlots_.swap(stillRunning);
        // The decal / geometry-sourced / dynamic-particle pools are not modelled (see the pending ledger).
    }
    killing_ = false;
    ActivateNewVisualEffects();
}

// @ 0x1005e30f8: stop (transition 1) every active effect only the manager references. The killing guard is left set;
// DeactivateUnusedVisualEffects clears it on the next frame (quirk).
void cSC4EffectsManager::KillOrphanedEffects() {
    activeVisualEffectCount_ = 0;
    killing_ = true;
    for (int index : activeVisualEffectSlots_) {
        auto* effect = visualEffects_[static_cast<std::size_t>(index)].get();
        if (effect->RefCount() == 1) effect->Stop(1);
    }
}

// @ 0x1005de004: effects that follow the view rotation re-apply their own source transform (the original passes
// effect+0x88 to SetSourceTransform; no camera rotation is read here — it is already in the transform).
void cSC4EffectsManager::DoRotationChanged() {
    for (int index : activeVisualEffectSlots_) {
        auto* effect = visualEffects_[static_cast<std::size_t>(index)].get();
        if (!effect->CheckEffectFlag(0)) continue;
        cS3DTransform source;
        effect->GetSourceTransform(source);
        effect->SetSourceTransform(source);
    }
}

// @ 0x1005ddf00: record the zoom, derive the LOD level, sample the camera-parameter curve (the original samples it at
// the new zoom) and refresh the active effects.
void cSC4EffectsManager::DoZoomChanged(int zoom) {
    zoom_ = zoom;
    previousLodLevel_ = lodLevel_;
    const int level = (zoom + 1) - lodOffset_;
    lodLevel_ = level > 0 ? level : 1;
    lodScale_ = SampleLodCurve(lodKeys_, static_cast<float>(zoom));
    worldUnitsPerViewportUnit_ = host_ ? host_->WorldUnitsPerViewportUnit() * host_->RenderHeightScale() : 0.0f;
    globalParams_.maxAlpha = worldUnitsPerViewportUnit_;   // the original reads +0xf24 as the particle max alpha
    UpdateActiveVisualEffects();
}

// @ i386 0x0040658e. The components the description made earlier are kept while the current LOD level is inside
// their record's [min, max) range (their LOD scale is refreshed when the level moved) and stopped and dropped
// otherwise; components added from outside (record index -1) are always kept. Then every record whose range holds the
// current level but did not hold the previous one (all of them when forced) gets its component, in record order, with
// the "random group" rule: consecutive records sharing a non-zero group are one weighted pick (chances out of 65536).
void cSC4EffectsManager::UpdateVisualEffect(cSC4VisualEffect* effect, bool forceUpdate) {
    if (!effect) return;
    std::vector<cSC4VisualEffect::cComponentRec>& components = effect->MutableComponents();
    if (!components.empty() && effect->Slot() >= 0 && static_cast<std::size_t>(effect->Slot()) < collection_.Effects().size()) {
        const cSC4EffectDescription& description = collection_.Effects()[static_cast<std::size_t>(effect->Slot())];
        std::size_t kept = 0;
        for (std::size_t i = 0; i < components.size(); ++i) {
            cSC4VisualEffect::cComponentRec& rec = components[i];
            bool keep = rec.reserved40 == 0xffffffffull;
            if (!keep && rec.reserved40 < description.records.size()) {
                const cSC4EffectDescription::cDescriptionRec& record = description.records[static_cast<std::size_t>(rec.reserved40)];
                if (!forceUpdate_ && record.flagA <= lodLevel_ && lodLevel_ < record.flagB) {
                    if (lodLevel_ != previousLodLevel_) UpdateLODScale(record, &rec);
                    keep = true;
                }
            }
            if (!keep) {
                if (rec.component) {
                    rec.component->Stop(1);
                    rec.component->Release();
                    rec.component = nullptr;
                }
                continue;
            }
            if (kept != i) components[kept] = rec;
            ++kept;
        }
        components.resize(kept);
    }
    if (forceUpdate_) {
        int index = -1;
        if (!effect->Name().empty())
            index = ResolveDescriptionByName(effect->Name());
        else
            index = ResolveDescriptionByKey(effect->Group(), effect->Type());
        effect->SetSlot(index);
    }
    if (effect->Slot() < 0 || static_cast<std::size_t>(effect->Slot()) >= collection_.Effects().size()) return;
    const cSC4EffectDescription& description = collection_.Effects()[static_cast<std::size_t>(effect->Slot())];
    cSC4ModelBasedParticlesEffect* chained = nullptr;
    std::uint16_t group = 0;
    std::uint32_t pick = 0, total = 0;
    for (std::size_t r = 0; r < description.records.size(); ++r) {
        const cSC4EffectDescription::cDescriptionRec& record = description.records[r];
        const bool inNow = record.flagA <= lodLevel_ && lodLevel_ < record.flagB;
        const bool inBefore = record.flagA <= previousLodLevel_ && previousLodLevel_ < record.flagB;
        if (record.intValue < 0 || !((inNow && !inBefore) || forceUpdate || forceUpdate_)) continue;
        if (record.shortC == 0) {
            group = 0;
        } else {
            if (group != record.shortC) {
                pick = random_.RandomUint32Uniform(0x10000);
                total = 0;
            }
            group = record.shortC;
            total += record.shortD;
            if (total < pick) continue;
            pick = 0x10000;
        }
        if (!inNow) continue;
        components.emplace_back();
        cSC4VisualEffect::cComponentRec* rec = &components.back();
        rec->reserved40 = r;
        CreateComponentFromDescription(record, rec, &chained);
        rec = &components.back();
        if (!rec->component) {   // a kind the port does not create (decals, dynamic / geometry particles, nested effects)
            components.pop_back();
            continue;
        }
        UpdateLODScale(record, rec);
        rec->transform = record.transform;
        cS3DTransform local = rec->transform;     // the record's transform times the effect's combined source
        nS3D::MultiplyTransform(local, effect->CombinedSource());
        rec->component->SetTransforms(local, effect->WorldTransform());
        if (effect->IsStarted() && !effect->IsStopping()) rec->component->Start(forceUpdate ? 0 : 1);
    }
    effect->ApplyParams();
}

// @ 0x1005e21f8: interpolate the record's two LOD curves at the current level and hand the pair to the component as
// float parameter 0x102. The fraction uses integer division (quirk) and is not clamped.
void cSC4EffectsManager::UpdateLODScale(const cSC4EffectDescription::cDescriptionRec& record,
                                        cSC4VisualEffect::cComponentRec* component) {
    if (!component || !component->component) return;
    const int first = record.flagA;
    const int last = record.flagB;
    float low = record.floats[0];
    float high = record.floats[2];
    if (first < last - 1) {
        const float fraction = static_cast<float>((lodLevel_ - first) / (last - first - 1));
        low = (record.floats[1] - record.floats[0]) * fraction + record.floats[0];
        high = (record.floats[3] - record.floats[2]) * fraction + record.floats[2];
    }
    const float values[2] = {low, high};
    component->component->SetFloatParams(0x102, values, 2);
}

// @ 0x1005e0d32: unregister the message triggers of the previous collection, clear both registries and start a new
// collection.
void cSC4EffectsManager::BeginCollection() {
    if (host_) {
        for (const cSC4MessageTriggerDescription& trigger : collection_.MessageTriggers())
            host_->UnregisterMessageHandler(trigger.message);
    }
    effectBindingsByKey_.clear();
    messageTriggers_.clear();
    collection_.BeginCollection();
}

// @ 0x1005e0dea: close the collection, cache the camera-parameter curve, build the (group,type) -> effect registry
// (first binding wins for a duplicate key) and register every message trigger. The LOD scale is sampled at the view
// zoom.
void cSC4EffectsManager::EndCollection() {
    collection_.EndCollection();
    const cSC4CameraParams& camera = collection_.CameraParams();
    lodKeys_ = camera.keys;
    for (int i = 0; i < 4; ++i) lodRange_[i] = camera.values[i];
    lodScale_ = SampleLodCurve(lodKeys_, host_ ? static_cast<float>(host_->ViewZoom()) : 0.0f);

    for (const auto& binding : collection_.EffectBindings()) {
        const auto range = collection_.EffectNames().equal_range(binding.first);
        for (auto it = range.first; it != range.second; ++it) {
            if (effectBindingsByKey_.find(binding.second) == effectBindingsByKey_.end())
                effectBindingsByKey_.emplace(binding.second, it->second);
        }
    }
    const auto& triggers = collection_.MessageTriggers();
    for (std::size_t i = 0; i < triggers.size(); ++i)
        messageTriggers_[triggers[static_cast<std::size_t>(i)].message] = static_cast<int>(i);
    if (host_) {
        for (const cSC4MessageTriggerDescription& trigger : triggers)
            host_->RegisterMessageHandler(trigger.message);
    }
}

// @ 0x1005e1318. The labels do not match the fields (quirk): the particle-system line prints the free lists in the
// "A" column and the new lists in the "F" column. The port only owns the cSC4ParticlesEffect family, so the other
// three families contribute zero.
void cSC4EffectsManager::GetStatsString(cRZString& out) {
    out.Sprintf("Active effects: N/%d A/%d F/%d T/%d, decals: N/%d A/%d F/%d, particle systems: "
                "N/%d A/%d F/%d, total particles: %d, density: %f\n",
                static_cast<int>(activeVisualEffectCount_),
                static_cast<int>(activeVisualEffectSlots_.size()),
                static_cast<int>(freeVisualEffectSlots_.size()),
                static_cast<int>(visualEffects_.size()),
                static_cast<int>(decalCount_), 0, 0,
                static_cast<int>(particleSystemCount_),
                static_cast<int>(newParticlesEffectSlots_.size()),
                static_cast<int>(freeParticlesEffectSlots_.size()),
                static_cast<int>(particlesDrawn_),
                static_cast<double>(globalParams_.rateScale));
}

// @ i386 0x00407524 (PARTIAL: the effect-map overlay, the depth sort of the drawn systems, the decal and
// geometry-sourced pools and the debug bounding boxes are not ported). Every running particle system advances by the
// time the animation ticks gathered since the last frame; the ones whose bounds are in view are drawn, the rest only
// simulate. Then the screen flash / lighting tint, the recycling of finished effects, and the tick time restarts.
bool cSC4EffectsManager::Update(SC4DrawContext* context) {
    const float elapsed = elapsedSeconds_;
    IParticlesDrawContext* draw = host_ ? host_->ParticlesDrawContext(context) : nullptr;
    particlesDrawn_ = 0;
    std::uint32_t systems = 0;
    auto run = [&](cSC4ParticlesEffect* effect) {
        if (!effect || !effect->IsStarted()) return;
        const bool visible = draw && host_->IsBoundingBoxInView(effect->BoundingBox());
        const int before = effect->LiveParticleCount();
        effect->Update(elapsed, visible ? draw : nullptr);
        static int traced = 0;
        if (std::getenv("SC4_FXDEBUG") && dynamic_cast<cSC4ModelBasedParticlesEffect*>(effect) && traced++ < 60)
            std::fprintf(stderr, "fx: update %p dt %.3f live %d -> %d started %d delayed %d box %.0f,%.0f,%.0f\n", static_cast<void*>(effect), elapsed, before,
                         effect->LiveParticleCount(), effect->IsStarted() ? 1 : 0, effect->IsInDelayedStop() ? 1 : 0,
                         effect->BoundingBox().min[0], effect->BoundingBox().min[1], effect->BoundingBox().min[2]);
        if (visible) {
            ++systems;
            particlesDrawn_ += static_cast<std::uint32_t>(effect->LiveParticleCount());
        }
    };
    for (int index : newParticlesEffectSlots_) run(particlesEffects_[static_cast<std::size_t>(index)].get());
    for (int index : newModelEffectSlots_) run(modelEffects_[static_cast<std::size_t>(index)].get());
    particleSystemCount_ = systems;
    if (0.0f < flashInverseDuration_) DrawScreenFlashes(context, elapsed);
    if (0.0f < tintInverseDuration_) UpdateLightingTint(elapsed);
    DeactivateUnusedVisualEffects();
    elapsedSeconds_ = 0.0f;
    tickCount_ = 0;
    return true;
}

// @ 0x1005e06be: the original's Pick is an empty "return false" (8 bytes).
bool cSC4EffectsManager::Pick(const cS3DVector3& origin, const cS3DVector3& direction, SC4DrawContext* context,
                              float* distance) {
    (void)origin;
    (void)direction;
    (void)context;
    (void)distance;
    return false;
}

// @ 0x1005e1464: for every event of the effect's description start the matching ancilliary (shake, screen flash or
// lighting tint) at the world position the original derives from the effect's and source transforms. Event type bits:
// 1 = shake, 2 = "the event carries a screen position", 4 = screen flash, 8 = lighting tint; the description index is
// the event's flags word (+0x24). The shake handle is remembered on the effect (+0x188) for StopAncilliary.
void cSC4EffectsManager::StartAncilliary(cSC4VisualEffect* effect) {
    if (!effect || !host_) return;
    cS3DTransform t;
    cS3DTransform s;
    effect->GetEffectTransform(t);
    effect->GetSourceTransform(s);
    // The original's asymmetric dot products: row 0 of the effect transform for x, column 2 for y.
    const float x = (t.rotation[0][2] * s.translation[2] + t.rotation[0][1] * s.translation[1] +
                     t.rotation[0][0] * s.translation[0]) * t.scale + t.translation[0];
    const float y = (s.translation[2] * t.rotation[2][2] + s.translation[1] * t.rotation[1][2] +
                     s.translation[0] * t.rotation[0][2]) * t.scale + t.translation[1];
    effect->SetAncilliaryHandle(0);
    const int slot = effect->Slot();
    if (slot < 0 || static_cast<std::size_t>(slot) >= collection_.Effects().size()) return;
    const cSC4EffectDescription& description = collection_.Effects()[static_cast<std::size_t>(slot)];
    const auto& shakes = collection_.Shakes();
    const auto& lights = collection_.Lights();
    for (const cSC4EffectDescription::cEventRec& event : description.events) {
        const std::uint64_t type = event.type;
        const std::uint32_t index = event.flags;
        cS3DVector2 position{x, y};
        cS3DVector2* positionPtr = (type & 2) != 0 ? &position : nullptr;
        if ((type & 1) != 0) {
            if (index < shakes.size()) {
                if (host_) effect->SetAncilliaryHandle(host_->AddShake(shakes[index], positionPtr));
            }
            continue;
        }
        if ((type & 4) != 0) {
            if (index < lights.size()) StartScreenFlash(lights[index], positionPtr, event.delay);
        } else if ((type & 8) != 0) {
            if (index < lights.size()) StartLightingTint(lights[index]);
        }
    }
}

// @ 0x1005e162e: copy the light's colour / intensity curves, set the duration and its reciprocal and reset the
// elapsed time; optionally remember a point the flash is centred on.
void cSC4EffectsManager::StartScreenFlash(const cSC4LightDescription& light, cS3DVector2* screenPosition, float scale) {
    flashColours_ = light.colors;
    flashIntensities_ = light.intensities;
    flashDuration_ = light.range;
    flashInverseDuration_ = 1.0f / flashDuration_;
    flashTime_ = 0.0f;
    if (!screenPosition) {
        flashAtPoint_ = false;
        return;
    }
    flashAtPoint_ = true;
    flashPoint_ = *screenPosition;
    flashPointScale_ = scale;
}

// @ 0x1005e16de: the same for the lighting tint.
void cSC4EffectsManager::StartLightingTint(const cSC4LightDescription& light) {
    tintColours_ = light.colors;
    tintIntensities_ = light.intensities;
    tintDuration_ = light.range;
    tintInverseDuration_ = 1.0f / tintDuration_;
    tintTime_ = 0.0f;
}

// @ 0x1005e174a: drop the shake, then start the description's label as a new effect at the same transforms
// (transition 0). The label is the description's +0x50 string, not an event.
void cSC4EffectsManager::StopAncilliary(cSC4VisualEffect* effect) {
    if (!effect) return;
    if (effect->AncilliaryHandle() != 0) {
        if (host_) host_->RemoveShake(effect->AncilliaryHandle());
        effect->SetAncilliaryHandle(0);
    }
    const int slot = effect->Slot();
    if (slot < 0 || static_cast<std::size_t>(slot) >= collection_.Effects().size()) return;
    const cSC4EffectDescription& description = collection_.Effects()[static_cast<std::size_t>(slot)];
    if (description.label.empty()) return;
    cSC4VisualEffect* ending = nullptr;
    if (!CreateVisualEffect(description.label.c_str(), &ending)) return;
    cS3DTransform source;
    cS3DTransform effectTransform;
    effect->GetSourceTransform(source);
    effect->GetEffectTransform(effectTransform);
    ending->SetSourceTransform(source);
    ending->SetEffectTransform(effectTransform);
    ending->Start(0);
    ending->Release();
}

// @ 0x1005dff0a (PARTIAL: the LightingManager call is a host seam; the curve sampling and timing are recovered).
void cSC4EffectsManager::UpdateLightingTint(float elapsed) {
    const float t = elapsed * tintInverseDuration_ + tintTime_;
    tintTime_ = t;
    if (t > 1.0f) {
        tintInverseDuration_ = 0.0f;
        if (host_) host_->ClearLightingTint();
        return;
    }
    cS3DVector3 colour{0, 0, 0};
    const std::size_t count = tintColours_.size();
    if (count == 1) {
        colour = tintColours_[0];
    } else if (count > 1) {
        const float scaled = t * static_cast<float>(count - 1);
        int index = static_cast<int>(std::floor(scaled));
        if (index < 0) index = 0;
        if (static_cast<std::size_t>(index) >= count - 1) index = static_cast<int>(count) - 2;
        const float fraction = scaled - static_cast<float>(index);
        const cS3DVector3& a = tintColours_[static_cast<std::size_t>(index)];
        const cS3DVector3& b = tintColours_[static_cast<std::size_t>(index) + 1];
        colour.x = b.x * fraction + a.x * (1.0f - fraction);
        colour.y = b.y * fraction + a.y * (1.0f - fraction);
        colour.z = b.z * fraction + a.z * (1.0f - fraction);
    }
    float intensity = 0.0f;
    const std::size_t intensityCount = tintIntensities_.size();
    if (intensityCount == 1) {
        intensity = tintIntensities_[0];
    } else if (intensityCount > 1) {
        const float scaled = t * static_cast<float>(intensityCount - 1);
        int index = static_cast<int>(std::floor(scaled));
        if (index < 0) index = 0;
        if (static_cast<std::size_t>(index) >= intensityCount - 1) index = static_cast<int>(intensityCount) - 2;
        const float fraction = scaled - static_cast<float>(index);
        intensity = tintIntensities_[static_cast<std::size_t>(index)];
        if (fraction > 0.0f)
            intensity += fraction * (tintIntensities_[static_cast<std::size_t>(index) + 1] - intensity);
    }
    if (host_) host_->SetLightingTint(intensity, colour);
}

// @ 0x1005dfb64 (PARTIAL: the renderer quad and the two packed states are ported, the colour/intensity curve lookup and
// the draw call are not because they need the draw context's driver). The elapsed-time bookkeeping is recovered.
void cSC4EffectsManager::DrawScreenFlashes(SC4DrawContext* context, float elapsed) {
    (void)context;
    const float t = elapsed * flashInverseDuration_ + flashTime_;
    flashTime_ = t;
    if (t > 1.0f) flashInverseDuration_ = 0.0f;
}

// @ 0x1005e2a74: allocate or recycle a cSC4ParticlesEffect, Init it, hand it the manager's global parameters and the
// vertex-buffer extension (when render properties enable the fast path — the host decides by returning null or not).
cSC4ParticlesEffect* cSC4EffectsManager::NewParticlesEffect() {
    if (freeParticlesEffectSlots_.empty()) {
        auto* effect = new cSC4ParticlesEffect();
        effect->Init();
        effect->SetGlobalParams(&globalParams_);
        if (vertexBufferExtension_) effect->SetVertexBufferExtension(vertexBufferExtension_);
        const int index = static_cast<int>(particlesEffects_.size());
        newParticlesEffectSlots_.push_back(index);
        particlesEffects_.emplace_back(effect);
        return effect;
    }
    const int index = freeParticlesEffectSlots_.back();
    freeParticlesEffectSlots_.pop_back();
    newParticlesEffectSlots_.push_back(index);
    return particlesEffects_[static_cast<std::size_t>(index)].get();
}

namespace {
// A particle system as a component of a visual effect (the original's particle classes are cISC4ComponentEffects
// themselves; the port's particle classes keep the particle-only interface, so the component is this adapter). It holds
// one reference on the system; the manager's pool holds the other.
class ParticlesComponent final : public cISC4VisualEffectComponent {
public:
    explicit ParticlesComponent(cSC4ParticlesEffect* effect) : effect_(effect) { effect_->AddRef(); }
    ~ParticlesComponent() override { effect_->Release(); }
    void Start(int transition) override { effect_->Start(transition); }
    void Stop(int transition) override { effect_->Stop(transition); }
    bool IsStarted() const override { return effect_->IsStarted(); }
    void SetTransforms(const cS3DTransform& effect, const cS3DTransform& source) override { effect_->SetTransforms(effect, source); }
    void ApplyEffect(float elapsed) override { effect_->ApplyEffect(elapsed); }
    bool SetFloatParams(int type, const float* values, int count) override { return effect_->SetFloatParams(type, values, count); }
    bool SetVoidParams(int type, void** values, int count) override { return effect_->SetVoidParams(type, values, count); }

private:
    cSC4ParticlesEffect* effect_;
};

bool ModelBased(const cSC4ParticlesDescription& description) { return (description.flag10 & 1) != 0; }
}  // namespace

// @ i386 0x00406246. Kind 0 = particle systems (count of them, each delayed by the record's spacing; model-based ones
// can chain), 3..15 = the component description tables (Instantiate). Decals (1), nested visual effects (2), dynamic
// particles (16) and geometry-sourced particle systems are not ported: the component stays empty and the caller drops
// it. Bit 0 of the record's type marks the component "not needed to finish".
void cSC4EffectsManager::CreateComponentFromDescription(const cSC4EffectDescription::cDescriptionRec& record,
                                                        cSC4VisualEffect::cComponentRec* component,
                                                        cSC4ModelBasedParticlesEffect** chained) {
    const std::uint32_t kind = record.kind;
    if (kind == 0) {
        if (record.intValue < 0 || static_cast<std::size_t>(record.intValue) >= collection_.Particles().size()) return;
        auto* description = const_cast<cSC4ParticlesDescription*>(&collection_.Particles()[static_cast<std::size_t>(record.intValue)]);
        if (std::getenv("SC4_FXDEBUG"))
            std::fprintf(stderr, "fx: record %s kind 0 idx %d count %u flag10 %02x flags %08llx model %08x ids %zu geom %.2f terrain %p\n", record.name.c_str(), record.intValue,
                         record.shortA, description->flag10, static_cast<unsigned long long>(description->flags), description->textureId, description->modelIds.size(),
                         description->values278[0], static_cast<void*>(ParticlesTerrain()));
        for (int i = 0; i < static_cast<int>(record.shortA); ++i) {
            cSC4ParticlesEffect* effect = nullptr;
            cSC4ModelBasedParticlesEffect* modelBased = nullptr;
            if (!ModelBased(*description)) {
                effect = NewParticlesEffect();
            } else if (description->values278[0] != 0.0f) {
                continue;   // geometry-sourced particles (NewGeometrySourcedParticlesEffect) are not ported
            } else {
                effect = modelBased = NewModelBasedParticlesEffect();
            }
            if (!effect) return;
            if (modelBased) modelBased->SetDescription(description);
            else effect->SetDescription(description);
            if (i > 0) {
                const float delay = static_cast<float>(static_cast<std::uint32_t>(record.shortB) * static_cast<std::uint32_t>(i));
                effect->SetFloatParams(0x101, &delay, 1);
            }
            if (!modelBased || (record.type & 2) == 0) {
                *chained = nullptr;
            } else {
                if (*chained) {
                    (*chained)->SetChainSystem(modelBased);
                    modelBased->SuppressEmission();
                }
                *chained = modelBased;
            }
            auto* wrapped = new ParticlesComponent(effect);
            wrapped->AddRef();
            if (component->component) component->component->Release();
            component->component = wrapped;
        }
    } else if (kind >= 3 && kind - 3 < 13) {
        if (record.intValue < 0) return;
        const auto& table = collection_.ComponentDescs()[kind - 3].descriptions;
        if (static_cast<std::size_t>(record.intValue) >= table.size()) return;
        cISC4ComponentDescription* description = table[static_cast<std::size_t>(record.intValue)].get();
        cISC4VisualEffectComponent* made = description ? description->Instantiate() : nullptr;
        if (!made) return;
        made->AddRef();
        component->component = made;
        component->flags |= 2;
    }
    if (record.type & 1) component->flags |= 4;
    else component->flags &= ~std::uint64_t{4};
}

// @ i386 0x00405ed8: a recycled system from the free list, or a new one Init'ed with the global parameters.
cSC4ModelBasedParticlesEffect* cSC4EffectsManager::NewModelBasedParticlesEffect() {
    if (!freeModelEffectSlots_.empty()) {
        const int index = freeModelEffectSlots_.back();
        freeModelEffectSlots_.pop_back();
        newModelEffectSlots_.push_back(index);
        return modelEffects_[static_cast<std::size_t>(index)].get();
    }
    if (!ParticlesTerrain()) return nullptr;   // Init sizes the system from the terrain
    auto* effect = new cSC4ModelBasedParticlesEffect();
    effect->Init();
    effect->SetGlobalParams(&globalParams_);
    newModelEffectSlots_.push_back(static_cast<int>(modelEffects_.size()));
    modelEffects_.emplace_back(effect);
    return effect;
}

// @ i386 0x00409e36 (the packed half; the plug-in "main.fx" text files need the SCRes parser host and are not loaded).
void cSC4EffectsManager::LoadAllEffects() {
    if (!host_) return;
    std::vector<std::vector<std::uint8_t>> libraries;
    host_->PackedEffectsResources(libraries);
    if (libraries.empty()) return;
    BeginCollection();
    for (const std::vector<std::uint8_t>& bytes : libraries) {
        PortMemoryIStream in(bytes.data(), bytes.size());
        cSC4EffectsResource library;
        if (library.Read(in)) library.AddToCollection(&collection_);
    }
    EndCollection();
}

// @ 0x1005e31cc: allocate the terrain snowline effect on first use, reset the current altitude (which forces the first
// update) and update; disabling removes the effect and clears the id.
void cSC4EffectsManager::SetSnowlineState(bool enabled) {
    if (!host_) return;
    if (enabled) {
        if (snowlineEffectId_ == -1) snowlineEffectId_ = host_->AddSnowlineEffect();
        snowlineCurrent_ = 0.0f;
        UpdateSnowline();
        return;
    }
    if (snowlineEffectId_ != -1) {
        host_->RemoveSnowlineEffect(snowlineEffectId_);
        snowlineEffectId_ = -1;
    }
}

// @ 0x1005de13c: interpolate between the two seasonal altitudes with cos(dayOfYear / 364 * 2pi), mirrored by half a
// year in the southern hemisphere (city hemisphere 2), and push it to the terrain view when it moved more than 8
// units. The original's call to the terrain view has more arguments than Ghidra recovered; the port passes the handle,
// the altitude and the bias.
void cSC4EffectsManager::UpdateSnowline() {
    if (snowlineEffectId_ == -1 || !host_) return;
    const int dayOfYear = host_->SimulatorDayOfYear();
    float t = static_cast<float>(dayOfYear - 1) / 364.0f;
    if (host_->CityHemisphere() == 2) {
        t += 0.5f;
        if (t > 1.0f) t -= 1.0f;
    }
    const float wave = std::cos(t * 6.2831855f);
    const float target = (snowlineLow_ - snowlineHigh_) * (wave + 1.0f) * 0.5f + snowlineHigh_;
    if (std::fabs(target - snowlineCurrent_) > 8.0f) {
        snowlineCurrent_ = target;
        host_->SetSnowlineAltitude(snowlineEffectId_, snowlineCurrent_, snowlineBias_);
    }
}

// @ 0x1005de054: when the effect resource changed, re-open the collection (the parse goes through the host), force a
// description update on every active effect, then take the weather wind into the global parameters and, on December
// 19th, turn the snowline on.
void cSC4EffectsManager::DoEffectsTick() {
    if (effectsResource_ && host_ && host_->EffectsResourceChanged()) {
        BeginCollection();
        host_->LoadEffectsFiles();
        EndCollection();
        forceUpdate_ = true;
        UpdateActiveVisualEffects();
        forceUpdate_ = false;
        host_->EffectsResourceReloaded();
    }
    if (weatherSim_ && host_) {
        float windX = 0.0f;
        float windZ = 0.0f;
        host_->WeatherWind(windX, windZ);
        globalParams_.velocity.x = windX;
        globalParams_.velocity.y = 0.0f;
        globalParams_.velocity.z = windZ;
        cRZDate today;
        if (today.Month() == 12 && today.DayOfMonth() == 19) SetSnowlineState(true);
    }
}

// @ 0x1005ddd1c (PARTIAL: the tick accumulation is recovered; the cursor ray and the per-effect Update go through the
// view manager / IParticlesDrawContext seam and are not ported).
void cSC4EffectsManager::DoAnimationTick(std::uint32_t elapsedMs) {
    std::uint32_t scaled = elapsedMs;
    if (host_) scaled = static_cast<std::uint32_t>(std::max(0.0f, static_cast<float>(elapsedMs) * host_->SimulatorSpeed()));
    tickCount_ += scaled;
    elapsedSeconds_ = static_cast<float>(static_cast<double>(tickCount_) * 1e-06);
    // @ i386 0x0040448c: every running effect applies this tick (the cursor-ray parameter of effects with flag 5 is
    // not ported).
    const float step = static_cast<float>(static_cast<double>(scaled) * 1e-06);
    for (int index : activeVisualEffectSlots_) {
        cSC4VisualEffect* effect = visualEffects_[static_cast<std::size_t>(index)].get();
        if (effect->IsStarted()) effect->ApplyEffect(step);
    }
}

// @ 0x1005dd7b8 (PARTIAL). The dispatch of the recovered message types is ported: the zoom/rotation notifications,
// the snowline update, the effects tick, the "effects active" flag and the cursor-ray broadcast. The message-trigger
// effects, the cheat console, the occupant insertion/removal, the animation tick and the render-properties refresh
// need the payload/transform reconstruction Ghidra dropped or the unrecovered neighbours; they are left out.
bool cSC4EffectsManager::DoMessage(cIGZMessage2* message) {
    if (!message) return false;
    const std::uint32_t type = message->GetType();
    if (type == 0xa6b79602 || type == 0xa6b79620) {
        auto* standard = static_cast<cIGZMessage2Standard*>(message);
        const auto* oldZoom = static_cast<const int*>(standard->GetVoid1());
        const auto* newZoom = static_cast<const int*>(standard->GetVoid2());
        if (oldZoom && newZoom) {
            if (type == 0xa6b79620 || oldZoom[0] != newZoom[0]) DoZoomChanged(newZoom[0]);
            if (oldZoom[1] != newZoom[1]) DoRotationChanged();
        }
        return true;
    }
    if (type == 0x2acd2cf3) {
        auto* standard = static_cast<cIGZMessage2Standard*>(message);
        const int x = static_cast<int>(standard->GetData1());
        const int y = static_cast<int>(standard->GetData2());
        float ray[6] = {0, 0, 0, 0, 0, 0};
        if (host_ && host_->ViewportPositionToRay(x, y, ray, ray + 3)) {
            for (int index : activeVisualEffectSlots_) {
                auto* effect = visualEffects_[static_cast<std::size_t>(index)].get();
                if (effect->IsStarted()) effect->SetVectorParams(0xe, reinterpret_cast<cS3DVector3*>(ray), 2);
            }
        }
        return true;
    }
    if (type == 0x86ad10ee) {   // the animation tick manager's tick: data 2 = the elapsed microseconds
        DoAnimationTick(static_cast<std::uint32_t>(static_cast<cIGZMessage2Standard*>(message)->GetData2()));
        return true;
    }
    switch (type) {
        case 0x66956814: UpdateSnowline(); break;
        case 0x697d01ed: DoEffectsTick(); break;
        case 0xab96b05f: effectsActive_ = true; UpdateActiveVisualEffects(); break;
        case 0x26d31ec2: suppressOccupantEffects_ = true; break;
        default: break;
    }
    return true;
}

// @ 0x1005de8d4 (a debug cheat: "Snow [low] [high] [bias]"; with no arguments it toggles).
bool cSC4EffectsManager::DoSnowCheat(cRZCmdLine& commandLine) {
    const int argc = commandLine.argc();
    if (argc > 1) snowlineLow_ = static_cast<float>(RZAtofA(commandLine.argv(1)));
    if (argc > 2) snowlineHigh_ = static_cast<float>(RZAtofA(commandLine.argv(2)));
    if (argc > 3) snowlineBias_ = static_cast<float>(RZAtofA(commandLine.argv(3)));
    bool enable = true;
    if (argc < 2) enable = (snowlineEffectId_ == -1);
    SetSnowlineState(enable);
    return true;
}

// @ 0x1005de30a (PARTIAL): needs the render-properties object graph (+0x30 / +0x48 sub-objects), the lighting manager
// and the atmospheric / water effect lookup; not ported.
void cSC4EffectsManager::DoRenderPropertiesChanged() {}

// @ 0x1005de68c (PARTIAL): needs the terrain effect maps and the occupant's box / flags (plus the free IsStaticBuilding
// helper at 0x1005e3337); not ported.
void cSC4EffectsManager::DoOccupantInserted(cISC4Occupant* occupant) { (void)occupant; }

// @ 0x1005de762 (PARTIAL): same neighbours as DoOccupantInserted plus the occupant-manager box query; not ported.
void cSC4EffectsManager::DoOccupantRemoved(cISC4Occupant* occupant) { (void)occupant; }

// 0x1005dc8c9 (the cRZCallbackTimer callback): the original ignores its user pointer and posts kEffectsTickMessage
// through the message server so the manager's DoMessage runs on the message thread.
void cSC4EffectsManager::TickCallback(void* userData) { EffectsTickCallback(userData); }

}  // namespace sc4

// The free function the callback timer is armed with (0x1005dc8c9).
void EffectsTickCallback(void* userData) {
    (void)userData;
    if (sc4::IEffectsManagerHost* host = sc4::EffectsManagerHost()) host->PostMessage(sc4::kEffectsTickMessage);
}
