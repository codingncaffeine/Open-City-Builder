#include "app/NativeAutomata.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "gz/cGZPersistResourceManager.h"
#include "rz/cRZMessage2Standard.h"
#include "s3d/ModelInstance.h"
#include "s3d/ModelInstanceAnimatedSprite.h"
#include "sc4/Automaton.h"
#include "sc4/TrafficAutomaton.h"
#include "sc4/TrafficNetworkMap.h"
#include "sc4/Render3D.h"
#include "sc4/ServiceLocator.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

namespace native {
sc4::cSC4AutomataController* NativeFindAttraction(sc4::cISC4Occupant* occupant, cS3DVector3* direction, float* distance, float* strength);   // app/NativeAutomataControllerHost.cpp
sc4::IAutomatonGroupTemplate* NativeFindAutomatonGroup(std::uint32_t id);                                                                  // app/NativeAutomataManagerHost.cpp
s3d::cS3DModelInstanceAnimatedSprite* NativeCreateSpriteInstance(const cGZPersistResourceKey& key);   // app/NativeSprites.cpp
namespace {
using namespace sc4;

constexpr std::uint32_t kGZIID_cIS3DResModel = 0x9ad0e7e3;

sc4::cSC43DRender* g_render = nullptr;
sc4::cSC4City* g_city = nullptr;
std::vector<sc4::cSC4Automaton*> g_automata;   // the started automata (one reference each)
std::chrono::steady_clock::time_point g_lastTick = std::chrono::steady_clock::now();
int g_zoom = 5, g_rotation = 0;
std::vector<s3d::cIS3DModelInstance*> g_pending;   // models made before the renderer existed

// One model instance of an automaton (the interface the recovered class drives).
class AutomatonModel final : public IAutomatonModel {
public:
    // `animated` is the same instance's animated interface when it has one (the sprites: frames, opacity, view).
    explicit AutomatonModel(s3d::cIS3DModelInstance* instance, s3d::cIS3DModelInstanceAnimated* animated = nullptr)
        : instance_(instance), animated_(animated) {}
    s3d::cIS3DModelInstance* Instance() const { return instance_; }
    void AddRef() override { ++refs_; }
    void Release() override { if (--refs_ <= 0) { instance_->Release(); delete this; } }
    void GetBounds(float* minXyz, float* maxXyz) override {
        cS3DBoundingBox box;
        instance_->GetBounds(box);
        for (int i = 0; i < 3; ++i) { minXyz[i] = box.min[i]; maxXyz[i] = box.max[i]; }
    }
    void GetTransform(cS3DTransform& out) override { instance_->GetTransform(out); }
    void SetTransform(const cS3DTransform& transform) override { instance_->SetTransform(transform); }
    void SetOpacity(float opacity, bool translucent) override { if (animated_) animated_->SetOpacity(translucent, opacity); }
    void SetAutoFrameRate(float rate) override { if (animated_) animated_->SetAutoFrameRate(rate); }
    void UpdateAnimation(float elapsed) override { if (animated_) animated_->DoAutoFrameUpdate(elapsed); }
    int GetFrame() const override { return animated_ ? animated_->GetFrame() : 0; }
    int GetFrameCount() const override { return animated_ ? animated_->GetFrameCount() : 1; }
    void SetZoomAndRotation(int zoom, int rotation) override { if (animated_) animated_->SetZoomAndRotation(zoom, rotation); }
    void SetPlayMode(s3d::PlayMode mode) override { if (animated_) animated_->SetPlayMode(mode); }

private:
    s3d::cIS3DModelInstance* instance_;
    s3d::cIS3DModelInstanceAnimated* animated_;
    int refs_ = 1;
};

class NullEffects final : public IAutomatonEffects {
public:
    void SetModel(IAutomatonModel*) override {}
    bool Init() override { return true; }
    void Update(const cS3DVector3&, const cS3DVector3&, float) override {}
    void StartAll() override {}
    void StopAll(bool) override {}
    void StartNamed(const cRZString&, int, bool) override {}
    void StopNamed(const cRZString&, int) override {}
    void ToggleNamed(const cRZString&, int, bool) override {}
    void RecreateNamed(const cRZString&, int, bool) override {}
    bool AddRegEffect(const cRZString&, const cRZString&) override { return true; }
    void* FindRegEffect(const cRZString&) override { return nullptr; }
    void RemoveRegEffect(const cRZString&) override {}
    void Shutdown() override {}
};

sc4::cSTETerrain* Terrain() {
    if (!g_city) return nullptr;
    cIGZUnknown* object = g_city->GetComponent(sc4::CityComponent::Terrain);
    void* serializable = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &serializable)) return nullptr;
    auto* terrain = static_cast<sc4::cSTETerrain*>(static_cast<cIGZSerializable*>(serializable));
    static_cast<cIGZSerializable*>(serializable)->Release();
    return terrain;
}

