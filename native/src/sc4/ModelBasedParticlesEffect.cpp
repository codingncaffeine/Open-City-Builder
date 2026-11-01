#include "sc4/ModelBasedParticlesEffect.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "s3d/ModelInstance.h"
#include "s3d/S3DMath.h"
#include "sc4/VisualEffect.h"

namespace sc4 {

namespace {
IModelParticlesHost* g_host = nullptr;

constexpr float kTwoPi = 6.2831855f;            // 0x00a707e4
constexpr double kSizeUnit = 0.02;              // 0x00a70fb0
constexpr float kNoHeading = -999.0f;           // 0x00a70b0c
constexpr float kSmallestPhysicsTick = 1.0f / 15.0f;   // _kSmallestPhysicsTick

// CurveLookup: the curve sampled at t in [0, 1], linear between the samples (the base class's CurveValue rules).
float Curve(const std::vector<float>& curve, float t) {
    if (curve.empty()) return 0.0f;
    const int last = static_cast<int>(curve.size()) - 1;
    if (last == 0) return curve[0];
    const float f = static_cast<float>(last) * t;
    const int index = std::clamp(static_cast<int>(f), 0, last);
    const float frac = f - static_cast<float>(index);
    float value = curve[static_cast<std::size_t>(index)];
    if (frac > 0.0f) value += frac * (curve[static_cast<std::size_t>(std::min(index + 1, last))] - value);
    return value;
}
cS3DVector3 Curve(const std::vector<cS3DVector3>& curve, float t) {
    if (curve.empty()) return cS3DVector3{1.0f, 1.0f, 1.0f};
    const int last = static_cast<int>(curve.size()) - 1;
    if (last == 0) return curve[0];
    const float f = static_cast<float>(last) * t;
    const int index = std::clamp(static_cast<int>(f), 0, last);
    const float frac = f - static_cast<float>(index);
    cS3DVector3 value = curve[static_cast<std::size_t>(index)];
    if (frac > 0.0f) {
        const cS3DVector3& next = curve[static_cast<std::size_t>(std::min(index + 1, last))];
        value.x += frac * (next.x - value.x);
        value.y += frac * (next.y - value.y);
        value.z += frac * (next.z - value.z);
    }
    return value;
}

// The original rounds with the 0.5 / 1.5 * 2^23 trick: the nearest integer below x + 0.5, i.e. floor for the
// non-negative frame accumulators it is used on.
int RoundDown(float x) { return static_cast<int>(std::nearbyint(x - 0.5f)); }

// nS3D::TurnsFromMatrix / TurnsFromTransform (0x00795c26 / 0x00795922): atan2(-m[2][0], m[0][0]) in turns.
float TurnsFromRotation(const float r[3][3]) { return std::atan2(-r[2][0], r[0][0]) / kTwoPi; }

// cS3DMatrix3::RotateY / RotateZ: this = this * R.
void RotateY(float m[3][3], float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    for (int row = 0; row < 3; ++row) {
        const float x = m[row][0], z = m[row][2];
        m[row][0] = x * c - z * s;
        m[row][2] = x * s + z * c;
    }
}
void RotateZ(float m[3][3], float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    for (int row = 0; row < 3; ++row) {
        const float x = m[row][0], y = m[row][1];
        m[row][0] = x * c + y * s;
        m[row][1] = -x * s + y * c;
    }
}

// WigglePoint (0x00795b40): every wiggle (rate, spatial frequency vector, amplitude vector) adds
// sin(dot(p0, frequency) + t * rate) * amplitude, p0 being the point before any wiggle.
void WigglePoint(float t, const std::vector<float>& wiggles, cS3DVector3& p) {
    const cS3DVector3 p0 = p;
    for (std::size_t i = 0; i + 7 <= wiggles.size(); i += 7) {
        const float s = std::sin(p0.x * wiggles[i + 1] + p0.y * wiggles[i + 2] + p0.z * wiggles[i + 3] + t * wiggles[i]);
        p.x += s * wiggles[i + 4];
        p.y += s * wiggles[i + 5];
        p.z += s * wiggles[i + 6];
    }
}

cSC4MBParticle* MB(cSCParticle* particle) { return static_cast<cSC4MBParticle*>(cSC4ParticlesEffect::EffectsParticle(particle)); }
}  // namespace

IModelParticlesHost* ModelParticlesHost() { return g_host; }
void SetModelParticlesHost(IModelParticlesHost* host) { g_host = host; }

cSC4ModelBasedParticlesEffect::cSC4ModelBasedParticlesEffect() = default;

cSC4ModelBasedParticlesEffect::~cSC4ModelBasedParticlesEffect() {
    ReleaseModel();
    SetChainSystem(nullptr);
}

void cSC4ModelBasedParticlesEffect::ReleaseModel() {
    if (model_) model_->Release();
    if (modelResource_) modelResource_->Release();
    model_ = nullptr;
    modelResource_ = nullptr;
}

bool cSC4ModelBasedParticlesEffect::Flag11(int bit) const {
    const unsigned word = static_cast<unsigned>(description_->flag10) | (static_cast<unsigned>(description_->flag10High) << 8);
    return ((word >> bit) & 1u) != 0;
}

bool cSC4ModelBasedParticlesEffect::Init() {
    if (initialised_) return true;
    return cSC4ParticlesEffect::Init();
}

bool cSC4ModelBasedParticlesEffect::Shutdown() {
    if (!initialised_) return true;
    ReleaseModel();
    return cSC4ParticlesEffect::Shutdown();
}

// A system with a chain hands its particles over as they die; stopping it stops the chain once nothing is alive.
void cSC4ModelBasedParticlesEffect::Stop(int transition) {
    if (chain_ && liveCount_ == 0) chain_->Stop(transition);
    cSC4ParticlesEffect::Stop(transition);
}

bool cSC4ModelBasedParticlesEffect::SetVoidParams(int type, void** values, int count) {
    if (type == 0xd) {
        voidParamSet_ = true;
        voidParam_ = values ? values[0] : nullptr;
        return true;
    }
    return cSC4ParticlesEffect::SetVoidParams(type, values, count);
}

void cSC4ModelBasedParticlesEffect::SetChainSystem(cSC4ModelBasedParticlesEffect* chain) {
    if (chain == chain_) return;
    if (chain) chain->AddRef();
    if (chain_) chain_->Release();
    chain_ = chain;
}

// Only descriptions with the 11-bit flag 0 ("model based") are taken. The model is one of the description's model id
// list at random, else its single id; the slave / death effects are looked up by name once here.
void cSC4ModelBasedParticlesEffect::SetDescription(cSC4ParticlesDescription* description) {
    if (!description) return;
    const unsigned word = static_cast<unsigned>(description->flag10) | (static_cast<unsigned>(description->flag10High) << 8);
    if ((word & 1u) == 0) return;
    cSC4ParticlesEffect::SetDescription(description);
    ReleaseModel();
    modelExtent_ = 1.0f;
    SetChainSystem(nullptr);
    voidParamSet_ = false;
    voidParam_ = nullptr;
    const std::uint32_t modelId = description->textureId;
    if (modelId - 1u < 0xfffffffeu || !description->modelIds.empty()) {
        if (!description->modelIds.empty()) {
            const auto pick = Random().RandomUint32Uniform(static_cast<std::uint32_t>(description->modelIds.size()));
            SetModelId(description->modelIds[pick]);
        } else {
            SetModelId(modelId);
        }
        // Self-lit models (32-bit flag 0) get draw attribute 4; descriptions with the 11-bit flags 8 or 2 also run the
        // model processor (lighting / deforming the mesh), which is not ported.
        if (model_ && (description->flags & 1) != 0) {
            s3d::SC4DrawAttributes attributes;
            attributes.flags = 4;
            model_->SetDrawAttributes(attributes);
        }
    }
    if (g_host) {
        hasSlave_ = g_host->HasVisualEffect((description->name + "_slave").c_str());
        hasDeath_ = g_host->HasVisualEffect((description->name + "_death").c_str());
    }
}

// The model resource 0x5ad0e817 / 0xbadb57f1 / id; the instance's bounds give the largest extent.
void cSC4ModelBasedParticlesEffect::SetModelId(std::uint32_t modelId) {
    if (modelId == 0 || !g_host) return;
    s3d::cIS3DResModel* resource = nullptr;
    s3d::cIS3DModelInstance* instance = nullptr;
    if (!g_host->CreateModel(modelId, &resource, &instance) || !instance) {
        if (resource) resource->Release();
        return;
    }
    SetModelInstance(instance, resource);
    instance->Release();
    if (resource) resource->Release();
}

void cSC4ModelBasedParticlesEffect::SetModelInstance(s3d::cIS3DModelInstance* instance, s3d::cIS3DResModel* model) {
    if (instance) instance->AddRef();
    if (model) model->AddRef();
    ReleaseModel();
    model_ = instance;
    modelResource_ = model;
    if (!model_) return;
    cS3DBoundingBox box;
    model_->GetBounds(box);
    modelExtent_ = std::max({box.max[0] - box.min[0], box.max[1] - box.min[1], box.max[2] - box.min[2]});
}

cSCParticle* cSC4ModelBasedParticlesEffect::CreateParticle() {
    auto* particle = static_cast<cSC4MBParticle*>(std::malloc(sizeof(cSC4MBParticle)));
    if (!particle) return nullptr;
    std::memset(static_cast<void*>(particle), 0, sizeof(cSC4MBParticle));
    InitEffectsParticle(particle);
    if (particle->alive) {
        particle->frame = 0.0f;
        particle->roll = 0.0f;
        const std::uint8_t align = description_->billboardMode;
        particle->heading = (align < 2 || align > 4) ? TurnsFromRotation(effectTransform_.rotation) : kNoHeading;
        if (!description_->name.empty() && g_host) {
            particle->slave = g_host->CreateVisualEffect((description_->name + "_slave").c_str());
            if (particle->slave) {
                if (voidParamSet_) particle->slave->SetVoidParams(0xd, &voidParam_, 1);
                particle->slave->Start(startTransition_);
            }
        }
    }
    return SimulatorParticle(particle);
}

void cSC4ModelBasedParticlesEffect::DestroyParticle(cSCParticle* raw) {
    cSC4MBParticle* particle = MB(raw);
    if (!chain_ || particle->age < particle->lifetime) {
        if (particle->slave) {
            particle->slave->Stop(stopTransition_);
        }
        if (hasDeath_ && stopTransition_ == 0 && g_host) {
            if (cSC4VisualEffect* death = g_host->CreateVisualEffect((description_->name + "_death").c_str())) {
                cS3DTransform at;
                at.flags = nS3D::kTransformTranslation;
                at.changeCounter = 1;
                at.translation[0] = particle->position.x;
                at.translation[1] = particle->position.y;
                at.translation[2] = particle->position.z;
                cS3DTransform world = at;
                nS3D::MultiplyTransform(world, sourceTransform_);
                death->SetSourceTransform(world);
                if (voidParamSet_) death->SetVoidParams(0xd, &voidParam_, 1);
                death->Start(0);
                death->Release();
            }
        }
        if (particle->slave) particle->slave->Release();
        std::free(particle);
    } else {
        HandOffParticleToChainSystem(particle);
    }
    if (chain_ && liveCount_ == 0 && delayedStop_) chain_->Stop(0);
}

// A dying particle of a chained system lives on in the chain: new lifetime, the chain's slave effect and model, and the
// chain's particle setup when its description asks for it.
void cSC4ModelBasedParticlesEffect::HandOffParticleToChainSystem(cSC4MBParticle* particle) {
    cSC4ModelBasedParticlesEffect* chain = chain_;
    const cSC4ParticlesDescription* chainDescription = chain->description_;
    particle->age = 0.0f;
    particle->lifetime = static_cast<float>(Random().RandomDoubleRangeUniform(chainDescription->lifetimeMin, chainDescription->lifetimeMax));
    particle->alive = 1;
    if (!chainDescription->name.empty() && chainDescription->name != description_->name) {
        cS3DTransform effect, source;
        if (particle->slave) {
            particle->slave->GetEffectTransform(effect);
            particle->slave->GetSourceTransform(source);
            particle->slave->Stop(stopTransition_);
            particle->slave->Release();
            particle->slave = nullptr;
        }
        if (g_host) particle->slave = g_host->CreateVisualEffect((chainDescription->name + "_slave").c_str());
        if (particle->slave) {
            particle->slave->SetEffectTransform(effect);
            particle->slave->SetSourceTransform(source);
            if (voidParamSet_) particle->slave->SetVoidParams(0xd, &voidParam_, 1);
            particle->slave->Start(startTransition_);
        }
    }
    if (model_ && !chain->model_) chain->SetModelInstance(model_, modelResource_);
    if (!chain->IsStarted()) chain->Start(0);
    const unsigned word = static_cast<unsigned>(chainDescription->flag10) | (static_cast<unsigned>(chainDescription->flag10High) << 8);
    if ((word >> 9) & 1u) chain->InitEffectsParticle(particle);
    if ((chainDescription->flags & (1u << 23)) != 0) {   // the chain walks on in the particle's direction
        chain->walkDirection_ = particle->velocity;
        chain->walkDirection_.y = 0.0f;
        chain->walkTimer_ = 0.0f;
        chain->UpdateRandomWalk(0.0f);
    }
    chain->AddParticle(SimulatorParticle(particle));
}

// The particle's transform: heading (or aligned to its velocity, with turn smoothing and banking), spin, wiggle, then
// the system's source transform.
void cSC4ModelBasedParticlesEffect::ProcessModelParticle(cSC4MBParticle* particle, float fraction, float size,
                                                         cS3DVector3& position, cS3DTransform& out) {
    cS3DTransform local;
    local.scale = size;
    if (size != 1.0f) local.flags |= nS3D::kTransformScale;
    ++local.changeCounter;
    const std::uint8_t align = description_->billboardMode;
    if (align < 2 || align > 4) {
        RotateY(local.rotation, kTwoPi * particle->heading);
    } else {
        cS3DVector3 direction = particle->velocity;
        const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
        if (0.0f < length) {
            direction.x /= length;
            direction.y /= length;
            direction.z /= length;
        }
        nS3D::Matrix3 frame;
        if (align == 3) nS3D::AlignYToDir(direction, frame);
        else if (align == 4) nS3D::AlignZToDirWithYUp(direction, frame);
        else nS3D::AlignXToDirWithYUp(direction, frame);
        float rotation[3][3];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) rotation[r][c] = frame.m[3 * r + c];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {   // local.rotation (identity so far) * frame
                float sum = 0.0f;
                for (int k = 0; k < 3; ++k) sum += local.rotation[r][k] * rotation[k][c];
                local.rotation[r][c] = sum;
            }
        local.flags |= nS3D::kTransformRotation;
        if (0.0f < description_->values214[0]) {   // turn smoothing: ease the heading toward the walk direction
            const float target = TurnsFromRotation(rotation);
            float heading = particle->heading;
            float eased = target;
            if (heading != kNoHeading) {
                if (static_cast<double>(target) < static_cast<double>(heading) - 0.5) heading -= 1.0f;
                else if (static_cast<double>(heading) + 0.5 < static_cast<double>(target)) heading += 1.0f;
                eased = (kSmallestPhysicsTick / description_->values214[0]) * (target - heading) + heading;
            }
            RotateY(local.rotation, (eased - target) * kTwoPi);
            particle->heading = eased;
        }
        if (description_->values214[1] != 0.0f) {   // banking into the turn
            const cS3DVector3 side{-direction.z, 0.0f, direction.x};   // cross_y (0x0062d010)
            float turn = 0.0f;   // with the 11-bit flag 7 the original reads a vector at +0x210 nothing in the port writes
            if (!Flag11(7)) turn = position_.x * side.x + position_.y * side.y + position_.z * side.z;
            float roll = particle->roll - particle->roll * description_->values214[2] * deltaTime_;
            roll += turn * description_->values214[1] * deltaTime_;
            particle->roll = roll;
            RotateZ(local.rotation, roll);
        }
    }
    RotateY(local.rotation, particle->rotation * Curve(description_->spinCurve, fraction) * kTwoPi);
    local.flags |= nS3D::kTransformRotation;
    if (!description_->waveRows.empty() && !Flag11(8))
        WigglePoint(static_cast<float>(timeAccumulator_), description_->waveRows, position);
    local.translation[0] = position.x;
    local.translation[1] = position.y;
    local.translation[2] = position.z;
    local.flags |= nS3D::kTransformTranslation;
    out = local;                                  // cS3DTransform::operator*: the particle's transform times the source
    nS3D::MultiplyTransform(out, sourceTransform_);
    if (box_.min[0] > box_.max[0]) {
        for (int k = 0; k < 3; ++k) box_.min[k] = box_.max[k] = out.translation[k];
    } else {
        for (int k = 0; k < 3; ++k) {
            box_.min[k] = std::min(box_.min[k], out.translation[k]);
            box_.max[k] = std::max(box_.max[k], out.translation[k]);
        }
    }
}

