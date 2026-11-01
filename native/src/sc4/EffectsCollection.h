// cSC4EffectsCollection — the description library of the effects system: every effect / particle / dynamic-particle /
// decal / shake / light / component description loaded from an effects resource, indexed by (lowercased) name, plus the
// camera parameters, the effect bindings and the message triggers of the resource. 27 function symbols at
// 0x100576350..0x100578cb5 (the two destructor entries at 0x1005767cc / 0x1005767d2 are compiler-emitted deleted
// virtuals: the original class is abstract, see below).
//
// The class implements cISC4EffectsCollection (typeinfo 0x100b0f780, class_bases.json: cIGZUnknown -> cISC4EffectsCollection
// -> cSC4EffectsCollection). Its own vtable (0x100ace700 group; vptr 0x100ace710) runs +0x08 per function:
//   +0x18 BeginCollection          +0x20 EndCollection
//   +0x28 AddEffectDescription     +0x30 GetEffectDescription
//   +0x38 AddParticleDescription   +0x40 GetParticleDescription
//   +0x48 AddDynamicParticleDescription  +0x50 GetDynamicParticleDescription
//   +0x58 AddDecalDescription      +0x60 GetDecalDescription
//   +0x68 AddShakeDescription      +0x70 GetShakeDescription
//   +0x78 AddLightDescription      +0x80 GetLightDescription
//   +0x88 AddComponentDescription  +0x90 GetComponentDescription
//   +0x98 SetCameraParams          +0xa0 AddEffectBinding
//   +0xa8 AddMessageTrigger        +0xb0 UpdateEffectPriority
//   +0xb8 / +0xc0 deleted virtual destructors (ud2), and +0x00 / +0x08 / +0x10 (QueryInterface / AddRef / Release,
// inherited from cIGZUnknown) are unresolved pure virtuals: the class is only ever constructed as the base subobject of
// cSC4EffectsManager / cSC4EffectsResource (the C2 constructor at 0x100576350 is theirs), whose vtables fill those slots.
// This recovery therefore models the class as concrete over the Add* seam below, which is exactly the part of
// cISC4EffectsCollection the recovered code reaches through.
//
// Original layout (0x220 bytes; this libc++ has the same string / vector / unordered_map sizes as the binary's, and the
// record and object sizes are asserted at the end of this header):
//   +0x000 vtable
//   +0x008 std::unordered_multimap<std::string,int>   effect names -> index into effects; one entry per (name, version)
//   +0x030 std::vector<cSC4EffectDescription>         the effect descriptions (0x68 each)
//   +0x048 std::unordered_map<std::string,int>        particle names -> index
//   +0x070 std::vector<cSC4ParticlesDescription>      (original: vector<cRZAutoRefCount<...>>, refcount at +0x2bc)
//   +0x088 std::unordered_map<std::string,int>        dynamic-particle names -> index
//   +0x0b0 std::vector<cSC4DynamicParticleDescription> (original: vector<cRZAutoRefCount<...>>)
//   +0x0c8 std::unordered_map<std::string,int>        decal names -> index
//   +0x0f0 std::vector<cSC4DecalDescription>          (0xa8 each)
//   +0x108 std::unordered_map<std::string,int>        shake names -> index
//   +0x130 std::vector<cSC4ShakeDescription>          (0x40 each)
//   +0x148 std::unordered_map<std::string,int>        light names -> index
//   +0x170 std::vector<cSC4LightDescription>          (0x38 each)
//   +0x188 std::vector<cCompDescsRec>                 13 records (0x40 each), component types 3..15
//   +0x1a0 u8 camera params valid (the constructor never writes it — see the quirks)
//   +0x1a8 cSC4CameraParams                           keys + values[4] (0x28)
//   +0x1d0 std::vector<std::pair<std::string,cEffectKey>>  effect bindings (0x20 each)
//   +0x1e8 std::vector<cSC4MessageTriggerDescription>  message triggers (0x20 each)
//   +0x200 u8 "add with names" merge mode
//   +0x208 std::string lowercase scratch (every name lookup lowercases through this one member)
// A cCompDescsRec is a name map + the refcounted component descriptions of one type:
//   +0x00 std::unordered_map<std::string,int>          name -> index
//   +0x28 std::vector<cRZAutoRefCount<cISC4ComponentDescription>>
//
// Quirks kept (each asserted in the test):
//  * Every Add* / Get* / UpdateEffectPriority lowercases the name into the shared member at +0x208 (that is why the Get*
//    functions are non-const). The original calls __tolower (0x10092af64) with the sign-extended char; the port casts
//    through unsigned char, which is identical in the C locale for the ASCII names effects resources use.
//  * AddEffectDescription keys its multimap by (name, version at +0x08 of the description); GetEffectDescription is a
//    plain find and therefore returns the first entry with that name whatever version it carries.
//  * AddEffectDescription resolves the negative record indices (+0x7c) of the incoming description in place: kind 0 ->
//    particle table, 1 -> decals, 2 -> "no index" (0), 16 -> dynamic particles, 3..15 -> components[kind-3]; any other
//    kind keeps the negative value. Records and events whose index is not negative are left alone. Event types with bit
//    0 resolve through the shake table, types with (type & 0xc) through the light table.
//  * AddComponentDescription(nullptr, d) appends without registering a name and returns the old element count.
//  * UpdateEffectPriority takes the first entry of the name range (not the version match), writes the new version there,
//    and then, for an alias, either registers alias -> same index or refuses the alias when it already maps to another
//    index — after the version has been written (the mutation sticks on the failure path).
//  * BeginCollection only clears the message triggers and the effect bindings and clears the camera-params flag;
//    EndCollection is an empty virtual.
//  * The constructor (0x100576350) never writes +0x1a0, so a freshly built collection's camera-params flag is
//    indeterminate in the original; the port starts it false so a new collection is deterministic.
//  * SetCameraParams sets the flag; the empty-destination branch of AddToCollection swaps every table except the camera
//    parameters and the flag, and then, if *this* had camera parameters, copies them into the argument without making
//    them valid there (the argument's flag is not set).
//  * AddToCollection(target) merges *this* into target (it reads through this and writes through the argument).
//  * AddToCollectionWithNames forwards in the order particles, dynamic particles, decals, shakes, lights, components,
//    effects (the effects are forwarded last, and every name forwarded is the stored lowercase key).
//  * AddToCollectionWithRenumbering appends the value tables but only merges the *effect* name map (shifted by the
//    destination's effect count); the other name maps are left alone, so the merged descriptions are unreachable by
//    name. It renumbers record indices for kinds 0/1/2/3..15 but not kind 16 (dynamic particles), and it renumbers
//    *this* collection's effect records in place, so merging the same collection twice shifts it twice.
//  * The two refcounted description classes (cSC4ParticlesDescription, refcount at +0x2bc; cSC4DynamicParticleDescription,
//    cRZRefCount) are unrecovered: only their data layouts exist (ParticlesEffect.h / EffectDescriptions.h). The
//    collection therefore holds the records by value; Add* copies in (they always did) and Get* copies out, so the
//    storage change is not observable through this API (nothing else ever mutates a stored description).
//  * GetComponentDescription / AddComponentDescription index components[(type - 3)] unchecked in the original (a type
//    outside 3..15 reads out of bounds); the port refuses those types instead.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rz/cRZAutoRefCount.h"
#include "sc4/EffectComponents.h"
#include "sc4/EffectDescriptions.h"
#include "sc4/ParticlesEffect.h"

