// cSC4EffectsManager tests: COM plumbing (the Release-stays-1 quirk, QueryInterface's interface ids, FinalRelease),
// the effect registry (name -> description version selection, the (group,type) binding table, the redundant lookup),
// the visual-effect pool with its new / active / free slot lists, the recycle and retarget rules, the description label
// started by StopAncilliary, the shake / screen-flash / lighting-tint state, the snowline curve, the effects tick, the
// message dispatch subset, the Init / Shutdown host protocol, the particle pool, the effect-file parse step
// (ParseEffectsFiles) and the game-data registry's create_effect command (sCreateEffect), plus the exact sequence the
// region screen uses for its region_global effect (create by name, SetEffectTransform, Start(0)).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "rz/cRZMessage2Standard.h"
#include "gz/CommandParameterSet.h"
#include "sc4/EffectsManager.h"
#include "sc4/RenderProperties.h"
#include "sc4/DrawContext.h"
#include "FakeRender3D.h"
#include "fake_gdriver.h"
#include "sc4/GameDataRegistry.h"

using namespace sc4;

namespace {

int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

class FakeUnknown final : public cIGZUnknown {
public:
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1) return false;
        *out = this;
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (refs > 0) --refs;
        if (refs == 0) deleted = true;
        return static_cast<std::uint32_t>(refs);
    }
    int refs = 0;
    bool deleted = false;
};

class FakeVertexBuffer final : public IParticlesVertexBuffer {
public:
    void AddRef() override { ++refs; }
    void Release() override {
        if (refs > 0) --refs;
        if (refs == 0) deleted = true;
    }
    std::uint32_t VertexCapacity() override { return 64; }
    void* Lock(std::uint32_t, std::uint32_t) override { return nullptr; }
    void Unlock(std::uint32_t) override {}
    void DrawIndexed(std::uint32_t, std::uint32_t, std::uint32_t, const std::uint16_t*, const void*, int) override {}
    int refs = 0;
    bool deleted = false;
};

class FakeParticlesTerrain final : public IParticlesTerrain {
public:
    void AddRef() override { ++refs; }
    void Release() override {
        if (refs > 0) --refs;
        if (refs == 0) deleted = true;
    }
    float CellSize() const override { return 16.0f; }
    int CellCountX() const override { return 8; }
    int CellCountZ() const override { return 6; }
    float TerrainHeight(float, float) override { return 0.0f; }
    float WaterHeight(float, float) override { return 0.0f; }
    void TerrainNormal(float, float, cS3DVector3& out) override { out = cS3DVector3{}; }
    float WaterLevel() const override { return 0.0f; }
    int refs = 0;
    bool deleted = false;
};

class FakeHost final : public IEffectsManagerHost {
public:
    void SendProgressMessage(int stage, const char* text) override {
        progress.push_back(stage);
        (void)text;
    }
    std::uint32_t CurrentThreadId() override { return threadId; }
    float TerrainCellSize() override { return 16.0f; }
    int TerrainCellsX() override { return 8; }
    int TerrainCellsZ() override { return 6; }
    float altitude = 0.0f;
    float TerrainAltitude(float, float) override { return altitude; }
    cIGZUnknown* Renderer() override {
        if (!makeRenderer) return nullptr;
        auto* renderer = new FakeUnknown();
        renderer->AddRef();
        renderers.push_back(renderer);
        return renderer;
    }
    void AttachRenderable(cIGZUnknown*) override { ++attachCalls; }
    void DetachRenderable(cIGZUnknown*) override { ++detachCalls; }
    IParticlesVertexBuffer* VertexBufferExtension() override {
        if (!makeVertexBuffer) return nullptr;
        vertexBuffer = new FakeVertexBuffer();
        vertexBuffer->AddRef();
        return vertexBuffer;
    }
    cIGZUnknown* WeatherSim() override {
        if (!makeWeather) return nullptr;
        auto* weather = new FakeUnknown();
        weather->AddRef();
        weatherSim = weather;
        return weather;
    }
    cSC4EffectMaps* CreateEffectMaps() override {
        effectMaps = new cSC4EffectMaps();
        return effectMaps;
    }
    cIGZUnknown* CreateEffectsResource() override {
        resource = new FakeUnknown();
        resource->AddRef();
        return resource;
    }
    void ShutdownEffectsResource(cIGZUnknown*) override { ++resourceShutdowns; }
    bool EffectsResourceChanged() override { return resourceChanged; }
    void EffectsResourceReloaded() override { ++resourceReloads; }
    cIGZUnknown* CreateEffectsParser() override {
        parser = new FakeUnknown();
        parser->AddRef();
        return parser;
    }
    void LoadEffectsFiles() override { ++loadEffects; }
    void RegisterMessageHandler(std::uint32_t message) override { registeredMessages.push_back(message); }
    void UnregisterMessageHandler(std::uint32_t message) override { unregisteredMessages.push_back(message); }
    void AddCheatHandler(cIGZUnknown*) override { ++cheatHandlers; }
    void RemoveCheatHandler(cIGZUnknown*) override { --cheatHandlers; }
    void RegisterCheatCode(std::uint32_t id, const char* text) override {
        cheats.emplace_back(id, text ? text : "");
    }
    void UnregisterCheatCode(std::uint32_t id) override { unregisteredCheats.push_back(id); }
    bool CheatsEnabled() override { return cheatsEnabled; }
    void AddTickNotification(cIGZUnknown*) override { ++tickNotifications; }
    void RemoveTickNotification(cIGZUnknown*) override { --tickNotifications; }
    void RegisterGameDataFunctions() override { ++gameDataRegisters; }
    void UnregisterGameDataFunctions() override { ++gameDataUnregisters; }
    void PostMessage(std::uint32_t type) override { postedMessages.push_back(type); }
    bool EffectsResourceParseFiles() override { return resourceParseOk; }
    const char* EffectsResourceErrorString() override { return resourceError.c_str(); }
    void SetView3DErrorReportString(const char* text) override {
        ++viewErrorSets;
        viewErrorWasNull = text == nullptr;
        viewError = text ? text : "";
    }
    int ViewZoom() override { return viewZoom; }
    float WorldUnitsPerViewportUnit() override { return unitsPerViewport; }
    float RenderHeightScale() override { return renderHeightScale; }
    std::uint32_t AddShake(const cSC4ShakeDescription&, const cS3DVector2* position) override {
        ++shakeAdds;
        shakeHadPosition = position != nullptr;
        if (position) lastShakePosition = *position;
        return shakeHandle;
    }
    void RemoveShake(std::uint32_t handle) override {
        ++shakeRemoves;
        lastRemovedShake = handle;
    }
    void SetLightingTint(float intensity, const cS3DVector3& colour) override {
        ++tintSets;
        lastTintIntensity = intensity;
        lastTintColour = colour;
    }
    void ClearLightingTint() override { ++tintClears; }
    std::int32_t AddSnowlineEffect() override {
        if (!hasTerrainView) return -1;
        ++snowlineAdds;
        return snowlineHandle;
    }
    void RemoveSnowlineEffect(std::int32_t handle) override {
        ++snowlineRemoves;
        lastSnowlineHandle = handle;
    }
    void SetSnowlineAltitude(std::int32_t handle, float altitude, float bias) override {
        ++snowlineSets;
        lastSnowlineHandle = handle;
        lastSnowlineAltitude = altitude;
        lastSnowlineBias = bias;
    }
    int SimulatorDayOfYear() override { return dayOfYear; }
    int CityHemisphere() override { return hemisphere; }
    bool WeatherEnabled() override { return true; }
    void WeatherWind(float& x, float& z) override {
        x = windX;
        z = windZ;
    }
    bool ViewportPositionToRay(int x, int y, float* origin, float* direction) override {
        ++rayCalls;
        lastRayX = x;
        lastRayY = y;
        if (!rayValid) return false;
        for (int i = 0; i < 3; ++i) {
            origin[i] = static_cast<float>(i + 1);
            direction[i] = static_cast<float>(i + 4);
        }
        return true;
    }
    float SimulatorSpeed() override { return simulatorSpeed; }

