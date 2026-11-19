// The visual-effects system in a native city: the effects manager's host (the packed effects library from the game data,
// the animation ticks, its draw pass in the renderer), the particle systems' terrain, model and effect lookups, and the
// terrain's CreateTerrainEffect (the god-mode animal brushes place their herds through it).
//
// The manager is a city component: cSC4City::Init initialises it in its own pass (after the terrain and the surface water, before
// the occupant manager), and its Init reaches the renderer through the host (SL::spRenderer, published by the view's
// DoPreCityInit; vslot 0x100: AddViewObject(manager, list 5, priority 0x76c)), the animation tick manager
// (AddTickableObject(manager, 0xc0)) and the message server. The props reach it as SL::spEffectsManager.
#include "app/NativeCheats.h"
#include "app/NativeEffects.h"
#include "app/NativeEffectComponents.h"
#include "sc4/NetworkOccupant.h"

#include <cstdio>
#include <cstdlib>
#include <typeinfo>
#include <algorithm>
#include <cmath>
#include <memory>
#include <map>
#include <set>
#include <string>
#include <cctype>

#include "sc4/DrawContext.h"
#include "sc4/PackedRenderState.h"
#include "sc4/Render3D.h"
#include "app/NativeAnimationTicks.h"
#include "app/NativeAudio.h"
#include "app/NativeCity.h"
#include "app/NativeModelMaker.h"
#include "app/NativeUi.h"
#include "gz/cGZPersistResourceManager.h"
#include "gz/cIGZPersistResourceKeyList.h"
#include "s3d/ModelInstance.h"
#include "sc4/AnimationTickManager.h"
#include "sc4/CameraControl.h"
#include "sc4/EffectsManager.h"
#include "rz/cRZAutoRefCount.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/WeatherSimulator.h"
#include "sc4/Simulator.h"
#include "sc4/LightingManager.h"
#include "sc4/EffectMaps.h"
#include "sc4/EffectsResource.h"
#include "sc4/ModelBasedParticlesEffect.h"
#include "sc4/VisualEffect.h"
#include "sc4/Occupant.h"
#include "sc4/PropOccupantVisualEffect.h"
#include "sc4/ServiceLocator.h"
#include "sc4/cSC4City.h"
#include "sc4/ViewUtilities.h"
#include "sc4/cSTETerrain.h"

namespace native {
sc4::cSC43DRender* CityRenderer();   // app/NativeCityView.cpp
namespace {

constexpr std::uint32_t kGZIID_cIS3DResModel = 0x9ad0e7e3;
constexpr std::uint32_t kModelType = 0x5ad0e817, kModelGroup = 0xbadb57f1;

sc4::cSC4EffectsManager* g_manager = nullptr;
sc4::cSC43DRender* g_render = nullptr;   // the renderer the effects pass is in (the manager's AttachRenderable)
bool g_debug = false;

sc4::cSTETerrain* CityTerrain() {
    sc4::cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(sc4::CityComponent::Terrain) : nullptr;
    void* serializable = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &serializable)) return nullptr;
    auto* terrain = static_cast<sc4::cSTETerrain*>(static_cast<cIGZSerializable*>(serializable));
    static_cast<cIGZSerializable*>(serializable)->Release();
    return terrain;
}

class TypeFilter final : public cIGZPersistResourceKeyFilter {
public:
    explicit TypeFilter(std::uint32_t type) : type_(type) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool IsKeyIncluded(const cGZPersistResourceKey& key) override { return key.type == type_; }
private:
    std::uint32_t type_;
};

// ---- the manager as a message target (the server and the animation tick manager both deliver through DoMessage) ----
class ManagerTarget final : public cIGZMessageTarget2 {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 2; }
    std::uint32_t Release() override { return 1; }
    bool DoMessage(cIGZMessage2* message) override { return g_manager && g_manager->DoMessage(message); }
};
ManagerTarget g_target;

// ---- the renderer's effects pass (list 5, priority 0x76c): the manager's Update with the frame's draw context ----
// The particle systems draw through the frame's SC4DrawContext (the original hands them the context itself); the billboard basis
// is the view utilities' right (+0x68) and up (+0x74) vectors and the rotation the camera's.
class ParticlesDraw final : public IParticlesDrawContext {
public:
    SC4DrawContext* context = nullptr;
    void SetVertexBuffer(std::uint32_t format, std::uint32_t count, const void* vertices) override {
        if (context) context->SetVertexBuffer(static_cast<int>(format), count, vertices);
    }
    void DrawPrims(std::uint32_t primitive, int first, int count) override { if (context) context->DrawPrims(static_cast<int>(primitive), first, count); }
    void DrawPrimsIndexed(std::uint32_t primitive, int count, const std::uint16_t* indices) override {
        if (context) context->DrawPrimsIndexed(static_cast<int>(primitive), count, indices);
    }
    void SetTexture(int texture, int stage) override { if (context) context->SetTexture(texture, stage); }
    void SetRenderState(const cSC4PackedRenderState& state) override { if (context) context->SetRenderState(state); }
    void SetSelfLitShade(const cS3DVector3& shade) override { if (context) context->SetSelfLitShade(shade); }
    void ResetShade() override { if (context) context->ResetShade(); }
    void ResetModelViewTransform() override { if (context) context->ResetModelViewTransform(); }
    int Rotation() const override {
        sc4::cSC4CameraControl* camera = g_render ? g_render->GetCameraControl() : nullptr;
        return camera ? camera->Rotation() : 0;
    }
    void GetBillboardBasis(cS3DVector3& a, cS3DVector3& b) const override {
        SC4ViewUtilities* view = g_render ? g_render->GetViewUtilities() : nullptr;
        a = view ? view->RightVector() : cS3DVector3{1, 0, 0};
        b = view ? view->UpVector() : cS3DVector3{0, 1, 0};
    }
    SC4DrawContext* DrawContext() override { return context; }
};
ParticlesDraw g_draw;

