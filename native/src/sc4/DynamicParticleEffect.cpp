// cSC4DynamicParticleEffect — see DynamicParticleEffect.h. Hand port of the Mac i386 functions.
#include "sc4/DynamicParticleEffect.h"

#include <cmath>
#include <ctime>

#include "rz/cRZDate.h"
#include "rz/cRZRandom.h"
#include "s3d/ModelInstance.h"
#include "s3d/S3DMath.h"
#include "sc4/DynamicParticleSystem.h"
#include "sc4/ModelBasedParticlesEffect.h"
#include "sc4/VisualEffect.h"

namespace sc4 {

namespace {

cSC4DynamicParticleSystem* g_system = nullptr;   // cSC4DynamicParticleEffect::spDynamicSystem

constexpr float kCollideSpeed = 0.01f;           // cSC4Tank::sfBarrelVelocityScalingFactor (0x00a706d0)
constexpr float kMasslessInverse = 1000000.0f;   // 0x49742400
constexpr const char* kDefaultSlave = "default_dynparticle_slave";

// The file's generator (0x00c6cd40), seeded at load with cRZTime()'s first word (today's date as a julian day, local
// time) ^ 0xac09c22b.
cRZRandom& FileRandom() {
    static cRZRandom random([] {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        const cRZDate today(static_cast<std::uint32_t>(local.tm_mon + 1), static_cast<std::uint32_t>(local.tm_mday),
                            static_cast<std::uint32_t>(local.tm_year + 1900));
        return today.JulianDay() ^ 0xac09c22bu;
    }());
    return random;
}

// A visual effect as a component of another (the slave's AddComponentEffect takes the effect itself in the original).
class AttachedEffect final : public cISC4VisualEffectComponent {
public:
    explicit AttachedEffect(cSC4VisualEffect* effect) : effect_(effect) { effect_->AddRef(); }
    ~AttachedEffect() override { effect_->Release(); }
    void Start(int transition) override { effect_->Start(transition); }
    void Stop(int transition) override { effect_->Stop(transition); }
    bool IsStarted() const override { return effect_->IsStarted(); }
    void SetTransforms(const cS3DTransform& source, const cS3DTransform& effect) override { effect_->SetTransforms(source, effect); }
    void ApplyEffect(float elapsed) override { effect_->ApplyEffect(elapsed); }
    bool SetFloatParams(int type, const float* values, int count) override {
        return effect_->SetFloatParams(static_cast<std::uint32_t>(type), values, count);
    }
    bool SetVoidParams(int type, void** values, int count) override {
        return effect_->SetVoidParams(static_cast<std::uint32_t>(type), values, count);
    }

private:
    cSC4VisualEffect* effect_;
};

void SetColumn(cS3DTransform& t, int column, const cS3DVector3& v) {
    t.rotation[0][column] = v.x;
    t.rotation[1][column] = v.y;
    t.rotation[2][column] = v.z;
}

}  // namespace

cSC4DynamicParticleSystem* cSC4DynamicParticleEffect::DynamicSystem() { return g_system; }

// 0x004ba4aa: the system made on first use (one reference), then one more Init count.
cSC4DynamicParticleSystem* cSC4DynamicParticleEffect::InitDynamicParticleSystem() {
    if (!g_system) {
        g_system = new cSC4DynamicParticleSystem();
        g_system->AddRef();
    }
    g_system->Init();
    return g_system;
}

// 0x004b8c56: one Init count less; the last one releases the system.
void cSC4DynamicParticleEffect::ShutdownDynamicParticleSystem() {
    if (!g_system) return;   // port guard: the original calls Shutdown on the pointer unchecked
    if (g_system->Shutdown()) {
        g_system->Release();
        g_system = nullptr;
    }
}

// 0x004bab5e: the references go (a started effect is stopped by its owner first).
cSC4DynamicParticleEffect::~cSC4DynamicParticleEffect() {
    if (slave_) slave_->Release();
    if (model_) model_->Release();
}

// 0x004b8dfc.
void cSC4DynamicParticleEffect::SetTransforms(const cS3DTransform& source, const cS3DTransform& local) {
    source_ = source;
    local_ = local;
}

// 0x004b8cc2: only the start velocity.
bool cSC4DynamicParticleEffect::SetFloatParams(int type, const float* values, int count) {
    (void)count;
    if (type != kDynamicParticleVelocityParam) return false;
    velocity_ = cS3DVector3{values[0], values[1], values[2]};
    return true;
}

// 0x004b909c.
bool cSC4DynamicParticleEffect::SetVoidParams(int type, void** values, int count) {
    (void)count;
    if (type == kDynamicParticleLinkIdParam) {
        if (particle_) particle_->linkId = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(values[0]));   // port guard
    } else if (type == kDynamicParticleModelParam) {
        SetModel(static_cast<s3d::cIS3DModelInstance*>(values[0]));
    } else if (type == kDynamicParticleLinkParam) {
        if (g_system && particle_)   // port guard: the original reads both unchecked
            g_system->LinkParticles(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(values[0])), particle_->linkId);
    } else if (type == kDynamicParticleAttachParam) {
        AttachSlaveEffect(static_cast<cSC4VisualEffect*>(values[0]), static_cast<const cS3DTransform*>(values[1]));
    } else {
        return false;
    }
    return true;
}