    std::uint32_t threadId = 1234;
    bool makeRenderer = false;
    bool makeVertexBuffer = false;
    bool makeWeather = false;
    bool cheatsEnabled = false;
    bool hasTerrainView = true;
    bool resourceChanged = false;
    bool rayValid = true;
    int viewZoom = 3;
    float unitsPerViewport = 2.0f;
    cSC4RenderProperties* renderProperties = nullptr;
    bool simulator = false, renderToScreen = true;
    int redraws = 0;
    std::uint32_t freePool = 0, textureLod = 0, textureBits = 0;
    cSC4RenderProperties* RenderProperties() override { return renderProperties; }
    bool SimulatorPresent() override { return simulator; }
    void SetEnableRenderToScreen(bool enable) override { renderToScreen = enable; }
    void SetTextureFactorySettings(std::uint32_t pool, std::uint32_t lod, std::uint32_t bits) override {
        freePool = pool;
        textureLod = lod;
        textureBits = bits;
    }
    void ForceFullRedraw() override { ++redraws; }
    float renderHeightScale = 5.0f;
    std::uint32_t shakeHandle = 42;
    std::int32_t snowlineHandle = 77;
    int dayOfYear = 1;
    int hemisphere = 1;
    float windX = 1.5f;
    float windZ = -2.5f;
    float simulatorSpeed = 1.0f;
    int lastRayX = 0, lastRayY = 0;

    int attachCalls = 0, detachCalls = 0, loadEffects = 0, resourceShutdowns = 0, resourceReloads = 0;
    int cheatHandlers = 0, tickNotifications = 0, gameDataRegisters = 0, gameDataUnregisters = 0;
    int viewErrorSets = 0;
    bool viewErrorWasNull = false;
    bool resourceParseOk = true;
    std::string resourceError = "main.fx: unknown command";
    std::string viewError;
    std::vector<std::uint32_t> postedMessages;
    int shakeAdds = 0, shakeRemoves = 0, tintSets = 0, tintClears = 0, snowlineAdds = 0, snowlineRemoves = 0;
    int snowlineSets = 0, rayCalls = 0;
    bool shakeHadPosition = false;
    cS3DVector2 lastShakePosition{};
    std::uint32_t lastRemovedShake = 0;
    float lastTintIntensity = 0;
    cS3DVector3 lastTintColour{};
    std::int32_t lastSnowlineHandle = 0;
    float lastSnowlineAltitude = 0;
    float lastSnowlineBias = 0;
    std::vector<int> progress;
    std::vector<std::uint32_t> registeredMessages, unregisteredMessages, unregisteredCheats;
    std::vector<std::pair<std::uint32_t, std::string>> cheats;
    ~FakeHost() {
        for (FakeUnknown* renderer : renderers) delete renderer;
        delete weatherSim;
        delete resource;
        delete parser;
        delete vertexBuffer;
        // effectMaps is a real cSC4EffectMaps: the manager owns and deletes it.
    }
    IParticlesDrawContext* ParticlesDrawContext(SC4DrawContext* context) override {
        (void)context;
        return drawContext;
    }
    bool IsBoundingBoxInView(const cS3DBoundingBox& box) override { return box.max[0] < hiddenFromX; }
    IParticlesDrawContext* drawContext = nullptr;
    float hiddenFromX = 1.0e9f;   // a particle system whose bounds reach this x is out of view
    std::vector<FakeUnknown*> renderers;
    FakeUnknown* weatherSim = nullptr;
    FakeVertexBuffer* vertexBuffer = nullptr;
    cSC4EffectMaps* effectMaps = nullptr;
    FakeUnknown* resource = nullptr;
    FakeUnknown* parser = nullptr;
};

// The game-data registry as sCreateEffect uses it: one value entry whose handle is the manager and whose context is
// the manager back again (the original registers the manager with AddValue<cSC4EffectsManager> and digs it back out
// with GetEntryID(handle) / GetEntryContext(entry)). Everything else is inert.
class FakeGameDataRegistry final : public cISC4GameDataRegistry {
public:
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1) return false;
        *out = this;
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (refs > 1) return --refs;
        refs = 0;
        return 0;
    }
    EntryId AddChildEntry(cIGZUnknown*, const char*, EntryId, int, void*) override { return 0; }
    EntryId AddChildEntry(void*, const char*, GameDataValueFn, EntryId, int, void*) override { return 0; }
    EntryId AddChildEntry(void* handle, const char*, GameDataCommandFn, EntryId, int, void*) override {
        valueHandle = handle;
        return kValueEntry;
    }
    bool RemoveChildEntry(cIGZUnknown*) override { return false; }
    bool RemoveChildEntry(void*) override { return false; }
    EntryId GetEntryID(cIGZUnknown*) const override { return 0; }
    EntryId GetEntryID(void* handle) const override { return handle != nullptr && handle == valueHandle ? kValueEntry : 0; }
    cIGZUnknown* GetNode(EntryId) const override { return nullptr; }
    void* GetValue(EntryId entry) const override { return entry == kValueEntry ? valueHandle : nullptr; }
    bool IsValidEntry(EntryId entry) const override { return entry == kValueEntry; }
    bool IsNodeEntry(EntryId) const override { return false; }
    const cRZString* GetEntryName(EntryId) const override { return nullptr; }
    bool DoValueExchange(EntryId, cIGZVariant&, bool) override { return false; }
    bool DoValueCall(EntryId, cIGZCommandParameterSet*, cIGZCommandParameterSet*) override { return false; }
    bool SetEntryContext(EntryId entry, void* context) override {
        if (entry != kValueEntry) return false;
        entryContext = context;
        return true;
    }
    void* GetEntryContext(EntryId entry) const override { return entry == kValueEntry ? entryContext : nullptr; }
    bool CheckEntryFlags(EntryId, int) const override { return false; }
    EntryId GetParentNodeEntry(EntryId) const override { return 0; }
    EntryId GetRootNodeEntry() const override { return 0; }
    bool VisitBreadthFirst(EntryId, EntryVisitFn, void*) override { return false; }
    bool VisitImmediateChildren(EntryId, EntryVisitFn, void*) override { return false; }
    bool AddNotificationTarget(cISC4GameDataRegistryNotificationTarget*) override { return false; }
    bool AddNotificationTarget(cIGZMessageTarget2*) override { return false; }
    bool RemoveNotificationTarget(cISC4GameDataRegistryNotificationTarget*) override { return false; }
    bool RemoveNotificationTarget(cIGZMessageTarget2*) override { return false; }
    void DebugDumpRecursive(EntryId) override {}

    static constexpr EntryId kValueEntry = 0x1000;
    std::uint32_t refs = 0;
    void* valueHandle = nullptr;
    void* entryContext = nullptr;
};

// One description record, for the LOD-scale test.
cSC4EffectDescription::cDescriptionRec MakeRecord() {
    cSC4EffectDescription::cDescriptionRec record;
    record.flagA = 1;
    record.flagB = 5;
    record.floats[0] = 10.0f;
    record.floats[1] = 20.0f;
    record.floats[2] = 30.0f;
    record.floats[3] = 40.0f;
    return record;
}

// A component that only records the float parameters the manager pushes.
class RecordingComponent final : public cISC4VisualEffectComponent {
public:
    void Start(int) override {}
    void Stop(int) override {}
    bool IsStarted() const override { return started; }
    void SetTransforms(const cS3DTransform&, const cS3DTransform&) override {}
    void ApplyEffect(float) override {}
    bool SetFloatParams(int type, const float* values, int count) override {
        lastType = type;
        lastCount = count;
        for (int i = 0; i < count && i < 2; ++i) lastValues[i] = values[i];
        return true;
    }
    bool SetVoidParams(int, void**, int) override { return false; }
    bool started = false;
    int lastType = 0, lastCount = 0;
    float lastValues[2] = {0, 0};
};

void TestCom() {
    FakeHost host;
    SetEffectsManagerHost(&host);
    cSC4EffectsManager manager;
    manager.SetHost(&host);
    CHECK(!manager.Initialised());
    CHECK(manager.RefCount() == 0);
    CHECK(manager.GetEffectMaps() == nullptr);
    CHECK(manager.GetTallestBuildingAltitude() == 0.0f);

    void* out = nullptr;
    CHECK(manager.QueryInterface(1, &out));
    CHECK(out == &manager);
    CHECK(manager.RefCount() == 1);
    CHECK(manager.QueryInterface(kGZIID_cISC4EffectsManager, &out));
    CHECK(out == &manager);
    CHECK(manager.QueryInterface(kGZIID_cISC4EffectsManagerAlt, &out));
    CHECK(manager.QueryInterface(kGZIID_cISC4EffectsCollection, &out));
    CHECK(out == &manager.Collection());
    CHECK(manager.RefCount() == 4);
    out = nullptr;
    CHECK(!manager.QueryInterface(kGZIID_cISC4EffectMaps, &out));   // no maps before Init
    CHECK(!manager.QueryInterface(0xdeadbeefu, &out));
    CHECK(!manager.QueryInterface(1, nullptr));

    // The Release quirk: success is reported, the counter does not move.
    CHECK(manager.Release() == 1);
    CHECK(manager.RefCount() == 4);
    CHECK(manager.AddRef() == 5);
    CHECK(manager.Release() == 1);
    CHECK(manager.RefCount() == 5);

    // FinalRelease on a heap object: Shutdown + delete (ASan watches the delete).
    auto* heap = new cSC4EffectsManager();
    heap->SetHost(&host);
    CHECK(heap->Init());
    heap->FinalRelease();
    CHECK(host.detachCalls == 0);   // Init did not attach a renderer (the host returns none)
}

