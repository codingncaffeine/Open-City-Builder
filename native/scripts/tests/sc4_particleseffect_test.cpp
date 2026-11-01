// cSC4ParticlesEffect — the deterministic particle simulation (Start / Update / emission / physics / ProcessParticle) and
// the draw half through the IParticlesDrawContext seam. The neighbours the binary reaches through engine globals are
// faked here (terrain map, city cells, occupant manager, effects manager, texture factory, vertex-buffer extension).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sc4/ParticlesEffect.h"

static int failures = 0;
using sc4::cSCParticle;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECK_EQ(a, b) do { auto va = (a); auto vb = (b); if (!(va == vb)) { \
    std::printf("FAIL line %d: %s == %s (%lld vs %lld)\n", __LINE__, #a, #b, (long long)va, (long long)vb); ++failures; } } while (0)
#define CHECK_NEAR(a, b, eps) do { double va = (a); double vb = (b); if (!(std::fabs(va - vb) <= (eps))) { \
    std::printf("FAIL line %d: %s ~= %s (%.9g vs %.9g)\n", __LINE__, #a, #b, va, vb); ++failures; } } while (0)

namespace {

// ---- fakes -----------------------------------------------------------------------------------------
struct FakeTerrain final : IParticlesTerrain {
    int addRefs = 0, releases = 0, heightCalls = 0;
    float cellSize = 2.0f;
    int cellsX = 3, cellsZ = 4;
    float height = 0.0f, water = 0.0f, waterLevel = 0.0f;
    void AddRef() override { ++addRefs; }
    void Release() override { ++releases; }
    float CellSize() const override { return cellSize; }
    int CellCountX() const override { return cellsX; }
    int CellCountZ() const override { return cellsZ; }
    float TerrainHeight(float, float) override { ++heightCalls; return height; }
    float WaterHeight(float, float) override { return water; }
    void TerrainNormal(float, float, cS3DVector3& out) override { out = cS3DVector3{0.0f, 1.0f, 0.0f}; }
    float WaterLevel() const override { return waterLevel; }
};

struct FakeCells final : IParticlesCityCells {
    unsigned flags = 0;
    unsigned CellFlags(int, int) override { return flags; }
};
struct FakeCity final : IParticlesCity {
    FakeCells cells;
    void WorldToCell(float x, float z, int& cellX, int& cellZ) override {
        cellX = static_cast<int>(x);
        cellZ = static_cast<int>(z);
    }
    IParticlesCityCells* Cells() override { return &cells; }
};

struct FakeOccupants final : IParticlesOccupants {
    bool blocked = false;
    int boxCalls = 0;
    cS3DVector3 attractorDirection{0.0f, 0.0f, 4.0f};
    bool BuildingInBox(const cS3DVector3&, const cS3DVector3&) override {
        ++boxCalls;
        return blocked;
    }
    bool AttractorGroundPoint(const cS3DVector3&, std::int32_t, cS3DVector3& out, float& length) override {
        out = attractorDirection;
        length = std::sqrt(attractorDirection.x * attractorDirection.x +
                           attractorDirection.y * attractorDirection.y +
                           attractorDirection.z * attractorDirection.z);
        return true;
    }
};

struct FakeLookup final : IParticlesEffectLookup {
    int starts = 0, releases = 0;
    cS3DTransform transform;
    void Release() override { ++releases; }
    void Start(int) override { ++starts; }
    void SetSourceTransform(const cS3DTransform& value) override { transform = value; }
};

struct FakeMaps final : IParticlesEffectMaps {
    int releases = 0;
    float height = 0.0f;
    void Release() override { ++releases; }
    float Height(float, float) override { return height; }
    void Normal(float, float, cS3DVector3& out) override { out = cS3DVector3{0.0f, 1.0f, 0.0f}; }
};

struct FakeEffects final : IParticlesEffects {
    FakeLookup lookup;
    FakeMaps maps;
    bool hasEffect = false;
    bool hasMaps = false;
    std::vector<std::string> lookedUp, triggered;
    IParticlesEffectLookup* FindEffect(const char* name) override {
        lookedUp.emplace_back(name);
        return hasEffect ? &lookup : nullptr;
    }
    IParticlesEffectLookup* FindTriggerEffect(const char* name) override {
        triggered.emplace_back(name);
        return hasEffect ? &lookup : nullptr;
    }
    IParticlesEffectMaps* EffectMaps() override { return hasMaps ? &maps : nullptr; }
};

struct FakeBinding final : IParticlesTextureBinding {
    std::uint32_t id = 0;
    int releases = 0;
    void Release() override { ++releases; }
    std::uint32_t TextureId() const override { return id; }
};
struct FakeTextures final : IParticlesTextures {
    FakeBinding binding;
    int creates = 0;
    IParticlesTextureBinding* Create(std::uint32_t textureId) override {
        ++creates;
        binding.id = 0x100u + textureId;
        return &binding;
    }
};

struct FakeModel final : IParticlesModelInstance {
    int calls = 0;
    cS3DVector3 position;
    void SetPosition(const cS3DVector3& value) override {
        ++calls;
        position = value;
    }
};

struct FakeVertexBuffer final : IParticlesVertexBuffer {
    int addRefs = 0, releases = 0, locks = 0, unlocks = 0, draws = 0;
    std::uint32_t capacity = 512;
    std::vector<float> vertices;
    std::uint32_t lastLockCount = 0, lastIndexCount = 0;
    int lastVertexBytes = 0;
    void AddRef() override { ++addRefs; }
    void Release() override { ++releases; }
    std::uint32_t VertexCapacity() override { return capacity; }
    void* Lock(std::uint32_t, std::uint32_t count) override {
        ++locks;
        lastLockCount = count;
        vertices.assign(static_cast<std::size_t>(count) * 8u, 0.0f);
        return vertices.data();
    }
    void Unlock(std::uint32_t) override { ++unlocks; }
    void DrawIndexed(std::uint32_t, std::uint32_t, std::uint32_t indexCount, const std::uint16_t*, const void*,
                     int vertexBytes) override {
        ++draws;
        lastIndexCount = indexCount;
        lastVertexBytes = vertexBytes;
    }
};

struct FakeDrawContext final : IParticlesDrawContext {
    int setTextureCalls = 0, lastTexture = -1;
    int selfLitCalls = 0, resetShadeCalls = 0, resetModelViewCalls = 0;
    int stateCalls = 0;
    cSC4PackedRenderState state;
    int rotation = 0;
    cS3DVector3 basisA{0.0f, 0.0f, 1.0f}, basisB{1.0f, 0.0f, 0.0f};
    std::uint32_t vertexFormat = 0, vertexCount = 0;
    const void* vertexData = nullptr;
    int vertexBufferCalls = 0, drawCalls = 0, lastDrawCount = 0;
    void SetVertexBuffer(std::uint32_t format, std::uint32_t count, const void* vertices) override {
        ++vertexBufferCalls;
        vertexFormat = format;
        vertexCount = count;
        vertexData = vertices;
    }
    void DrawPrims(std::uint32_t, int, int count) override {
        ++drawCalls;
        lastDrawCount = count;
    }
    void DrawPrimsIndexed(std::uint32_t, int count, const std::uint16_t*) override {
        ++drawCalls;
        lastDrawCount = count;
    }
    void SetTexture(int texture, int) override {
        ++setTextureCalls;
        lastTexture = texture;
    }
    void SetRenderState(const cSC4PackedRenderState& value) override {
        ++stateCalls;
        state = value;
    }
    void SetSelfLitShade(const cS3DVector3&) override { ++selfLitCalls; }
    void ResetShade() override { ++resetShadeCalls; }
    void ResetModelViewTransform() override { ++resetModelViewCalls; }
    int Rotation() const override { return rotation; }
    void GetBillboardBasis(cS3DVector3& a, cS3DVector3& b) const override {
        a = basisA;
        b = basisB;
    }
};

// ---- helpers ---------------------------------------------------------------------------------------
struct Hosts {
    FakeTerrain terrain;
    FakeCity city;
    FakeOccupants occupants;
    FakeEffects effects;
    FakeTextures textures;
    Hosts() {
        SetParticlesTerrain(&terrain);
        SetParticlesCity(&city);
        SetParticlesOccupants(&occupants);
        SetParticlesEffects(&effects);
        SetParticlesTextures(&textures);
    }
    ~Hosts() {
        SetParticlesTerrain(nullptr);
        SetParticlesCity(nullptr);
        SetParticlesOccupants(nullptr);
        SetParticlesEffects(nullptr);
        SetParticlesTextures(nullptr);
    }
};

cSC4ParticlesDescription MakeDescription() {
    cSC4ParticlesDescription description;
    description.lifetimeMin = 10.0f;
    description.lifetimeMax = 10.0f;
    description.spawnPhase = 1.0f;
    description.maxParticles = 100;
    description.delayMinA = -1.0f;
    description.delayMinB = -1.0f;
    description.speedMin = 0.0f;
    description.speedMax = 0.0f;
    description.rateCurve.assign(1, 0.0f);
    description.sizeCurve.assign(1, 1.0f);
    description.alphaCurve.assign(1, 1.0f);
    description.spinCurve.assign(1, 0.0f);
    description.colorCurve.assign(1, cS3DVector3{1.0f, 1.0f, 1.0f});
    description.emitMin[0] = description.emitMin[1] = description.emitMin[2] = 0.0f;
    description.emitMax[0] = description.emitMax[1] = description.emitMax[2] = 0.0f;
    description.textureId = 0;
    return description;
}

cSC4EffectsParticle* AsParticle(cSCParticle* particle) {
    return reinterpret_cast<cSC4EffectsParticle*>(particle);
}

// One fully initialised effect: terrain host, description, running.
struct Fixture {
    Hosts& hosts;
    cSC4ParticlesDescription description;
    sc4::cSC4ParticlesEffect effect;
    explicit Fixture(Hosts& hostRef, cSC4ParticlesDescription desc = MakeDescription())
        : hosts(hostRef), description(std::move(desc)) {
        effect.SetDescription(&description);
        effect.Init();
    }
};

// ---- tests -----------------------------------------------------------------------------------------
void TestLayoutAndConstruction() {
    CHECK_EQ(sizeof(cSC4ParticlesDescription), static_cast<std::size_t>(0x278));
    CHECK_EQ(sizeof(cSC4EffectsParticle), static_cast<std::size_t>(0x44));
    CHECK_EQ(sizeof(sc4::cSC4ParticlesEffect), static_cast<std::size_t>(0x280));
    sc4::cSC4ParticlesEffect effect;
    CHECK(effect.Description() == nullptr);
    CHECK_NEAR(effect.Position().x, 0.0, 0);
    const cS3DBoundingBox box = effect.BoundingBox();
    CHECK_NEAR(box.min[0], 1.0, 0);
    CHECK_NEAR(box.max[0], 0.0, 0);
    CHECK(!effect.IsStarted());
    CHECK_EQ(effect.TextureId(), 0u);
    CHECK(!effect.Suppressed());
}

void TestDescriptionLifecycle() {
    Hosts hosts;
    static int retains = 0, releases = 0;
    SetParticlesDescriptionLifecycle([](cSC4ParticlesDescription*) { ++retains; },
                                     [](cSC4ParticlesDescription*) { ++releases; });
    {
        cSC4ParticlesDescription first = MakeDescription();
        cSC4ParticlesDescription second = MakeDescription();
        sc4::cSC4ParticlesEffect effect;
        effect.SetDescription(&first);
        CHECK_EQ(retains, 1);
        CHECK_EQ(releases, 0);
        effect.SetDescription(&second);
        CHECK_EQ(retains, 2);
        CHECK_EQ(releases, 1);
        effect.Shutdown();
        CHECK_EQ(releases, 2);
    }
    SetParticlesDescriptionLifecycle(nullptr, nullptr);
    retains = releases = 0;
}

void TestInitAndShutdown() {
    Hosts hosts;
    cSC4ParticlesDescription description = MakeDescription();
    sc4::cSC4ParticlesEffect effect;
    effect.SetDescription(&description);
    CHECK(effect.Init());
    CHECK_EQ(hosts.terrain.addRefs, 1);
    CHECK_NEAR(effect.WorldWidth(), 6.0, 0);   // 3 cells * 2
    CHECK_NEAR(effect.WorldDepth(), 8.0, 0);   // 4 cells * 2
    CHECK(effect.Init());                      // second Init is a no-op
    CHECK_EQ(hosts.terrain.addRefs, 1);
    effect.Start(0);
    effect.Update(0.1f, nullptr);              // reads the water level
    CHECK_NEAR(effect.WaterLevel(), hosts.terrain.waterLevel, 0);
    CHECK(effect.Shutdown());
    CHECK_EQ(hosts.terrain.releases, 1);
    // Init again takes a new reference.
    CHECK(effect.Init());
    CHECK_EQ(hosts.terrain.addRefs, 2);
    effect.Shutdown();
}

void TestStartDelaysAndWalk() {
    Hosts hosts;
    cSC4ParticlesDescription description = MakeDescription();
    description.flags = 0x8000ull | 0x1000000ull | 8ull;   // random walk, direction from the description, start transition
    description.velocityMin[0] = description.velocityMax[0] = 3.0f;
    description.velocityMin[1] = description.velocityMax[1] = 4.0f;
    description.velocityMin[2] = description.velocityMax[2] = 5.0f;
    description.walkSpeedMin = description.walkSpeedMax = 2.0f;
    description.walkIntervalMin = description.walkIntervalMax = 0.5f;
    description.origin[0] = 1.0f;
    description.origin[1] = 2.0f;
    description.origin[2] = 3.0f;
    description.delayMinA = description.delayMaxA = 0.25f;
    sc4::cSC4ParticlesEffect effect;
    effect.SetDescription(&description);
    effect.Init();
    sc4::cSC4ParticlesEffect::Random().Seed(0x1234567u);
    effect.Start(1);
    CHECK(effect.IsStarted());
    CHECK_EQ(effect.StartTransition(), 1u);
    CHECK_EQ(effect.StopTransition(), 0u);
    CHECK_NEAR(effect.SpawnDelay(), 0.25, 1e-7);
    CHECK_NEAR(effect.Position().x, 1.0, 0);
    CHECK_NEAR(effect.Position().y, 2.0, 0);
    CHECK_NEAR(effect.Position().z, 3.0, 0);
    // Walk direction = (3, 0, 5) normalised to the walk speed (y stays 0), walk timer 0.5.
    const cS3DVector3 direction = effect.WalkDirection();
    CHECK_NEAR(std::sqrt(direction.x * direction.x + direction.z * direction.z), 2.0, 1e-6);
    CHECK_NEAR(direction.y, 0.0, 0);
    CHECK_NEAR(direction.x / direction.z, 3.0 / 5.0, 1e-6);
    effect.UpdateRandomWalk(0.6f);   // timer goes negative, a new direction is drawn
    const cS3DVector3 redrawn = effect.WalkDirection();
    CHECK_NEAR(std::sqrt(redrawn.x * redrawn.x + redrawn.z * redrawn.z), 2.0, 1e-5);
    CHECK_NEAR(redrawn.y, 0.0, 0);
    // A transition of 1 with a particle cap and without flag 8 refuses to start.
    {
        cSC4ParticlesDescription guarded = MakeDescription();
        guarded.maxParticles = 4;
        sc4::cSC4ParticlesEffect refused;
        refused.SetDescription(&guarded);
        refused.Init();
        refused.Start(1);
        CHECK(!refused.IsStarted());
        guarded.flags |= 8;
        refused.SetDescription(&guarded);
        refused.Start(1);
        CHECK(refused.IsStarted());
        refused.Shutdown();
    }
    sc4::cSC4ParticlesEffect second;
    second.SetDescription(&description);
    second.Init();
    second.Start(0);
    CHECK(second.IsStarted());
    CHECK_EQ(second.StartTransition(), 0u);
    CHECK_NEAR(second.SpawnDelay(), 0.25, 1e-7);
    // Stop(0) asks for a delayed stop (the particles run out); Stop(1) is immediate.
    second.Stop(0);
    CHECK(second.IsStarted());
    CHECK(second.IsInDelayedStop());
    CHECK_EQ(second.StopTransition(), 0u);
    second.Stop(1);
    CHECK(!second.IsStarted());
    CHECK_EQ(second.StopTransition(), 1u);
}

void TestRandomWalkDeterminism() {
    Hosts hosts;
    cSC4ParticlesDescription description = MakeDescription();
    description.flags = 0x8000ull;
    description.walkIntervalMin = description.walkIntervalMax = 1.0f;
    description.walkSpeedMin = description.walkSpeedMax = 1.0f;
    description.walkAngle = 0.0f;
    description.walkAngle2 = 0.0f;
    sc4::cSC4ParticlesEffect a, b;
    a.SetDescription(&description);
    b.SetDescription(&description);
    sc4::cSC4ParticlesEffect::Random().Seed(0x99u);
    a.UpdateRandomWalk(1.0f);
    sc4::cSC4ParticlesEffect::Random().Seed(0x99u);
    b.UpdateRandomWalk(1.0f);
    CHECK_NEAR(a.WalkDirection().x, b.WalkDirection().x, 0);
    CHECK_NEAR(a.WalkDirection().z, b.WalkDirection().z, 0);
    CHECK_NEAR(std::sqrt(a.WalkDirection().x * a.WalkDirection().x +
                         a.WalkDirection().z * a.WalkDirection().z), 1.0, 1e-5);
}

void TestParticleCreationAndPhysics() {
    Hosts hosts;
    cSC4ParticlesDescription description = MakeDescription();
    description.emitMin[0] = description.emitMax[0] = 10.0f;
    description.emitMin[1] = description.emitMax[1] = 20.0f;
    description.emitMin[2] = description.emitMax[2] = 30.0f;
    sc4::cSC4ParticlesEffect effect;
    effect.SetDescription(&description);
    effect.Init();
    effect.Start(0);
    cSCParticle* base = effect.CreateParticle();
    CHECK(base != nullptr);
    cSC4EffectsParticle* particle = AsParticle(base);
    CHECK_EQ(particle->alive, 1);
    CHECK_NEAR(particle->lifetime, 10.0, 0);
    CHECK_NEAR(particle->age, 0.0, 0);
    CHECK_NEAR(particle->position.x, 10.0, 0);
    CHECK_NEAR(particle->position.y, 20.0, 0);
    CHECK_NEAR(particle->position.z, 30.0, 0);
    CHECK_NEAR(particle->velocity.x, 0.0, 0);
    CHECK_NEAR(particle->size, 1.0, 0);      // sizeScale * scaleBias, no jitter
    CHECK_NEAR(particle->alpha, 1.0, 0);
    CHECK_NEAR(particle->rotation, 1.0, 0);  // spin defaults to 1 turn
    CHECK_NEAR(particle->colourR, 1.0, 0);
    // Physics: velocity gains the effect position as acceleration, then the position integrates with the mean velocity.
    CHECK(effect.UpdateParticlePhysics(particle, 0.25f));
    CHECK_NEAR(particle->age, 0.25, 1e-7);
    CHECK_NEAR(particle->velocity.x, 0.0, 1e-7);    // position_.x = 0 here
    CHECK_NEAR(particle->velocity.y, 0.0, 1e-7);
    CHECK_NEAR(particle->position.x, 10.0, 1e-7);
    // With a non-zero effect position the acceleration shows up.
    cSCParticle* secondBase = effect.CreateParticle();
    cSC4EffectsParticle* second = AsParticle(secondBase);
    second->position = cS3DVector3{10.0f, 20.0f, 30.0f};
    second->velocity = cS3DVector3{1.0f, 2.0f, 3.0f};
    CHECK(effect.UpdateParticlePhysics(second, 0.5f));
    CHECK_NEAR(second->velocity.x, 1.0, 1e-7);
    CHECK_NEAR(second->position.x, 10.5, 1e-7);
    CHECK_NEAR(second->position.y, 21.0, 1e-7);
    CHECK_NEAR(second->position.z, 31.5, 1e-7);
    CHECK(!effect.UpdateParticlePhysics(particle, 10.0f));
    CHECK_EQ(particle->alive, 0);
    effect.DestroyParticle(base);
    effect.DestroyParticle(secondBase);
    effect.Shutdown();
}

void TestActiveParticlesBox() {
    Hosts hosts;
    cSC4ParticlesDescription description = MakeDescription();
    description.sizeCurve.assign(1, 2.0f);
    sc4::cSC4ParticlesEffect effect;
    effect.SetDescription(&description);
    effect.Init();
    effect.UpdateActiveParticles();
    cS3DBoundingBox box = effect.BoundingBox();
    CHECK_NEAR(box.min[0], 1.0, 0);
    CHECK_NEAR(box.max[0], 0.0, 0);
    cSC4EffectsParticle* particle = AsParticle(effect.CreateParticle());
    particle->position = cS3DVector3{10.0f, 20.0f, 30.0f};
    effect.AddParticle(reinterpret_cast<cSCParticle*>(particle));
    effect.UpdateActiveParticles();
    box = effect.BoundingBox();
    CHECK_NEAR(box.min[0], 8.0, 0);
    CHECK_NEAR(box.min[1], 18.0, 0);
    CHECK_NEAR(box.min[2], 28.0, 0);
    CHECK_NEAR(box.max[0], 12.0, 0);
    CHECK_NEAR(box.max[1], 22.0, 0);
    CHECK_NEAR(box.max[2], 32.0, 0);
    // A second particle extends the box.
    cSC4EffectsParticle* other = AsParticle(effect.CreateParticle());
    other->position = cS3DVector3{-10.0f, 0.0f, 0.0f};
    effect.AddParticle(reinterpret_cast<cSCParticle*>(other));
    effect.UpdateActiveParticles();
    box = effect.BoundingBox();
    CHECK_NEAR(box.min[0], -12.0, 0);
    CHECK_NEAR(box.max[0], 12.0, 0);
    effect.Shutdown();
}

void TestEmissionModes() {
    Hosts hosts;
    // MaintainParticles: the rate curve value is the target live count.
    {
        cSC4ParticlesDescription description = MakeDescription();
        description.flags = 4;   // maintain
        description.rateCurve.assign(1, 3.0f);
        sc4::cSC4ParticlesEffect effect;
        effect.SetDescription(&description);
        effect.Init();
        effect.Start(0);
        effect.MaintainParticles(1.0f);
        CHECK_EQ(effect.LiveParticleCount(), 3);
        effect.MaintainParticles(1.0f);
        CHECK_EQ(effect.LiveParticleCount(), 3);   // stays at the target
        effect.Shutdown();
    }
    // InjectParticles: ceil(rate) particles, capped by the description's particle limit.
    {
        cSC4ParticlesDescription description = MakeDescription();
        description.flags = 2;   // inject
        description.rateCurve.assign(1, 2.5f);
        sc4::cSC4ParticlesEffect effect;
        effect.SetDescription(&description);
        effect.Init();
        effect.Start(0);
        effect.InjectParticles(1.0f);
        CHECK_EQ(effect.LiveParticleCount(), 3);   // ceil(2.5)
        effect.Shutdown();
    }
    // Reaching the particle cap calls Stop(0), which drops every particle and the running state.
    {
        cSC4ParticlesDescription description = MakeDescription();
        description.flags = 2;
        description.maxParticles = 1;
        description.rateCurve.assign(1, 1.0f);
        sc4::cSC4ParticlesEffect effect;
        effect.SetDescription(&description);
        effect.Init();
        effect.Start(0);
        effect.InjectParticles(1.0f);
        CHECK(effect.IsStarted());          // the cap asks for a delayed stop
        CHECK(effect.IsInDelayedStop());
        CHECK_EQ(effect.LiveParticleCount(), 1);
        effect.Shutdown();
    }
    // AddRateCurveParticles: the default mode, rate * step summed over the 1/15 s steps.
    {
        cSC4ParticlesDescription description = MakeDescription();
        description.rateCurve.assign(1, 30.0f);
        sc4::cSC4ParticlesEffect effect;
        effect.SetDescription(&description);
        effect.Init();
        effect.Start(0);
        effect.AddRateCurveParticles(1.0f);
        CHECK(effect.LiveParticleCount() >= 29 && effect.LiveParticleCount() <= 31);
        effect.Shutdown();
    }
    // A positive spawnDelay swallows the first frame.
    {
        cSC4ParticlesDescription description = MakeDescription();
        description.delayMinA = description.delayMaxA = 1.0f;
        sc4::cSC4ParticlesEffect effect;
        effect.SetDescription(&description);
        effect.Init();
        effect.Start(0);
        CHECK_NEAR(effect.SpawnDelay(), 1.0, 0);
        effect.Update(0.25f, nullptr);
        CHECK_NEAR(effect.SpawnDelay(), 0.75, 1e-7);
        effect.Shutdown();
    }
}

void TestProcessParticle() {
    Hosts hosts;
    cSC4ParticlesDescription description = MakeDescription();
    description.rateCurve.assign(1, 1.0f);
    description.colorCurve.assign(1, cS3DVector3{1.0f, 0.5f, 0.25f});
    sc4::cSC4ParticlesEffect effect;
    effect.SetDescription(&description);
    effect.Init();
    FakeDrawContext context;
    effect.PrepareForRendering(&context);
    cSC4EffectsParticle* particle = AsParticle(effect.CreateParticle());
    particle->position = cS3DVector3{10.0f, 20.0f, 30.0f};
    cS3DVector3 corner{}, right{}, up{};
    cS3DColorARGB8 colour8{};
    effect.ProcessParticle(particle, 0, corner, right, up, colour8);
    CHECK_NEAR(corner.x, 9.5, 1e-6);    // position - (right(1,0,0) + up(0,0,1)) / 2
    CHECK_NEAR(corner.y, 20.0, 1e-6);
    CHECK_NEAR(corner.z, 29.5, 1e-6);
    CHECK_NEAR(right.x, 1.0, 1e-6);
    CHECK_NEAR(up.z, 1.0, 1e-6);
    CHECK_EQ(colour8.r, 255);          // PackedColorByte(1.0)
    CHECK_EQ(colour8.g, 128);          // PackedColorByte(0.5) = 0.5 * 256
    CHECK_EQ(colour8.b, 64);           // PackedColorByte(0.25)
    CHECK_EQ(colour8.a, 255);
    cS3DColorARGB32 colour32{};
    effect.ProcessParticle(particle, 0, corner, right, up, colour32);
    CHECK_NEAR(colour32.r, 1.0, 1e-6);
    CHECK_NEAR(colour32.g, 0.5, 1e-6);
    CHECK_NEAR(colour32.b, 0.25, 1e-6);
    CHECK_NEAR(colour32.a, 1.0, 1e-6);
    // Colours above 1 wrap through the high-byte fold: 2.0 -> 254.
    particle->colourR = 2.0f;
    effect.ProcessParticle(particle, 0, corner, right, up, colour8);
    CHECK_EQ(colour8.r, 254);
    particle->colourR = 1.0f;
    // The max width stretches the right vector.
    description.maxWidth = 2.0f;
    particle->velocity = cS3DVector3{3.0f, 0.0f, 0.0f};
    effect.ProcessParticle(particle, 0, corner, right, up, colour8);
    CHECK_NEAR(right.x, 1.5, 1e-6);    // length 3 / maxWidth 2
    CHECK_NEAR(corner.x, 10.0 - 0.75, 1e-6);
    description.maxWidth = 0.0f;
    // The colour curve at fraction 0 keeps the first sample, at 0.5 it interpolates.
    description.colorCurve.assign(2, cS3DVector3{});
    description.colorCurve[0] = cS3DVector3{0.0f, 0.0f, 0.0f};
    description.colorCurve[1] = cS3DVector3{1.0f, 1.0f, 1.0f};
    particle->age = 0.0f;
    particle->lifetime = 10.0f;
    effect.ProcessParticle(particle, 1, corner, right, up, colour8);
    CHECK_EQ(colour8.r, 0);
    particle->age = 5.0f;
    effect.ProcessParticle(particle, 1, corner, right, up, colour8);
    CHECK_EQ(colour8.r, 128);
    effect.DestroyParticle(reinterpret_cast<cSCParticle*>(particle));
    effect.Shutdown();
}

void TestFloatAndVoidParams() {
    Hosts hosts;
    cSC4ParticlesDescription description = MakeDescription();
    description.speedMin = description.speedMax = 1.0f;
    description.velocityMin[0] = description.velocityMax[0] = 1.0f;
    sc4::cSC4ParticlesEffect effect;
    effect.SetDescription(&description);
    effect.Init();
    // Type 1 scales the size.
    const float size = 2.0f;
    CHECK(effect.SetFloatParams(1, &size, 1));
    cSC4EffectsParticle* particle = AsParticle(effect.CreateParticle());
    CHECK_NEAR(particle->size, 2.0, 1e-6);
    effect.DestroyParticle(reinterpret_cast<cSCParticle*>(particle));
    // Type 0 scales the velocity (the speed range is 1 and the direction is +x).
    const float velocityBias = 3.0f;
    CHECK(effect.SetFloatParams(0, &velocityBias, 1));
    particle = AsParticle(effect.CreateParticle());
    CHECK_NEAR(particle->velocity.x, 3.0, 1e-6);
    effect.DestroyParticle(reinterpret_cast<cSCParticle*>(particle));
    // Type 0x102 sets the rate bias and the scale bias (the size uses the latter).
    const float biases[2] = {5.0f, 4.0f};
    CHECK(effect.SetFloatParams(0x102, biases, 2));
    particle = AsParticle(effect.CreateParticle());
    CHECK_NEAR(particle->size, 8.0, 1e-6);   // the earlier type-1 size scale (2) times this scale bias (4)
    effect.DestroyParticle(reinterpret_cast<cSCParticle*>(particle));
    // Types 6..8 and 0xc..0xe are rejected.
    CHECK(!effect.SetFloatParams(6, &size, 1));
    CHECK(!effect.SetFloatParams(0xd, &size, 1));
    CHECK(!effect.SetFloatParams(99, &size, 1));
    // Type 5 forwards to SetTarget, which rebuilds the effect rotation (flag 4 = rotation).
    const float target[3] = {0.0f, 0.0f, 0.0f};
    CHECK(effect.SetFloatParams(5, target, 3));
    CHECK((effect.EffectTransform().flags & 4) != 0);
    // Type 10 sets the attractor target, type 0xb the length.
    const float point[3] = {1.0f, 2.0f, 3.0f};
    CHECK(effect.SetFloatParams(10, point, 3));
    const float length = 5.0f;
    CHECK(effect.SetFloatParams(0xb, &length, 1));
    // SetVoidParams type 7 places the model at the emission box minimum when flag 0x40 is set.
    FakeModel model;
    void* modelPointer = &model;
    description.flags |= 0x40;
    CHECK(effect.SetVoidParams(7, &modelPointer, 1));
    CHECK_EQ(model.calls, 1);
    CHECK_NEAR(model.position.x, description.emitMin[0], 0);
    CHECK(!effect.SetVoidParams(1, &modelPointer, 1));
    effect.Shutdown();
}

void TestTransforms() {
    Hosts hosts;
    cSC4ParticlesDescription description = MakeDescription();
    description.emitMin[0] = -1.0f;
    description.emitMax[0] = 1.0f;
    description.emitMin[1] = -2.0f;
    description.emitMax[1] = 2.0f;
    description.emitMin[2] = -3.0f;
    description.emitMax[2] = 3.0f;
    sc4::cSC4ParticlesEffect effect;
    effect.SetDescription(&description);
    effect.Init();
    cS3DTransform source{};
    source.flags = 1;
    source.translation[0] = 10.0f;
    source.translation[1] = 20.0f;
    source.translation[2] = 30.0f;
    source.scale = 2.0f;
    cS3DTransform effectTransform{};
    effect.SetTransforms(effectTransform, source);
    CHECK((effect.SourceTransform().flags & 1) != 0);
    CHECK_NEAR(effect.SourceTransform().scale, 2.0, 0);
    const cS3DBoundingBox box = effect.BoundingBox();
    CHECK_NEAR(box.min[0], 20.0, 1e-6);   // translation * scale
    CHECK_NEAR(box.min[1], 40.0, 1e-6);
    CHECK_NEAR(box.min[2], 60.0, 1e-6);
    CHECK_NEAR(box.max[0], 20.0, 1e-6);
    // The aligned mode (description flag 0x1000) drops both translations and centres on the emission box.
    description.flags |= 0x1000;
    effect.SetTransforms(effectTransform, source);
    CHECK_NEAR(effect.EffectTransform().translation[0], 0.0, 0);
    CHECK_NEAR(effect.SourceTransform().translation[0], 0.0, 0);
    const cS3DBoundingBox aligned = effect.BoundingBox();
    CHECK_NEAR(aligned.min[0], -1.0, 0);
    CHECK_NEAR(aligned.max[0], 1.0, 0);
    CHECK_NEAR(aligned.min[1], -2.0, 0);
    CHECK_NEAR(aligned.max[2], 3.0, 0);
    effect.Shutdown();
}

void TestTimedEffectsAndTrigger() {
    Hosts hosts;
    cSC4ParticlesDescription description = MakeDescription();
    description.name = "boom";
    description.timedEffects.push_back(cSC4TimedEffectRecord{"spark", 0.0f, 0});
    description.timedEffects.push_back(cSC4TimedEffectRecord{"flash", 0.5f, 0});
    description.flags |= 0x80000000ull;   // Update launches the timed effects
    sc4::cSC4ParticlesEffect effect;
    effect.SetDescription(&description);
    effect.Init();
    effect.Start(0);
    cSC4EffectsParticle* particle = AsParticle(effect.CreateParticle());
    particle->age = 0.4f;
    effect.AddParticle(reinterpret_cast<cSCParticle*>(particle));
    effect.Update(0.2f, nullptr);
    // Only the record that crosses its time this frame fires; name = description name + entry name.
    CHECK_EQ(hosts.effects.triggered.size(), static_cast<std::size_t>(1));
    if (!hosts.effects.triggered.empty()) CHECK(hosts.effects.triggered[0] == "boomflash");   // the record name is appended verbatim
    // The time-zero path fires the leading zero-time records only.
    hosts.effects.triggered.clear();
    effect.LaunchTimeZeroEffects(reinterpret_cast<cSCParticle*>(particle));
    CHECK_EQ(hosts.effects.triggered.size(), static_cast<std::size_t>(1));
    if (!hosts.effects.triggered.empty()) CHECK(hosts.effects.triggered[0] == "boomspark");
    // Direct trigger: the lookup receives the transform at the position and is started.
    hosts.effects.triggered.clear();
    hosts.effects.hasEffect = true;
    effect.TriggerEffect("_water", cS3DVector3{1.0f, 2.0f, 3.0f});
    CHECK_EQ(hosts.effects.triggered.size(), static_cast<std::size_t>(1));
    if (!hosts.effects.triggered.empty()) CHECK(hosts.effects.triggered[0] == "boom_water");
    CHECK_EQ(hosts.effects.lookup.starts, 1);
    CHECK_NEAR(hosts.effects.lookup.transform.translation[2], 3.0, 0);
    CHECK_EQ(hosts.effects.lookup.releases, 1);
    effect.Shutdown();
}

void TestDescriptionEffectLookup() {
    Hosts hosts;
    cSC4ParticlesDescription description = MakeDescription();
    description.flags |= 0x100;
    description.name = "smoke";
    sc4::cSC4ParticlesEffect effect;
    effect.SetDescription(&description);
    CHECK(!effect.HasTerrainEffect());
    CHECK(!effect.HasWaterEffect());
    hosts.effects.hasEffect = true;
    effect.SetDescription(&description);
    CHECK(effect.HasTerrainEffect());
    CHECK(effect.HasWaterEffect());
    CHECK_EQ(hosts.effects.lookedUp.size(), static_cast<std::size_t>(4));
    if (hosts.effects.lookedUp.size() == 4) {
        CHECK(hosts.effects.lookedUp[2] == "smoke_terrain");
        CHECK(hosts.effects.lookedUp[3] == "smoke_water");
    }
    effect.Shutdown();
}

void TestHeightBasedEmit() {
    Hosts hosts;
    cSC4ParticlesDescription description = MakeDescription();
    description.flags = 0x4000;   // height based emission, clamp to the water level when below it
    sc4::cSC4ParticlesEffect effect;
    effect.SetDescription(&description);
    effect.Init();
    effect.Start(0);
    effect.Update(0.1f, nullptr);   // caches the water level
    hosts.terrain.height = 3.0f;
    cSC4EffectsParticle* particle = AsParticle(effect.CreateParticle());
    particle->position = cS3DVector3{5.0f, 0.0f, 6.0f};
    CHECK(effect.DoHeightBasedEmit(particle));
    CHECK_EQ(particle->alive, 1);
    CHECK_NEAR(particle->position.x, 5.0, 1e-6);
    CHECK_NEAR(particle->position.y, 3.0, 1e-6);   // snapped to the terrain height plus the (empty) emission band
    CHECK_NEAR(particle->position.z, 6.0, 1e-6);
    // Outside the world: killed.
    particle->position = cS3DVector3{-1.0f, 0.0f, 6.0f};
    CHECK(!effect.DoHeightBasedEmit(particle));
    CHECK_EQ(particle->alive, 0);
    // Height band: flag 0x10000 restricts the terrain height minus the water level.
    particle->alive = 1;
    particle->position = cS3DVector3{5.0f, 0.0f, 6.0f};
    description.flags |= 0x10000;
    description.heightMin = 10.0f;
    description.heightMax = 20.0f;
    CHECK(!effect.DoHeightBasedEmit(particle));
    // Lake / sea cell tests.
    particle->alive = 1;
    particle->position = cS3DVector3{5.0f, 0.0f, 6.0f};
    description.flags = 0x60000ull | 0x8000ull;   // 0x20000 | 0x40000 and the negative low word
    hosts.city.cells.flags = 0;
    CHECK(!effect.DoHeightBasedEmit(particle));
    particle->alive = 1;
    hosts.city.cells.flags = 0x1;
    CHECK(effect.DoHeightBasedEmit(particle));
    effect.DestroyParticle(reinterpret_cast<cSCParticle*>(particle));
    effect.Shutdown();
}

void TestExtraPhysics() {
    Hosts hosts;
    cSC4ParticlesDescription description = MakeDescription();
    description.flags = 0x800ull;   // extra physics without the tractor / attractor bits
    sc4::cSC4ParticlesEffect effect;
    effect.SetDescription(&description);
    effect.Init();
    effect.Start(0);
    effect.Update(0.1f, nullptr);
    cSC4EffectsParticle* particle = AsParticle(effect.CreateParticle());
    particle->position = cS3DVector3{3.0f, 3.0f, 3.0f};
    particle->velocity = cS3DVector3{0.0f, 0.0f, 0.0f};
    CHECK(effect.UpdateParticlePhysics(particle, 0.1f));
    CHECK_EQ(particle->alive, 1);
    CHECK_NEAR(particle->velocity.x, 0.0, 1e-7);   // position_ is (0,0,0)
    CHECK_NEAR(particle->position.x, 3.0, 1e-7);
    // The building test is dispatched from the extra-physics step (description bit 32).
    particle->alive = 1;
    hosts.occupants.blocked = true;
    description.flags = 0x100000800ull;
    CHECK(!effect.UpdateParticlePhysics(particle, 0.1f));   // the collision kills it, so the step reports false
    CHECK_EQ(hosts.occupants.boxCalls, 1);
    CHECK_EQ(particle->alive, 0);
    hosts.occupants.blocked = false;
    effect.DestroyParticle(reinterpret_cast<cSCParticle*>(particle));
    effect.Shutdown();
}

void TestTractorForce() {
    Hosts hosts;
    cSC4ParticlesDescription description = MakeDescription();
    std::vector<cSC4TractorRecord> records(2);
    records[0].from = cS3DVector3{0.0f, 0.0f, 0.0f};
    records[0].to = cS3DVector3{1.0f, 0.0f, 0.0f};
    records[0].time = 0.0f;
    records[0].end = 1.0f;
    records[1].from = cS3DVector3{1.0f, 0.0f, 0.0f};
    records[1].to = cS3DVector3{2.0f, 0.0f, 0.0f};
    records[1].time = 1.0f;
    records[1].end = 1.0f;
    description.tractors = records;
    sc4::cSC4ParticlesEffect effect;
    effect.SetDescription(&description);
    effect.Init();
    cSC4EffectsParticle* particle = AsParticle(effect.CreateParticle());
    particle->position = cS3DVector3{0.0f, 0.0f, 0.0f};
    particle->velocity = cS3DVector3{1.0f, 0.0f, 0.0f};
    particle->lifetime = 2.0f;
    // Past the record's end the particle just drifts.
    particle->age = 3.0f;
    effect.ApplyTractorForce(particle, 0.5f);
    CHECK_NEAR(particle->position.x, 0.5, 1e-6);
    particle->position = cS3DVector3{0.0f, 0.0f, 0.0f};
    particle->age = 1.5f;   // in the second record, whose end is the lifetime
    effect.ApplyTractorForce(particle, 0.5f);
    CHECK_EQ(particle->alive, 1);
    particle->position = cS3DVector3{0.0f, 0.0f, 0.0f};
    particle->age = 0.5f;   // in the first record
    effect.ApplyTractorForce(particle, 0.25f);
    CHECK_EQ(particle->alive, 1);
    effect.DestroyParticle(reinterpret_cast<cSCParticle*>(particle));
    effect.Shutdown();
}

void TestTerrainForcesAndCollision() {
    Hosts hosts;
    hosts.terrain.waterLevel = 1.0f;
    cSC4ParticlesDescription description = MakeDescription();
    sc4::cSC4ParticlesEffect effect;
    effect.SetDescription(&description);
    effect.Init();
    effect.Start(0);
    effect.Update(0.1f, nullptr);
    cSC4EffectsParticle* particle = AsParticle(effect.CreateParticle());
    // Without the effect-maps service the function reports success and leaves the particle alone.
    CHECK(effect.ApplyTerrainForces(particle, cS3DVector3{1.0f, 2.0f, 1.0f}, 0.1f));
    CHECK_EQ(particle->alive, 1);
    // A particle below the water level is killed (terrainBounce[6] = 1 means always).
    description.terrainBounce[6] = 1.0f;
    CHECK(!effect.ApplyTerrainForces(particle, cS3DVector3{1.0f, 0.5f, 1.0f}, 0.1f));
    CHECK_EQ(hosts.effects.maps.releases, 0);
    // The water effect fires when the effect was named and has the _water suffix.
    hosts.effects.hasEffect = true;
    description.flags |= 0x100;
    description.name = "splash";
    effect.SetDescription(&description);
    hosts.effects.triggered.clear();
    hosts.effects.maps.releases = 0;
    CHECK(!effect.ApplyTerrainForces(particle, cS3DVector3{1.0f, 0.5f, 1.0f}, 0.1f));
    CHECK_EQ(hosts.effects.triggered.size(), static_cast<std::size_t>(1));
    if (!hosts.effects.triggered.empty()) CHECK(hosts.effects.triggered[0] == "splash_water");
    // Building collision: the occupant manager seam decides.
    hosts.occupants.blocked = true;
    particle->alive = 1;
    effect.TestBuildingCollision(particle);
    CHECK_EQ(hosts.occupants.boxCalls, 1);
    CHECK_EQ(particle->alive, 0);
    hosts.occupants.blocked = false;
    particle->alive = 1;
    effect.TestBuildingCollision(particle);
    CHECK_EQ(particle->alive, 1);
    effect.Shutdown();
}

void TestUpdateDraws() {
    Hosts hosts;
    cSC4ParticlesDescription description = MakeDescription();
    description.flags = 1;        // self-lit around the draw
    description.drawMode = 0;
    description.textureId = 7;
    description.sizeCurve.assign(1, 1.0f);
    description.rateCurve.assign(1, 1.0f);   // the alpha curve: a zero here skips every vertex
    sc4::cSC4ParticlesEffect effect;
    effect.SetDescription(&description);
    effect.Init();
    CHECK_EQ(hosts.textures.creates, 1);
    CHECK_EQ(effect.TextureId(), 0x107u);
    effect.Start(0);
    cSC4EffectsParticle* particle = AsParticle(effect.CreateParticle());
    particle->position = cS3DVector3{0.0f, 0.0f, 0.0f};
    effect.AddParticle(reinterpret_cast<cSCParticle*>(particle));
    CHECK_EQ(effect.LiveParticleCount(), 1);
    FakeDrawContext context;
    CHECK(effect.Update(0.1f, &context));
    CHECK_EQ(context.resetModelViewCalls, 1);
    CHECK_EQ(context.stateCalls, 1);
    CHECK_EQ(context.selfLitCalls, 1);
    CHECK_EQ(context.resetShadeCalls, 1);
    CHECK_EQ(context.setTextureCalls, 1);
    CHECK_EQ(context.lastTexture, 0x107);
    CHECK_EQ(context.vertexBufferCalls, 1);
    CHECK_EQ(context.vertexFormat, 10u);
    CHECK_EQ(context.vertexCount, 4u);
    CHECK_EQ(context.drawCalls, 1);
    CHECK_EQ(context.lastDrawCount, 6);   // one quad = 6 indices
    // Draw mode 0: depth test on, depth write off, blend with src 4 / dst 5, depth func 3.
    CHECK((context.state.value & renderstate::kDepthWrite) == 0);
    CHECK((context.state.value & renderstate::kDepthTest) != 0);
    CHECK((context.state.value & renderstate::kBlend) != 0);
    CHECK_EQ(renderstate::DepthFunc(context.state.value), 3u);
    CHECK_EQ(renderstate::BlendSrc(context.state.value), 4u);
    CHECK_EQ(renderstate::BlendDst(context.state.value), 5u);
    // The quad: corner+right, corner, corner+up, corner+up+right with (0,1)-(0,0)-(1,1)-(1,0) texture coordinates.
    if (context.vertexData) {
        const auto* values = static_cast<const float*>(context.vertexData);
        CHECK_NEAR(values[0], 0.5, 1e-6);
        CHECK_NEAR(values[1], 0.0, 1e-6);
        CHECK_NEAR(values[2], -0.5, 1e-6);
        CHECK_EQ(reinterpret_cast<const std::uint8_t*>(&values[3])[0], 255);
        CHECK_NEAR(values[4], 0.0, 0);
        CHECK_NEAR(values[5], 0.0, 0);
        CHECK_NEAR(values[6], -0.5, 1e-6);
        CHECK_NEAR(values[7], 0.0, 1e-6);
        CHECK_NEAR(values[8], -0.5, 1e-6);
        CHECK_NEAR(values[10], 0.0, 0);
        CHECK_NEAR(values[11], 1.0, 0);
        CHECK_NEAR(values[12], -0.5, 1e-6);
        CHECK_NEAR(values[13], 0.0, 1e-6);
        CHECK_NEAR(values[14], 0.5, 1e-6);
        CHECK_NEAR(values[16], 1.0, 0);
        CHECK_NEAR(values[17], 1.0, 0);
        CHECK_NEAR(values[18], 0.5, 1e-6);
        CHECK_NEAR(values[19], 0.0, 1e-6);
        CHECK_NEAR(values[20], 0.5, 1e-6);
        CHECK_NEAR(values[22], 1.0, 0);
        CHECK_NEAR(values[23], 0.0, 0);
    } else {
        CHECK(false);
    }
    // The bounding box was expanded by sizeCurve[0].
    const cS3DBoundingBox box = effect.BoundingBox();
    CHECK_NEAR(box.min[0], -1.0, 1e-6);
    CHECK_NEAR(box.max[0], 1.0, 1e-6);
    // With no live particle Update takes the update-only branch (no draw).
    effect.RemoveParticle(reinterpret_cast<cSCParticle*>(particle));
    FakeDrawContext second;
    effect.Update(0.1f, &second);
    CHECK_EQ(second.drawCalls, 0);
    CHECK_EQ(second.setTextureCalls, 0);
    CHECK_EQ(effect.LiveParticleCount(), 0);   // Update freed the dead particle
    effect.Shutdown();
}

void TestDrawModes() {
    Hosts hosts;
    for (std::uint32_t mode : {0u, 5u, 6u, 7u}) {
        cSC4ParticlesDescription description = MakeDescription();
        description.drawMode = static_cast<std::uint8_t>(mode);
        description.textureId = 1;
        sc4::cSC4ParticlesEffect effect;
        effect.SetDescription(&description);
        effect.Init();
        effect.Start(0);
        cSC4EffectsParticle* particle = AsParticle(effect.CreateParticle());
        effect.AddParticle(reinterpret_cast<cSCParticle*>(particle));
        FakeDrawContext context;
        effect.Update(0.1f, &context);
        CHECK_EQ(context.stateCalls, 1);
        if (mode == 5 || mode == 6) {
            CHECK_EQ(renderstate::BlendDst(context.state.value), 1u);
        } else if (mode == 7) {
            CHECK_EQ(renderstate::BlendSrc(context.state.value), 0u);
            CHECK_EQ(renderstate::BlendDst(context.state.value), 2u);
            CHECK_EQ(renderstate::DepthFunc(context.state.value), 3u);
        }
        effect.Shutdown();
    }
    // An unknown draw mode leaves the packed state untouched.
    {
        cSC4ParticlesDescription description = MakeDescription();
        description.drawMode = 42;
        sc4::cSC4ParticlesEffect effect;
        effect.SetDescription(&description);
        effect.Init();
        effect.Start(0);
        cSC4EffectsParticle* particle = AsParticle(effect.CreateParticle());
        effect.AddParticle(reinterpret_cast<cSCParticle*>(particle));
        FakeDrawContext context;
        effect.Update(0.1f, &context);
        CHECK_EQ(context.stateCalls, 1);
        CHECK_EQ(context.state.value, 0u);
        effect.Shutdown();
    }
}

void TestVertexBufferPath() {
    Hosts hosts;
    cSC4ParticlesDescription description = MakeDescription();
    description.textureId = 3;
    description.flags = 0;   // no flip
    sc4::cSC4ParticlesEffect effect;
    effect.SetDescription(&description);
    effect.Init();
    FakeVertexBuffer buffer;
    effect.SetVertexBufferExtension(&buffer);
    CHECK_EQ(buffer.addRefs, 1);
    effect.Start(0);
    cSC4EffectsParticle* particle = AsParticle(effect.CreateParticle());
    effect.AddParticle(reinterpret_cast<cSCParticle*>(particle));
    FakeDrawContext context;
    effect.Update(0.1f, &context);
    CHECK_EQ(buffer.locks, 1);
    CHECK_EQ(buffer.unlocks, 1);
    CHECK_EQ(buffer.draws, 1);
    CHECK_EQ(buffer.lastIndexCount, 6u);
    CHECK_EQ(buffer.lastVertexBytes, 0x20 * 4);
    CHECK_EQ(context.drawCalls, 0);   // the pool path is not used
    // Replacing the extension releases the old one.
    FakeVertexBuffer other;
    effect.SetVertexBufferExtension(&other);
    CHECK_EQ(buffer.releases, 1);
    CHECK_EQ(other.addRefs, 1);
    effect.SetVertexBufferExtension(nullptr);
    CHECK_EQ(other.releases, 1);
    effect.Shutdown();   // the simulator's Shutdown frees the live particle
}

void TestSuppressAndBoundingBox() {
    Hosts hosts;
    cSC4ParticlesDescription description = MakeDescription();
    sc4::cSC4ParticlesEffect effect;
    effect.SetDescription(&description);
    effect.Init();
    effect.Start(0);
    effect.SuppressEmission();
    CHECK(effect.Suppressed());
    // Aligned SetTransforms does not reset the box while emission is suppressed.
    const std::uint64_t flags = description.flags;
    description.flags |= 0x1000;
    cS3DTransform identity{};
    effect.SetTransforms(identity, identity);
    CHECK((effect.EffectTransform().flags & 1) == 0);
    description.flags = flags;
    effect.Shutdown();
}

void TestGlobalParamsAndTarget() {
    Hosts hosts;
    cSC4ParticlesDescription description = MakeDescription();
    sc4::cSC4ParticlesEffect effect;
    effect.SetDescription(&description);
    effect.Init();
    cSC4GlobalEffectParams params;
    params.sizeScale = 5.0f;
    params.maxAlpha = 2.0f;
    effect.SetGlobalParams(&params);
    cSC4EffectsParticle* particle = AsParticle(effect.CreateParticle());
    CHECK_NEAR(particle->size, 5.0, 1e-6);   // the global size scale is applied (description flag 0x10 clear)
    effect.DestroyParticle(reinterpret_cast<cSCParticle*>(particle));
    description.flag10 |= 1;   // keep the local scale
    particle = AsParticle(effect.CreateParticle());
    CHECK_NEAR(particle->size, 1.0, 1e-6);
    effect.DestroyParticle(reinterpret_cast<cSCParticle*>(particle));
    // SetTarget rebuilds the effect rotation from the direction to the target.
    effect.SetTransforms(cS3DTransform{}, cS3DTransform{});
    effect.SetTarget(cS3DVector3{10.0f, 0.0f, 0.0f});
    CHECK((effect.EffectTransform().flags & 4) != 0);
    CHECK_NEAR(effect.EffectTransform().rotation[0][0], 1.0, 1e-6);
    CHECK_NEAR(effect.EffectTransform().rotation[1][1], 1.0, 1e-6);
    effect.Shutdown();
}

}  // namespace

int main() {
    TestLayoutAndConstruction();
    TestDescriptionLifecycle();
    TestInitAndShutdown();
    TestStartDelaysAndWalk();
    TestRandomWalkDeterminism();
    TestParticleCreationAndPhysics();
    TestActiveParticlesBox();
    TestEmissionModes();
    TestProcessParticle();
    TestFloatAndVoidParams();
    TestTransforms();
    TestTimedEffectsAndTrigger();
    TestDescriptionEffectLookup();
    TestHeightBasedEmit();
    TestExtraPhysics();
    TestTractorForce();
    TestTerrainForcesAndCollision();
    TestUpdateDraws();
    TestDrawModes();
    TestVertexBufferPath();
    TestSuppressAndBoundingBox();
    TestGlobalParamsAndTarget();
    if (failures == 0) std::printf("sc4_particleseffect: all checks passed\n");
    else std::printf("sc4_particleseffect: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
