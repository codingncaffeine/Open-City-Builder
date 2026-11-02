// cSC4DecalEffect — the decal component of a visual effect (i386 0x00412620..0x004133ac, 0x124 bytes; Windows main
// table 0x00A9EE78). It owns one decal in one of the terrain view's overlay managers: Start adds it (a plain decal of the
// description's FSH texture, a texture handed over through void parameter 6, or a ring decal), Update advances the
// normalized phase through the description's curves and pushes placement, opacity and colour, Stop removes it. The
// placement comes from the effect transforms (SetTransforms) or an explicit cell rectangle (float parameter 12).
//
// Original layout (i386): +0x04 refcount, +0x08 started, +0x0c cSC4DecalDescription (flags +0x0c, texture +0x10,
// layer +0x14, lifetime +0x15, life +0x18, curves rotation +0x1c / size +0x28 / opacity +0x34 / colour +0x40 /
// aspect +0x4c, variances +0x58..+0x60, mapping +0x64, offset +0x68/+0x6c), +0x70 decal handle (-1 none),
// +0x74/+0xac two transforms, +0xe4/+0xe8 x/z, +0xec size, +0xf0 angle (turns), +0xf4/+0xf8/+0xfc random opacity /
// size / rotation factors, +0x100 mapping factor, +0x104 aspect, +0x108 external opacity, +0x10c external size,
// +0x110 explicit placement, +0x114 elapsed, +0x118 1/life, +0x11c texture override, +0x120 overlay manager.
//
// The overlay manager comes from SL::TerrainView() vslot 0x48 (index 0 dynamic land, 1 dynamic water, 2 land, 3 water)
// and the whole-city size from SL::spCity vslots 0x164 / 0x168; both go through IDecalEffectHost.
#pragma once
#include <cstdint>

#include "sc4/EffectComponents.h"
#include "sc4/EffectDescriptions.h"

namespace sc4 {

class cSTEOverlayManager;
class IOverlayTexture;

class IDecalEffectHost {
public:
    virtual ~IDecalEffectHost() = default;
    virtual bool HasTerrainView() = 0;                         // SL::TerrainView() != null
    virtual cSTEOverlayManager* OverlayManager(int index) = 0;  // terrain view vslot 0x48 (borrowed; the effect AddRefs it)
    virtual float CityWorldSizeX() = 0;                         // spCity vslot 0x164
    virtual float CityWorldSizeZ() = 0;                         // spCity vslot 0x168
};
IDecalEffectHost* DecalEffectHost();
void SetDecalEffectHost(IDecalEffectHost* host);

class cSC4DecalEffect final : public cISC4VisualEffectComponent {
public:
    cSC4DecalEffect() = default;                                                  // 0x004133ac
    ~cSC4DecalEffect() override;                                                  // 0x004130e6

    void Init();                                                                  // 0x00412690
    void Shutdown();                                                              // 0x00412620
    void SetDescription(const cSC4DecalDescription& description);                // 0x0041318e

    void Start(int transition) override;                                          // 0x00412b26
    void Stop(int transition) override;                                           // 0x004126b6
    bool IsStarted() const override { return started_; }                          // 0x00412654
    void SetTransforms(const cS3DTransform& local, const cS3DTransform& world) override;   // 0x00412f4a
    void ApplyEffect(float) override {}                                           // 0x00795ec2 (empty)
    bool SetFloatParams(int type, const float* values, int count) override;      // 0x0041270a
    bool SetVoidParams(int type, void** values, int count) override;             // 0x00412660
    void Update(float seconds);                                                   // 0x004127fa (the manager's decal pass)

    std::uint32_t RefCount() const { return refs_; }
    std::uint32_t Handle() const { return handle_; }
    cSTEOverlayManager* Overlays() const { return overlays_; }

private:
    void SendPlacement(float phase);   // the shared cDecalInfo update of Update and SetTransforms

    bool started_ = false;                       // +0x08
    cSC4DecalDescription description_;           // +0x0c
    std::uint32_t handle_ = 0xffffffffu;         // +0x70
    float x_ = 0.0f, z_ = 0.0f;                  // +0xe4 / +0xe8
    float size_ = 1.0f;                          // +0xec
    float angle_ = 0.0f;                         // +0xf0 turns
    float randomOpacity_ = 1.0f;                 // +0xf4
    float randomSize_ = 1.0f;                    // +0xf8
    float randomRotation_ = 1.0f;                // +0xfc
    float mappingScale_ = 1.0f;                  // +0x100
    float aspect_ = 1.0f;                        // +0x104
    float externalOpacity_ = 1.0f;               // +0x108
    float externalSize_ = 1.0f;                  // +0x10c
    bool explicitPlacement_ = false;             // +0x110
    float elapsed_ = 0.0f;                       // +0x114
    float inverseLife_ = 1.0f;                   // +0x118
    IOverlayTexture* texture_ = nullptr;         // +0x11c (one reference)
    cSTEOverlayManager* overlays_ = nullptr;     // +0x120 (one reference)
};

// CurveLookup (i386 0x0078f298 / 0x0078ed4e): uniformly spaced keys over [0, 1]; a single key is constant.
float DecalCurve(const std::vector<float>& curve, float phase);
cS3DVector3 DecalCurve(const std::vector<cS3DVector3>& curve, float phase);

}  // namespace sc4
