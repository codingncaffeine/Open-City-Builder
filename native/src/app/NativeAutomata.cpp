#include "app/NativeAutomata.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "gz/cGZPersistResourceManager.h"
#include "rz/cRZMessage2Standard.h"
#include "s3d/ModelInstance.h"
#include "sc4/Automaton.h"
#include "sc4/Render3D.h"
#include "sc4/ServiceLocator.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

namespace native {
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
    explicit AutomatonModel(s3d::cIS3DModelInstance* instance) : instance_(instance) {}
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
    void SetOpacity(float, bool) override {}
    void UpdateAnimation(float) override {}
    int GetFrame() const override { return 0; }
    int GetFrameCount() const override { return 1; }
    void SetZoomAndRotation(int, int) override {}
    void SetPlayMode(s3d::PlayMode) override {}

private:
    s3d::cIS3DModelInstance* instance_;
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

class AutomataHost final : public IAutomatonHost {
public:
    IAutomatonModel* CreateModel(const cGZPersistResourceKey& key) override {
        void* resource = nullptr;
        if (!SL::spRM || !SL::spRM->GetResource(key, kGZIID_cIS3DResModel, &resource, 0, nullptr) || !resource) return nullptr;
        auto* res = static_cast<s3d::cIS3DResModel*>(resource);
        s3d::cIS3DModelInstance* instance = nullptr;
        const bool ok = res->CreateModelInstance(0, &instance);
        res->Release();
        if (std::getenv("SC4_DEBUG")) { static int shown = 0; if (shown++ < 4) std::fprintf(stderr, "automata: model %08x/%08x/%08x -> %s\n", key.type, key.group, key.instance, ok && instance ? "ok" : "none"); }
        return ok && instance ? new AutomatonModel(instance) : nullptr;
    }
    void DestroyModel(IAutomatonModel* model) override { if (model) model->Release(); }
    IAutomatonEffects* CreateEffects() override { return new NullEffects(); }
    void DestroyEffects(IAutomatonEffects* effects) override { delete effects; }
    bool AddModelToRenderer(IAutomatonModel* model) override {
        auto* instance = static_cast<AutomatonModel*>(model)->Instance();
        if (std::getenv("SC4_DEBUG")) { static int n = 0; if (n++ < 3) { cS3DTransform t; instance->GetTransform(t); std::fprintf(stderr, "automata: insert model %p render %p pos %.1f %.1f %.1f\n", static_cast<void*>(instance), static_cast<void*>(g_render), t.translation[0], t.translation[1], t.translation[2]); } }
        if (!g_render) { g_pending.push_back(instance); return true; }
        return g_render->InsertModel(instance);
    }
    bool RemoveModelFromRenderer(IAutomatonModel* model) override {
        auto* instance = static_cast<AutomatonModel*>(model)->Instance();
        for (auto it = g_pending.begin(); it != g_pending.end(); ++it) if (*it == instance) { g_pending.erase(it); return true; }
        return g_render && g_render->RemoveModel(instance);
    }
    void FadeModelIn(IAutomatonModel*) override {}
    void FadeModelOut(IAutomatonModel*, int) override {}
    bool InsertOccupant(cISC4Occupant*) override { return true; }
    void RemoveOccupant(cISC4Occupant*) override {}
    bool RefreshOccupant(cISC4Occupant*) override { return true; }
    bool HasCity() override { return g_city != nullptr; }
    float CitySizeX() override { auto* t = Terrain(); return t ? static_cast<float>(t->CellCountX()) * 16.0f : 0.0f; }
    float CitySizeZ() override { auto* t = Terrain(); return t ? static_cast<float>(t->CellCountZ()) * 16.0f : 0.0f; }
    float TerrainHeight(float x, float z) override { auto* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    bool NetworkAt(int, int) override { return false; }
    bool LotAt(int, int) override { return false; }
    void PlaceOnNetwork(const cS3DVector3&, const cS3DVector3&, std::uint32_t) override {}
    cSC4AutomataController* FindAttraction(cISC4Occupant*, cS3DVector3*, float*, float*) override { return nullptr; }
    IAutomatonGroupTemplate* FindAutomataGroup(std::uint32_t) override { return nullptr; }
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

// The city posts message 0x26d31ec1 once it is up and every automata manager then starts the automata it restored. The manager's own
// registry cannot do that yet (its IAutomataAutomaton view is not implemented by the recovered automata), so the restored objects are
// started here with the same calls HandlePostCityInit makes: Init, the current zoom and rotation, then into the renderer.
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
            automaton->PlaceInRenderer(true);
            automaton->AddRef();
            g_automata.push_back(automaton);
            ++started;
        }
    }
    std::fprintf(stderr, "automata: %d started, %d skipped\n", started, skipped);
}

void TickAutomata() {
    if (g_automata.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    float ms = std::chrono::duration<float, std::milli>(now - g_lastTick).count();
    g_lastTick = now;
    if (ms > 100.0f) ms = 100.0f;
    for (sc4::cSC4Automaton* automaton : g_automata) automaton->DoAnimationTick(0, ms);
}

}  // namespace native