namespace sc4 {

class cSC4EffectsCollection;

// cISC4EffectsCollection — the GZCOM interface the class implements (unrecovered). AddToCollectionWithNames is the only
// recovered function that reaches through it: it sends this collection's descriptions to another collection through the
// interface's seven Add* slots. Those calls are the seam; the slot offsets are the original vtable offsets (vptr+slot).
class IEffectsCollectionHost {
public:
    virtual ~IEffectsCollectionHost() = default;
    virtual void AddEffectDescription(const char* name, const cSC4EffectDescription& description) = 0;        // vslot 0x28
    virtual void AddParticleDescription(const char* name, const cSC4ParticlesDescription& description) = 0;   // vslot 0x38
    virtual void AddDynamicParticleDescription(const char* name,
                                               const cSC4DynamicParticleDescription& description) = 0;      // vslot 0x48
    virtual void AddDecalDescription(const char* name, const cSC4DecalDescription& description) = 0;          // vslot 0x58
    virtual void AddShakeDescription(const char* name, const cSC4ShakeDescription& description) = 0;          // vslot 0x68
    virtual void AddLightDescription(const char* name, const cSC4LightDescription& description) = 0;          // vslot 0x78
    virtual std::uint32_t AddComponentDescription(const char* name,
                                                 cISC4ComponentDescription* description) = 0;               // vslot 0x88
};

class cSC4EffectsCollection : public IEffectsCollectionHost {
public:
    // The second half of an effect binding pair (+0x18 / +0x1c); AddEffectBinding(0x100577e3e) stores the two u32s as-is.
    struct cEffectKey {
        std::uint32_t id = 0;          // +0x18
        std::uint32_t value = 0;       // +0x1c
    };