cSC4TrafficNetworkMap* NetworkMap() {
    cIGZUnknown* object = g_city ? g_city->GetComponent(CityComponent::TrafficNetwork) : nullptr;
    return object ? dynamic_cast<cSC4TrafficNetworkMap*>(object) : nullptr;
}

class AutomataHost final : public IAutomatonHost {
public:
    IAutomatonModel* CreateModel(const cGZPersistResourceKey& key) override {
        if (key.type == s3d::kGZIID_cIS3DResSpriteAnim) {   // a sprite animation (GetAnimatedModelFromResourceKey's first branch)
            s3d::cS3DModelInstanceAnimatedSprite* sprite = NativeCreateSpriteInstance(key);
            IAutomatonModel* model = sprite ? new AutomatonModel(sprite->AsModelInstance(), sprite) : nullptr;
            if (ModelTrace()) std::fprintf(stderr, "automata: model %p created from sprite %08x\n", static_cast<void*>(model), key.instance);
            return model;
        }
        void* resource = nullptr;
        if (!SL::spRM || !SL::spRM->GetResource(key, kGZIID_cIS3DResModel, &resource, 0, nullptr) || !resource) return nullptr;
        auto* res = static_cast<s3d::cIS3DResModel*>(resource);
        s3d::cIS3DModelInstance* instance = nullptr;
        const bool ok = res->CreateModelInstance(0, &instance);
        res->Release();
        if (std::getenv("SC4_DEBUG")) { static int shown = 0; if (shown++ < 4) std::fprintf(stderr, "automata: model %08x/%08x/%08x -> %s\n", key.type, key.group, key.instance, ok && instance ? "ok" : "none"); }
        IAutomatonModel* model = ok && instance ? new AutomatonModel(instance) : nullptr;
        if (ModelTrace()) std::fprintf(stderr, "automata: model %p created from %08x\n", static_cast<void*>(model), key.instance);
        return model;
    }
    void DestroyModel(IAutomatonModel* model) override {
        if (ModelTrace()) std::fprintf(stderr, "automata: model %p destroyed\n", static_cast<void*>(model));
        if (model) model->Release();
    }
    IAutomatonEffects* CreateEffects() override { return new NullEffects(); }
    void DestroyEffects(IAutomatonEffects* effects) override { delete effects; }
    bool AddModelToRenderer(IAutomatonModel* model) override {
        auto* instance = static_cast<AutomatonModel*>(model)->Instance();
        if (std::getenv("SC4_DEBUG")) { static int n = 0; if (n++ < 3) { cS3DTransform t; instance->GetTransform(t); std::fprintf(stderr, "automata: insert model %p render %p pos %.1f %.1f %.1f\n", static_cast<void*>(instance), static_cast<void*>(g_render), t.translation[0], t.translation[1], t.translation[2]); } }
        if (!g_render) { g_pending.push_back(instance); return true; }
        const bool inserted = g_render->InsertModel(instance);
        if (ModelTrace()) { cS3DTransform t; instance->GetTransform(t); std::fprintf(stderr, "automata: model %p insert %d at (%.0f, %.1f, %.0f)\n", static_cast<void*>(model), inserted ? 1 : 0, t.translation[0], t.translation[1], t.translation[2]); }
        return inserted;
    }
    bool RemoveModelFromRenderer(IAutomatonModel* model) override {
        auto* instance = static_cast<AutomatonModel*>(model)->Instance();
        for (auto it = g_pending.begin(); it != g_pending.end(); ++it) if (*it == instance) { g_pending.erase(it); return true; }
        const bool removed = g_render && g_render->RemoveModel(instance);
        if (ModelTrace()) std::fprintf(stderr, "automata: model %p remove %d\n", static_cast<void*>(model), removed ? 1 : 0);
        return removed;
    }
    static bool ModelTrace() { static const bool on = std::getenv("SC4_MODELDEBUG") != nullptr; return on; }   // every renderer insert / remove
    void FadeModelIn(IAutomatonModel*) override {}
    void FadeModelOut(IAutomatonModel*, int) override {}
    bool InsertOccupant(cISC4Occupant*) override { return true; }
    void RemoveOccupant(cISC4Occupant*) override {}
    bool RefreshOccupant(cISC4Occupant*) override { return true; }
    bool HasCity() override { return g_city != nullptr; }
    float CitySizeX() override { auto* t = Terrain(); return t ? static_cast<float>(t->CellCountX()) * 16.0f : 0.0f; }
    float CitySizeZ() override { auto* t = Terrain(); return t ? static_cast<float>(t->CellCountZ()) * 16.0f : 0.0f; }
    float TerrainHeight(float x, float z) override { auto* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    bool ConstrainVectorToNetworkTile(const cS3DVector3& from, cS3DVector3& point, std::uint32_t flags) override {
        cSC4TrafficNetworkMap* map = NetworkMap();
        return map && map->ConstrainVectorToNetworkTile(from, point, flags);
    }
    float RoadAltitude(float x, float z) override {
        cSC4TrafficNetworkMap* map = NetworkMap();
        return map ? map->GetRoadAltitude(x, z) : TerrainHeight(x, z);
    }
    bool NetworkAt(int, int) override { return false; }
    bool LotAt(int, int) override { return false; }
    void PlaceOnNetwork(const cS3DVector3&, const cS3DVector3&, std::uint32_t) override {}
    cSC4AutomataController* FindAttraction(cISC4Occupant* occupant, cS3DVector3* direction, float* distance, float* strength) override {
        static const bool off = std::getenv("SC4_NOATTRACTION") != nullptr;   // A/B switch
        return off ? nullptr : NativeFindAttraction(occupant, direction, distance, strength);
    }
    IAutomatonGroupTemplate* FindAutomataGroup(std::uint32_t id) override { return NativeFindAutomatonGroup(id); }
    bool AnimationUpdateAllowed() override { return true; }
    float SimSpeedModifier() override { return 1.0f; }
    float MaxSpeedUnits() override { return 3600.0f; }   // the tests of the recovered classes use the same value
    float FadeDurationSeconds() override { return 0.5f; }
    void PostMessage(cIGZMessage2*) override {}
    std::uint32_t GenerateSoundInstanceId() override { return ++sound_; }
    void StopSoundLoop(std::uint32_t) override {}
    void PostAudioEvent(std::uint32_t) override {}
    std::uint32_t RandomBelow(std::uint32_t range) override { return range ? static_cast<std::uint32_t>(rng_() % range) : 0; }
    std::int32_t RandomInt(std::int32_t lo, std::int32_t hi) override { return hi > lo ? lo + static_cast<std::int32_t>(rng_() % static_cast<std::uint32_t>(hi - lo)) : lo; }
    double RandomDouble(double lo, double hi) override { return lo + (hi - lo) * RandomUnit(); }
    double RandomUnit() override { return static_cast<double>(rng_()) / 4294967296.0; }

private:
    std::mt19937 rng_{12345};
    std::uint32_t sound_ = 0;
};

}  // namespace