// The update without a draw (culled systems): the bounds, and every running slave effect moved with its particle.
void cSC4ModelBasedParticlesEffect::UpdateActiveParticles() {
    box_ = cS3DBoundingBox{{1, 1, 1}, {0, 0, 0}};
    for (cSCParticle* raw : particles_) {
        cSC4MBParticle* particle = MB(raw);
        cS3DVector3 position = particle->position;
        if (!particle->slave || !particle->slave->IsStarted()) {
            cS3DVector3 world;
            nS3D::TransformPoints(sourceTransform_, &position, &world, 1);
            if (box_.min[0] > box_.max[0]) {
                for (int k = 0; k < 3; ++k) box_.min[k] = box_.max[k] = (&world.x)[k];
            } else {
                for (int k = 0; k < 3; ++k) {
                    box_.min[k] = std::min(box_.min[k], (&world.x)[k]);
                    box_.max[k] = std::max(box_.max[k], (&world.x)[k]);
                }
            }
            continue;
        }
        const float fraction = particle->age / particle->lifetime;
        const float size = static_cast<float>(static_cast<double>(particle->size) * kSizeUnit * Curve(description_->sizeCurve, fraction));
        cS3DTransform transform;
        ProcessModelParticle(particle, fraction, size, position, transform);
        if (Flag11(4)) {
            float alpha = particle->alphaBias * Curve(description_->curveC8, fraction) * attractorScale_;
            float* values = &alpha;
            particle->slave->SetFloatParams(3, values, 1);
        }
        if (!Flag11(3)) {
            particle->slave->SetSourceTransform(transform);
        } else {
            cS3DTransform at;
            at.flags = nS3D::kTransformTranslation;
            at.changeCounter = 1;
            for (int k = 0; k < 3; ++k) at.translation[k] = transform.translation[k];
            particle->slave->SetEffectTransform(at);
        }
    }
    if (!(box_.min[0] > box_.max[0]) && !description_->sizeCurve.empty()) {
        const float grow = static_cast<float>(static_cast<double>(modelExtent_ * description_->sizeCurve[0]) * kSizeUnit);
        for (int k = 0; k < 3; ++k) {
            box_.min[k] -= grow;
            box_.max[k] += grow;
        }
    }
}