// The particle systems' texture bindings: the FSH of the effects' texture group (0x1abe787d), kept for the process like the
// model textures.
class ParticleTexture final : public IParticlesTextureBinding {
public:
    explicit ParticleTexture(std::uint32_t id) : id_(id) {}
    void Release() override {}
    std::uint32_t TextureId() const override { return id_; }
private:
    std::uint32_t id_;
};
class ParticleTextures final : public IParticlesTextures {
public:
    IParticlesTextureBinding* Create(std::uint32_t instance) override {
        auto& slot = bindings_[instance];
        if (!slot) {
            const unsigned id = FshTextureId(0x1abe787d, instance);
            if (id == 0) return nullptr;
            slot = std::make_unique<ParticleTexture>(id);
        }
        return slot.get();
    }
private:
    std::map<std::uint32_t, std::unique_ptr<ParticleTexture>> bindings_;
};
ParticleTextures g_textures;

class EffectsPass final : public cISC4ViewObject3D {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override { return --refs_; }
    void DrawViewObject(SC4DrawContext& context) override {
        if (!g_manager) return;
        g_draw.context = &context;
        if (g_debug) {
            static int frame = 0;
            if (++frame % 10 == 0) {
                int inUse = 0, started = 0, particles = 0, withModel = 0;
                g_manager->ModelEffectCensus(inUse, started, particles, withModel);
                std::fprintf(stderr, "effects: frame %d, %zu visual effects active, model systems %d (started %d, with model %d), %d particles, dt %.3f, zoom %d lod %d\n",
                                        frame, g_manager->ActiveVisualEffectSlots().size(), inUse, started, withModel, particles, g_manager->ElapsedSeconds(),
                                        g_manager->Zoom(), g_manager->LodLevel());
            }
        }
        g_manager->Update(&context);
        g_draw.context = nullptr;
    }
private:
    std::uint32_t refs_ = 1;
};
EffectsPass g_pass;

sc4::cSC4WeatherSimulator* CityWeather() {
    sc4::cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(sc4::CityComponent::WeatherSimulator) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    auto* weather = serializable->GetGZCLSID() == sc4::kGZCLSID_cSC4WeatherSimulator ? static_cast<sc4::cSC4WeatherSimulator*>(serializable) : nullptr;
    serializable->Release();
    return weather;
}
sc4::cISC4LightingManager* Lighting() { return dynamic_cast<sc4::cISC4LightingManager*>(SL::spLightingManager); }

