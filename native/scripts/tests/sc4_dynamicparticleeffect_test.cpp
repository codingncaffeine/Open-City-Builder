// cSC4DynamicParticleEffect (Mac i386 0x004b8c56..0x004bab5e): the description's model, the body Start makes from the
// model's bounds and the source transform, the collide effect on a moving hit, Stop giving the body and the system back.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "s3d/ModelInstance.h"
#include "sc4/DynamicParticleEffect.h"
#include "sc4/DynamicParticleSystem.h"
#include "sc4/ModelBasedParticlesEffect.h"
#include "sc4/VisualEffect.h"

using namespace sc4;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static bool Near(float a, float b, float e = 1e-4f) { return std::fabs(a - b) <= e; }

struct FakeModel final : s3d::cIS3DModelInstance {
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    cIGZUnknown* GetOwner() override { return nullptr; }
    void SetOwner(cIGZUnknown*) override {}
    void* GetCustomData() override { return nullptr; }
    void SetCustomData(void*) override {}
    void GetBounds(cS3DBoundingBox& out) override {
        out.min[0] = -1.0f; out.min[1] = 0.0f; out.min[2] = -1.0f;
        out.max[0] = 1.0f; out.max[1] = 2.0f; out.max[2] = 1.0f;
    }
    void GetWorldBounds(cS3DBoundingBox& out) override { GetBounds(out); }
    void GetTransform(cS3DTransform& out) override { out = transform; }
    void SetTransform(const cS3DTransform& t) override { transform = t; }
    bool Pick(cSC4PickContext*) override { return false; }
    int Draw(SC4DrawContext*, bool) override { return 0; }
    int GetProperties() override { return 0; }
    void SetProperties(int) override {}
    s3d::SC4DrawAttributes GetDrawAttributes() const override { return {}; }
    void SetDrawAttributes(const s3d::SC4DrawAttributes&) override {}
    void GetSortValue(std::uint32_t& out) const override { out = 0; }
    s3d::cIS3DResModel* GetResModel() const override { return nullptr; }
    int refs = 1;
    cS3DTransform transform;
};

struct Host final : IModelParticlesHost {
    bool CreateModel(std::uint32_t modelId, s3d::cIS3DResModel** model, s3d::cIS3DModelInstance** instance) override {
        createdModels.push_back(modelId);
        *model = nullptr;
        fake.AddRef();
        *instance = &fake;
        return true;
    }
    bool HasVisualEffect(const char* name) override { return std::string(name) == "cone_collide"; }
    cSC4VisualEffect* CreateVisualEffect(const char* name) override {
        madeEffects.push_back(name);
        if (std::string(name) != "cone_collide") return nullptr;
        auto* effect = new cSC4VisualEffect();
        effect->AddRef();
        return effect;
    }
    FakeModel fake;
    std::vector<std::uint32_t> createdModels;
    std::vector<std::string> madeEffects;
};

int main() {
    Host host;
    SetModelParticlesHost(&host);
    cSC4DynamicParticleDescription description;
    description.name = "cone";
    description.modelId = 77;
    description.values[0] = 2.0f;   // mass
    description.values[2] = 1.0f;   // forward friction

    auto* effect = new cSC4DynamicParticleEffect();
    effect->AddRef();
    effect->Start(0);               // no model yet: nothing starts
    CHECK(!effect->IsStarted() && cSC4DynamicParticleEffect::DynamicSystem() == nullptr);
    effect->SetDescription(description);
    CHECK(host.createdModels.size() == 1 && host.createdModels[0] == 77 && effect->Model() == &host.fake);
    CHECK(effect->HasCollideEffect());

    // started at the source transform: a 1.5 x 1.5 box (the bounds less half a unit), the full height, standing on the
    // source's height, turned by -atan2(r00, r02), with the mass and friction of the description
    cS3DTransform source;
    source.translation[0] = 200.0f;
    source.translation[1] = 5.0f;
    source.translation[2] = 300.0f;
    effect->SetTransforms(source, cS3DTransform{});
    const float velocity[3] = {3.0f, 0.0f, 4.0f};
    CHECK(effect->SetFloatParams(kDynamicParticleVelocityParam, velocity, 3) && !effect->SetFloatParams(3, velocity, 3));
    effect->Start(0);
    CHECK(effect->IsStarted() && cSC4DynamicParticleEffect::DynamicSystem() != nullptr);
    cSC4DynamicParticle* particle = effect->Particle();
    CHECK(particle && particle->object && particle->model == &host.fake);
    if (particle && particle->object) {
        const cSC4RigidBodySimulator2D::Object& body = *particle->object;
        CHECK(Near(body.x, 200.0f) && Near(body.y, 300.0f) && Near(body.halfX, 0.75f) && Near(body.halfY, 0.75f));
        CHECK(Near(body.halfHeight, 1.0f) && Near(body.height, 6.0f) && Near(body.inverseMass, 0.5f));
        CHECK(Near(body.angle, -1.5707964f) && body.friction && Near(body.forwardFriction, 1.0f));
        CHECK(Near(body.vx, 3.0f) && Near(body.vy, 4.0f) && !body.kinematic);
    }
    CHECK(host.madeEffects.size() == 1 && host.madeEffects[0] == "cone_slave");   // no slave effect defined

    // a hit while moving starts "cone_collide" and clears the hit count; a hit while still does nothing
    if (particle && particle->object) {
        particle->object->contacts = 2;
        particle->object->vx = 0.0f;
        particle->object->vy = 0.0f;
        effect->ApplyEffect(0.016f);
        CHECK(particle->object->contacts == 2 && host.madeEffects.size() == 1);
        particle->object->vx = 1.0f;
        effect->ApplyEffect(0.016f);
        CHECK(particle->object->contacts == 0 && host.madeEffects.size() == 2 && host.madeEffects[1] == "cone_collide");
    }
    // the link id goes on the particle
    void* id = reinterpret_cast<void*>(std::uintptr_t{9});
    CHECK(effect->SetVoidParams(kDynamicParticleLinkIdParam, &id, 1) && particle->linkId == 9);
    CHECK(!effect->SetVoidParams(3, &id, 1));

    // Stop: the body back, the system released (the last user)
    effect->Stop(0);
    CHECK(!effect->IsStarted() && effect->Particle() == nullptr && cSC4DynamicParticleEffect::DynamicSystem() == nullptr);

    // the model can be handed over directly
    FakeModel other;
    void* model = &other;
    CHECK(effect->SetVoidParams(kDynamicParticleModelParam, &model, 1) && effect->Model() == &other && other.refs == 2);
    effect->Release();
    CHECK(other.refs == 1);
    std::puts(failures ? "sc4_dynamicparticleeffect: FAILED" : "sc4_dynamicparticleeffect: ok");
    return failures != 0;
}
