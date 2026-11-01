#include "sc4/EffectsResource.h"

#include "gz/cIGZIStream.h"
#include "rz/cRZString.h"
#include "sc4/EffectDescriptions.h"

namespace {
std::uint16_t ReadU16(cIGZIStream& s) { std::uint16_t v = 0; s.GetUint16(v); return v; }
std::uint32_t ReadU32(cIGZIStream& s) { std::uint32_t v = 0; s.GetUint32(v); return v; }
std::string ReadString(cIGZIStream& s) { return ReadEffectString(s); }
void ReadFloats(cIGZIStream& s, float* out, int count) {
    for (int i = 0; i < count; ++i) s.GetFloat32(out[i]);
}
template <typename T, typename ReadOne>
void ReadCounted(cIGZIStream& s, std::vector<T>& v, ReadOne readOne) {
    v.resize(ReadU32(s));
    for (T& e : v) readOne(e);
}
}  // namespace

cIGZIStream& operator>>(cIGZIStream& s, cSC4ParticlesDescription& v) {
    v.flags = ReadU32(s);                                    // std::bitset<32>
    v.flags8 = ReadU32(s);                                   // std::bitset<8>
    const std::uint32_t flags10 = ReadU32(s);                // std::bitset<11>
    v.flag10 = static_cast<std::uint8_t>(flags10);
    v.flag10High = static_cast<std::uint16_t>(flags10 >> 8);
    s.GetFloat32(v.lifetimeMin);
    s.GetFloat32(v.lifetimeMax);
    s.GetFloat32(v.spawnPhase);
    s.GetUint32(v.maxParticles);
    s.GetFloat32(v.startDelay);
    s.GetFloat32(v.delayMinA);
    s.GetFloat32(v.delayMaxA);
    s.GetFloat32(v.delayMinB);
    s.GetFloat32(v.delayMaxB);
    ReadFloats(s, v.velocityMin, 3);                         // cS3DBoundingBox
    ReadFloats(s, v.velocityMax, 3);
    s.GetFloat32(v.speedMin);
    s.GetFloat32(v.speedMax);
    ReadFloats(s, v.emitMin, 3);                             // cS3DBoundingBox
    ReadFloats(s, v.emitMax, 3);
    s.GetFloat32(v.sizeJitter);
    s.GetFloat32(v.alphaJitter);
    s.GetFloat32(v.spinRange);
    s.GetFloat32(v.spinBase);
    s.GetFloat32(v.jitter84);
    s.GetFloat32(v.jitter88);                                // cS3DVector3
    s.GetFloat32(v.jitter8c);
    s.GetFloat32(v.jitter90);
    s >> v.rateCurve;
    ReadCounted(s, v.colorCurve, [&](cS3DVector3& e) { s >> e; });
    s >> v.curveC8;
    s >> v.sizeCurve;
    s >> v.alphaCurve;
    s >> v.spinCurve;
    s.GetUint32(v.textureId);
    s.GetUint8(v.drawMode);
    s.GetUint8(v.billboardMode);
    s.GetFloat32(v.value130);
    s.GetFloat32(v.maxWidth);
    ReadFloats(s, v.origin, 3);
    s.GetFloat32(v.globalOffsetScale);
    s.GetFloat32(v.terrainRadius);
    ReadFloats(s, v.terrainCentre, 3);
    s.GetFloat32(v.damping);
    s.GetFloat32(v.spinAroundY);
    v.waveRows.resize(static_cast<std::size_t>(ReadU32(s)) * 7);   // cWiggle records (@ i386 0x003f60ec): rate, 2 x vec3
    ReadFloats(s, v.waveRows.data(), static_cast<int>(v.waveRows.size()));
    s.GetFloat32(v.value178);
    ReadFloats(s, v.range17c, 2);
    ReadFloats(s, v.vector184, 3);
    s >> v.curve190;
    s.GetFloat32(v.terrainDrag);
    ReadFloats(s, v.terrainBounce, 7);
    s.GetFloat32(v.heightMin);
    s.GetFloat32(v.heightMax);
    v.name = ReadString(s);
    s.GetUint16(v.value1e8);
    s.GetUint16(v.value1ea);
    s.GetFloat32(v.value1ec);
    s.GetFloat32(v.walkIntervalMin);
    s.GetFloat32(v.walkIntervalMax);
    s.GetFloat32(v.walkSpeedMin);
    s.GetFloat32(v.walkSpeedMax);
    s.GetFloat32(v.walkAngle);
    s.GetFloat32(v.walkAngle2);
    ReadFloats(s, v.walkBias, 3);
    ReadFloats(s, v.values214, 3);
    s >> v.attractorCurve;
    s.GetFloat32(v.attractorTime);
    std::uint32_t mode = 0;
    s.GetUint32(mode);
    v.attractorMode = static_cast<std::int32_t>(mode);
    ReadCounted(s, v.tractors, [&](cSC4TractorRecord& e) {   // @ i386 0x003f612c
        s >> e.from;
        s >> e.to;
        s.GetFloat32(e.time);
        s.GetFloat32(e.end);
    });
    s.GetFloat32(v.tractorRadius);
    ReadCounted(s, v.timedEffects, [&](cSC4TimedEffectRecord& e) {   // @ i386 0x003f617a
        e.name = ReadString(s);
        s.GetFloat32(e.time);
    });
    ReadFloats(s, v.values278, 2);
    s >> v.modelIds;
    ReadFloats(s, v.values298, 2);
    s >> v.curve2a0;
    s.GetFloat32(v.value2b8);
    return s;
}

