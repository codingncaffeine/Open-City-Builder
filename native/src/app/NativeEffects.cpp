// The visual-effects system in a native city: the effects manager's host (the packed effects library from the game data,
// the animation ticks, its draw pass in the renderer), the particle systems' terrain, model and effect lookups, and the
// terrain's CreateTerrainEffect (the god-mode animal brushes place their herds through it).
//
// The original's manager Init reaches the renderer (vslot 0x100: AddViewObject(manager, list 5, priority 0x76c)), the
// animation tick manager (AddTickableObject(manager, 0xc0)) and the message server; here the city view attaches it once
// its renderer exists (AttachEffects) and detaches it when the renderer shuts down (DetachEffects).
#include "app/NativeEffects.h"

#include <cstdio>
#include <cstdlib>
#include <typeinfo>
#include <algorithm>
#include <cmath>

#include "sc4/DrawContext.h"
#include "sc4/PackedRenderState.h"
#include "sc4/Render3D.h"
#include "app/NativeAnimationTicks.h"
#include "app/NativeCity.h"
#include "app/NativeUi.h"
#include "gz/cGZPersistResourceManager.h"
#include "gz/cIGZPersistResourceKeyList.h"
#include "s3d/ModelInstance.h"
#include "sc4/AnimationTickManager.h"
#include "sc4/CameraControl.h"
#include "sc4/EffectsManager.h"
#include "sc4/EffectsResource.h"
#include "sc4/ModelBasedParticlesEffect.h"
#include "sc4/ServiceLocator.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

