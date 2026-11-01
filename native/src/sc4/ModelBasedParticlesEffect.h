// cSC4ModelBasedParticlesEffect — a particle system whose particles are drawn as one 3D model each: the god-mode animal
// herds (cows, horses, deer, moose, the safari herd), birds, boats and similar. It is a cSC4ParticlesEffect (the particle
// simulation, walk and terrain forces are the base class's) that keeps ONE model instance and, per particle, turns it to
// the particle's heading, sets its colour / alpha and animation frame and draws it.
//
// Recovered from the Mac i386 build (symbols, 18 functions at 0x0040feb4..0x00412526): Init, Shutdown, SetVoidParams,
// SetChainSystem, Stop, HandOffParticleToChainSystem, ProcessModelParticle, UpdateActiveParticles, RenderActiveParticles,
// SetModel(model id), SetModel(instance), SetDescription, CreateParticle, DestroyParticle and the constructor.
// Original layout past the base (i386): +0x214 model instance, +0x218 its animated view, +0x21c model processor,
// +0x25c description kept for the processor, +0x260 processor ready, +0x261 a "<name>_slave" effect exists, +0x262 a
// "<name>_death" effect exists, +0x263 void parameter 0xd set, +0x264 its value, +0x268 the model's largest extent,
// +0x26c the chained system a dying particle is handed to.
// A particle (cSC4MBParticle) is the base particle followed by its slave effect (+0x44), the animation frame accumulator
// (+0x48), the roll (+0x4c) and the heading in turns (+0x50, -999 = "take it from the walk").
//
// Not ported (each named where it would run): the cSC4EffectsModelProcessor path (lighting / deforming the mesh for
// descriptions with 11-bit flags 2 or 8) and ApplyFakePerspective (flag 1: birds pulled away from the camera, with the
// "camera_collision" effect); those particles draw through the plain path.
#pragma once
#include <cstdint>

#include "sc4/ParticlesEffect.h"

namespace s3d { class cIS3DModelInstance; class cIS3DResModel; }

namespace sc4 {

class cSC4VisualEffect;

// The neighbours the class reaches: the resource manager (SL::spRM, the S3D model resource 0x5ad0e817 / 0xbadb57f1) and
// the effects manager (SL::spEffectsManager vslots 0x14 HasVisualEffect / 0x18 CreateVisualEffect).
class IModelParticlesHost {
public:
    virtual ~IModelParticlesHost() = default;
    // The model resource of `modelId` and one instance created from it; both returned with one reference.
    virtual bool CreateModel(std::uint32_t modelId, s3d::cIS3DResModel** model, s3d::cIS3DModelInstance** instance) = 0;
    virtual bool HasVisualEffect(const char* name) = 0;
    virtual cSC4VisualEffect* CreateVisualEffect(const char* name) = 0;   // one reference, null when unknown
};
IModelParticlesHost* ModelParticlesHost();
void SetModelParticlesHost(IModelParticlesHost* host);

// A particle of a model-based system.
struct cSC4MBParticle : cSC4EffectsParticle {
    cSC4VisualEffect* slave = nullptr;   // +0x44 "<name>_slave" (the shadow decal), referenced
    float frame = 0;                     // +0x48 animation frame accumulator
    float roll = 0;                      // +0x4c
    float heading = 0;                   // +0x50 turns
};

class cSC4ModelBasedParticlesEffect final : public cSC4ParticlesEffect {
public:
    cSC4ModelBasedParticlesEffect();                                        // @ 0x0041245c
    ~cSC4ModelBasedParticlesEffect() override;

    bool Init() override;                                                   // @ 0x0040feb4
    bool Shutdown() override;                                               // @ 0x0040fed8
    void Stop(int transition) override;                                     // @ 0x0040ff9c
    bool SetVoidParams(int type, void** values, int count) override;        // @ 0x0040ff28
    void SetChainSystem(cSC4ModelBasedParticlesEffect* chain);              // @ 0x0040ff74
    void SetDescription(cSC4ParticlesDescription* description);             // @ 0x00411d1a
    void SetModelId(std::uint32_t modelId);                                 // @ 0x00411a88 SetModel(unsigned long)
    void SetModelInstance(s3d::cIS3DModelInstance* instance, s3d::cIS3DResModel* model);   // @ 0x00412526

    cSCParticle* CreateParticle() override;                                 // @ 0x0041204a
    void DestroyParticle(cSCParticle* particle) override;                   // @ 0x004121de
    void RenderActiveParticles(IParticlesDrawContext* context) override;    // @ 0x00411196
    void UpdateActiveParticles() override;                                  // @ 0x00410aa6

    s3d::cIS3DModelInstance* ModelInstance() const { return model_; }
    float ModelExtent() const { return modelExtent_; }

private:
    void HandOffParticleToChainSystem(cSC4MBParticle* particle);            // @ 0x004102b6
    void ProcessModelParticle(cSC4MBParticle* particle, float fraction, float size, cS3DVector3& position,
                              cS3DTransform& out);                          // @ 0x004105fc
    void DrawParticle(cSC4MBParticle* particle, SC4DrawContext* context);   // one turn of RenderActiveParticles' loop
    void ReleaseModel();
    bool Flag11(int bit) const;

    s3d::cIS3DModelInstance* model_ = nullptr;         // +0x214
    s3d::cIS3DResModel* modelResource_ = nullptr;      // the frames of +0x218 (cIS3DModelInstanceAnimated)
    bool hasSlave_ = false;                            // +0x261
    bool hasDeath_ = false;                            // +0x262
    bool voidParamSet_ = false;                        // +0x263
    void* voidParam_ = nullptr;                        // +0x264
    float modelExtent_ = 0;                            // +0x268
    cSC4ModelBasedParticlesEffect* chain_ = nullptr;   // +0x26c (referenced)
};

}  // namespace sc4