namespace sc4 {

namespace {
template <typename T>
cISC4ComponentDescription* NewComponent() { return new T(); }
using ComponentMaker = cISC4ComponentDescription* (*)();
// The six component tables Read fills, in stream order (component types 3..8).
constexpr ComponentMaker kComponentMakers[] = {
    &NewComponent<cSC4BrushDescription>,    &NewComponent<cSC4AttractorDescription>,
    &NewComponent<cSC4ScrubberDescription>, &NewComponent<cSC4SequenceDescription>,
    &NewComponent<cSC4SoundDescription>,    &NewComponent<cSC4CameraDescription>,
};
}  // namespace

bool cSC4EffectsResource::Read(cIGZIStream& stream) {
    const std::uint16_t version = ReadU16(stream);
    if (static_cast<std::uint16_t>(version - 3) >= 2) return false;

    // The original reads sub-version 1 particle and effect records with ReadVersion1; the shipped library and every
    // later one is sub-version 2, so the old record is refused instead of guessed.
    if (ReadU16(stream) == 1) return false;
    ReadCounted(stream, particles_, [&](cSC4ParticlesDescription& e) { stream >> e; });
    ReadU16(stream);
    ReadCounted(stream, decals_, [&](cSC4DecalDescription& e) { stream >> e; });
    ReadU16(stream);
    ReadCounted(stream, shakes_, [&](cSC4ShakeDescription& e) { stream >> e; });
    ReadU16(stream);
    ReadCounted(stream, lights_, [&](cSC4LightDescription& e) { stream >> e; });
    for (std::size_t kind = 0; kind < std::size(kComponentMakers); ++kind) {
        ComponentList& list = components_[kind].descriptions;
        list.resize(ReadU32(stream));
        for (cRZAutoRefCount<cISC4ComponentDescription>& entry : list) {
            entry = kComponentMakers[kind]();
            entry->Read(stream);
        }
    }
    if (version == 4) {
        ReadU16(stream);
        ReadCounted(stream, dynamicParticles_, [&](cSC4DynamicParticleDescription& e) { stream >> e; });
    }
    if (ReadU16(stream) == 1) return false;
    ReadCounted(stream, effects_, [&](cSC4EffectDescription& e) { stream >> e; });
    for (;;) {
        std::string name = ReadString(stream);
        const std::uint32_t index = ReadU32(stream);
        if (name == "end" && index == 0xffffffffu) break;   // the list closes with ("end", -1)
        if (stream.GetError()) return false;
        effectNames_.emplace(std::move(name), static_cast<int>(index));
    }
    std::uint8_t hasCamera = 0;
    stream.GetUint8(hasCamera);
    cameraParamsSet_ = hasCamera != 0;
    if (cameraParamsSet_) stream >> cameraParams_;
    ReadCounted(stream, effectBindings_, [&](std::pair<std::string, cEffectKey>& e) {
        e.first = ReadString(stream);
        stream.GetUint32(e.second.id);
        stream.GetUint32(e.second.value);
    });
    ReadU16(stream);
    ReadCounted(stream, messageTriggers_, [&](cSC4MessageTriggerDescription& e) { stream >> e; });
    return !stream.GetError();
}

}  // namespace sc4
