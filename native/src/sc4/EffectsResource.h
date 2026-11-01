// cSC4EffectsResource — the packed effects library (resource type 0xea5118b0, group 0xea5118b1; the base game ships it
// as instance 1 of SimCity_1.dat, mods add more instances). The original class is a cGZPersistResource whose second base
// is a cSC4EffectsCollection: Read fills that collection's tables straight from the stream and AddToCollection merges
// them into the effects manager's collection (cSC4EffectsManager::LoadAllEffects).
//
// Recovered from the Mac i386 build (symbols): Read 0x003dda9c (2,286 bytes), AddToCollection 0x003dd520, the
// cSC4ParticlesDescription reader 0x003f61aa and the cWiggle / cTractorPoint / cTimedEffect readers 0x003f60ec..617a.
// Stream format (all little-endian):
//   u16 version (3 or 4)
//   particles:  u16 sub-version, u32 count, count x cSC4ParticlesDescription (sub-version 1 = the old record, refused)
//   decals / shakes / lights: u16 sub-version, u32 count, count x record
//   components: for brush, attractor, scrubber, sequence, sound, camera: u32 count, count x Read
//   version 4 only: dynamic particles: u16, u32 count, count x record
//   effects:    u16 sub-version, u32 count, count x cSC4EffectDescription
//   effect names: (string name, u32 index) pairs up to the pair ("end", 0xffffffff); strings are u32 length + bytes
//   u8 camera parameters present [+ cSC4CameraParams]
//   effect bindings: u32 count, count x (string name, u32 group, u32 type)
//   message triggers: u16 sub-version, u32 count, count x record
#pragma once
#include <cstdint>

#include "sc4/EffectsCollection.h"

class cIGZIStream;

namespace sc4 {

constexpr std::uint32_t kEffectsResourceType = 0xea5118b0;
constexpr std::uint32_t kEffectsResourceGroup = 0xea5118b1;

class cSC4EffectsResource : public cSC4EffectsCollection {
public:
    bool Read(cIGZIStream& stream);                                  // @ i386 0x003dda9c
    void AddToCollection(cSC4EffectsCollection* target) {            // @ i386 0x003dd520
        cSC4EffectsCollection::AddToCollection(target);
    }
};

}  // namespace sc4

cIGZIStream& operator>>(cIGZIStream& s, cSC4ParticlesDescription& v);            // @ i386 0x003f61aa
