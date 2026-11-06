// cSC4PropOccupantVisualEffect — the visual effect a prop occupant plays (chimney smoke, street-light glow,
// fountain spray...). It is the cISC4PropOccupantVisualEffect implementation (iid 0x09820792, class id
// 0x2977aa49) that cSC4PropManager attaches to a prop. 24 function symbols at 0x1001606f2..0x100163494 (the
// GZCOM helpers at 0x1001605d4 / 0x1001607d7 are recorded in src/gz/GZCOM.cpp: the CLSID getter already returns
// 0x2977aa49 and the creator is the weak nullptr stub, like the other recovered classes), plus the
// multiple-inheritance thunks that the compiler generates.
//
// Original layout (0xe8 bytes; sPropOccupantVisualEffectAllocator = 0xe8, read by the creator at 0x1001605d4):
//   +0x00 cISC4PropOccupantVisualEffect vptr (primary table 0x100a77228)
//   +0x08 cSC4PropOccupant (0xa0 bytes; its cSC4Occupant sub-object at +0x18, the secondary vtables 0x100a772d8 /
//         0x100a77410 / 0x100a77440 / 0x100a77588 / 0x100a775c8 / 0x100a776b8)
//   +0xa8 cSC4VisualEffect* effect (one reference)
//   +0xb0 cRZString effect name (vptr 0x100a67890, std::string at +0xb8, refcount at +0xd0)
//   +0xd8 u32 effect type id
//   +0xdc u32 name-variant count (properties 0xc98204b8 + state)
//   +0xe0 u32 type-variant count (properties 0x2a6def88 + state)
//   +0xe4 u8 the effect is (or should be) running; saved in the record
//   +0xe5 u8 the first start uses the effect's default transition; saved in the record
//
// Port layout note: the repo's cSC4PropOccupant keeps its cSC4Occupant sub-object at the prop's +0x00 (the original
// nests it at prop +0x10 behind two more interface vtables; the repo base is 0x10 bytes larger, so the prop's own
// field offsets still match the binary). AsOccupant() therefore resolves to object +0x08 here, where the original
// returns object +0x18; every field and the 0xe8 object size are unchanged.
//
// Primary vtable 0x100a77228: +0x00 QueryInterface 0x100161188, +0x08 AddRef 0x1001610cc, +0x10 Release
// 0x10016112a, +0x18 AsOccupant 0x100163452, +0x20 AsPropOccupant 0x10016345c, +0x28 SetEffectType(char const*)
// 0x1001614c2, +0x30 SetEffectType(uint) 0x10016162c, +0x38 StartEffect(bool) 0x10016167a, +0x40 StopEffect(bool)
// 0x1001616cc, +0x48/+0x50 destructor entries 0x1001608b4 / 0x1001608e8 (both call the body at 0x1001607e6),
// +0x58 Init 0x100160950, +0x60 Shutdown
// 0x100160e40, +0x68 Write 0x100160ec2, +0x70 Read 0x100160ffa, +0x78 GetGZCLSID 0x100163466.
//
// Properties (exemplar ids, read in Init):
//   0x27812810  three floats = the prop's box size (default 16 x 16 x 16 m)
//   0xc98204b8  effect name; 0xc98204b9..0xc98204c7 select the name for prop state 1..15
//   0x2a6def88  effect type id; 0x2a6def89..0x2a6def97 select it for prop state 1..15
// The state selects one of the two variant families; a state past the count disables the effect.
//
// Stream record (version 3): the prop record (cSC4PropOccupant version 6), u8 running flag, u8 default-transition
// flag, the effect name (or an empty name when variant properties exist), u32 effect type id.
//
// Quirks kept and exercised by scripts/tests/sc4_propoccupantvisualeffect_test.cpp:
//  * a variant effect named "off" disables the effect: RestartEffect gives up but sets the running flag;
//  * the record only carries the name when no variant property exists (the variants write an empty string) and
//    Read does not restore the variant counts - Init re-derives them from the exemplar;
//  * SetState writes the prop's state byte directly and restarts the effect; it does NOT post the
//    "prop state changed" message that cSC4PropOccupant::SetState posts (hence SetStateRaw);
//  * StartEffect/StopEffect and SetAllFlags only call the effect when the occupant's flag byte has bit 2 set; with
//    a fresh occupant (flags 4) they only flip the running flag, so the effect stays unstarted until the 3D view
//    flags the occupant (set-visible path = SetAllFlags(flags | 2));
//  * RestartEffect calls StopEffect(true), which stops the old effect with transition 0 (a fade: it keeps
//    "started" and sets "stopping") - and only when flag bit 2 is set; the effect is then released;
//  * Shutdown stops with transition 0, which cSC4VisualEffect treats as a fade: it sets "stopping" but keeps
//    "started" set, and then releases the effect;
//  * UpdateVisualEffectTransform builds the effect transform with change counter 2 and flags 5 (translation |
//    rotation) and a Y rotation of -orientation * pi/2 whose zero entries carry the original -0.0 signs;
//  * Init reads the box size, saves the old centre through the virtual GetPosition (which dispatches back into this
//    class's SetPosition thunk in the original) and restores it after replacing the box.
#pragma once
#include <cstdint>