void TestRegistry() {
    FakeHost host;
    SetEffectsManagerHost(&host);
    cSC4EffectsManager manager;
    manager.SetHost(&host);

    manager.BeginCollection();
    cSC4EffectDescription desc;
    desc.version = 3;
    manager.Collection().AddEffectDescription("Smoke", desc);
    desc.version = 6;                                    // above the manager's maximum (5)
    manager.Collection().AddEffectDescription("Smoke", desc);
    desc.version = 2;
    manager.Collection().AddEffectDescription("smoke", desc);
    // The binding table maps (group,type) to the effect index; EndCollection resolves it by the effect name.
    cSC4EffectDescription spark;
    spark.version = 3;
    manager.Collection().AddEffectDescription("spark", spark);
    manager.Collection().AddEffectBinding("spark", 0xaa6dec65u, 0x11u);
    cSC4MessageTriggerDescription trigger;
    trigger.message = 0x12345678u;
    trigger.name = "Smoke";
    manager.Collection().AddMessageTrigger(trigger);
    manager.EndCollection();

    CHECK(manager.Collection().EffectNameCount("smoke") == 3);   // Smoke x3 (spark is a different name)
    CHECK(manager.HasVisualEffect("Smoke"));
    CHECK(manager.HasVisualEffect("SMOKE"));
    CHECK(!manager.HasVisualEffect("nope"));
    CHECK(!manager.HasVisualEffect(""));
    CHECK(!manager.HasVisualEffect(nullptr));

    // EndCollection registered the trigger and built the binding table.
    CHECK(host.registeredMessages.size() == 1 && host.registeredMessages[0] == 0x12345678u);
    CHECK(manager.EffectBindings().size() == 1);

    // Name lookup picks the highest version <= 5 (3, not 6 or 2).
    cSC4VisualEffect* effect = nullptr;
    CHECK(manager.CreateVisualEffect("Smoke", &effect));
    CHECK(effect != nullptr);
    CHECK(effect->Slot() >= 0);
    CHECK(manager.Collection().Effects()[static_cast<std::size_t>(effect->Slot())].version == 3);
    CHECK(effect->Name() == "smoke");
    CHECK(effect->RefCount() == 2);                       // pool + the returned reference
    CHECK(manager.VisualEffects().size() == 1);
    CHECK(manager.ActiveVisualEffectSlots().size() == 1);
    CHECK(manager.ActiveVisualEffectSlots()[0] == effect->Slot());
    effect->Release();

    // Unknown / empty / null names and a null out pointer fail.
    effect = nullptr;
    CHECK(!manager.CreateVisualEffect("nope", &effect));
    CHECK(effect == nullptr);
    CHECK(!manager.CreateVisualEffect("", &effect));
    CHECK(!manager.CreateVisualEffect(nullptr, &effect));
    CHECK(!manager.CreateVisualEffect("Smoke", nullptr));

    // (group,type) lookup: known key works, unknown key fails, the effect remembers its key.
    cSC4VisualEffect* byType = nullptr;
    CHECK(manager.CreateVisualEffect(0xaa6dec65u, 0x11u, &byType));
    CHECK(byType->Group() == 0xaa6dec65u && byType->Type() == 0x11u);
    CHECK(byType->Slot() >= 0);
    CHECK(manager.Collection().Effects()[static_cast<std::size_t>(byType->Slot())].version == 3);
    byType->Release();
    byType = nullptr;
    CHECK(!manager.CreateVisualEffect(0xaa6dec65u, 0x99u, &byType));
    CHECK(!manager.CreateVisualEffect(0xaa6dec65u, 0x11u, nullptr));
    CHECK(manager.EffectBindings().size() == 1);

    // A name that resolves above the maximum version creates a live effect with no description.
    manager.BeginCollection();
    cSC4EffectDescription future;
    future.version = 7;
    manager.Collection().AddEffectDescription("future", future);
    manager.EndCollection();
    cSC4VisualEffect* futureEffect = nullptr;
    CHECK(manager.CreateVisualEffect("future", &futureEffect));
    CHECK(futureEffect->Slot() == -1);
    CHECK(futureEffect->RefCount() == 2);
    CHECK(manager.NewVisualEffectSlots().size() == 1);    // never activated: no description
    futureEffect->Release();

    // The manager's maximum version changes the selection (nothing qualifies now).
    manager.SetMaxEffectVersion(1);
    cSC4VisualEffect* limited = nullptr;
    CHECK(manager.CreateVisualEffect("Smoke", &limited));
    CHECK(limited->Slot() == -1);
    limited->Release();
    manager.SetMaxEffectVersion(5);

    // Out-of-range description versions via the binding path still create an effect with slot -1.
    manager.BeginCollection();
    CHECK(manager.EffectBindings().empty());              // BeginCollection cleared the registry
    cSC4EffectDescription tooNew;
    tooNew.version = 9;
    manager.Collection().AddEffectDescription("toonew", tooNew);
    manager.Collection().AddEffectBinding("toonew", 5u, 6u);
    manager.EndCollection();
    cSC4VisualEffect* invalid = nullptr;
    CHECK(manager.CreateVisualEffect(5u, 6u, &invalid));
    CHECK(invalid->Slot() == -1);
    invalid->Release();

    // The description's label starts a new effect when the ancilliary stops (StopAncilliary); no label -> nothing.
    CHECK(manager.Collection().EffectNameCount("smoke") == 3);   // Smoke x3 (spark is a different name)
}

void TestPoolAndRecycle() {
    FakeHost host;
    SetEffectsManagerHost(&host);
    cSC4EffectsManager manager;
    manager.SetHost(&host);

    manager.BeginCollection();
    cSC4EffectDescription smoke;
    smoke.version = 1;
    manager.Collection().AddEffectDescription("smoke", smoke);
    cSC4EffectDescription hold;                            // flag bit 1: never auto-finishes
    hold.version = 1;
    hold.id = 2;
    manager.Collection().AddEffectDescription("hold", hold);
    manager.EndCollection();

    cSC4VisualEffect* effect = nullptr;
    CHECK(manager.CreateVisualEffect("smoke", &effect));
    const int slot = effect->Slot();
    CHECK(manager.ActiveVisualEffectSlots().size() == 1);

    // A not-started effect counts as stopped; while we hold a reference it is kept but not counted (quirk).
    manager.DeactivateUnusedVisualEffects();
    CHECK(manager.ActiveVisualEffectCount() == 0);
    CHECK(manager.ActiveVisualEffectSlots().size() == 1);
    effect->Release();

    // Dropping the last outside reference lets the next frame recycle it into the free list.
    manager.DeactivateUnusedVisualEffects();
    CHECK(manager.ActiveVisualEffectSlots().empty());
    CHECK(manager.FreeVisualEffectSlots().size() == 1);
    CHECK(manager.FreeVisualEffectSlots()[0] == slot);
    const cSC4VisualEffect* recycled = manager.VisualEffects()[static_cast<std::size_t>(slot)].get();
    CHECK(recycled->Name().empty());
    CHECK(recycled->Slot() == -1);
    CHECK(recycled->RefCount() == 1);
    CHECK(recycled->Components().empty());

    // The next instance reuses the slot instead of growing the pool.
    cSC4VisualEffect* reused = nullptr;
    CHECK(manager.CreateVisualEffect("smoke", &reused));
    CHECK(reused == recycled);
    CHECK(manager.VisualEffects().size() == 1);
    CHECK(manager.NewVisualEffectSlots().empty());
    CHECK(reused->Name() == "smoke");
    reused->Release();

    // A running effect with the never-auto-finish flag stays active and is counted.
    cSC4VisualEffect* held = nullptr;
    CHECK(manager.CreateVisualEffect("hold", &held));
    held->Start(0);
    CHECK(held->CheckEffectFlag(1));
    manager.DeactivateUnusedVisualEffects();
    CHECK(manager.ActiveVisualEffectCount() == 1);
    // The unstarted "smoke" instance was recycled during the same pass, so only "hold" is left.
    CHECK(manager.ActiveVisualEffectSlots().size() == 1);
    held->Release();

    // KillOrphanedEffects stops everything only the manager holds and leaves the killing guard set, so a newly
    // created effect is not activated until the next DeactivateUnusedVisualEffects.
    manager.KillOrphanedEffects();
    CHECK(!manager.KillGuardCleared());
    cSC4VisualEffect* blocked = nullptr;
    CHECK(manager.CreateVisualEffect("hold", &blocked));
    CHECK(manager.ActiveVisualEffectSlots().size() == 1);   // the killing guard blocks the activation
    blocked->Release();
    manager.DeactivateUnusedVisualEffects();
    CHECK(manager.KillGuardCleared());
    CHECK(manager.ActiveVisualEffectSlots().size() >= 1);
}

