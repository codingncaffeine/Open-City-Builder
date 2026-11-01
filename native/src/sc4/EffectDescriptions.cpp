#include "sc4/EffectDescriptions.h"

#include "rz/cRZString.h"

namespace {
std::string ReadString(cIGZIStream& s) { return ReadEffectString(s); }
std::uint32_t ReadU32(cIGZIStream& s) { std::uint32_t v = 0; s.GetUint32(v); return v; }
std::uint8_t ReadU8(cIGZIStream& s) { std::uint8_t v = 0; s.GetUint8(v); return v; }
// A u32 count followed by that many elements.
void ReadVector3s(cIGZIStream& s, std::vector<cS3DVector3>& v) {
    const std::uint32_t count = ReadU32(s);
    v.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) s >> v[i];
}
void WriteVector3s(cIGZOStream& s, const std::vector<cS3DVector3>& v) {
    s.SetUint32(static_cast<std::uint32_t>(v.size()));
    for (const cS3DVector3& e : v) s << e;
}
}  // namespace

std::string ReadEffectString(cIGZIStream& s) {
    std::uint32_t length = 0;
    if (!s.GetUint32(length) || length == 0 || length > (1u << 20)) return {};
    std::string text(length, '\0');
    s.GetVoid(text.data(), length);
    return text;
}

void S3DTransformLegacySave(cIGZOStream& s, const cS3DTransform& v) {
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) s.SetFloat32(v.rotation[row][col]);
    for (float t : v.translation) s.SetFloat32(t);
    s.SetFloat32(v.scale);
    s.SetUint32(v.changeCounter);
}

// ---- shake ----
cIGZIStream& operator>>(cIGZIStream& s, cSC4ShakeDescription& v) {
    s.GetFloat32(v.amplitude);
    s.GetFloat32(v.frequency);
    s >> v.curveA;
    s >> v.curveB;
    s.GetFloat32(v.falloff);
    v.flags = ReadU8(s);
    return s;
}
cIGZOStream& operator<<(cIGZOStream& s, const cSC4ShakeDescription& v) {
    s.SetFloat32(v.amplitude);
    s.SetFloat32(v.frequency);
    s << v.curveA;
    s << v.curveB;
    s.SetFloat32(v.falloff);
    s.SetUint8(static_cast<std::uint8_t>(v.flags));
    return s;
}

// ---- decal ----
cIGZIStream& operator>>(cIGZIStream& s, cSC4DecalDescription& v) {
    v.flags = ReadU32(s);
    s.GetUint32(v.textureId);
    s.GetUint8(v.mode);
    s.GetUint8(v.kind);
    s.GetFloat32(v.scale);
    s >> v.sizeCurve;
    s >> v.alphaCurve;
    s >> v.rotationCurve;
    ReadVector3s(s, v.path);
    s >> v.extraCurve;
    for (float& f : v.values) s.GetFloat32(f);
    s >> v.offset;
    if (v.kind == 0) {
        v.kind = 2;
        v.flags |= 0x40;
    }
    return s;
}
cIGZOStream& operator<<(cIGZOStream& s, const cSC4DecalDescription& v) {
    s.SetUint32(static_cast<std::uint32_t>(v.flags));
    s.SetUint32(v.textureId);
    s.SetUint8(v.mode);
    s.SetUint8(v.kind);
    s.SetFloat32(v.scale);
    s << v.sizeCurve;
    s << v.alphaCurve;
    s << v.rotationCurve;
    WriteVector3s(s, v.path);
    s << v.extraCurve;
    for (float f : v.values) s.SetFloat32(f);
    s << v.offset;
    return s;
}

// ---- camera ----
cIGZIStream& operator>>(cIGZIStream& s, cSC4CameraParams& v) {
    std::uint16_t version = 0;
    s.GetUint16(version);
    s >> v.keys;
    for (float& f : v.values) s.GetFloat32(f);
    return s;
}
cIGZOStream& operator<<(cIGZOStream& s, const cSC4CameraParams& v) {
    s.SetUint16(0);
    s << v.keys;
    for (float f : v.values) s.SetFloat32(f);
    return s;
}

// ---- light ----
cIGZIStream& operator>>(cIGZIStream& s, cSC4LightDescription& v) {
    ReadVector3s(s, v.colors);
    s >> v.intensities;
    s.GetFloat32(v.range);
    return s;
}
cIGZOStream& operator<<(cIGZOStream& s, const cSC4LightDescription& v) {
    WriteVector3s(s, v.colors);
    s << v.intensities;
    s.SetFloat32(v.range);
    return s;
}

// ---- message trigger ----
cIGZIStream& operator>>(cIGZIStream& s, cSC4MessageTriggerDescription& v) {
    s.GetUint32(v.message);
    v.name = ReadString(s);
    return s;
}
cIGZOStream& operator<<(cIGZOStream& s, const cSC4MessageTriggerDescription& v) {
    s.SetUint32(v.message);
    s.SetRZCharStr(v.name.c_str());
    return s;
}