// ---- IEffectsManagerHost ----
class ManagerHost final : public sc4::IEffectsManagerHost {
public:
    void SendProgressMessage(int, const char*) override {}
    std::uint32_t CurrentThreadId() override { return 0; }
    float TerrainCellSize() override { return 16.0f; }
    int TerrainCellsX() override { sc4::cSTETerrain* t = CityTerrain(); return t ? static_cast<int>(t->CellCountX()) : 64; }
    int TerrainCellsZ() override { sc4::cSTETerrain* t = CityTerrain(); return t ? static_cast<int>(t->CellCountZ()) : 64; }
    // SL::spRenderer, one reference (the manager keeps it until its Shutdown).
    cIGZUnknown* Renderer() override {
        cIGZUnknown* renderer = SL::spRenderer;
        if (renderer) renderer->AddRef();
        return renderer;
    }
    void AttachRenderable(cIGZUnknown*) override;     // renderer vslot 0x100 (list 5, priority 0x76c)
    void DetachRenderable(cIGZUnknown*) override;     // renderer vslot 0x108
    IParticlesVertexBuffer* VertexBufferExtension() override { return nullptr; }
    // SL::spWeatherSim: the city's weather simulator, one reference (the manager keeps it until its Shutdown).
    cIGZUnknown* WeatherSim() override {
        sc4::cSC4WeatherSimulator* weather = CityWeather();
        if (!weather) return nullptr;
        cIGZUnknown* object = static_cast<sc4::cISC4WeatherSimulator*>(weather);
        object->AddRef();
        return object;
    }
    // new cSC4EffectMaps (the manager Inits it).
    sc4::cSC4EffectMaps* CreateEffectMaps() override {
        auto* maps = new sc4::cSC4EffectMaps();
        maps->AddRef();
        return maps;
    }
    cIGZUnknown* CreateEffectsResource() override { return nullptr; }
    void ShutdownEffectsResource(cIGZUnknown*) override {}
    bool EffectsResourceChanged() override { return false; }
    void EffectsResourceReloaded() override {}
    cIGZUnknown* CreateEffectsParser() override { return nullptr; }
    void LoadEffectsFiles() override {}
    void PackedEffectsResources(std::vector<std::vector<std::uint8_t>>& out) override {
        out.clear();
        std::vector<std::uint8_t> bytes;
        if (LoadResourceBytes(sc4::kEffectsResourceType, sc4::kEffectsResourceGroup, 1, bytes)) out.push_back(std::move(bytes));
        if (SL::spRM) {   // the others (mods) after the base library, like LoadAllEffects
            cIGZPersistResourceKeyList* list = nullptr;
            TypeFilter filter(sc4::kEffectsResourceType);
            SL::spRM->GetAvailableResourceList(&list, &filter);
            if (list) {
                for (std::uint32_t i = 0; i < list->Size(); ++i) {
                    const cGZPersistResourceKey* key = list->GetKey(i);
                    if (!key || key->instance == 1) continue;
                    std::vector<std::uint8_t> extra;
                    if (LoadResourceBytes(key->type, key->group, key->instance, extra)) out.push_back(std::move(extra));
                }
                list->Release();
            }
        }
        if (g_debug) std::fprintf(stderr, "effects: %zu packed librar%s\n", out.size(), out.size() == 1 ? "y" : "ies");
    }
    void RegisterMessageHandler(std::uint32_t message) override { if (SL::spMS2) SL::spMS2->AddNotification(&g_target, message); }
    void UnregisterMessageHandler(std::uint32_t message) override { if (SL::spMS2) SL::spMS2->RemoveNotification(&g_target, message); }
    // SL::spCCM vslots 0x38 / 0x40 (listener, 0): the manager hears every cheat.
    void AddCheatHandler(cIGZUnknown* listener) override { native::AddGameCheatListener(dynamic_cast<cIGZMessageTarget2*>(listener), 0); }
    void RemoveCheatHandler(cIGZUnknown* listener) override { native::RemoveGameCheatListener(dynamic_cast<cIGZMessageTarget2*>(listener), 0); }
    void RegisterCheatCode(std::uint32_t id, const char* text) override { native::RegisterGameCheat(id, text); }
    void UnregisterCheatCode(std::uint32_t id) override { native::UnregisterGameCheat(id); }
    bool CheatsEnabled() override { return native::GameDebugMode(); }
    void AddTickNotification(cIGZUnknown*) override {
        if (sc4::cSC4AnimationTickManager* atm = AnimationTickManager()) atm->AddTickableObject(&g_target, 0xc0);
    }
    void RemoveTickNotification(cIGZUnknown*) override {
        if (sc4::cSC4AnimationTickManager* atm = AnimationTickManager()) atm->RemoveTickableObject(&g_target, 0xc0);
    }
    void RegisterGameDataFunctions() override {}
    void UnregisterGameDataFunctions() override {}
    // SL::spMS2 MessagePost of a cRZMessage2Standard of this type (the effects tick, the parse of the effect files).
    void PostMessage(std::uint32_t type) override {
        if (!SL::spMS2) return;
        cRZAutoRefCount<cRZMessage2Standard> message(new cRZMessage2Standard());
        message->SetType(type);
        SL::spMS2->MessagePost(message.get(), false);
    }
    bool EffectsResourceParseFiles() override { return false; }
    const char* EffectsResourceErrorString() override { return ""; }
    void SetView3DErrorReportString(const char*) override {}
    int ViewZoom() override { return SL::spViewUtilities ? SL::spViewUtilities->Zoom() : 0; }   // SL::spViewUtilities +0x14
    // The particle size cap: SL::spViewUtilities +0x158 (world units per viewport unit at this zoom) times the render property
    // kParticleSizeThreshold (float 11, default 300 viewport units).
    float WorldUnitsPerViewportUnit() override {
        SC4ViewUtilities* view = g_render ? g_render->GetViewUtilities() : nullptr;
        return view ? view->ViewportUnitsToWorldUnits(1.0f) : 1.0f;
    }
    float RenderHeightScale() override { return 300.0f; }
    // The renderer's screen shakes (vslots 0x158 / 0x160): the shake's handle goes on the effect, which stops it with its effect.
    std::uint32_t AddShake(const cSC4ShakeDescription& shake, const cS3DVector2* position) override {
        return g_render ? g_render->StartScreenShake(shake, position) : 0;
    }
    void RemoveShake(std::uint32_t handle) override { if (g_render) g_render->StopScreenShake(handle); }
    // SL::spLightingManager vslots 0x38 / 0x40 (the lightning's flash, the fires' glow).
    void SetLightingTint(float intensity, const cS3DVector3& colour) override {
        if (sc4::cISC4LightingManager* lighting = Lighting()) lighting->SetLightingTint(colour, intensity);
    }
    void ClearLightingTint() override {
        if (sc4::cISC4LightingManager* lighting = Lighting()) lighting->ResetLightingTint();
    }
    std::int32_t AddSnowlineEffect() override { return -1; }
    void RemoveSnowlineEffect(std::int32_t) override {}
    void SetSnowlineAltitude(std::int32_t, float, float) override {}
    int SimulatorDayOfYear() override {   // SL::spSimulator's date, day of the year
        sc4::cSC4Simulator* simulator = CitySimulatorObject();
        if (!simulator) return 0;
        int year = 0, month = 0, day = 0, dayOfYear = 0, weekDay = 0;
        simulator->GetSimDate(&year, &month, &day, &dayOfYear, &weekDay);
        return dayOfYear;
    }
    int CityHemisphere() override { sc4::cSC4City* city = HostCity(); return city ? city->GetWorldHemisphere() : 0; }   // SL::spCity vslot 0xf0
    bool WeatherEnabled() override { return false; }
    bool ViewportPositionToRay(int x, int y, float* origin, float* direction) override {
        return SL::spViewUtilities && SL::spViewUtilities->ViewportPositionToRay(x, y, origin, direction);
    }
    // SL::spSimulator vslot 0x100 GetAnimationTimeDilation: 0 while paused, 1.5 / 2 at the faster speeds (effects.md 6); 1 without a
    // simulator.
    float SimulatorSpeed() override {
        sc4::cSC4Simulator* simulator = CitySimulatorObject();
        return simulator ? simulator->GetAnimationTimeDilation() : 1.0f;
    }
    // The weather simulator's ambient wind (vslot 0xb0): its direction scaled to its speed (weather.md 5.1.2).
    void WeatherWind(float& x, float& z) override {
        x = 0.0f;
        z = 0.0f;
        sc4::cSC4WeatherSimulator* weather = CityWeather();
        if (!weather) return;
        float wind[2] = {0.0f, 0.0f};
        weather->GetAmbientWind(wind);
        x = wind[0];
        z = wind[1];
    }
    // SC4ViewUtilities::IsBoundingBoxInViewFrustum: particle systems outside the view are simulated but not drawn.
    bool IsBoundingBoxInView(const cS3DBoundingBox& box) override {
        if (!SL::spViewUtilities) return true;
        return SL::spViewUtilities->IsBoundingBoxInViewFrustum(box.min, box.max);
    }
    IParticlesDrawContext* ParticlesDrawContext(SC4DrawContext* context) override { return context ? &g_draw : nullptr; }
};
ManagerHost g_host;