void TestAncilliaryAndLights() {
    FakeHost host;
    SetEffectsManagerHost(&host);
    cSC4EffectsManager manager;
    manager.SetHost(&host);

    cSC4ShakeDescription shake;
    cSC4LightDescription light;
    light.colors.push_back(cS3DVector3{1, 0, 0});
    light.colors.push_back(cS3DVector3{0, 1, 0});
    light.intensities.push_back(0.0f);
    light.intensities.push_back(1.0f);
    light.range = 4.0f;

    manager.BeginCollection();
    cSC4EffectDescription flare;
    flare.version = 1;
    flare.label = "ending";
    cSC4EffectDescription::cEventRec shakeEvent;
    shakeEvent.type = 1 | 2;      // shake + position
    shakeEvent.flags = 0;
    flare.events.push_back(shakeEvent);
    cSC4EffectDescription::cEventRec flashEvent;
    flashEvent.type = 4;          // screen flash
    flashEvent.flags = 0;
    flare.events.push_back(flashEvent);
    cSC4EffectDescription::cEventRec tintEvent;
    tintEvent.type = 8;           // lighting tint
    tintEvent.flags = 0;
    flare.events.push_back(tintEvent);
    manager.Collection().AddEffectDescription("flare", flare);
    cSC4EffectDescription ending;
    ending.version = 1;
    manager.Collection().AddEffectDescription("ending", ending);
    manager.Collection().AddShakeDescription("shake", shake);
    manager.Collection().AddLightDescription("light", light);
    manager.EndCollection();

    cSC4VisualEffect* effect = nullptr;
    CHECK(manager.CreateVisualEffect("flare", &effect));
    effect->SetEffectTransform(cS3DTransform{});   // identity
    cS3DTransform source;
    source.translation[0] = 1.0f;
    source.translation[1] = 2.0f;
    source.translation[2] = 3.0f;
    effect->SetSourceTransform(source);

    manager.StartAncilliary(effect);
    CHECK(host.shakeAdds == 1);
    CHECK(host.shakeHadPosition);
    CHECK(host.lastShakePosition.x == 1.0f);    // row 0 of the effect rotation . source translation
    CHECK(host.lastShakePosition.y == 3.0f);    // column 2 of the effect rotation . source translation
    CHECK(effect->AncilliaryHandle() == host.shakeHandle);
    CHECK(manager.ScreenFlashColours().size() == 2);
    CHECK(manager.ScreenFlashIntensities().size() == 2);
    CHECK(manager.ScreenFlashInverseDuration() == 0.25f);
    CHECK(manager.ScreenFlashTime() == 0.0f);
    CHECK(!manager.ScreenFlashAtPoint());       // the flash event carries no position bit
    CHECK(manager.LightingTintColours().size() == 2);
    CHECK(manager.LightingTintTime() == 0.0f);

    // StopAncilliary removes the shake and starts the description's label ("ending") at the same transforms.
    manager.StopAncilliary(effect);
    CHECK(host.shakeRemoves == 1);
    CHECK(host.lastRemovedShake == host.shakeHandle);
    CHECK(effect->AncilliaryHandle() == 0);
    const cSC4VisualEffect* endingEffect = manager.VisualEffects().back().get();
    CHECK(endingEffect->Name() == "ending");
    CHECK(endingEffect->IsStarted());
    CHECK(manager.ActiveVisualEffectSlots().size() == 2);

    // The elapsed-time bookkeeping of the flash and the tint.
    manager.DrawScreenFlashes(nullptr, 2.0f);   // 2 * 0.25 = 0.5
    CHECK(manager.ScreenFlashTime() == 0.5f);
    manager.DrawScreenFlashes(nullptr, 2.0f);   // 1.0
    CHECK(manager.ScreenFlashTime() == 1.0f);
    manager.DrawScreenFlashes(nullptr, 2.0f);   // 1.5 -> finished
    CHECK(manager.ScreenFlashInverseDuration() == 0.0f);

    manager.UpdateLightingTint(1.0f);           // t = 0.25 -> interpolation between the two keys
    CHECK(host.tintSets == 1);
    CHECK(host.lastTintColour.y > 0.0f);
    CHECK(std::fabs(host.lastTintIntensity - 0.25f) < 1e-5f);
    manager.UpdateLightingTint(4.0f);           // t = 1.25 -> clears the tint
    CHECK(host.tintClears == 1);

    // UpdateLODScale interpolates the two LOD curves at the current level (integer division of the fraction).
    RecordingComponent component;
    component.AddRef();
    cSC4VisualEffect::cComponentRec record;
    record.component = &component;
    const cSC4EffectDescription::cDescriptionRec descriptionRecord = MakeRecord();
    manager.SetLodLevel(2);
    manager.UpdateLODScale(descriptionRecord, &record);
    CHECK(component.lastType == 0x102);
    CHECK(component.lastCount == 2);
    // The fraction is an integer division quirk: (2 - 1) / (5 - 1 - 1) = 0 -> the first keys.
    CHECK(component.lastValues[0] == 10.0f);
    CHECK(component.lastValues[1] == 30.0f);
    manager.SetLodLevel(4);
    manager.UpdateLODScale(descriptionRecord, &record);
    // (4 - 1) / (5 - 1 - 1) = 1 -> the second keys.
    CHECK(component.lastValues[0] == 20.0f);
    CHECK(component.lastValues[1] == 40.0f);

    effect->Release();
}

void TestSnowline() {
    FakeHost host;
    SetEffectsManagerHost(&host);
    cSC4EffectsManager manager;
    manager.SetHost(&host);

    CHECK(manager.SnowlineEffectId() == -1);
    CHECK(manager.SnowlineLow() == 300.0f);
    CHECK(manager.SnowlineHigh() == 600.0f);
    CHECK(manager.SnowlineBias() == 200.0f);
    CHECK(manager.SnowlineAltitude() == 512.0f);

    host.dayOfYear = 1;                    // cos(0) = 1 -> the low seasonal altitude (300)
    manager.SetSnowlineState(true);
    CHECK(host.snowlineAdds == 1);
    CHECK(manager.SnowlineEffectId() == host.snowlineHandle);
    CHECK(host.snowlineSets == 1);
    CHECK(host.lastSnowlineHandle == host.snowlineHandle);
    CHECK(host.lastSnowlineAltitude == 300.0f);
    CHECK(host.lastSnowlineBias == 200.0f);
    CHECK(manager.SnowlineAltitude() == 300.0f);

    manager.UpdateSnowline();              // same target: no update
    CHECK(host.snowlineSets == 1);

    host.dayOfYear = 183;                  // half a year: the high altitude (600)
    manager.UpdateSnowline();
    CHECK(host.snowlineSets == 2);
    CHECK(host.lastSnowlineAltitude == 600.0f);

    // The southern hemisphere is mirrored by half a year: day 183 is 600 in the north and 300 here.
    host.hemisphere = 2;
    host.dayOfYear = 183;
    manager.UpdateSnowline();
    CHECK(host.snowlineSets == 3);
    CHECK(host.lastSnowlineAltitude == 300.0f);

    manager.SetSnowlineState(false);
    CHECK(host.snowlineRemoves == 1);
    CHECK(host.lastSnowlineHandle == host.snowlineHandle);
    CHECK(manager.SnowlineEffectId() == -1);

    // A host without a terrain view cannot allocate a snowline effect.
    host.hasTerrainView = false;
    manager.SetSnowlineState(true);
    CHECK(manager.SnowlineEffectId() == -1);
    CHECK(host.snowlineSets == 3);

    // The "Snow" cheat (the first argument is the command name): 1-3 arguments set the curve and enable.
    host.hasTerrainView = true;
    cRZCmdLine cmdLine("Snow 100 200 50");
    CHECK(manager.DoSnowCheat(cmdLine));
    CHECK(manager.SnowlineLow() == 100.0f);
    CHECK(manager.SnowlineHigh() == 200.0f);
    CHECK(manager.SnowlineBias() == 50.0f);
    CHECK(manager.SnowlineEffectId() == host.snowlineHandle);
    manager.SetSnowlineState(false);

    // With no arguments the cheat toggles: nothing active -> enable.
    cRZCmdLine toggle("Snow");
    host.dayOfYear = 1;
    CHECK(manager.DoSnowCheat(toggle));
    CHECK(manager.SnowlineEffectId() == host.snowlineHandle);
    CHECK(manager.DoSnowCheat(toggle));    // now active -> disable
    CHECK(manager.SnowlineEffectId() == -1);
}