// ---- effect ----
cIGZIStream& operator>>(cIGZIStream& s, cSC4EffectDescription::cDescriptionRec& v) {
    v.name = ReadString(s);
    v.kind = ReadU8(s);
    v.type = ReadU32(s);
    LoadLegacyS3DTransform(s, v.transform);
    s.GetUint8(v.flagA);
    s.GetUint8(v.flagB);
    s.GetUint16(v.shortA);
    s.GetUint16(v.shortB);
    for (float& f : v.floats) s.GetFloat32(f);
    s.GetUint16(v.shortC);
    s.GetUint16(v.shortD);
    s.GetSint32(v.intValue);
    return s;
}
cIGZOStream& operator<<(cIGZOStream& s, const cSC4EffectDescription::cDescriptionRec& v) {
    s.SetRZCharStr(v.name.c_str());
    s.SetUint8(static_cast<std::uint8_t>(v.kind));
    s.SetUint32(static_cast<std::uint32_t>(v.type));
    S3DTransformLegacySave(s, v.transform);
    s.SetUint8(v.flagA);
    s.SetUint8(v.flagB);
    s.SetUint16(v.shortA);
    s.SetUint16(v.shortB);
    for (float f : v.floats) s.SetFloat32(f);
    s.SetUint16(v.shortC);
    s.SetUint16(v.shortD);
    s.SetSint32(v.intValue);
    return s;
}

cIGZIStream& operator>>(cIGZIStream& s, cSC4EffectDescription::cEventRec& v) {
    v.type = ReadU32(s);
    v.name = ReadString(s);
    s.GetFloat32(v.delay);
    s.GetUint32(v.flags);
    return s;
}
cIGZOStream& operator<<(cIGZOStream& s, const cSC4EffectDescription::cEventRec& v) {
    s.SetUint32(static_cast<std::uint32_t>(v.type));
    s.SetRZCharStr(v.name.c_str());
    s.SetFloat32(v.delay);
    s.SetUint32(v.flags);
    return s;
}

cIGZIStream& operator>>(cIGZIStream& s, cSC4EffectDescription& v) {
    v.id = ReadU32(s);
    s.GetUint32(v.version);
    const std::uint32_t records = ReadU32(s);
    v.records.resize(records);
    for (std::uint32_t i = 0; i < records; ++i) s >> v.records[i];
    const std::uint32_t events = ReadU32(s);
    v.events.resize(events);
    for (std::uint32_t i = 0; i < events; ++i) s >> v.events[i];
    v.label = ReadString(s);
    for (std::uint32_t& x : v.values) s.GetUint32(x);
    return s;
}
cIGZOStream& operator<<(cIGZOStream& s, const cSC4EffectDescription& v) {
    s.SetUint32(static_cast<std::uint32_t>(v.id));
    s.SetUint32(v.version);
    s.SetUint32(static_cast<std::uint32_t>(v.records.size()));
    for (const auto& r : v.records) s << r;
    s.SetUint32(static_cast<std::uint32_t>(v.events.size()));
    for (const auto& e : v.events) s << e;
    s.SetRZCharStr(v.label.c_str());
    for (std::uint32_t x : v.values) s.SetUint32(x);
    return s;
}

// ---- sequence item, particle pieces ----
cIGZIStream& operator>>(cIGZIStream& s, sc4::cSC4SequenceDescription::cItemRec& v) {
    s >> v.range;
    v.name = ReadString(s);
    return s;
}

cIGZIStream& operator>>(cIGZIStream& s, cSC4ParticlesDescriptionBase::cTractorPoint& v) {
    s >> v.position;
    s >> v.direction;
    s.GetFloat32(v.strength);
    s.GetFloat32(v.radius);
    return s;
}
cIGZOStream& operator<<(cIGZOStream& s, const cSC4ParticlesDescriptionBase::cTractorPoint& v) {
    s << v.position;
    s << v.direction;
    s.SetFloat32(v.strength);
    s.SetFloat32(v.radius);
    return s;
}

cIGZIStream& operator>>(cIGZIStream& s, cSC4ParticlesDescriptionBase::cTimedEffect& v) {
    v.name = ReadString(s);
    s.GetFloat32(v.time);
    return s;
}

cIGZIStream& operator>>(cIGZIStream& s, cSC4DynamicParticleDescription& v) {
    v.type = ReadU32(s);
    v.name = ReadString(s);
    s.GetUint32(v.count);
    s >> v.curve;
    for (float& f : v.values) s.GetFloat32(f);
    return s;
}
cIGZOStream& operator<<(cIGZOStream& s, const cSC4DynamicParticleDescription& v) {
    s.SetUint32(static_cast<std::uint32_t>(v.type));
    s.SetRZCharStr(v.name.c_str());
    s.SetUint32(v.count);
    s << v.curve;
    for (float f : v.values) s.SetFloat32(f);
    return s;
}
