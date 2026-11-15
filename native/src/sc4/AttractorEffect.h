// cSC4AttractorEffect (Mac i386 0x003e1ef4..0x003e244a, 0x2c bytes; made by cSC4AttractorDescription::Instantiate 0x003e2408)
// and cSC4SequenceEffect (0x003e047a..0x003e0b42, 0xa8 bytes; cSC4SequenceDescription::Instantiate 0x003e0978).
//
// The attractor puts an invisible occupant where the effect is and hangs automata controllers on it, so the automata
// around an effect react to it: sims run from the volcano and the tornado, fire engines come to the torch, tanks keep off
// the monster truck. The description names either one controller template (flag clear) or an occupant group whose
// controllers are all added (flag set; every shipped attractor is of this kind).
//
// The sequence plays other effects one after the other: each item lasts a random time out of its range and names the
// effect to run meanwhile (an empty name is a pause). Description flags: 1 start over after the last item, 2 an item's
// effect is cut when the next begins (clear: it fades out, and the sequence counts as started until every fading effect
// is done), 4 the transition the items' effects are started with.
#pragma once
#include <cstdint>
#include <vector>

#include "sc4/EffectComponents.h"

namespace sc4 {

class IAttractorHost {
public:
    virtual ~IAttractorHost() = default;
    virtual bool HasOccupantManager() = 0;                               // SL::spOccupantManager
    // a new cSC4DummyOccupant, Init'ed and inserted in the occupant manager (vslot 0x50); a counted handle
    virtual void* CreateAnchor() = 0;
    // the automata controller manager (city vslot 0xbc) vslot 0x14 AddControllerFromTemplateName(anchor, name, null); a
    // counted handle, null when the template is unknown
    virtual void* AddController(void* anchor, const char* templateName) = 0;
    virtual void RemoveController(void* controller) = 0;                 // vslot 0x20, then the reference goes
    virtual void AddGroupControllers(const char* group, void* anchor) = 0;      // vslot 0x24 (group, anchor, null)
    virtual void RemoveGroupControllers(const char* group, void* anchor) = 0;   // vslot 0x2c
    // the anchor's SetPosition, the occupant manager's MoveOccupant(anchor, false) when `inManager`, then the controller
    // manager's MoveControllers(anchor)
    virtual void MoveAnchor(void* anchor, const float xyz[3], bool inManager) = 0;
    // the occupant manager's RemoveOccupant(anchor, false, 0), the anchor's Shutdown, and the reference goes
    virtual void DestroyAnchor(void* anchor) = 0;
};
IAttractorHost* AttractorHost();
void SetAttractorHost(IAttractorHost* host);

class cSC4AttractorEffect final : public cISC4VisualEffectComponent {
public:
    explicit cSC4AttractorEffect(cSC4AttractorDescription* description);   // @ 0x003e2390
    ~cSC4AttractorEffect() override;                                       // @ 0x003e244a

    void Start(int transition) override;                                   // @ 0x003e21d6
    void Stop(int transition) override;                                    // @ 0x003e200a
    bool IsStarted() const override { return started_; }                   // @ 0x003e1f94
    void SetTransforms(const cS3DTransform& parent, const cS3DTransform& local) override;   // @ 0x003e211a
    void ApplyEffect(float elapsed) override { time_ += elapsed; }         // @ 0x003e1fb0
    bool SetFloatParams(int, const float*, int) override { return false; }
    bool SetVoidParams(int, void**, int) override { return false; }

    const cS3DVector3& Position() const { return position_; }
    float Time() const { return time_; }

private:
    cSC4AttractorDescription* description_;   // +0x0c (reference held)
    cS3DVector3 position_{0.0f, 0.0f, 0.0f};  // +0x10
    bool started_ = false;                    // +0x1c
    float time_ = 0.0f;                       // +0x20
    void* anchor_ = nullptr;                  // +0x24
    void* controller_ = nullptr;              // +0x28
};

// ---- sequence -----------------------------------------------------------------------------------------------------------
constexpr std::uint32_t kSequenceLoop = 1, kSequenceCutItems = 2, kSequenceStartTransition = 4;

// One effect an item plays (cISC4VisualEffect vslots 0xc, 0x10, 0x14, 0x1c, 0x20 and the reference count).
class ISequenceItemEffect {
public:
    virtual ~ISequenceItemEffect() = default;
    virtual void AddRef() = 0;
    virtual void Release() = 0;
    virtual void Start(int transition) = 0;
    virtual void Stop(int transition) = 0;
    virtual bool IsStarted() = 0;
    virtual void SetEffectTransform(const cS3DTransform& transform) = 0;
    virtual void SetSourceTransform(const cS3DTransform& transform) = 0;
};
class ISequenceHost {
public:
    virtual ~ISequenceHost() = default;
    // SL::spEffectsManager vslot 0x18 CreateVisualEffect(name): one reference, null when there is no such effect
    virtual ISequenceItemEffect* CreateEffect(const char* name) = 0;
    virtual double RandomRange(double low, double high) = 0;             // the class's cRZRandom mRandom
};
ISequenceHost* SequenceHost();
void SetSequenceHost(ISequenceHost* host);

class cSC4SequenceEffect final : public cISC4VisualEffectComponent {
public:
    explicit cSC4SequenceEffect(cSC4SequenceDescription* description);     // @ 0x003e0856
    ~cSC4SequenceEffect() override;                                        // @ 0x003e09ba

    void Start(int transition) override;                                   // @ 0x003e050a
    void Stop(int transition) override;                                    // @ 0x003e063e
    bool IsStarted() const override { return started_ || !fading_.empty(); }   // @ 0x003e04de
    void SetTransforms(const cS3DTransform& parent, const cS3DTransform& local) override;   // @ 0x003e047a
    void ApplyEffect(float elapsed) override;                              // @ 0x003e0b42
    bool SetFloatParams(int, const float*, int) override { return false; }
    bool SetVoidParams(int, void**, int) override { return false; }

    std::uint32_t Item() const { return item_; }
    std::size_t FadingCount() const { return fading_.size(); }
    bool Playing() const { return current_ != nullptr; }

private:
    void CullSoftStoppedEffects();                                         // @ 0x003e059a
    void SetCurrent(ISequenceItemEffect* effect);

    cSC4SequenceDescription* description_;    // +0x0c (reference held)
    bool started_ = false;                    // +0x10
    bool fresh_ = true;                       // +0x11: never started yet
    float elapsed_ = 0.0f;                    // +0x14
    std::uint32_t item_ = 0;                  // +0x18
    float nextTime_ = 0.0f;                   // +0x1c: when the current item is over
    cS3DTransform source_, effect_;           // +0x20, +0x58
    int startTransition_ = 0;                 // +0x90
    int itemStopTransition_ = 0;              // +0x94
    ISequenceItemEffect* current_ = nullptr;  // +0x98
    std::vector<ISequenceItemEffect*> fading_;   // +0x9c: stopped softly, still running out
};

}  // namespace sc4