    using NameMap = std::unordered_map<std::string, int>;                 // 0x28 bytes
    using ComponentList = std::vector<cRZAutoRefCount<cISC4ComponentDescription>>;   // 0x18 bytes

    // One slot of the component table at +0x188 (types 3..15). The constructor builds 13 of them.
    struct cCompDescsRec {
        NameMap names;                 // +0x00 name -> index
        ComponentList descriptions;    // +0x28 refcounted component descriptions of this type
    };

    cSC4EffectsCollection();                                    // @ 0x100576350 (C2: base-object constructor)
    ~cSC4EffectsCollection() override;                          // @ 0x1005765bc (D2)

    // ---- cISC4EffectsCollection (slots +0x18..+0xb0) ------------------------------------------------------------
    void BeginCollection();                                                              // @ 0x1005767d8
    void EndCollection();                                                                // @ 0x100576862
    void AddParticleDescription(const char* name, const cSC4ParticlesDescription& description) override;   // @ 0x100576868
    bool GetParticleDescription(const char* name, cSC4ParticlesDescription* description);         // @ 0x100576a42
    void AddDynamicParticleDescription(const char* name,
                                       const cSC4DynamicParticleDescription& description) override;        // @ 0x100576af2
    bool GetDynamicParticleDescription(const char* name,
                                       cSC4DynamicParticleDescription* description);              // @ 0x100576c54
    void AddDecalDescription(const char* name, const cSC4DecalDescription& description) override;          // @ 0x100576d0a
    bool GetDecalDescription(const char* name, cSC4DecalDescription* description);                // @ 0x100576f8e
    void AddShakeDescription(const char* name, const cSC4ShakeDescription& description) override;          // @ 0x100577106
    bool GetShakeDescription(const char* name, cSC4ShakeDescription* description);                // @ 0x1005772a6
    void AddLightDescription(const char* name, const cSC4LightDescription& description) override;          // @ 0x100577392
    bool GetLightDescription(const char* name, cSC4LightDescription* description);                // @ 0x1005774b2
    void AddEffectDescription(const char* name, const cSC4EffectDescription& description) override;        // @ 0x100577594
    bool GetEffectDescription(const char* name, cSC4EffectDescription* description);              // @ 0x1005779fe
    std::uint32_t AddComponentDescription(const char* name,
                                          cISC4ComponentDescription* description) override;      // @ 0x100577b04
    bool GetComponentDescription(const char* name, std::uint32_t type,
                                 cISC4ComponentDescription** description);                       // @ 0x100577d24
    void SetCameraParams(const cSC4CameraParams& params);                                         // @ 0x100577df6
    void AddEffectBinding(const char* name, std::uint32_t id, std::uint32_t value);              // @ 0x100577e3e
    void AddMessageTrigger(const cSC4MessageTriggerDescription& trigger);                        // @ 0x100577f9e
    bool UpdateEffectPriority(const char* name, std::uint32_t version, const char* aliasName);   // @ 0x100577fec
    void AddToCollection(cSC4EffectsCollection* target);                                         // @ 0x10057825c
    void AddToCollectionWithNames(IEffectsCollectionHost* target);                               // @ 0x100578608
    void AddToCollectionWithRenumbering(cSC4EffectsCollection* target);                          // @ 0x100578826

    // The merge-mode byte at +0x200. No recovered function writes it: the effects resource's loader (cSC4EffectsResource
    // / cSC4EffectsManager, both unrecovered) sets it before merging, so the port exposes it for the loader and the test.
    void SetAddWithNames(bool value) { addWithNames_ = value; }
    bool AddWithNames() const { return addWithNames_; }