void InstallAutomataHost() {
    static AutomataHost host;
    sc4::SetAutomatonHost(&host);
}

void SetAutomataRenderer(sc4::cSC43DRender* render, sc4::cSC4City* city) {
    g_render = render;
    g_city = city;
    if (g_render) {
        for (s3d::cIS3DModelInstance* instance : g_pending) g_render->InsertModel(instance);
        g_pending.clear();
    }
}

// The city posts message 0x26d31ec1 once it is up and every automata manager Inits the automata it restored (HandlePostCityInit);
// the managers then tick them. What is left here: the restored automata go into the renderer at the current zoom and rotation.
// PARTIAL: where the original places a restored automaton in the renderer is not traced; this is the port's stand-in.
void PostCityInitToAutomataManagers(sc4::cSC4City* city) {
    if (!city) return;
    if (const char* zoom = std::getenv("SC4_ZOOM")) g_zoom = std::atoi(zoom);
    g_lastTick = std::chrono::steady_clock::now();
    int started = 0, skipped = 0;
    for (sc4::CityComponent component : {sc4::CityComponent::VehicleManager, sc4::CityComponent::PedestrianManager, sc4::CityComponent::AircraftManager,
                                         sc4::CityComponent::WatercraftManager}) {
        for (void* raw : LoadedAutomataOf(city->GetComponent(component))) {
            auto* automaton = static_cast<sc4::cSC4Automaton*>(raw);
            if (automaton->CheckAutomataFlag(0x8000)) { ++skipped; continue; }
            if (!automaton->Init()) { ++skipped; continue; }
            automaton->SetZoomAndRotation(g_zoom, g_rotation);
            automaton->SetVisible(true);   // occupant visibility (the managers tick visible automata) and into the renderer
            automaton->AddRef();
            g_automata.push_back(automaton);
            ++started;
        }
    }
    std::fprintf(stderr, "automata: %d started, %d skipped\n", started, skipped);
    // The managers keep their view rectangle from the camera's messages; none has come since they subscribed, so one camera move is
    // announced now (their HandleViewChanged reads the current view). Without it every automaton counts as offscreen.
    if (SL::spMS2) {
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(0xa6b79602);
        SL::spMS2->MessagePost(message, false);
        message->Release();
    }
}