namespace native {
namespace {

constexpr std::uint32_t kGZIID_cIS3DResModel = 0x9ad0e7e3;
constexpr std::uint32_t kModelType = 0x5ad0e817, kModelGroup = 0xbadb57f1;

sc4::cSC4EffectsManager* g_manager = nullptr;
sc4::cSC4City* g_city = nullptr;
sc4::cSC43DRender* g_render = nullptr;   // attached, waiting for its effects manager (created after the view in a loaded city)
bool g_debug = false;
bool BindManager();

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
class ParticlesDraw final : public IParticlesDrawContext {
public:
    SC4DrawContext* context = nullptr;
    void SetVertexBuffer(std::uint32_t, std::uint32_t, const void*) override {}
    void DrawPrims(std::uint32_t, int, int) override {}
    void DrawPrimsIndexed(std::uint32_t, int, const std::uint16_t*) override {}
    void SetTexture(int, int) override {}
    void SetRenderState(const cSC4PackedRenderState&) override {}
    void SetSelfLitShade(const cS3DVector3&) override {}
    void ResetShade() override {}
    void ResetModelViewTransform() override {}
    int Rotation() const override { return 0; }
    void GetBillboardBasis(cS3DVector3& a, cS3DVector3& b) const override { a = cS3DVector3{1, 0, 0}; b = cS3DVector3{0, 1, 0}; }
    SC4DrawContext* DrawContext() override { return context; }
};
ParticlesDraw g_draw;

class EffectsPass final : public cISC4ViewObject3D {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override { return --refs_; }
    void DrawViewObject(SC4DrawContext& context) override {
        if (!g_manager && !BindManager()) return;
        g_draw.context = &context;
        if (g_debug) {
            static int frame = 0;
            if (++frame % 10 == 0) {
                int inUse = 0, started = 0, particles = 0, withModel = 0;
                g_manager->ModelEffectCensus(inUse, started, particles, withModel);
                std::fprintf(stderr, "effects: frame %d, %zu visual effects active, model systems %d (started %d, with model %d), %d particles, dt %.3f\n",
                                        frame, g_manager->ActiveVisualEffectSlots().size(), inUse, started, withModel, particles, g_manager->ElapsedSeconds());
            }
        }
        g_manager->Update(&context);
        g_draw.context = nullptr;
    }
private:
    std::uint32_t refs_ = 1;
};

// ---- IEffectsManagerHost ----
class ManagerHost final : public sc4::IEffectsManagerHost {
public:
    void SendProgressMessage(int, const char*) override {}
    std::uint32_t CurrentThreadId() override { return 0; }
    float TerrainCellSize() override { return 16.0f; }
    int TerrainCellsX() override { sc4::cSTETerrain* t = CityTerrain(); return t ? static_cast<int>(t->CellCountX()) : 64; }
    int TerrainCellsZ() override { sc4::cSTETerrain* t = CityTerrain(); return t ? static_cast<int>(t->CellCountZ()) : 64; }
    cIGZUnknown* Renderer() override { return nullptr; }   // AttachEffects adds the pass once the renderer exists
    void AttachRenderable(cIGZUnknown*) override {}
    void DetachRenderable(cIGZUnknown*) override {}
    IParticlesVertexBuffer* VertexBufferExtension() override { return nullptr; }
    cIGZUnknown* WeatherSim() override { return nullptr; }
    sc4::cSC4EffectMaps* CreateEffectMaps() override { return nullptr; }
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
    void AddCheatHandler(cIGZUnknown*) override {}
    void RemoveCheatHandler(cIGZUnknown*) override {}
    void RegisterCheatCode(std::uint32_t, const char*) override {}
    void UnregisterCheatCode(std::uint32_t) override {}
    bool CheatsEnabled() override { return false; }
    void AddTickNotification(cIGZUnknown*) override {
        if (sc4::cSC4AnimationTickManager* atm = AnimationTickManager()) atm->AddTickableObject(&g_target, 0xc0);
    }
    void RemoveTickNotification(cIGZUnknown*) override {
        if (sc4::cSC4AnimationTickManager* atm = AnimationTickManager()) atm->RemoveTickableObject(&g_target, 0xc0);
    }
    void RegisterGameDataFunctions() override {}
    void UnregisterGameDataFunctions() override {}
    void PostMessage(std::uint32_t) override {}
    bool EffectsResourceParseFiles() override { return false; }
    const char* EffectsResourceErrorString() override { return ""; }
    void SetView3DErrorReportString(const char*) override {}
    int ViewZoom() override { return 5; }
    float WorldUnitsPerViewportUnit() override { return 1.0f; }
    float RenderHeightScale() override { return 1.0f; }
    std::uint32_t AddShake(const cSC4ShakeDescription&, const cS3DVector2*) override { return 0; }
    void RemoveShake(std::uint32_t) override {}
    void SetLightingTint(float, const cS3DVector3&) override {}
    void ClearLightingTint() override {}
    std::int32_t AddSnowlineEffect() override { return -1; }
    void RemoveSnowlineEffect(std::int32_t) override {}
    void SetSnowlineAltitude(std::int32_t, float, float) override {}
    int SimulatorDayOfYear() override { return 0; }
    int CityHemisphere() override { return 0; }
    bool WeatherEnabled() override { return false; }
    bool ViewportPositionToRay(int, int, float*, float*) override { return false; }
    float SimulatorSpeed() override { return 1.0f; }
    void WeatherWind(float& x, float& z) override { x = 0.0f; z = 0.0f; }
    IParticlesDrawContext* ParticlesDrawContext(SC4DrawContext* context) override { return context ? &g_draw : nullptr; }
};
ManagerHost g_host;

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

}  // namespace

namespace {
// The city's effects manager, once it exists: hosts installed, Init (the packed library, the messages, the tick).
bool BindManager() {
    if (g_manager) return true;
    if (!g_city) return false;
    cIGZUnknown* component = g_city->GetComponent(sc4::CityComponent::EffectsManager);
    g_manager = dynamic_cast<sc4::cSC4EffectsManager*>(component);
    if (!g_manager) return false;
    SetParticlesTerrain(&g_terrain);
    SetParticlesCity(&g_cityCells);
    sc4::SetModelParticlesHost(&g_models);
    g_manager->SetHost(&g_host);
    g_manager->Init();                        // loads the packed library, registers the messages and the tick
    g_manager->SetEffectsActive(true);       // the 0xab96b05f "effects active" message of the city start-up
    // The view's zoom messages set the level of detail; the one the view sent before the manager existed is replayed.
    if (sc4::cSC4CameraControl* camera = g_render ? g_render->GetCameraControl() : nullptr) g_manager->DoZoomChanged(camera->Zoom());
    if (g_debug)
        std::fprintf(stderr, "effects: manager up, %zu effects, %zu bindings\n", g_manager->Collection().Effects().size(),
                     g_manager->EffectBindings().size());
    return true;
}
}  // namespace

void AttachEffects(sc4::cSC43DRender& render, sc4::cSC4City& city) {
    g_debug = std::getenv("SC4_DEBUG") != nullptr;
    g_city = &city;
    g_render = &render;
    g_manager = nullptr;
    render.AddViewObject(new EffectsPass(), 5, 0x76c);
    BindManager();
}

void DetachEffects() {
    if (g_manager) {
        if (sc4::cSC4AnimationTickManager* atm = AnimationTickManager()) atm->RemoveTickableObject(&g_target, 0xffffffffu);
        g_manager->Shutdown();
    }
    g_manager = nullptr;
    g_city = nullptr;
    g_render = nullptr;
    sc4::SetModelParticlesHost(nullptr);
}

sc4::ITerrainEffect* CreateTerrainEffect(std::uint32_t group, std::uint32_t type) {
    if (!BindManager()) return nullptr;
    sc4::cSC4VisualEffect* effect = nullptr;
    const bool ok = g_manager->CreateVisualEffect(group, type, &effect) && effect;
    if (g_debug) {
        static int shown = 0;
        if (shown++ < 8) std::fprintf(stderr, "effects: terrain effect %08x/%08x -> %s (slot %d)\n", group, type, ok ? "made" : "none", ok ? effect->Slot() : -1);
    }
    if (!ok) return nullptr;
    if (effect->Slot() < 0) {   // a binding without a usable description: nothing would draw
        effect->Release();
        return nullptr;
    }
    return new TerrainEffect(effect);
}

}  // namespace native
