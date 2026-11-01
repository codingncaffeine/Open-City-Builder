// cSC4VisualEffect — one running visual effect: a set of components (camera, sound, sequence, particles...) sharing
// one effect transform and one source transform (38 functions, 0x1005fa460..0x1005fb928). Interface cISC4VisualEffect
// (iid 0x2c279933) is the second vtable group (+8). Layout: +0x10 reference count, +0x18 name, +0x38 manager slot id
// (-1 = none), +0x3c started, +0x3d stopping, +0x40 description flag bits (bitset<9>: 0 follow the view rotation,
// 1 never auto-finish, 2 stop with fade by default, 3 apply the transforms to the components unmerged, 4 parameters
// locked, 6 ignore rotations, 7 finish only when stopped), +0x48/+0x4c finish message, +0x50 effect transform, +0x88
// source transform, +0xc0 world transform, +0xf8 combined source, +0x130 components (0x50 bytes each), +0x148..0x150
// start message, +0x158 parameter records, +0x170 parameter floats, +0x190 the effects manager.
// Transition types: 0 default (fade), 1 immediate, 2 cancel without bookkeeping.
// Quirks kept: SetFloatParams accepts type 0x13 but the original sized its table for 0x13 entries (this one holds 0x14).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "gz/StreamOps.h"
#include "gz/cIGZMessage2.h"
#include "sc4/EffectComponents.h"
#include "sc4/EffectDescriptions.h"
#include "sc4/Occupant.h"

namespace sc4 {

constexpr std::uint32_t kGZIID_cISC4VisualEffect = 0x2c279933;

class cSC4VisualEffect;
// The effects manager side of a started effect (cSC4EffectsManager::StartAncilliary / StopAncilliary).
class IEffectsManagerLink {
public:
    virtual ~IEffectsManagerLink() = default;
    virtual void AddRef() = 0;
    virtual void Release() = 0;
    virtual void StartAncilliary(cSC4VisualEffect* effect) = 0;
    virtual void StopAncilliary(cSC4VisualEffect* effect) = 0;
};

// The 3D view (SC4ViewUtilities::GetZoomAndRotation) and the occupant's first model.
struct VisualEffectServices {
    std::function<void(int& zoom, std::uint32_t& rotation)> zoomAndRotation;
    // The first model instance of an occupant's placeable object and its transform; false when it has none.
    std::function<bool(cISC4Occupant* occupant, void*& model, cS3DTransform& transform)> firstModel;
};
VisualEffectServices& GetVisualEffectServices();

class cISC4VisualEffect {
public:
    enum tParamType : std::uint32_t {};
    virtual ~cISC4VisualEffect() = default;
};

class cSC4VisualEffect : public cIGZUnknown {
public:
    struct cComponentRec {                       // 0x50 bytes
        cS3DTransform transform;                 // +0
        cISC4VisualEffectComponent* component = nullptr;   // +0x38
        std::uint64_t reserved40 = 0;            // +0x40
        std::uint64_t flags = 0;                 // +0x48 (bit 2: ignored when deciding whether the effect has finished)
    };
    struct cParamRec { std::uint16_t offset = 0, count = 0; };

    cSC4VisualEffect();
    ~cSC4VisualEffect();

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    void Shutdown();                                                       // @ 0x1005fa70e

    void Start(int transition);                                            // @ 0x1005fa7da
    void Stop(int transition);                                             // @ 0x1005fa922
    bool IsStarted() const { return started_; }
    bool IsStopping() const { return stopping_; }
    void SetEffectTransform(const cS3DTransform& effect);
    void SetSourceTransform(const cS3DTransform& source);
    void SetTransforms(const cS3DTransform& source, const cS3DTransform& effect);   // @ 0x1005fa7c
    void ApplyEffect(float elapsed);
    bool SetModelFromOccupant(cISC4Occupant* occupant);
    void SetFinishNotifyMessage(std::uint32_t message, std::uint32_t data) { finishMessage_ = message; finishData_ = data; }
    bool SetVectorParams(std::uint32_t type, const cS3DVector3* values, int count);
    bool SetFloatParams(std::uint32_t type, const float* values, int count);
    bool SetVoidParams(std::uint32_t type, void** values, int count);
    void GetEffectTransform(cS3DTransform& out) const;
    void GetSourceTransform(cS3DTransform& out) const;
    void SetDescription(const cSC4EffectDescription& description);
    bool CheckEffectFlag(std::uint32_t flag) const;
    void ApplyParams();
    bool CheckIfStopped();                                                 // true once the effect should be removed
    bool AddComponentEffect(cISC4VisualEffectComponent* component, const cS3DTransform& transform);