void cSC4ModelBasedParticlesEffect::RenderActiveParticles(IParticlesDrawContext* context) {
    SC4DrawContext* draw = context ? context->DrawContext() : nullptr;
    box_ = cS3DBoundingBox{{1, 1, 1}, {0, 0, 0}};
    for (cSCParticle* raw : particles_) DrawParticle(MB(raw), draw);
    if (context) context->ResetShade();
    if (!(box_.min[0] > box_.max[0]) && !description_->sizeCurve.empty()) {
        const float grow = static_cast<float>(static_cast<double>(modelExtent_ * description_->sizeCurve[0]) * kSizeUnit);
        for (int k = 0; k < 3; ++k) {
            box_.min[k] -= grow;
            box_.max[k] += grow;
        }
    }
}

void cSC4ModelBasedParticlesEffect::DrawParticle(cSC4MBParticle* particle, SC4DrawContext* draw) {
    cS3DVector3 position = particle->position;
    if (description_->spinAroundY != 0.0f) {   // turn the particle about the effect's vertical axis by its height
        const float dx = position.x - effectTransform_.translation[0];
        const float dy = position.y - effectTransform_.translation[1];
        const float dz = position.z - effectTransform_.translation[2];
        const float c = std::cos(dy * description_->spinAroundY), s = std::sin(dy * description_->spinAroundY);
        position.x = (c * dx - s * dz) + position.x;
        position.z = s * dx + c * dz + position.z;
    }
    const float fraction = particle->age / particle->lifetime;
    const float size = static_cast<float>(static_cast<double>(particle->size) * kSizeUnit * Curve(description_->sizeCurve, fraction));
    // (11-bit flag 1, ApplyFakePerspective, is not ported: those particles draw here unchanged.)
    cS3DTransform transform;
    ProcessModelParticle(particle, fraction, size, position, transform);
    const cS3DVector3 colour = Curve(description_->colorCurve, fraction);
    const float red = particle->colourR * colour.x;
    const float green = particle->colourG * colour.y;
    const float blue = particle->colourB * colour.z;
    float alpha = particle->alphaBias * Curve(description_->curveC8, fraction) * attractorScale_;
    if (model_) {
        model_->SetTransform(transform);
        s3d::SC4DrawAttributes attributes = model_->GetDrawAttributes();
        if ((description_->flags & 1) != 0) attributes.flags |= 4;
        attributes.shade[3] = alpha;
        if (alpha != 1.0f || description_->curveC8.size() > 1) attributes.flags |= 2;
        else attributes.flags &= ~2u;
        const int frames = modelResource_ ? modelResource_->FrameCount() : 1;
        if (frames >= 2) {   // the animation: the model's rate, or the particle's speed (11-bit flag 6)
            float step = deltaTime_;
            if (0.0f < description_->values278[1]) step *= description_->values278[1];
            float rate;
            if (!Flag11(6)) {
                const int msPerFrame = modelResource_->FrameRate();
                rate = msPerFrame > 0 ? 1000.0f / static_cast<float>(msPerFrame) : 0.0f;
            } else {
                const cS3DVector3& v = particle->velocity;
                rate = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            }
            particle->frame += rate * step;
            float f = particle->frame;
            int frame;
            if (!Flag11(5)) {   // loop
                if (static_cast<float>(frames) <= f) {
                    f -= static_cast<float>(RoundDown(f / static_cast<float>(frames)) * frames);
                    particle->frame = f;
                }
                frame = RoundDown(f);
            } else {            // play once and hold the last frame
                frame = f < static_cast<float>(frames) ? RoundDown(f) : frames - 1;
            }
            modelResource_->SelectFrame(model_, std::clamp(frame, 0, frames - 1));
        }
        attributes.shade[0] = red;
        attributes.shade[1] = green;
        attributes.shade[2] = blue;
        model_->SetDrawAttributes(attributes);
        if (draw) model_->Draw(draw, false);
        static int traced = 0;
        if (std::getenv("SC4_FXDEBUG") && traced++ % 40 == 0)
            std::fprintf(stderr, "fx: draw %p at %.1f %.1f %.1f scale %.3f frame %.1f/%d alpha %.2f draw %p\n", static_cast<void*>(this), transform.translation[0],
                         transform.translation[1], transform.translation[2], transform.scale, particle->frame, frames, alpha, static_cast<void*>(draw));
    }
    if (particle->slave && particle->slave->IsStarted()) {
        if (Flag11(4)) {
            float* values = &alpha;
            particle->slave->SetFloatParams(3, values, 1);
        }
        if (!Flag11(3)) {
            particle->slave->SetSourceTransform(transform);
        } else {
            cS3DTransform at;
            at.flags = nS3D::kTransformTranslation;
            at.changeCounter = 1;
            for (int k = 0; k < 3; ++k) at.translation[k] = transform.translation[k];
            particle->slave->SetEffectTransform(at);
        }
    }
}

}  // namespace sc4
