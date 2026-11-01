// Description records of the effects system and their stream operators (0x10056a178..0x1006fd207): shake, decal, camera,
// light, message trigger, effect (with its description / event records), tractor points, timed effects and dynamic
// particles. `>>` reads from a cIGZIStream and `<<` writes to a cIGZOStream; field order is the stream order. Field names
// follow how the effects code uses them where that is known; the offsets are the original object offsets.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "gz/StreamOps.h"
#include "sc4/EffectComponents.h"

struct cSC4ShakeDescription {
    float amplitude = 0;                     // +0
    float frequency = 0;                     // +4
    std::vector<float> curveA;               // +8
    std::vector<float> curveB;               // +0x20
    float falloff = 0;                       // +0x38
    std::uint32_t flags = 0;                 // +0x3c (one byte in the stream)
};

struct cSC4DecalDescription {
    std::uint64_t flags = 0;                 // +0 (32 bits in the stream; bit 0x40 is set when kind is missing)
    std::uint32_t textureId = 0;             // +8
    std::uint8_t mode = 0;                   // +0xc
    std::uint8_t kind = 0;                   // +0xd (0 on load becomes 2 and sets flag 0x40)
    float scale = 0;                         // +0x10
    std::vector<float> sizeCurve;            // +0x18
    std::vector<float> alphaCurve;           // +0x30
    std::vector<float> rotationCurve;        // +0x48
    std::vector<cS3DVector3> path;           // +0x60 (u32 count first)
    std::vector<float> extraCurve;           // +0x78
    float values[4] = {0, 0, 0, 0};          // +0x90..+0x9c
    cS3DVector2 offset;                      // +0xa0
};

struct cSC4CameraParams {
    std::vector<float> keys;                 // +0
    float values[4] = {0, 0, 0, 0};          // +0x18..+0x24 (a 16-bit version field of 0 precedes everything)
};

struct cSC4LightDescription {
    std::vector<cS3DVector3> colors;         // +0 (u32 count first)
    std::vector<float> intensities;          // +0x18
    float range = 0;                         // +0x30
};

struct cSC4MessageTriggerDescription {
    std::uint32_t message = 0;               // +0
    std::string name;                        // +8
};

struct cSC4EffectDescription {
    struct cDescriptionRec {
        std::string name;                    // +0
        std::uint32_t kind = 0;              // +0x18 (a byte in the stream)
        std::uint64_t type = 0;              // +0x20 (32 bits in the stream)
        cS3DTransform transform;             // +0x28 (written in the legacy transform format)
        std::uint8_t flagA = 0;              // +0x60
        std::uint8_t flagB = 0;              // +0x61
        std::uint16_t shortA = 0;            // +0x62
        std::uint16_t shortB = 0;            // +0x64
        float floats[4] = {0, 0, 0, 0};      // +0x68..+0x74
        std::uint16_t shortC = 0;            // +0x78
        std::uint16_t shortD = 0;            // +0x7a
        std::int32_t intValue = 0;           // +0x7c
    };
    struct cEventRec {
        std::uint64_t type = 0;              // +0 (32 bits in the stream)
        std::string name;                    // +8
        float delay = 0;                     // +0x20
        std::uint32_t flags = 0;             // +0x24
    };
    std::uint64_t id = 0;                    // +0 (32 bits in the stream)
    std::uint32_t version = 0;               // +8
    std::vector<cDescriptionRec> records;    // +0x10
    std::vector<cEventRec> events;           // +0x28
    std::uint32_t values[3] = {0, 0, 0};     // +0x40, +0x44, +0x48
    std::string label;                       // +0x50
};

struct cSC4ParticlesDescriptionBase {
    struct cTractorPoint {
        cS3DVector3 position;                // +0
        cS3DVector3 direction;               // +0xc
        float strength = 0;                  // +0x18
        float radius = 0;                    // +0x1c
    };
    struct cTimedEffect {
        std::string name;                    // +0
        float time = 0;                      // +0x18
    };
};