void ManagerHost::AttachRenderable(cIGZUnknown*) {
    g_render = CityRenderer();
    if (g_render) g_render->AddViewObject(&g_pass, 5, 0x76c);
}
void ManagerHost::DetachRenderable(cIGZUnknown*) {
    if (g_render) g_render->RemoveViewObject(&g_pass);
    g_render = nullptr;
}

// The effect components' services (sc4/EffectComponents.h): the sound components post their events to the sound system, which
// here plays the sound through the game's sound manager. PARTIAL: the positional half (the move events, the 3D attenuation),
// the stop of a looping sound, and the camera effects (no ICameraControl natively).
class ComponentsHost final : public sc4::IEffectsHost {
public:
    sc4::ICameraControl* Camera() override { return nullptr; }
    bool CameraEffectsEnabled() override { return false; }
    void AddNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->AddNotification(target, message); }
    void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->RemoveNotification(target, message); }
    void PostSoundEvent(std::uint32_t event, std::uint32_t a, std::uint64_t, std::uint64_t, std::uint64_t) override {
        if (g_debug) std::fprintf(stderr, "effects: sound event %08x %08x\n", event, a);
        if (event == sc4::kSoundEventPlay) PlayGameSound(a);
    }
    bool HasSoundSystem() override { return true; }
} g_componentsHost;