#include "gz/StreamOps.h"
#include "rz/cRZString.h"
#include "sc4/PropOccupant.h"
#include "sc4/VisualEffect.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4PropOccupantVisualEffect = 0x2977aa49;
constexpr std::uint32_t kGZIID_cISC4PropOccupantVisualEffect = 0x09820792;
constexpr std::uint32_t kPropertyPropEffectBoxSize = 0x27812810;
constexpr std::uint32_t kPropertyPropEffectName = 0xc98204b8;
constexpr std::uint32_t kPropertyPropEffectType = 0x2a6def88;
constexpr std::uint32_t kPropEffectVariantCount = 16;   // name/type ids [base, base + 16)
constexpr std::uint32_t kPropEffectGroupId = 0xaa6dec65;  // CreateVisualEffect(group, type) group
constexpr char kPropEffectOffName[] = "off";
constexpr std::uint16_t kPropVisualEffectRecordVersion = 3;
constexpr std::uint8_t kOccupantFlagShown = 0x2;   // cSC4Occupant flags byte, bit 1: flagged by the view

// The effects manager (SL::spEffectsManager, the global at 0x100b21368) as this class uses it. The manager itself
// (cSC4EffectsManager) is not recovered; each method mirrors one original vtable call.
// The effect object is the cSC4VisualEffect the manager returns; the calls this class makes on it are its primary
// vtable's 0x18 Start / 0x20 Stop / 0x38 SetEffectTransform / 0x10 Release.
class IPropVisualEffectHost {
public:
    virtual ~IPropVisualEffectHost() = default;
    // manager vslot 0x30: cSC4EffectsManager::CreateVisualEffect(char const*, cISC4VisualEffect**) @ 0x1005e075c.
    // One reference on success (the manager AddRefs before returning); the caller releases it.
    virtual bool CreateEffectByName(const char* name, cSC4VisualEffect** out) = 0;
    // manager vslot 0x38: CreateVisualEffect(uint group, uint type, cISC4VisualEffect**) @ 0x1005e0c08.
    virtual bool CreateEffectByType(std::uint32_t groupId, std::uint32_t typeId, cSC4VisualEffect** out) = 0;
};
IPropVisualEffectHost* PropVisualEffectHost();
void SetPropVisualEffectHost(IPropVisualEffectHost* host);

// cISC4PropOccupantVisualEffect (iid 0x09820792). Slots after the cIGZUnknown trio; the two destructor slots of
// the original (0x48 / 0x50) sit between StopEffect and Init there, but C++ places ours at the end of the table.
class cISC4PropOccupantVisualEffect : public cIGZUnknown {
public:
    virtual cISC4Occupant* AsOccupant() = 0;                    // 0x18 @ 0x100163452
    virtual cSC4PropOccupant* AsPropOccupant() = 0;             // 0x20 @ 0x10016345c
    virtual void SetEffectType(const char* name) = 0;           // 0x28 @ 0x1001614c2
    virtual void SetEffectType(std::uint32_t type) = 0;         // 0x30 @ 0x10016162c
    virtual bool StartEffect(bool instant) = 0;                 // 0x38 @ 0x10016167a
    virtual bool StopEffect(bool instant) = 0;                  // 0x40 @ 0x1001616cc
    virtual bool Init() = 0;                                    // 0x58 @ 0x100160950
    virtual bool Shutdown() = 0;                                // 0x60 @ 0x100160e40
    virtual bool Write(cIGZOStream& stream) const = 0;          // 0x68 @ 0x100160ec2
    virtual bool Read(cIGZIStream& stream) = 0;                 // 0x70 @ 0x100160ffa
    virtual std::uint32_t GetGZCLSID() const = 0;               // 0x78 @ 0x100163466
};