void TickAutomata() {
    if (g_automata.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    float ms = std::chrono::duration<float, std::milli>(now - g_lastTick).count();
    g_lastTick = now;
    if (ms > 100.0f) ms = 100.0f;
    (void)ms;   // the automata managers tick their automata (cSC4AutomataManager::DoAnimationTick through the animation ticks)
    static int ticks = 0;
    if (std::getenv("SC4_AUTOMATADEBUG") && ticks++ % 60 == 0 && !g_automata.empty()) {
        int alive = 0, moved = 0;
        sc4::cSC4Automaton* sample = nullptr;
        for (sc4::cSC4Automaton* a : g_automata)
            if (!a->Models().empty() && a->GetOpacity() > 0.0f) {
                ++alive;
                if (a->GetActualSpeed() > 0.0f) ++moved;
                if (!sample || a == g_automata.front()) sample = a;
            }
        std::fprintf(stderr, "automata: tick %d visible %d moving %d\n", ticks, alive, moved);
        if (sample) {
            float p[3] = {};
            sample->GetPosition(p);
            auto* traffic = dynamic_cast<sc4::cSC4TrafficAutomaton*>(sample);
            std::fprintf(stderr, "automata: sample type %08x pos %.1f %.1f %.1f speed %.2f self %d reached %d ignorepaths %d occupant %p pathkey %08x state %u\n",
                         sample->GetType(), p[0], p[1], p[2], sample->GetActualSpeed(), sample->GetSelfControlling() ? 1 : 0,
                         sample->GetReachedDestination() ? 1 : 0, sample->CheckAutomataFlag(0x2000) ? 1 : 0,
                         traffic ? static_cast<void*>(traffic->GetNetworkOccupant()) : nullptr, traffic ? traffic->GetPathKey() : 0u,
                         sample->GetState() ? sample->GetState()->stateId : 0u);
        }
        for (std::size_t i = 0; i < g_automata.size() && i < 0; ++i) {
            float p[3] = {};
            g_automata[i]->GetPosition(p);
            std::fprintf(stderr, "automata: tick %d #%zu type %08x pos %.1f %.1f %.1f speed %.2f models %zu opacity %.2f state %u\n", ticks, i,
                         g_automata[i]->GetType(), p[0], p[1], p[2], g_automata[i]->GetActualSpeed(), g_automata[i]->Models().size(),
                         g_automata[i]->GetOpacity(), g_automata[i]->GetState() ? g_automata[i]->GetState()->stateId : 0u);
        }
    }
}

}  // namespace native