// ---- the particle systems' terrain (SL::spTerrainMap) ----
class ParticlesTerrainHost final : public IParticlesTerrain {
public:
    void AddRef() override {}
    void Release() override {}
    float CellSize() const override { return 16.0f; }
    int CellCountX() const override { sc4::cSTETerrain* t = CityTerrain(); return t ? static_cast<int>(t->CellCountX()) : 64; }
    int CellCountZ() const override { sc4::cSTETerrain* t = CityTerrain(); return t ? static_cast<int>(t->CellCountZ()) : 64; }
    float TerrainHeight(float x, float z) override { sc4::cSTETerrain* t = CityTerrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    float WaterHeight(float x, float z) override {
        sc4::cSTETerrain* t = CityTerrain();
        return t ? std::max(t->GetAltitude(x, z), t->GetSeaLevel()) : 0.0f;
    }
    void TerrainNormal(float x, float z, cS3DVector3& out) override {
        sc4::cSTETerrain* t = CityTerrain();
        if (!t) { out = cS3DVector3{0, 1, 0}; return; }
        const float h = t->GetAltitude(x, z);
        const float dx = t->GetAltitude(x + 1.0f, z) - h, dz = t->GetAltitude(x, z + 1.0f) - h;
        cS3DVector3 n{-dx, 1.0f, -dz};
        const float length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        out = cS3DVector3{n.x / length, n.y / length, n.z / length};
    }
    float WaterLevel() const override { sc4::cSTETerrain* t = CityTerrain(); return t ? t->GetSeaLevel() : 0.0f; }
};
ParticlesTerrainHost g_terrain;

// ---- the city's cell map (land / water) for land-only and water-only emission ----
class CellsHost final : public IParticlesCityCells {
public:
    unsigned CellFlags(int x, int z) override {
        sc4::cSTETerrain* t = CityTerrain();
        if (!t || x < 0 || z < 0 || x >= static_cast<int>(t->CellCountX()) || z >= static_cast<int>(t->CellCountZ())) return 0x80;
        const float cx = (static_cast<float>(x) + 0.5f) * 16.0f, cz = (static_cast<float>(z) + 0.5f) * 16.0f;
        return t->GetAltitude(cx, cz) < t->GetSeaLevel() ? 1u : 0u;
    }
};
class CityHost final : public IParticlesCity {
public:
    void WorldToCell(float x, float z, int& cellX, int& cellZ) override {
        cellX = static_cast<int>(std::floor(x / 16.0f));
        cellZ = static_cast<int>(std::floor(z / 16.0f));
    }
    IParticlesCityCells* Cells() override { return &cells_; }
private:
    CellsHost cells_;
};
CityHost g_cityCells;

// ---- model lookups and effect creation for the model-based particle systems ----
class ModelsHost final : public sc4::IModelParticlesHost {
public:
    bool CreateModel(std::uint32_t modelId, s3d::cIS3DResModel** model, s3d::cIS3DModelInstance** instance) override {
        *model = nullptr;
        *instance = nullptr;
        if (!SL::spRM) return false;
        void* raw = nullptr;
        const cGZPersistResourceKey key{kModelType, kModelGroup, modelId};
        if (!SL::spRM->GetResource(key, kGZIID_cIS3DResModel, &raw, 0, nullptr) || !raw) {
            if (g_debug) std::fprintf(stderr, "effects: model %08x not found\n", modelId);
            return false;
        }
        auto* resource = static_cast<s3d::cIS3DResModel*>(raw);
        if (!resource->CreateModelInstance(0, instance) || !*instance) {
            resource->Release();
            return false;
        }
        *model = resource;
        return true;
    }
    bool HasVisualEffect(const char* name) override { return g_manager && g_manager->HasVisualEffect(name); }
    sc4::cSC4VisualEffect* CreateVisualEffect(const char* name) override {
        sc4::cSC4VisualEffect* effect = nullptr;
        if (!g_manager || !g_manager->CreateVisualEffect(name, &effect)) return nullptr;
        return effect;
    }
};
ModelsHost g_models;

// ---- the terrain's effect (BrushFnPlaceEffect: SetPlacement, Start, Release) ----
class TerrainEffect final : public sc4::ITerrainEffect {
public:
    explicit TerrainEffect(sc4::cSC4VisualEffect* effect) : effect_(effect) {}
    void SetPlacement(const sc4::TerrainEffectPlacement& placement) override {
        cS3DTransform source;
        source.flags = 1;   // translation
        source.changeCounter = 1;
        source.translation[0] = placement.x;
        source.translation[1] = placement.y;
        source.translation[2] = placement.z;
        effect_->SetSourceTransform(source);
    }
    void Start() override { effect_->Start(0); }
    void Release() override {
        effect_->Release();
        delete this;
    }
private:
    sc4::cSC4VisualEffect* effect_;
};

// ---- the effects a particle system starts by name (SL::spEffectsManager as cSC4ParticlesEffect uses it) ----
// A particle's timed effects and what it leaves where it hits the ground or the water ("<name>_terrain", "<name>_water":
// a meteor's crater) are ordinary visual effects made by name.
class ParticleTriggeredEffect final : public IParticlesEffectLookup {
public:
    explicit ParticleTriggeredEffect(sc4::cSC4VisualEffect* effect) : effect_(effect) {}   // takes the caller's reference
    void Release() override {
        if (effect_) effect_->Release();
        delete this;
    }
    void Start(int transition) override { effect_->Start(transition); }
    void SetSourceTransform(const cS3DTransform& transform) override { effect_->SetSourceTransform(transform); }
private:
    sc4::cSC4VisualEffect* effect_;
};
class ParticleEffectPresent final : public IParticlesEffectLookup {   // "there is such an effect": nothing to hold
public:
    void Release() override {}
    void Start(int) override {}
    void SetSourceTransform(const cS3DTransform&) override {}
};
class ParticlesEffectsHost final : public IParticlesEffects {
public:
    IParticlesEffectLookup* FindEffect(const char* name) override {   // vslot 0x14 HasVisualEffect
        static ParticleEffectPresent present;
        return g_manager && g_manager->HasVisualEffect(name) ? &present : nullptr;
    }
    IParticlesEffectLookup* FindTriggerEffect(const char* name) override {   // vslot 0x18 CreateVisualEffect(name)
        sc4::cSC4VisualEffect* effect = nullptr;
        if (!g_manager || !g_manager->CreateVisualEffect(name, &effect) || !effect) return nullptr;
        if (std::getenv("SC4_FXDEBUG")) std::fprintf(stderr, "effects: a particle starts %s\n", name);
        return new ParticleTriggeredEffect(effect);
    }
    IParticlesEffectMaps* EffectMaps() override { return nullptr; }
};
ParticlesEffectsHost g_particleEffects;

}  // namespace