class cSC4PropOccupantVisualEffect final : public cISC4PropOccupantVisualEffect, public cSC4PropOccupant {
public:
    cSC4PropOccupantVisualEffect();                  // @ 0x1001606f2
    ~cSC4PropOccupantVisualEffect() override;        // @ 0x1001607e6 (deleting variant 0x1001608e8)

    // cIGZUnknown (via both bases)
    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x100161188
    std::uint32_t AddRef() override;                               // @ 0x1001610cc
    std::uint32_t Release() override;                              // @ 0x10016112a

    // cISC4Occupant / cIGZSerializable overrides (shared with the cSC4PropOccupant sub-object, like the original)
    bool Init() override;                                          // @ 0x100160950
    bool Shutdown() override;                                      // @ 0x100160e40
    bool Write(cIGZOStream& stream) const override;                // @ 0x100160ec2
    bool Read(cIGZIStream& stream) override;                       // @ 0x100160ffa
    void SetPosition(const float* xyz) override;                   // @ 0x100161216
    void SetAllFlags(std::uint32_t flags) override;                // @ 0x1001613fe
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4PropOccupantVisualEffect; }   // @ 0x100163466

    // cISC4PropOccupantVisualEffect
    cISC4Occupant* AsOccupant() override;                          // @ 0x100163452
    cSC4PropOccupant* AsPropOccupant() override;                   // @ 0x10016345c
    void SetEffectType(const char* name) override;                 // @ 0x1001614c2
    void SetEffectType(std::uint32_t type) override;               // @ 0x10016162c
    bool StartEffect(bool instant) override;                       // @ 0x10016167a
    bool StopEffect(bool instant) override;                        // @ 0x1001616cc

    // cISC4PropOccupant slots replaced by this class (references in the original's PropOccupant secondary table)
    bool SetOrientation(std::int32_t orientation);                 // @ 0x1001613b4 (thunk 0x1001613d6)
    void SetState(std::uint32_t state);                            // @ 0x100161482 (thunk 0x1001614a0)

    // The two helpers the original also exposes (they are called from Init / RestartEffect / Set*).
    void RestartEffect();                                          // @ 0x100160b8e
    void UpdateVisualEffectTransform();                            // @ 0x100161236

    // Inspection (tests and the game's effect manager).
    cSC4VisualEffect* Effect() const { return effect_; }
    const cRZString& EffectName() const { return effectName_; }
    cRZString& EffectName() { return effectName_; }
    std::uint32_t EffectType() const { return effectType_; }
    std::uint32_t NameVariantCount() const { return nameVariantCount_; }
    std::uint32_t TypeVariantCount() const { return typeVariantCount_; }
    bool EffectActive() const { return effectActive_; }
    bool DefaultTransition() const { return defaultTransition_; }

private:
    bool RestartFromNameVariant();   // the 0xc98204b8 + state property family
    bool RestartFromTypeVariant();   // the 0x2a6def88 + state property family
    bool CreateByName();
    bool CreateByType(std::uint32_t typeId);

    cSC4VisualEffect* effect_ = nullptr;   // +0xa8
    cRZString effectName_;                 // +0xb0 (std::string +0xb8, refcount +0xd0)
    std::uint32_t effectType_ = 0;         // +0xd8
    std::uint32_t nameVariantCount_ = 0;   // +0xdc
    std::uint32_t typeVariantCount_ = 0;   // +0xe0
    bool effectActive_ = false;            // +0xe4
    bool defaultTransition_ = true;        // +0xe5
};

// The original object is 0xe8 bytes; the port's is 8 more since the prop base carries the original's cIGZMessageTarget2
// view as a C++ base (its vptr), which the port's layout had left out. The member offsets above stay the original's.
static_assert(sizeof(cSC4PropOccupantVisualEffect) == 0xe8 + 8,
              "cSC4PropOccupantVisualEffect layout: the 0xe8-byte original object + the prop's message-target vptr");

}  // namespace sc4