void TestInitShutdownAndTick() {
    FakeHost host;
    host.makeRenderer = true;
    host.makeVertexBuffer = true;
    host.makeWeather = true;
    SetEffectsManagerHost(&host);
    cSC4EffectsManager manager;
    manager.SetHost(&host);

    CHECK(manager.Init());
    CHECK(manager.Initialised());
    CHECK(manager.ThreadId() == host.threadId);
    CHECK(manager.GetEffectMaps() != nullptr);
    CHECK(manager.GetEffectMaps() == host.effectMaps);
    CHECK(host.attachCalls == 1);
    CHECK(host.loadEffects == 1);
    CHECK(host.registeredMessages.size() == 11);
    CHECK(host.registeredMessages[0] == 0xa6b79620u);
    CHECK(host.cheats.size() == 1);        // cheats disabled: only the "Effect" code
    CHECK(host.cheats[0].first == kCheatEffectCode);
    CHECK(host.cheats[0].second == "Effect");
    CHECK(host.cheatHandlers == 1);
    CHECK(host.tickNotifications == 1);
    CHECK(host.gameDataRegisters == 1);
    CHECK(host.progress.size() == 4);      // 1, 2, 2, 1
    CHECK(host.progress[0] == 1 && host.progress[1] == 2 && host.progress[2] == 2 && host.progress[3] == 1);
    CHECK(manager.VertexBufferExtension() == host.vertexBuffer);
    CHECK(host.vertexBuffer->refs == 1);
    CHECK(manager.Renderer() == host.renderers[0]);
    CHECK(host.renderers[0]->refs == 1);
    // CreateFlashMaterials packed both states (the lit state sets the blend bit, the dark state differs).
    CHECK(manager.FlashStateLit().value != 0);
    CHECK(manager.FlashStateLit().value != manager.FlashStateDark().value);

    // The second Init is a no-op.
    CHECK(manager.Init());
    CHECK(host.loadEffects == 1);
    CHECK(host.attachCalls == 1);

    // QueryInterface(effect maps) now forwards to the maps object.
    void* mapsOut = nullptr;
    CHECK(manager.QueryInterface(kGZIID_cISC4EffectMaps, &mapsOut));
    CHECK(mapsOut == manager.GetEffectMaps());

    // DoEffectsTick: a changed resource re-opens the collection and refreshes the active effects; the weather wind
    // lands in the global parameters.
    host.resourceChanged = true;
    manager.DoEffectsTick();
    CHECK(host.loadEffects == 2);
    CHECK(host.resourceReloads == 1);
    CHECK(!manager.ForceDescriptionUpdate());
    host.resourceChanged = false;
    host.windX = 3.0f;
    host.windZ = -4.0f;
    manager.DoEffectsTick();
    CHECK(manager.GlobalParams().velocity.x == 3.0f);
    CHECK(manager.GlobalParams().velocity.y == 0.0f);
    CHECK(manager.GlobalParams().velocity.z == -4.0f);
    CHECK(host.loadEffects == 2);

    // The zoom / rotation notification carries two int pairs through void1 / void2.
    cRZMessage2Standard zoomMessage;
    zoomMessage.SetType(0xa6b79620u);
    int oldView[2] = {1, 0};
    int newView[2] = {3, 1};
    zoomMessage.SetVoid1(oldView);
    zoomMessage.SetVoid2(newView);
    CHECK(manager.DoMessage(&zoomMessage));
    CHECK(manager.Zoom() == 3);
    CHECK(manager.LodLevel() == 4);        // zoom + 1 - lodOffset (1)

    // 0xab96b05f marks the manager active, 0x66956814 refreshes the snowline, 0x697d01ed runs the effects tick.
    cRZMessage2Standard activeMessage;
    activeMessage.SetType(0xab96b05fu);
    CHECK(manager.DoMessage(&activeMessage));
    CHECK(manager.EffectsActive());
    cRZMessage2Standard snowMessage;
    snowMessage.SetType(0x66956814u);
    CHECK(manager.DoMessage(&snowMessage));
    cRZMessage2Standard tickMessage;
    tickMessage.SetType(0x697d01edu);
    CHECK(manager.DoMessage(&tickMessage));
    CHECK(host.loadEffects == 2);

    // The cursor-ray broadcast asks the view utilities for the ray and feeds it to every started effect.
    manager.BeginCollection();
    cSC4EffectDescription rayEffect;
    rayEffect.version = 1;
    manager.Collection().AddEffectDescription("ray", rayEffect);
    manager.EndCollection();
    cSC4VisualEffect* ray = nullptr;
    CHECK(manager.CreateVisualEffect("ray", &ray));
    ray->Start(0);
    cRZMessage2Standard rayMessage;
    rayMessage.SetType(0x2acd2cf3u);
    rayMessage.SetData1(11);
    rayMessage.SetData2(22);
    CHECK(manager.DoMessage(&rayMessage));
    CHECK(host.rayCalls == 1);
    CHECK(host.lastRayX == 11 && host.lastRayY == 22);
    ray->Release();

    // GetStatsString prints the counters; the particle-system columns are the family lists (quirk).
    cRZString stats;
    manager.GetStatsString(stats);
    CHECK(std::strstr(stats.ToChar(), "Active effects: N/") != nullptr);
    CHECK(std::strstr(stats.ToChar(), "density:") != nullptr);

    // Shutdown releases every service and reference and leaves the manager reusable.
    CHECK(manager.Shutdown());
    CHECK(!manager.Initialised());
    CHECK(manager.GetEffectMaps() == nullptr);
    CHECK(host.detachCalls == 1);
    CHECK(host.unregisteredMessages.size() == 11);
    CHECK(host.cheatHandlers == 0);
    CHECK(host.tickNotifications == 0);
    CHECK(host.gameDataUnregisters == 1);
    CHECK(host.resourceShutdowns == 1);
    CHECK(host.renderers[0]->deleted);
    CHECK(host.weatherSim->deleted);
    CHECK(host.vertexBuffer->deleted);
    CHECK(host.resource->deleted);
    CHECK(host.parser->deleted);
    CHECK(manager.VisualEffects().empty());
    CHECK(manager.ActiveVisualEffectSlots().empty());
    CHECK(manager.EffectBindings().empty());
    CHECK(!manager.HasVisualEffect("ray"));    // the effect descriptions were cleared
    CHECK(manager.Shutdown());                  // idempotent
    CHECK(host.resourceShutdowns == 1);
}

void TestParticlePool() {
    FakeHost host;
    SetEffectsManagerHost(&host);
    FakeParticlesTerrain terrain;
    SetParticlesTerrain(&terrain);          // cSC4ParticlesEffect::Init dereferences the terrain host
    cSC4EffectsManager manager;
    manager.SetHost(&host);
    CHECK(manager.Init());

    // DeactivateUnusedVisualEffects only sweeps the particle families when it recycled a visual effect in the same
    // pass (the original gates the whole block on that "changed" flag), so give it one finished visual effect.
    manager.BeginCollection();
    cSC4EffectDescription smoke;
    smoke.version = 1;
    manager.Collection().AddEffectDescription("smoke", smoke);
    manager.EndCollection();
    cSC4VisualEffect* visual = nullptr;
    CHECK(manager.CreateVisualEffect("smoke", &visual));
    visual->Release();                     // not started, only the pool holds it

    cSC4ParticlesEffect* effect = manager.NewParticlesEffect();
    CHECK(effect != nullptr);
    CHECK(manager.ParticleEffects().size() == 1);
    CHECK(manager.NewParticlesEffectSlots().size() == 1);
    CHECK(effect->RefCount() == 1);             // the pool only
    CHECK(effect->Description() == nullptr);

    // The finished particle system (refcount 1, not started) is retired by the visual-effect deactivation.
    manager.DeactivateUnusedVisualEffects();
    CHECK(manager.FreeVisualEffectSlots().size() == 1);
    CHECK(manager.NewParticlesEffectSlots().empty());
    CHECK(manager.FreeParticlesEffectSlots().size() == 1);
    const cSC4ParticlesEffect* pooled = manager.ParticleEffects()[0].get();
    cSC4ParticlesEffect* reused = manager.NewParticlesEffect();
    CHECK(reused == pooled);
    CHECK(manager.ParticleEffects().size() == 1);
    CHECK(manager.NewParticlesEffectSlots().size() == 1);

    // Shutdown shuts the particle systems down and clears the pool (the host has no Init state, so call it directly).
    manager.Shutdown();
    CHECK(manager.ParticleEffects().empty());
    SetParticlesTerrain(nullptr);
    CHECK(terrain.refs == 0);               // Shutdown released the reference Init took
}