namespace {
// SC4_EFFECTDUMP=name,name: what the named effects are made of (their records, the component descriptions behind them and the
// effects a sequence plays), printed once when the manager comes up.
void DumpEffect(const sc4::cSC4EffectsCollection& collection, const std::string& name, int depth, std::set<std::string>& seen) {
    std::string key = name;
    for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string pad(static_cast<std::size_t>(depth) * 2, ' ');
    const auto range = collection.EffectNames().equal_range(key);
    if (range.first == range.second) { std::fprintf(stderr, "fxdump: %s%s: no such effect\n", pad.c_str(), name.c_str()); return; }
    if (!seen.insert(key).second) { std::fprintf(stderr, "fxdump: %s%s: (shown above)\n", pad.c_str(), name.c_str()); return; }
    for (auto it = range.first; it != range.second; ++it) {
        const cSC4EffectDescription& d = collection.Effects()[static_cast<std::size_t>(it->second)];
        std::fprintf(stderr, "fxdump: %s%s [%d]: flags %llx version %u start message %08x %08x %08x, %zu records, %zu events\n", pad.c_str(), name.c_str(), it->second,
                     static_cast<unsigned long long>(d.id), d.version, d.values[0], d.values[1], d.values[2], d.records.size(), d.events.size());
        for (const auto& r : d.records) {
            std::fprintf(stderr, "fxdump: %s  record kind %u type %llx index %d '%s' lod %u..%u count %u spacing %u group %u chance %u at (%.1f, %.1f, %.1f) scale %.2f\n", pad.c_str(),
                         r.kind, static_cast<unsigned long long>(r.type), r.intValue, r.name.c_str(), r.flagA, r.flagB, r.shortA, r.shortB, r.shortC, r.shortD,
                         r.transform.translation[0], r.transform.translation[1], r.transform.translation[2], r.transform.scale);
            if (r.kind == 2) DumpEffect(collection, r.name, depth + 2, seen);
            if (r.kind == 0 && r.intValue >= 0 && static_cast<std::size_t>(r.intValue) < collection.Particles().size()) {
                const cSC4ParticlesDescription& p = collection.Particles()[static_cast<std::size_t>(r.intValue)];
                std::fprintf(stderr, "fxdump: %s    particles flags %08llx flags8 %02llx flag10 %02x/%04x max %u life %.2f..%.2f model ids %zu name '%s' terrain drag %.2f bounce %.2f %.2f %.2f %.2f %.2f %.2f %.2f height %.1f..%.1f\n",
                             pad.c_str(), static_cast<unsigned long long>(p.flags), static_cast<unsigned long long>(p.flags8), p.flag10, p.flag10High, p.maxParticles, p.lifetimeMin,
                             p.lifetimeMax, p.modelIds.size(), p.name.c_str(), p.terrainDrag, p.terrainBounce[0], p.terrainBounce[1], p.terrainBounce[2], p.terrainBounce[3],
                             p.terrainBounce[4], p.terrainBounce[5], p.terrainBounce[6], p.heightMin, p.heightMax);
                for (const auto& timed : p.timedEffects) {
                    std::fprintf(stderr, "fxdump: %s      timed effect '%s' at %.2f\n", pad.c_str(), timed.name.c_str(), timed.time);
                    if (!timed.name.empty()) DumpEffect(collection, timed.name, depth + 4, seen);
                }
                if (p.flag10 & 1) {
                    DumpEffect(collection, p.name + "_slave", depth + 4, seen);
                    DumpEffect(collection, p.name + "_death", depth + 4, seen);
                }
            }
            if (r.kind < 3 || r.kind - 3 >= collection.ComponentDescs().size() || r.intValue < 0) continue;
            const auto& table = collection.ComponentDescs()[r.kind - 3].descriptions;
            if (static_cast<std::size_t>(r.intValue) >= table.size()) continue;
            sc4::cISC4ComponentDescription* c = table[static_cast<std::size_t>(r.intValue)].get();
            if (auto* b = dynamic_cast<sc4::cSC4BrushDescription*>(c))
                std::fprintf(stderr, "fxdump: %s    brush %#x rate %.3f life %.3f oneshot %u zoom %d strength %.3f..%.3f width %.3f..%.3f level %.1f\n", pad.c_str(), b->id, b->f18, b->f1c,
                             b->flag, static_cast<int>(b->u24), b->v28.x, b->v28.y, b->v30.x, b->v30.y, b->f38);
            else if (auto* s = dynamic_cast<sc4::cSC4ScrubberDescription*>(c))
                std::fprintf(stderr, "fxdump: %s    scrubber u18 %#x u20 %#x u24 %#x u28 %u f2c %.3f f30 %.3f u34 %#x u38 %#x u3c %#x u40 %#x u44 %#x u48 %#x f4c %.2f v50 (%.2f, %.2f) f58 %.3f f5c %.3f\n",
                             pad.c_str(), s->u18, s->u20, s->u24, s->u28, s->f2c, s->f30, s->u34, s->u38, s->u3c, s->u40, s->u44, s->u48, s->f4c, s->v50.x, s->v50.y, s->f58, s->f5c);
            else if (auto* a = dynamic_cast<sc4::cSC4AttractorDescription*>(c))
                std::fprintf(stderr, "fxdump: %s    attractor '%s' flag %d\n", pad.c_str(), a->name.c_str(), a->flag);
            else if (auto* snd = dynamic_cast<sc4::cSC4SoundDescription*>(c))
                std::fprintf(stderr, "fxdump: %s    sound %08x flags %#x interval %.2f duration %.2f\n", pad.c_str(), snd->soundId, snd->flags, snd->interval, snd->duration);
            else if (auto* cam = dynamic_cast<sc4::cSC4CameraDescription*>(c))
                std::fprintf(stderr, "fxdump: %s    camera flags %#x zoom %u rotation %u radius %.1f\n", pad.c_str(), cam->flags, cam->zoom, cam->rotation, cam->radius);
            else if (auto* q = dynamic_cast<sc4::cSC4SequenceDescription*>(c)) {
                std::fprintf(stderr, "fxdump: %s    sequence value %#x, %zu items\n", pad.c_str(), q->value, q->items.size());
                for (const auto& item : q->items) {
                    std::fprintf(stderr, "fxdump: %s      item %.2f..%.2f '%s'\n", pad.c_str(), item.range.x, item.range.y, item.name.c_str());
                    if (!item.name.empty()) DumpEffect(collection, item.name, depth + 4, seen);
                }
            }
        }
        for (const auto& e : d.events)
            std::fprintf(stderr, "fxdump: %s  event type %llx '%s' delay %.2f flags %#x\n", pad.c_str(), static_cast<unsigned long long>(e.type), e.name.c_str(), e.delay, e.flags);
    }
}

}  // namespace

