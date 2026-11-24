// cSC4DynamicParticleEffect (Mac i386 0x004b8c56..0x004bab5e, 0xa0 bytes) — the "dynamic particle" component of a
// visual effect (component kind 16): a prop model that, once started, becomes a body of the dynamic particle system
// (sc4/DynamicParticleSystem.h) and is knocked about by the driven vehicle. Its description names the model (one id or a
// list to pick from), the mass and the three frictions; "<name>_slave" is an effect that follows the prop and
// "<name>_collide" one started where it hits something while moving.
//
// The process-wide system (spDynamicSystem) is made on first use and counted by Init / Shutdown: every started effect and
// every player-driven vehicle holds one count (InitDynamicParticleSystem / ShutdownDynamicParticleSystem).
#pragma once
#include <cstdint>

#include "gz/StreamOps.h"
#include "sc4/EffectComponents.h"
#include "sc4/EffectDescriptions.h"

namespace s3d { class cIS3DModelInstance; }
struct cSC4GlobalEffectParams;   // sc4/ParticlesEffect.h

namespace sc4 {

class cSC4DynamicParticleSystem;
struct cSC4DynamicParticle;
class cSC4VisualEffect;

constexpr int kDynamicParticleVelocityParam = 0xf;    // SetFloatParams: the start velocity (x, y, z)
constexpr int kDynamicParticleModelParam = 7;         // SetVoidParams: a model instance
constexpr int kDynamicParticleLinkIdParam = 0x10;     // SetVoidParams: the particle's link id
constexpr int kDynamicParticleLinkParam = 0x11;       // SetVoidParams: link to the particle with this id
constexpr int kDynamicParticleAttachParam = 0x12;     // SetVoidParams: (effect, transform) attached to the slave effect

class cSC4DynamicParticleEffect final : public cISC4VisualEffectComponent {
public:
    cSC4DynamicParticleEffect() = default;            // @ 0x004baada
    ~cSC4DynamicParticleEffect() override;            // @ 0x004bab5e

    void Start(int transition) override;              // @ 0x004ba6b0
    void Stop(int transition) override;               // @ 0x004baa50
    bool IsStarted() const override { return started_; }   // @ 0x004b8cb2
    void SetTransforms(const cS3DTransform& source, const cS3DTransform& local) override;   // @ 0x004b8dfc
    void ApplyEffect(float elapsed) override;         // @ 0x004b9fc0
    bool SetFloatParams(int type, const float* values, int count) override;   // @ 0x004b8cc2
    bool SetVoidParams(int type, void** values, int count) override;          // @ 0x004b909c

    void Init() {}                                    // @ 0x004b8c86
    void Shutdown() {}                                // @ 0x004b8c90
    void SetModel(s3d::cIS3DModelInstance* model);    // @ 0x004b8cf6
    void SetModel(std::uint32_t modelId);             // @ 0x004b8d22
    void SetTarget(const cS3DVector3&) {}             // @ 0x004b8d16
    void SetGlobalParams(const cSC4GlobalEffectParams*) {}   // @ 0x004b8d1c
    void SetDescription(const cSC4DynamicParticleDescription& description);   // @ 0x004b91a8
    void AttachSlaveEffect(cSC4VisualEffect* effect, const cS3DTransform* transform);   // @ 0x004b8fce
    std::uint32_t RefCount() const { return refs_; }

    static cSC4DynamicParticleSystem* InitDynamicParticleSystem();   // @ 0x004ba4aa
    static void ShutdownDynamicParticleSystem();                     // @ 0x004b8c56
    static cSC4DynamicParticleSystem* DynamicSystem();               // spDynamicSystem

    // accessors for the tests
    cSC4DynamicParticle* Particle() const { return particle_; }
    s3d::cIS3DModelInstance* Model() const { return model_; }
    cSC4VisualEffect* Slave() const { return slave_; }
    bool HasCollideEffect() const { return hasCollide_; }
    const cS3DVector3& Velocity() const { return velocity_; }

private:
    cS3DTransform source_;                            // +0x0c
    cS3DTransform local_;                             // +0x44
    cS3DVector3 velocity_{0.0f, 0.0f, 0.0f};          // +0x7c
    s3d::cIS3DModelInstance* model_ = nullptr;        // +0x8c (counted)
    cSC4DynamicParticle* particle_ = nullptr;         // +0x90
    cSC4DynamicParticleDescription description_;      // +0x94 (the original keeps a counted pointer)
    bool hasDescription_ = false;
    cSC4VisualEffect* slave_ = nullptr;               // +0x98 (counted)
    bool started_ = false;                            // +0x9c
    bool hasCollide_ = false;                         // +0x9d
};

}  // namespace sc4