struct cSC4DynamicParticleDescription {
    std::uint64_t type = 0;                  // +0x10 (32 bits in the stream; the preceding 16 bytes belong to the base class)
    std::string name;                        // +0x18
    float values[6] = {0, 0, 0, 0, 0, 0};    // +0x30..+0x44
    std::uint32_t count = 0;                 // +0x48
    std::vector<float> curve;                // +0x50
};

// A string of the packed effects library: a u32 byte count, then the bytes (the PC library is written this way; the
// stream's own GetGZStr reads the 7-bit length the Mac build writes, which misreads every record after the first name).
std::string ReadEffectString(cIGZIStream& s);

// The legacy (pre-version-5) transform: 3x3 rotation row by row, translation, scale, then the change counter as a u32.
// @ 0x1000cfa93; the load side is LoadLegacyS3DTransform (0x1000cf954).
void S3DTransformLegacySave(cIGZOStream& s, const cS3DTransform& v);

cIGZIStream& operator>>(cIGZIStream& s, cSC4ShakeDescription& v);                       // @ 0x10056a178
cIGZOStream& operator<<(cIGZOStream& s, const cSC4ShakeDescription& v);                 // @ 0x10056a1e3
cIGZIStream& operator>>(cIGZIStream& s, cSC4DecalDescription& v);                       // @ 0x10056b160
cIGZOStream& operator<<(cIGZOStream& s, const cSC4DecalDescription& v);                 // @ 0x10056bc62
cIGZIStream& operator>>(cIGZIStream& s, cSC4CameraParams& v);                           // @ 0x1005c2210
cIGZOStream& operator<<(cIGZOStream& s, const cSC4CameraParams& v);                     // @ 0x1005c2276
cIGZIStream& operator>>(cIGZIStream& s, cSC4LightDescription& v);                       // @ 0x1005c2a69
cIGZOStream& operator<<(cIGZOStream& s, const cSC4LightDescription& v);                 // @ 0x1005c2ae5
cIGZIStream& operator>>(cIGZIStream& s, cSC4MessageTriggerDescription& v);              // @ 0x1005c2b61
cIGZOStream& operator<<(cIGZOStream& s, const cSC4MessageTriggerDescription& v);        // @ 0x1005c2c0e
cIGZIStream& operator>>(cIGZIStream& s, cSC4EffectDescription::cDescriptionRec& v);     // @ 0x1005c22d9
cIGZOStream& operator<<(cIGZOStream& s, const cSC4EffectDescription::cDescriptionRec& v);   // @ 0x1005c2453
cIGZIStream& operator>>(cIGZIStream& s, cSC4EffectDescription::cEventRec& v);           // @ 0x1005c253e
cIGZOStream& operator<<(cIGZOStream& s, const cSC4EffectDescription::cEventRec& v);     // @ 0x1005c261f
cIGZIStream& operator>>(cIGZIStream& s, cSC4EffectDescription& v);                      // @ 0x1005c266f
cIGZOStream& operator<<(cIGZOStream& s, const cSC4EffectDescription& v);                // @ 0x1005c296f
cIGZIStream& operator>>(cIGZIStream& s, sc4::cSC4SequenceDescription::cItemRec& v);          // @ 0x1005729b0
cIGZIStream& operator>>(cIGZIStream& s, cSC4ParticlesDescriptionBase::cTractorPoint& v);   // @ 0x1005b596c
cIGZOStream& operator<<(cIGZOStream& s, const cSC4ParticlesDescriptionBase::cTractorPoint& v);   // @ 0x1005b59ae
cIGZIStream& operator>>(cIGZIStream& s, cSC4ParticlesDescriptionBase::cTimedEffect& v);   // @ 0x1005b59f1
cIGZIStream& operator>>(cIGZIStream& s, cSC4DynamicParticleDescription& v);             // @ 0x1006fd0d8
cIGZOStream& operator<<(cIGZOStream& s, const cSC4DynamicParticleDescription& v);       // @ 0x1006fd207