    // ---- internals (public for the test; the original's resource / manager read these by offset) ------------------
    const cSC4CameraParams& CameraParams() const { return cameraParams_; }              // +0x1a8
    bool CameraParamsSet() const { return cameraParamsSet_; }                           // +0x1a0
    const std::vector<cSC4EffectDescription>& Effects() const { return effects_; }      // +0x30
    const std::vector<cSC4ParticlesDescription>& Particles() const { return particles_; }             // +0x70
    const std::vector<cSC4DynamicParticleDescription>& DynamicParticles() const { return dynamicParticles_; }   // +0xb0
    const std::vector<cSC4DecalDescription>& Decals() const { return decals_; }         // +0xf0
    const std::vector<cSC4ShakeDescription>& Shakes() const { return shakes_; }         // +0x130
    const std::vector<cSC4LightDescription>& Lights() const { return lights_; }         // +0x170
    const std::vector<cCompDescsRec>& ComponentDescs() const { return components_; }    // +0x188
    const std::vector<std::pair<std::string, cEffectKey>>& EffectBindings() const { return effectBindings_; }  // +0x1d0
    const std::vector<cSC4MessageTriggerDescription>& MessageTriggers() const { return messageTriggers_; }      // +0x1e8
    std::size_t EffectNameCount(const std::string& lowercaseName) const { return effectNames_.count(lowercaseName); }
    // cSC4EffectsManager::CreateVisualEffect(char const*, ...) reads the +0x008 multimap range by offset (0x1005e075c);
    // the manager picks the highest description version <= its maximum effect version out of that range.
    const std::unordered_multimap<std::string, int>& EffectNames() const { return effectNames_; }               // +0x008
    // @ 0x1005dcd62 (cSC4EffectsManager::Shutdown): the original destroys the effect / particle / dynamic-particle /
    // decal / shake tables and the component records through their member offsets and deliberately leaves the light
    // table, the camera parameters, the effect bindings and the message triggers in place.
    void ClearForEffectManagerShutdown() {
        effectNames_.clear();
        effects_.clear();
        particleNames_.clear();
        particles_.clear();
        dynamicParticleNames_.clear();
        dynamicParticles_.clear();
        decalNames_.clear();
        decals_.clear();
        shakeNames_.clear();
        shakes_.clear();
        components_.clear();
    }

protected:
    // cSC4EffectsResource (sc4/EffectsResource.h) is the other class built on this one: its Read fills these tables.
    // The same member at +0x208 used by every lookup (see the quirks): assign the name and lowercase it.
    const std::string& LowercasedName(const char* name);
    // The component table index of a type, or -1 for a type outside 3..15 (the original indexes unchecked).
    static int ComponentIndex(std::uint32_t type);

    std::unordered_multimap<std::string, int> effectNames_;     // +0x008 one entry per (name, version)
    std::vector<cSC4EffectDescription> effects_;                // +0x030
    NameMap particleNames_;                                     // +0x048
    std::vector<cSC4ParticlesDescription> particles_;           // +0x070 (see the storage quirk)
    NameMap dynamicParticleNames_;                              // +0x088
    std::vector<cSC4DynamicParticleDescription> dynamicParticles_;   // +0x0b0
    NameMap decalNames_;                                        // +0x0c8
    std::vector<cSC4DecalDescription> decals_;                  // +0x0f0
    NameMap shakeNames_;                                        // +0x108
    std::vector<cSC4ShakeDescription> shakes_;                  // +0x130
    NameMap lightNames_;                                        // +0x148
    std::vector<cSC4LightDescription> lights_;                  // +0x170
    std::vector<cCompDescsRec> components_;                     // +0x188
    bool cameraParamsSet_ = false;                              // +0x1a0
    cSC4CameraParams cameraParams_;                             // +0x1a8
    std::vector<std::pair<std::string, cEffectKey>> effectBindings_;   // +0x1d0
    std::vector<cSC4MessageTriggerDescription> messageTriggers_;       // +0x1e8
    bool addWithNames_ = false;                                 // +0x200
    std::string scratch_;                                       // +0x208
};

// The record layouts the collection strides over (the original's element sizes, from the decompiled offsets).
static_assert(sizeof(cSC4EffectsCollection::cCompDescsRec) == 0x40, "cCompDescsRec must stay 0x40 bytes");
static_assert(sizeof(std::pair<std::string, cSC4EffectsCollection::cEffectKey>) == 0x20, "binding records are 0x20");
static_assert(sizeof(cSC4EffectDescription) == 0x68, "effect descriptions are 0x68 bytes");
static_assert(sizeof(cSC4EffectDescription::cDescriptionRec) == 0x80, "effect description records are 0x80 bytes");
static_assert(sizeof(cSC4EffectDescription::cEventRec) == 0x28, "effect event records are 0x28 bytes");
static_assert(sizeof(cSC4DecalDescription) == 0xa8, "decal descriptions are 0xa8 bytes");
static_assert(sizeof(cSC4ShakeDescription) == 0x40, "shake descriptions are 0x40 bytes");
static_assert(sizeof(cSC4LightDescription) == 0x38, "light descriptions are 0x38 bytes");
static_assert(sizeof(cSC4CameraParams) == 0x28, "camera parameters are 0x28 bytes");
static_assert(sizeof(cSC4MessageTriggerDescription) == 0x20, "message triggers are 0x20 bytes");
static_assert(sizeof(cSC4DynamicParticleDescription) == 0x58, "dynamic particle payload is 0x58 bytes");
static_assert(sizeof(cSC4ParticlesDescription) == 0x2c0, "particle description layout is 0x2c0 bytes");
static_assert(sizeof(cSC4EffectsCollection) == 0x220, "the collection is 0x220 bytes");

}  // namespace sc4