    void SetManager(IEffectsManagerLink* manager);
    IEffectsManagerLink* Manager() const { return manager_; }
    void SetSlot(std::int32_t slot) { slot_ = slot; }
    std::int32_t Slot() const { return slot_; }
    const std::vector<cComponentRec>& Components() const { return components_; }
    // cSC4EffectsManager::UpdateVisualEffect merges the description's components into this list (+0x130).
    std::vector<cComponentRec>& MutableComponents() { return components_; }
    const cS3DTransform& WorldTransform() const { return world_; }
    const cS3DTransform& CombinedSource() const { return combined_; }

    // ---- the fields cSC4EffectsManager reads and writes by offset -------------------------------------------------
    // cSC4EffectsManager::CreateVisualEffect(unsigned,unsigned,...) keys the effect with the (group, type) pair it was
    // created from (0x1005e0c08 stores them at +0x30 / +0x34); UpdateVisualEffect re-resolves the description through
    // that pair, and DeactivateUnusedVisualEffects zeroes the pair and the slot when it recycles the object.
    void SetGroup(std::uint32_t group) { group_ = group; }             // +0x30
    std::uint32_t Group() const { return group_; }
    void SetType(std::uint32_t type) { type_ = type; }                 // +0x34
    std::uint32_t Type() const { return type_; }
    void SetName(const std::string& name) { name_ = name; }            // +0x18
    const std::string& Name() const { return name_; }
    std::uint32_t RefCount() const { return refCount_; }               // +0x10
    // +0x188: the renderer handle of a shake started by cSC4EffectsManager::StartAncilliary (0 = none). SetDescription
    // clears it.
    void SetAncilliaryHandle(std::uint32_t handle) { ancilliaryHandle_ = handle; }
    std::uint32_t AncilliaryHandle() const { return ancilliaryHandle_; }
    // @ 0x1005e0642 (the recycle path of DeactivateUnusedVisualEffects): release the components, clear the name and
    // zero group / type / slot. The manager gets the slot index back from FreeVisualEffectSlots().
    void ClearForReuse();

private:
    void PostMessage(std::uint32_t type, std::uint32_t data1, std::uint32_t data2, bool withData2);

    std::uint32_t refCount_ = 0;                // +0x10
    std::string name_;                          // +0x18
    std::uint32_t group_ = 0;                   // +0x30 description lookup key (CreateVisualEffect by group/type)
    std::uint32_t type_ = 0;                    // +0x34
    std::int32_t slot_ = -1;                    // +0x38
    bool started_ = false;                      // +0x3c
    bool stopping_ = false;                     // +0x3d
    std::uint64_t flags_ = 0;                   // +0x40
    std::uint32_t finishMessage_ = 0, finishData_ = 0;   // +0x48
    cS3DTransform effect_, source_, world_, combined_;   // +0x50, +0x88, +0xc0, +0xf8
    std::vector<cComponentRec> components_;     // +0x130
    std::uint32_t startMessage_[3] = {};        // +0x148
    std::vector<cParamRec> paramRecs_;          // +0x158
    std::vector<float> params_;                 // +0x170
    std::uint32_t ancilliaryHandle_ = 0;        // +0x188 (the original's reserved188_)
    IEffectsManagerLink* manager_ = nullptr;    // +0x190
};

}  // namespace sc4