// The manager's host and the services its effects reach (the particle systems' terrain, cells, textures and effects, the effect
// components, the models): installed at start-up, before any city creates its manager (the manager keeps the host it was made with).
void InstallEffectsManagerHost() {
    SetParticlesTerrain(&g_terrain);
    SetParticlesCity(&g_cityCells);
    SetParticlesTextures(&g_textures);
    SetParticlesEffects(&g_particleEffects);
    sc4::SetEffectsHost(&g_componentsHost);
    // cSC4VisualEffect::SetModelFromOccupant: the occupant's first model instance and its transform (the demolition effects
    // hand the building's or tree's model to their geometry-sourced particles).
    sc4::GetVisualEffectServices().firstModel = [](sc4::cISC4Occupant* occupant, void*& model, cS3DTransform& transform) {
        sc4::cISC43DPlaceableObject* placeable = occupant ? occupant->GetPlaceableObject() : nullptr;
        if (!placeable || placeable->GetModelCount() < 1) return false;
        sc4::cIS3DModelInstance* models[16] = {};   // the placeable keeps the s3d instances under its own view type (borrowed)
        std::uint32_t count = 16;
        placeable->GetModels(models, count);
        if (count == 0 || !models[0]) return false;
        auto* instance = reinterpret_cast<s3d::cIS3DModelInstance*>(models[0]);
        instance->GetTransform(transform);
        // A network piece is its own model instance in the original: GetModelInstances (i386 0x001335fa) sets the transform's
        // translation from the piece's position first. Without it the piece's transform is the identity and its demolition debris
        // and smoke start at the map origin.
        if (auto* network = dynamic_cast<sc4::cSC4NetworkOccupant*>(occupant)) network->GetModelInstanceTransform(transform);
        model = instance;
        return true;
    };
    // cSC4ParticlesEffect::SetModel: the same model instance answers its local bounds (the emission box of the fire and smoke
    // that rise from a burning building).
    SetParticlesModelBounds([](void* model, cS3DBoundingBox& bounds) {
        static_cast<s3d::cIS3DModelInstance*>(model)->GetBounds(bounds);
        return true;
    });
    sc4::SetModelParticlesHost(&g_models);
    InstallEffectComponentHosts();            // the scrubber, attractor and sequence classes and the brush component's terrain
    sc4::SetEffectsManagerHost(&g_host);
}