// The particle view of the draw context Update draws through: it keeps the textures in the order the systems drew.
class RecordingDrawContext final : public IParticlesDrawContext {
public:
    void SetVertexBuffer(std::uint32_t, std::uint32_t, const void*) override {}
    void DrawPrims(std::uint32_t, int, int) override {}
    void DrawPrimsIndexed(std::uint32_t, int, const std::uint16_t*) override {}
    void SetTexture(int texture, int) override { textures.push_back(texture); }
    void SetRenderState(const cSC4PackedRenderState&) override {}
    void SetSelfLitShade(const cS3DVector3&) override {}
    void ResetShade() override {}
    void ResetModelViewTransform() override {}
    int Rotation() const override { return rotation; }
    void GetBillboardBasis(cS3DVector3& a, cS3DVector3& b) const override {
        a = cS3DVector3{0.0f, 0.0f, 1.0f};
        b = cS3DVector3{1.0f, 0.0f, 0.0f};
    }
    int rotation = 0;
    std::vector<int> textures;
};

class OrderTextures final : public IParticlesTextures {
public:
    class Binding final : public IParticlesTextureBinding {
    public:
        void Release() override {}
        std::uint32_t TextureId() const override { return id; }
        std::uint32_t id = 0;
    };
    IParticlesTextureBinding* Create(std::uint32_t textureId) override {
        bindings.emplace_back(new Binding());
        bindings.back()->id = textureId;
        return bindings.back();
    }
    ~OrderTextures() override {
        for (Binding* binding : bindings) delete binding;
    }
    std::vector<Binding*> bindings;
};

cSC4ParticlesDescription DrawnDescription(std::uint32_t texture, float x, float z, float bias) {
    cSC4ParticlesDescription description;
    description.lifetimeMin = description.lifetimeMax = 10.0f;
    description.spawnPhase = 1.0f;
    description.maxParticles = 4;
    description.delayMinA = description.delayMinB = -1.0f;
    description.rateCurve.assign(1, 1.0f);
    description.sizeCurve.assign(1, 1.0f);
    description.alphaCurve.assign(1, 1.0f);
    description.spinCurve.assign(1, 0.0f);
    description.colorCurve.assign(1, cS3DVector3{1.0f, 1.0f, 1.0f});
    description.textureId = texture;
    description.origin[0] = x;
    description.origin[2] = z;
    description.value130 = bias;   // i386 description +0xd8: added to the depth key
    return description;
}

// Update (i386 0x00407524): the running systems in view draw in ascending order of their depth key (the view rotation
// picks the axis and its sign, the description adds its bias), the ones out of view only advance, a description with
// flag 0x10 draws even out of view, a system not started is left alone.
void TestUpdateDrawOrder() {
    FakeHost host;
    SetEffectsManagerHost(&host);
    FakeParticlesTerrain terrain;
    SetParticlesTerrain(&terrain);
    OrderTextures textures;
    SetParticlesTextures(&textures);
    RecordingDrawContext draw;
    host.drawContext = &draw;
    host.hiddenFromX = 500.0f;
    cSC4EffectsManager manager;
    manager.SetHost(&host);
    CHECK(manager.Init());

    std::vector<cSC4ParticlesDescription> descriptions;
    descriptions.reserve(6);
    descriptions.push_back(DrawnDescription(1, 9.0f, 5.0f, 0.0f));     // rotation 0 key  5, rotation 1 key -9
    descriptions.push_back(DrawnDescription(2, 1.0f, 1.0f, 0.0f));     //                 1                 -1
    descriptions.push_back(DrawnDescription(3, 4.0f, 3.0f, -10.0f));   //                -7               -14
    descriptions.push_back(DrawnDescription(4, 1000.0f, 0.0f, 0.0f));  // out of view
    descriptions.push_back(DrawnDescription(5, 1000.0f, 2.0f, 0.0f));  // out of view, flag 0x10: key 2 / -1000
    descriptions.back().flags = 0x10u;
    descriptions.push_back(DrawnDescription(6, 0.0f, 0.0f, 0.0f));     // never started
    std::vector<cSC4ParticlesEffect*> effects;
    for (std::size_t i = 0; i < descriptions.size(); ++i) {
        cSC4ParticlesEffect* effect = manager.NewParticlesEffect();
        CHECK(effect != nullptr);
        if (!effect) return;
        effect->SetDescription(&descriptions[i]);
        if (i != 5) effect->Start(0);
        cSC4EffectsParticle* particle = reinterpret_cast<cSC4EffectsParticle*>(effect->CreateParticle());
        particle->position = cS3DVector3{descriptions[i].origin[0], 0.0f, descriptions[i].origin[2]};
        effect->AddParticle(reinterpret_cast<cSCParticle*>(particle));
        effects.push_back(effect);
    }
    // A system's bounds come from its own Update: one frame without a draw context sets them.
    host.drawContext = nullptr;
    CHECK(manager.Update(nullptr));
    CHECK(draw.textures.empty());
    CHECK(effects[3]->BoundingBox().max[0] >= 500.0f);   // the out-of-view systems really are out of view
    CHECK(effects[0]->BoundingBox().max[0] < 500.0f);
    host.drawContext = &draw;

    CHECK(manager.Update(nullptr));
    CHECK((draw.textures == std::vector<int>{3, 2, 5, 1}));
    CHECK(manager.ParticleSystemCount() == 4);

    draw.textures.clear();
    draw.rotation = 1;              // key = -x + bias
    CHECK(manager.Update(nullptr));
    CHECK((draw.textures == std::vector<int>{5, 3, 1, 2}));

    // The particles the test added belong to their systems; Stop(false) frees them (an unstarted system must start first).
    for (cSC4ParticlesEffect* effect : effects) {
        if (!effect->IsStarted()) effect->Start(0);
        effect->Stop(false, false);
    }

    manager.Shutdown();
    SetParticlesTextures(nullptr);
    SetParticlesTerrain(nullptr);
}

void TestDuplicateBindings() {
    FakeHost host;
    SetEffectsManagerHost(&host);
    cSC4EffectsManager manager;
    manager.SetHost(&host);

    manager.BeginCollection();
    cSC4EffectDescription a;
    a.version = 1;
    manager.Collection().AddEffectDescription("a", a);
    cSC4EffectDescription b;
    b.version = 2;
    manager.Collection().AddEffectDescription("b", b);
    // Two names bound to the same key: EndCollection keeps the first effect index it meets.
    manager.Collection().AddEffectBinding("a", 9u, 9u);
    manager.Collection().AddEffectBinding("b", 9u, 9u);
    manager.EndCollection();
    CHECK(manager.EffectBindings().size() == 1);
    const int index = manager.EffectBindings().at(cSC4EffectsCollection::cEffectKey{9u, 9u});
    CHECK(index >= 0 && index < 2);
}

// ParseEffectsFiles (0x1005e324a): the parse result clears the 3D view's error report and posts kEffectsParsedMessage,
// or hands the parser's error string to the view; a manager without a parser does nothing.
void TestParseEffectsFiles() {
    FakeHost host;
    SetEffectsManagerHost(&host);
    cSC4EffectsManager manager;
    manager.SetHost(&host);

    manager.ParseEffectsFiles();                  // no parser yet
    CHECK(host.postedMessages.empty());
    CHECK(host.viewErrorSets == 0);

    CHECK(manager.Init());                        // the host creates the parser (+0xeb0)
    manager.ParseEffectsFiles();
    CHECK(host.viewErrorSets == 1);
    CHECK(host.viewErrorWasNull);                 // SetErrorReportString(nullptr)
    CHECK(host.postedMessages.size() == 1);
    CHECK(host.postedMessages[0] == kEffectsParsedMessage);

    // A failed parse reports the parser's error string instead and posts nothing.
    host.resourceParseOk = false;
    host.resourceError = "main.fx: unknown command 'bogus'";
    manager.ParseEffectsFiles();
    CHECK(host.viewErrorSets == 2);
    CHECK(!host.viewErrorWasNull);
    CHECK(host.viewError == host.resourceError);
    CHECK(host.postedMessages.size() == 1);

    // The tick callback posts kEffectsTickMessage through the same seam.
    EffectsTickCallback(nullptr);
    CHECK(host.postedMessages.size() == 2);
    CHECK(host.postedMessages[1] == kEffectsTickMessage);
    CHECK(manager.Shutdown());
}

