// cSC4ModelRegEffects — the visual effects that ride on a model (Mac i386 0x0016055c..0x00161250, 0x20 bytes): one effect per
// registration point of the model whose name begins "fx_" (the effect is the rest of the name, after a leading "<digits>_"),
// plus the effects added by name (riot levels, player-drive keys). Each effect follows its point through the model's animation
// frames: every update gives it the model's transform times the point's transform of the current frame.
//
// The model's registration points and the effects manager are the IRegEffectsHost seam.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "sc4/Automaton.h"

namespace sc4 {

constexpr const char* kRegEffectPrefix = "fx_";
constexpr std::uint32_t kEffectParamVelocity = 0xf;     // SetVectorParams: how far the model moves in this update
constexpr std::uint32_t kEffectFlagStartsOff = 8;       // cSC4VisualEffect::CheckEffectFlag(8): not started with the others

// cS3DRegPoints::cTransform: where a registration point is in one animation frame.
struct RegPointTransform {
    float translation[3];
    float axis[3];
    float angle;
};
using RegPointFrames = std::vector<RegPointTransform>;
using RegPointMap = std::map<std::string, RegPointFrames>;

// A visual effect as the collection drives it (cISC4VisualEffect vslots 0x4, 0x8, 0xc, 0x10, 0x14, 0x20, 0x2c, 0x44).
// FindRegEffect answers this object.
class IRegEffect {
public:
    virtual ~IRegEffect() = default;
    virtual void AddRef() = 0;
    virtual void Release() = 0;
    virtual void Start(int transition) = 0;
    virtual void Stop(int transition) = 0;
    virtual bool IsStarted() = 0;
    virtual void SetSourceTransform(const cS3DTransform& transform) = 0;
    virtual void SetVectorParams(std::uint32_t type, const cS3DVector3* values, int count) = 0;
    virtual bool CheckEffectFlag(std::uint32_t flag) = 0;
};

class IRegEffectsHost {
public:
    virtual ~IRegEffectsHost() = default;
    // model instance vslot 0x48 -> vslot 0xc: the registration points of the model the instance was made from; null without any
    virtual const RegPointMap* RegPoints(IAutomatonModel* model) = 0;
    // effects manager (city vslot 0x154) vslot 0x18: a new effect by name, one reference; null when there is no such effect
    virtual IRegEffect* CreateEffect(const char* name) = 0;
};
IRegEffectsHost* RegEffectsHost();
void SetRegEffectsHost(IRegEffectsHost* host);

class cSC4ModelRegEffects final : public IAutomatonEffects {
public:
    struct tRegEffect {
        IRegEffect* effect = nullptr;              // +0 (a reference)
        const RegPointFrames* frames = nullptr;    // +4 the point's transforms, in the model's own table
        std::string effectName;                    // +8
    };

    cSC4ModelRegEffects() = default;                                                  // @ 0x00160778
    ~cSC4ModelRegEffects() override;                                                  // @ 0x0063a20a

    bool Init() override;                                                             // 0x00 @ 0x0016055c
    void Shutdown() override;                                                         // 0x04 @ 0x00160e3e
    void SetModel(IAutomatonModel* model) override;                                   // 0x08 @ 0x0016057c
    IAutomatonModel* GetModel() const { return model_; }                              // 0x0c @ 0x0068e440
    void UpdatePlacement() override;                                                  // 0x10 @ 0x0016102e Update()
    void Update(const cS3DVector3& position, const cS3DVector3& direction, float dt) override;   // 0x14 @ 0x00161250
    void StartAllEffects(int transition);                                             // 0x18 @ 0x001609e6
    void StopAllEffects(int transition);                                              // 0x1c @ 0x00160a36
    IRegEffect* AddRegEffect(const std::string& point, const std::string& effect);    // 0x20 @ 0x001605ae
    void* FindRegEffect(const cRZString& point) override;                             // 0x24 @ 0x00160798
    void RemoveRegEffect(const cRZString& point) override;                            // 0x28 @ 0x001607c8
    bool StartNamedEffects(const cRZString& effect, int transition, bool create) override;   // 0x2c @ 0x00160a86
    void StopNamed(const cRZString& effect, int transition) override;                 // 0x30 @ 0x00160b44
    void ToggleNamed(const cRZString& effect, int transition, bool create) override;  // 0x34 @ 0x00160bbc
    void RecreateNamed(const cRZString& effect, int transition, bool create) override;   // 0x38 @ 0x00160ca8
    void InitEffectsList();                                                           // 0x3c @ 0x00160e6e
    IRegEffect* AddRegEffect(const std::string& point, const std::string& effect, const RegPointFrames* frames);   // 0x40 @ 0x00160622
    void ClearEffectsList();                                                          // @ 0x00160de0

    // IAutomatonEffects, in the automaton's words
    void StartAll() override { StartAllEffects(1); }
    void StopAll(bool fade) override { StopAllEffects(fade ? 1 : 0); }
    void StartNamed(const cRZString& effect, int transition, bool create) override { StartNamedEffects(effect, transition, create); }
    bool AddRegEffect(const cRZString& point, const cRZString& effect) override;      // cSC4Automaton::AddRegEffect (0x000bb4ee): add and start
    bool AddKeyEffect(const cRZString& point, const cRZString& effect) override {     // cSC4Automaton::InitKeyEffects: add, not started
        return AddRegEffect(std::string(point.ToChar()), std::string(effect.ToChar())) != nullptr;
    }
    void StartAllEffectsByKeys(const std::vector<std::pair<std::string, bool>>& keyed) override;   // cSC4Automaton::StartAllEffects (0x000c06c6)

    const std::map<std::string, tRegEffect>& Effects() const { return effects_; }

private:
    void Place(tRegEffect& entry, const cS3DTransform& model, int frame);

    IAutomatonModel* model_ = nullptr;                // +4 (a reference)
    std::map<std::string, tRegEffect> effects_;       // +8 by registration point name
};

}  // namespace sc4