// cSC4City::Init / Shutdown, slot 0x18 / 0x20 of the effects manager component (NativeCityComponents.cpp dispatches here). Init loads
// the packed library, attaches the effects pass to the renderer and registers the messages, the cheats and the tick; the city posts
// "effects active" (0xab96b05f) after its Init and the camera's first view-change message sets the level of detail.
bool CityComponentInit_cSC4EffectsManager(cIGZUnknown* object, bool init) {
    auto* manager = dynamic_cast<sc4::cSC4EffectsManager*>(object);
    if (!manager) return false;
    if (!init) {
        manager->Shutdown();
        if (g_manager == manager) g_manager = nullptr;
        return true;
    }
    g_debug = std::getenv("SC4_DEBUG") != nullptr;
    g_manager = manager;
    manager->Init();
    if (g_debug)
        std::fprintf(stderr, "effects: manager up, %zu effects, %zu bindings\n", manager->Collection().Effects().size(),
                     manager->EffectBindings().size());
    if (const char* part = std::getenv("SC4_EFFECTNAMES")) {   // every effect name that holds the given text
        for (const auto& entry : manager->Collection().EffectNames())
            if (entry.first.find(part) != std::string::npos) std::fprintf(stderr, "fxname: %s [%d]\n", entry.first.c_str(), entry.second);
    }
    if (const char* names = std::getenv("SC4_EFFECTDUMP")) {
        std::set<std::string> seen;
        std::string list = names;
        for (std::size_t at = 0; at <= list.size();) {
            const std::size_t comma = std::min(list.find(',', at), list.size());
            if (comma > at) DumpEffect(manager->Collection(), list.substr(at, comma - at), 0, seen);
            at = comma + 1;
        }
    }
    return true;
}

// The renderer shuts down (leaving the city): the effects pass leaves it. The manager itself shuts down in the city's pass.
void DetachEffects() {
    if (g_render) g_render->RemoveViewObject(&g_pass);
    g_render = nullptr;
}

sc4::ITerrainEffect* CreateTerrainEffect(std::uint32_t group, std::uint32_t type) {
    if (!g_manager) {
        if (std::getenv("SC4_FXDEBUG")) std::fprintf(stderr, "effects: terrain effect %08x/%08x: no manager\n", group, type);
        return nullptr;
    }
    sc4::cSC4VisualEffect* effect = nullptr;
    const bool ok = g_manager->CreateVisualEffect(group, type, &effect) && effect;
    if (g_debug) {
        static int shown = 0;
        if (shown++ < 8 || std::getenv("SC4_FXDEBUG")) std::fprintf(stderr, "effects: terrain effect %08x/%08x -> %s (slot %d)\n", group, type, ok ? "made" : "none", ok ? effect->Slot() : -1);
    }
    if (!ok) return nullptr;
    if (effect->Slot() < 0) {   // a binding without a usable description: nothing would draw
        effect->Release();
        return nullptr;
    }
    return new TerrainEffect(effect);
}

bool HasNamedEffect(const char* name) { return g_manager && g_manager->HasVisualEffect(name); }

sc4::cSC4VisualEffect* CreateNamedEffect(const char* name) {
    if (!name || !g_manager) return nullptr;
    sc4::cSC4VisualEffect* effect = nullptr;
    if (!g_manager->CreateVisualEffect(name, &effect) || !effect) return nullptr;
    return effect;
}

sc4::cSC4VisualEffect* CreateKeyedEffect(std::uint32_t group, std::uint32_t type) {
    if (!g_manager) return nullptr;
    sc4::cSC4VisualEffect* effect = nullptr;
    const bool ok = g_manager->CreateVisualEffect(group, type, &effect) && effect;
    if (std::getenv("SC4_FXDEBUG")) std::fprintf(stderr, "effects: keyed effect %08x/%08x -> %s\n", group, type, ok ? "made" : "none");
    return ok ? effect : nullptr;
}

int StartNamedEffect(const char* name, float x, float y, float z) {
    if (!g_manager) return -1;
    sc4::cSC4VisualEffect* effect = nullptr;
    if (!g_manager->CreateVisualEffect(name, &effect) || !effect) return 0;
    cS3DTransform source;   // identity rotation, translation, scale 1
    source.flags = 1;
    source.changeCounter = 1;
    source.translation[0] = x;
    source.translation[1] = y;
    source.translation[2] = z;
    effect->SetSourceTransform(source);
    effect->Start(0);
    effect->Release();
    return 1;
}

}  // namespace native