// sCreateEffect (0x1005dcb24): the registry's "create_effect" command. Parameters 0..3 are the name and the source
// position; the effect is created, given a translation-only source transform and started (Start(0)). The out set's
// parameter 1 carries the success flag and the creation reference is released.
void TestCreateEffectCommand() {
    FakeHost host;
    SetEffectsManagerHost(&host);
    cSC4EffectsManager manager;
    manager.SetHost(&host);
    manager.BeginCollection();
    cSC4EffectDescription spark;
    spark.version = 2;
    manager.Collection().AddEffectDescription("spark", spark);
    manager.EndCollection();

    FakeGameDataRegistry registry;
    registry.AddChildEntry(static_cast<void*>(&manager), "create_effect",
                           static_cast<sc4::GameDataCommandFn>(nullptr), FakeGameDataRegistry::kValueEntry, 1,
                           nullptr);
    CHECK(registry.SetEntryContext(FakeGameDataRegistry::kValueEntry, &manager));

    cGZCommandParameterSet in;
    in.SetParameter(0, cRZVariant(cRZString("spark")));
    in.SetParameter(1, cRZVariant(1.5f));
    in.SetParameter(2, cRZVariant(-2.5f));
    in.SetParameter(3, cRZVariant(3.5f));
    cGZCommandParameterSet out;
    CHECK(cSC4EffectsManager::sCreateEffect(&manager, &in, &out, &registry));

    bool created = false;
    CHECK(out[1].GetValBool(created));
    CHECK(created);
    CHECK(manager.VisualEffects().size() == 1);
    CHECK(manager.ActiveVisualEffectSlots().size() == 1);      // CreateVisualEffect activated it
    const cSC4VisualEffect* effect = manager.VisualEffects()[0].get();
    CHECK(effect->Name() == "spark");
    CHECK(effect->Slot() == 0);                                // the description it resolved
    CHECK(effect->IsStarted());
    CHECK(effect->RefCount() == 1);                            // the command released its reference
    cS3DTransform source;
    effect->GetSourceTransform(source);
    CHECK(source.changeCounter == 1);
    CHECK(source.flags == 1);                                  // translation only
    CHECK(source.translation[0] == 1.5f);
    CHECK(source.translation[1] == -2.5f);
    CHECK(source.translation[2] == 3.5f);
    CHECK(source.scale == 1.0f);

    // An unknown name still answers true (the command ran) but reports false and creates nothing.
    cGZCommandParameterSet missingIn;
    missingIn.SetParameter(0, cRZVariant(cRZString("no-such-effect")));
    cGZCommandParameterSet missingOut;
    CHECK(cSC4EffectsManager::sCreateEffect(&manager, &missingIn, &missingOut, &registry));
    created = true;
    CHECK(missingOut[1].GetValBool(created));
    CHECK(!created);
    CHECK(manager.VisualEffects().size() == 1);

    // A handle the registry does not know (or no registry at all) leaves the manager alone.
    FakeGameDataRegistry other;
    cGZCommandParameterSet otherOut;
    CHECK(cSC4EffectsManager::sCreateEffect(&manager, &in, &otherOut, &other));
    created = true;
    CHECK(otherOut[1].GetValBool(created));
    CHECK(!created);
    CHECK(manager.VisualEffects().size() == 1);
    cGZCommandParameterSet nullOut;
    CHECK(cSC4EffectsManager::sCreateEffect(nullptr, &in, &nullOut, &registry));
    created = true;
    CHECK(nullOut[1].GetValBool(created));
    CHECK(!created);
    CHECK(cSC4EffectsManager::sCreateEffect(nullptr, &in, &nullOut, nullptr));
}

// The region screen's region_global block (0x1002386ec..0x1002387e9), the sequence cSC4WinRegionScreen::Init needs:
// CreateVisualEffect("region_global") through the manager (vslot 0x30), a transform whose translation is
// (cellsX/2, 0, cellsZ/2) * cellSize from SL::spTerrainMap, SetEffectTransform (effect vslot 0x38) and Start(0)
// (vslot 0x18), with the returned reference kept in the screen's +0x280 field.
void TestRegionGlobalEffectPipeline() {
    FakeHost host;
    SetEffectsManagerHost(&host);
    cSC4EffectsManager manager;
    manager.SetHost(&host);
    manager.BeginCollection();
    cSC4EffectDescription global;
    global.version = 1;
    manager.Collection().AddEffectDescription("region_global", global);
    manager.EndCollection();
    CHECK(manager.HasVisualEffect("region_global"));

    cSC4VisualEffect* effect = nullptr;                       // the screen's +0x280
    CHECK(manager.CreateVisualEffect("region_global", &effect));
    CHECK(effect != nullptr);
    CHECK(effect->RefCount() == 2);                           // the pool's reference plus the screen's

    const float cellSize = host.TerrainCellSize();
    cS3DTransform transform;
    transform.changeCounter = 1;
    transform.flags = 1;                                      // kTransformTranslation
    transform.translation[0] = static_cast<float>(host.TerrainCellsX()) * 0.5f * cellSize;
    transform.translation[1] = 0.0f;
    transform.translation[2] = static_cast<float>(host.TerrainCellsZ()) * 0.5f * cellSize;
    effect->SetEffectTransform(transform);                    // effect vslot 0x38
    effect->Start(0);                                          // effect vslot 0x18
    CHECK(effect->IsStarted());
    CHECK(manager.ActiveVisualEffectSlots().size() == 1);

    cS3DTransform stored;
    effect->GetEffectTransform(stored);
    CHECK(stored.translation[0] == 4.0f * 16.0f);             // 8 cells / 2 * 16 units
    CHECK(stored.translation[1] == 0.0f);
    CHECK(stored.translation[2] == 3.0f * 16.0f);             // 6 cells / 2 * 16 units
    CHECK(stored.flags == 1);

    effect->Release();                                        // the screen dropping its +0x280 reference
    CHECK(manager.VisualEffects()[0]->RefCount() == 1);
    // An effect whose description has no components finishes immediately: the next frame recycles the slot.
    manager.DeactivateUnusedVisualEffects();
    CHECK(manager.ActiveVisualEffectCount() == 0);
    CHECK(manager.ActiveVisualEffectSlots().empty());
    CHECK(manager.FreeVisualEffectSlots().size() == 1);
}

}  // namespace

// A message the effects data binds to an effect (a message trigger, as 0x26d31ec4 -> fireworks_establish) starts that effect
// over the city's centre (half the terrain map's extent, set by Init), 4 m above the ground there (i386 0x0040a90a).
void TestMessageTriggerEffect() {
    FakeHost host;
    host.altitude = 250.0f;
    SetEffectsManagerHost(&host);
    cSC4EffectsManager manager;
    manager.SetHost(&host);
    CHECK(manager.Init());
    manager.BeginCollection();
    cSC4EffectDescription burst;
    burst.version = 3;
    manager.Collection().AddEffectDescription("burst", burst);
    cSC4MessageTriggerDescription trigger;
    trigger.message = 0x26d31ec4u;
    trigger.name = "burst";
    manager.Collection().AddMessageTrigger(trigger);
    manager.EndCollection();
    const std::size_t before = manager.VisualEffects().size();
    cRZMessage2Standard other;
    other.SetType(0x26d31ec5u);                          // no trigger: nothing starts
    CHECK(manager.DoMessage(&other));
    CHECK(manager.VisualEffects().size() == before);
    cRZMessage2Standard established;
    established.SetType(0x26d31ec4u);
    CHECK(manager.DoMessage(&established));
    CHECK(manager.VisualEffects().size() == before + 1);
    if (manager.VisualEffects().size() == before + 1) {
        const cSC4VisualEffect* effect = manager.VisualEffects().back().get();
        CHECK(effect->Name() == "burst");
        CHECK(effect->IsStarted());
        const cS3DTransform& source = effect->SourceTransform();
        CHECK((source.flags & 1) != 0);
        CHECK(source.translation[0] == 8 * 16.0f * 0.5f && source.translation[2] == 6 * 16.0f * 0.5f);
        CHECK(source.translation[1] == 254.0f);
    }
    manager.Shutdown();
}

// A plain (not model-based) particle system whose description carries a non-zero +0x278 value still gets its description
// (i386 0x00406246 calls SetDescription on every system it makes): the pollution haze stayed empty without it.
void TestPlainParticlesGetDescription() {
    FakeHost host;
    SetEffectsManagerHost(&host);
    FakeParticlesTerrain terrain;
    SetParticlesTerrain(&terrain);          // cSC4ParticlesEffect::Init dereferences the terrain host
    cSC4EffectsManager manager;
    manager.SetHost(&host);
    CHECK(manager.Init());
    manager.BeginCollection();
    cSC4ParticlesDescription haze;
    haze.flag10 = 0x10;                  // bit 0 clear: not model based
    haze.values278[0] = 0.3f;
    haze.rateCurve.assign(1, 1.0f);
    manager.Collection().AddParticleDescription("haze", haze);
    cSC4EffectDescription smog;
    smog.version = 3;
    cSC4EffectDescription::cDescriptionRec record;
    record.kind = 0;
    record.intValue = 0;
    record.shortA = 1;
    record.flagA = 0;
    record.flagB = 100;
    record.name = "haze";
    smog.records.push_back(record);
    manager.Collection().AddEffectDescription("smog", smog);
    manager.EndCollection();
    manager.SetLodLevel(1);   // as the first zoom message sets it (the level starts at -1)
    cSC4VisualEffect* effect = nullptr;
    CHECK(manager.CreateVisualEffect("smog", &effect) && effect);
    if (effect) {
        CHECK(effect->Components().size() == 1);
        const cSC4ParticlesEffect* system = effect->Components().empty() || !effect->Components()[0].component
                                                ? nullptr : effect->Components()[0].component->ParticleSystem();
        CHECK(system != nullptr);
        CHECK(system && system->Description() != nullptr);
        effect->Release();
    }
    manager.Shutdown();
    SetParticlesTerrain(nullptr);
}