// 0x004b8cf6.
void cSC4DynamicParticleEffect::SetModel(s3d::cIS3DModelInstance* model) {
    if (model) model->AddRef();
    if (model_) model_->Release();
    model_ = model;
}

// 0x004b8d22: an instance of the S3D model resource 0x5ad0e817 / 0xbadb57f1 / id (the old model goes once the resource
// is there).
void cSC4DynamicParticleEffect::SetModel(std::uint32_t modelId) {
    if (modelId == 0) return;
    IModelParticlesHost* host = ModelParticlesHost();
    if (!host) return;
    s3d::cIS3DResModel* resource = nullptr;
    s3d::cIS3DModelInstance* instance = nullptr;
    if (!host->CreateModel(modelId, &resource, &instance)) {
        if (resource) resource->Release();
        if (instance) instance->Release();
        return;
    }
    if (model_) model_->Release();
    model_ = instance;   // the host's reference
    if (resource) resource->Release();
}

// 0x004b91a8: the description kept; the model is its id (when it is neither 0 nor 0xffffffff) or one of its list,
// picked with the file's generator; whether a "<name>_collide" effect exists is noted.
void cSC4DynamicParticleEffect::SetDescription(const cSC4DynamicParticleDescription& description) {
    description_ = description;
    hasDescription_ = true;
    if (description.modelId - 1u < 0xfffffffeu || !description.modelIds.empty()) {
        if (!description.modelIds.empty()) {
            const std::uint32_t pick = FileRandom().RandomUint32Uniform(static_cast<std::uint32_t>(description.modelIds.size()));
            SetModel(description.modelIds[pick]);
        } else {
            SetModel(description.modelId);
        }
    }
    IModelParticlesHost* host = ModelParticlesHost();
    hasCollide_ = host && host->HasVisualEffect((description_.name + "_collide").c_str());
}

// 0x004ba6b0: with a model and not yet started, the system's body: at the effect's place, turned as its source
// transform's x axis (-atan2(r00, r02)), with the start velocity, a box of the model's bounds less half a unit across
// (the full height), the description's mass (1e6 inverse without one) and frictions; then the "<name>_slave" effect.
void cSC4DynamicParticleEffect::Start(int transition) {
    if (!model_) {
        started_ = false;
        return;
    }
    if (started_) return;
    started_ = true;
    cSC4DynamicParticleSystem* system = InitDynamicParticleSystem();
    cS3DBoundingBox box;
    model_->GetBounds(box);
    const float sizeY = box.max[1] - box.min[1];
    const float sizeX = (box.max[0] - box.min[0]) - 0.5f;
    const float sizeZ = (box.max[2] - box.min[2]) - 0.5f;
    cSC4RigidBodySimulator2D::ObjectDesc desc;
    desc.x = source_.translation[0];
    desc.y = source_.translation[2];
    desc.angle = -static_cast<float>(std::atan2(static_cast<double>(source_.rotation[0][0]), static_cast<double>(source_.rotation[0][2])));
    desc.vx = velocity_.x;
    desc.vy = velocity_.z;
    desc.halfX = sizeX * 0.5f;
    desc.halfY = sizeZ * 0.5f;
    desc.halfHeight = 0.5f * sizeY;
    desc.vh = velocity_.y;
    desc.height = desc.halfHeight + source_.translation[1];
    desc.angularVelocity = 0.0f;
    desc.inverseMass = description_.values[0] > 0.0f ? 1.0f / description_.values[0] : kMasslessInverse;
    desc.forwardFriction = description_.values[2];
    desc.sideFriction = description_.values[3];
    desc.ax = 0.0f;
    desc.ay = 0.0f;
    desc.angularFriction = description_.values[4];
    desc.friction = 0.0f < description_.values[2] || 0.0f < description_.values[3] || 0.0f < description_.values[4];
    desc.contacts = 0;
    particle_ = system->AddParticle(desc, model_);
    if (!description_.name.empty()) {
        if (slave_) {
            slave_->Release();
            slave_ = nullptr;
        }
        IModelParticlesHost* host = ModelParticlesHost();
        slave_ = host ? host->CreateVisualEffect((description_.name + "_slave").c_str()) : nullptr;
        if (slave_) {
            slave_->Start(transition);
            if (slave_->IsStarted()) slave_->SetSourceTransform(particle_->transform);
        }
    }
}