// DrawScreenFlashes (i386 0x00404924): one viewport rectangle per frame through the draw context's renderer, the colour /
// intensity curves at the flash time, the lit state for a positive intensity (alpha up to 1), the dark state for a negative
// one ((colour - white) x intensity, alpha -intensity); a flash at a point fades with the squared distance from the view's
// terrain centre over (scale x world units per pixel)^2, nothing when out of reach, and by 0.6 per zoom level below 4.
void TestScreenFlashDraw() {
    struct View final : IS4ViewUtilities {
        float ViewportUnitsPerWorld() const override { return 0.5f; }
        float WorldUnitsPerViewportUnit() const override { return 2.0f; }
        void GetViewportTerrainCenter(float* out) const override { out[0] = 100.0f; out[1] = 50.0f; out[2] = 200.0f; }
    } view;
    FakeRender3D renderer;
    renderer.width = 800;
    renderer.height = 600;
    sc4test::FakeGDriver driver;
    SC4DrawContext context;
    context.SetDriver(&driver);
    context.Init();
    context.SetRenderer(&renderer);
    context.SetViewUtilities(&view);
    FakeHost host;
    SetEffectsManagerHost(&host);
    cSC4EffectsManager manager;
    manager.SetHost(&host);
    manager.DoZoomChanged(4);

    cSC4LightDescription light;
    light.colors.push_back(cS3DVector3{0.5f, 0.25f, 1.0f});
    light.intensities.push_back(2.0f);   // above 1: alpha is capped
    light.range = 1.0f;
    manager.StartScreenFlash(light, nullptr, 0.0f);
    manager.DrawScreenFlashes(&context, 0.5f);
    CHECK(renderer.rectangles.size() == 1);
    if (!renderer.rectangles.empty()) {
        const auto& r = renderer.rectangles.back();
        CHECK(r.rect.left == 0 && r.rect.top == 0 && r.rect.right == 800 && r.rect.bottom == 600);
        CHECK(r.color[0] == 0.5f && r.color[1] == 0.25f && r.color[2] == 1.0f && r.color[3] == 1.0f);
        CHECK(!r.depthMask && r.colorMask);
    }
    manager.DrawScreenFlashes(&context, 0.6f);   // t = 1.1: over, nothing drawn
    CHECK(renderer.rectangles.size() == 1);
    CHECK(manager.ScreenFlashInverseDuration() == 0.0f);

    light.intensities[0] = -0.5f;                // darkening
    manager.StartScreenFlash(light, nullptr, 0.0f);
    manager.DrawScreenFlashes(&context, 0.1f);
    CHECK(renderer.rectangles.size() == 2);
    if (renderer.rectangles.size() == 2) {
        const auto& r = renderer.rectangles.back();
        CHECK(std::fabs(r.color[0] - 0.25f) < 1e-6f && std::fabs(r.color[1] - 0.375f) < 1e-6f && r.color[2] == 0.0f);
        CHECK(r.color[3] == 0.5f);
    }

    // At a point 30 m east and 40 m south of the centre (d^2 = 2500), scale 50 at 2 m per pixel (R^2 = 10000): 0.75, at zoom 4.
    light.intensities[0] = 0.8f;
    cS3DVector2 point{130.0f, 240.0f};
    manager.StartScreenFlash(light, &point, 50.0f);
    manager.DrawScreenFlashes(&context, 0.1f);
    CHECK(renderer.rectangles.size() == 3);
    if (renderer.rectangles.size() == 3) CHECK(std::fabs(renderer.rectangles.back().color[3] - 0.6f) < 1e-6f);
    manager.DoZoomChanged(2);                    // two levels below 4: x 0.36
    manager.StartScreenFlash(light, &point, 50.0f);
    manager.DrawScreenFlashes(&context, 0.1f);
    CHECK(renderer.rectangles.size() == 4);
    if (renderer.rectangles.size() == 4) CHECK(std::fabs(renderer.rectangles.back().color[3] - 0.216f) < 1e-5f);
    cS3DVector2 far{100.0f, 400.0f};             // 200 m away, beyond 100: nothing
    manager.StartScreenFlash(light, &far, 50.0f);
    manager.DrawScreenFlashes(&context, 0.1f);
    CHECK(renderer.rectangles.size() == 4);
    SetEffectsManagerHost(nullptr);
}

// DoRenderPropertiesChanged (i386 0x004099bc) on the render-properties message: the particle settings, kRenderUI, the
// texture factory settings, the atmospheric and water effects following their switches (with a simulator), a full redraw.
void TestRenderPropertiesChanged() {
    FakeHost host;
    host.unitsPerViewport = 2.0f;
    SetEffectsManagerHost(&host);
    cSC4EffectsManager manager;
    manager.SetHost(&host);
    CHECK(manager.Init());
    manager.BeginCollection();
    cSC4EffectDescription description;
    description.version = 3;
    manager.Collection().AddEffectDescription("atmospheric_effects", description);
    manager.Collection().AddEffectDescription("water_effects", description);
    manager.EndCollection();
    cSC4RenderProperties properties;
    properties.SetFloatValue(3, 2.0f);       // kParticleDensity
    properties.SetFloatValue(5, 0.5f);       // kParticleScale
    properties.SetFloatValue(11, 100.0f);    // kParticleSizeThreshold
    properties.SetFloatValue(2, 3.5f);       // kTexBindMaxFree (MB)
    properties.SetIntValue(2, 16);           // kTextureBits
    properties.SetIntValue(3, 2);            // kLODOffset
    properties.SetIntValue(6, 1);            // kParticleLODOffset
    properties.SetBoolValue(4, true);        // kRenderCloudsAndFog
    properties.SetBoolValue(3, false);       // kRenderWaterEffects
    properties.SetBoolValue(23, false);      // kRenderUI
    host.renderProperties = &properties;
    host.simulator = true;
    cRZMessage2Standard changed;
    changed.SetType(0xc9f775bb);
    CHECK(manager.DoMessage(&changed));
    CHECK(manager.GlobalParams().rateScale == 2.0f && manager.GlobalParams().sizeScale == 0.5f);
    CHECK(manager.WorldUnitsPerViewportUnit() == 200.0f && manager.LodOffset() == 1 && manager.MaxEffectVersion() == 5);
    CHECK(!host.renderToScreen && host.freePool == 3500000u && host.textureLod == 2 && host.textureBits == 16);
    CHECK(manager.AtmosphericEffect() && manager.AtmosphericEffect()->IsStarted() && !manager.WaterEffect());
    CHECK(host.redraws == 1);
    // the switches flipped: the atmospheric effect stops and goes, the water effect starts
    properties.SetBoolValue(4, false);
    properties.SetBoolValue(3, true);
    CHECK(manager.DoMessage(&changed));
    CHECK(!manager.AtmosphericEffect() && manager.WaterEffect() && manager.WaterEffect()->IsStarted());
    // a new priority level is taken; without a simulator the two effects are left alone
    host.simulator = false;
    properties.SetIntValue(7, 3);            // kEffectPriorityLevel
    properties.SetBoolValue(3, false);
    CHECK(manager.DoMessage(&changed));
    CHECK(manager.MaxEffectVersion() == 3 && manager.WaterEffect() != nullptr && host.redraws == 3);
    manager.Shutdown();
    SetEffectsManagerHost(nullptr);
}

int main() {
    TestRenderPropertiesChanged();
    TestCom();
    TestRegistry();
    TestPoolAndRecycle();
    TestAncilliaryAndLights();
    TestScreenFlashDraw();
    TestSnowline();
    TestInitShutdownAndTick();
    TestParticlePool();
    TestUpdateDrawOrder();
    TestDuplicateBindings();
    TestParseEffectsFiles();
    TestCreateEffectCommand();
    TestRegionGlobalEffectPipeline();
    TestMessageTriggerEffect();
    TestPlainParticlesGetDescription();
    SetEffectsManagerHost(nullptr);
    if (failures == 0) {
        std::printf("all effects manager tests passed\n");
        return 0;
    }
    std::printf("%d failure(s)\n", failures);
    return 1;
}