// 0x004baa50: the slave stopped and dropped, the body given back, one system count less.
void cSC4DynamicParticleEffect::Stop(int transition) {
    if (!started_) return;
    started_ = false;
    if (slave_) {
        slave_->Stop(transition);
        slave_->Release();
        slave_ = nullptr;
    }
    if (particle_) {
        if (g_system) g_system->DeleteParticle(particle_);
        particle_ = nullptr;
    }
    ShutdownDynamicParticleSystem();
}

// 0x004b9fc0: a body that left the city stops the effect; otherwise the slave follows the model, and a hit while the
// body moves faster than 0.01 starts "<name>_collide" at the contact point, facing along the contact normal.
void cSC4DynamicParticleEffect::ApplyEffect(float elapsed) {
    (void)elapsed;
    if (!particle_) return;   // port guard: the original reads the particle unchecked (it is called only when started)
    cSC4RigidBodySimulator2D::Object* body = particle_->object;
    if (!body) {
        Stop(0);
        return;
    }
    if (slave_ && slave_->IsStarted()) slave_->SetSourceTransform(particle_->transform);
    if (body->contacts == 0 || !hasCollide_ || !(kCollideSpeed < std::sqrt(body->vx * body->vx + body->vy * body->vy))) return;
    body->contacts = 0;
    IModelParticlesHost* host = ModelParticlesHost();
    cSC4VisualEffect* collide = host ? host->CreateVisualEffect((description_.name + "_collide").c_str()) : nullptr;
    if (!collide) return;
    cS3DVector3 direction{body->normal.x, 0.0f, body->normal.y};
    nS3D::Normalize(direction);
    const cS3DVector3 up{0.0f, 1.0f, 0.0f};
    cS3DTransform t;
    SetColumn(t, 0, direction);
    SetColumn(t, 1, up);
    SetColumn(t, 2, ::cross(direction, up));
    t.translation[0] = body->contact.x;
    t.translation[1] = body->contact.y;
    t.translation[2] = body->contact.z;
    t.flags |= 1;
    ++t.changeCounter;
    t.flags |= 4;
    ++t.changeCounter;
    collide->SetSourceTransform(t);
    collide->Start(0);
    collide->Release();
}

// 0x004b8fce: the effect becomes a component of the slave (the "default_dynparticle_slave" effect, made and started
// when there is none yet) at the given transform.
void cSC4DynamicParticleEffect::AttachSlaveEffect(cSC4VisualEffect* effect, const cS3DTransform* transform) {
    if (!slave_) {
        IModelParticlesHost* host = ModelParticlesHost();
        slave_ = host ? host->CreateVisualEffect(kDefaultSlave) : nullptr;
        if (slave_) slave_->Start(0);
    }
    if (!slave_ || !effect) return;
    auto* component = new AttachedEffect(effect);
    component->AddRef();
    slave_->AddComponentEffect(component, transform ? *transform : cS3DTransform{});
    component->Release();
}

}  // namespace sc4
