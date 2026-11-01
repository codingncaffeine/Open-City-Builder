#include "sc4/EffectComponents.h"

#include "rz/cRZString.h"
#include "sc4/EffectDescriptions.h"

#include <cmath>
#include <cstring>
#include <map>

namespace sc4 {

namespace {
IEffectsHost* g_host = nullptr;
ITerrainBrush* g_terrainBrush = nullptr;
std::map<EffectComponentType, EffectComponentFactory>& Factories() {
    static std::map<EffectComponentType, EffectComponentFactory> factories;
    return factories;
}
cISC4VisualEffectComponent* Make(EffectComponentType type, cISC4ComponentDescription* description) {
    auto it = Factories().find(type);
    return it == Factories().end() || !it->second ? nullptr : it->second(description);
}
std::string ReadString(cIGZIStream& stream) { return ReadEffectString(stream); }
}  // namespace

IEffectsHost* EffectsHost() { return g_host; }
void SetEffectsHost(IEffectsHost* host) { g_host = host; }
void SetTerrainBrush(ITerrainBrush* brush) { g_terrainBrush = brush; }
ITerrainBrush* TerrainBrush() { return g_terrainBrush; }
void SetEffectComponentFactory(EffectComponentType type, EffectComponentFactory factory) { Factories()[type] = std::move(factory); }

// ----------------------------------------------------------------------------------------- descriptions

cISC4VisualEffectComponent* cSC4CameraDescription::Instantiate() { return new cSC4CameraEffect(this); }
cISC4VisualEffectComponent* cSC4SoundDescription::Instantiate() { return new cSC4SoundEffect(this); }
cISC4VisualEffectComponent* cSC4AttractorDescription::Instantiate() { return Make(EffectComponentType::Attractor, this); }
cISC4VisualEffectComponent* cSC4SequenceDescription::Instantiate() { return Make(EffectComponentType::Sequence, this); }
cISC4VisualEffectComponent* cSC4BrushDescription::Instantiate() {
    auto* effect = new cSC4BrushEffect();
    effect->SetDescription(this);
    return effect;
}
cISC4VisualEffectComponent* cSC4ScrubberDescription::Instantiate() { return Make(EffectComponentType::Scrubber, this); }

void cSC4CameraDescription::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    stream.GetUint32(flags);
    stream.GetUint8(zoom);
    stream.GetUint8(rotation);
    stream.GetFloat32(radius);
}
void cSC4CameraDescription::Write(cIGZOStream& stream) const {
    stream.SetUint16(0);
    stream.SetUint32(flags);
    stream.SetUint8(zoom);
    stream.SetUint8(rotation);
    stream.SetFloat32(radius);
}

void cSC4AttractorDescription::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    name = ReadString(stream);
    std::uint8_t byte = 0;
    stream.GetUint8(byte);
    flag = byte != 0;
}
void cSC4AttractorDescription::Write(cIGZOStream& stream) const {
    stream.SetUint16(0);
    stream.SetRZCharStr(name.c_str());
    stream.SetUint8(flag ? 1 : 0);
}

void cSC4SoundDescription::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    stream.GetUint32(flags);
    stream.GetUint32(soundId);
    stream.GetFloat32(interval);
    stream.GetFloat32(duration);
}
void cSC4SoundDescription::Write(cIGZOStream& stream) const {
    stream.SetUint16(0);
    stream.SetUint32(flags);
    stream.SetUint32(soundId);
    stream.SetFloat32(interval);
    stream.SetFloat32(duration);
}

void cSC4SequenceDescription::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    std::uint32_t count = 0;
    stream.GetUint32(count);
    items.clear();
    items.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        stream >> items[i].range;
        items[i].name = ReadString(stream);
    }
    stream.GetUint32(value);
}
void cSC4SequenceDescription::Write(cIGZOStream& stream) const {
    stream.SetUint16(1);
    stream.SetUint32(static_cast<std::uint32_t>(items.size()));
    for (const cItemRec& item : items) {
        stream << item.range;
        stream.SetRZCharStr(item.name.c_str());
    }
    stream.SetUint32(value);
}

void cSC4BrushDescription::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    stream.GetUint32(id);
    stream.GetFloat32(f18);
    stream.GetFloat32(f1c);
    stream.GetUint8(flag);
    stream.GetUint32(u24);
    stream >> v28;
    stream >> v30;
    stream.GetFloat32(f38);
}
void cSC4BrushDescription::Write(cIGZOStream& stream) const {
    stream.SetUint16(0);
    stream.SetUint32(id);
    stream.SetFloat32(f18);
    stream.SetFloat32(f1c);
    stream.SetUint8(flag);
    stream.SetUint32(u24);
    stream << v28;
    stream << v30;
    stream.SetFloat32(f38);
}

void cSC4ScrubberDescription::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    stream.GetUint32(u18);
    stream.GetUint32(u20);
    stream.GetUint32(u24);
    stream.GetUint32(u28);
    stream.GetFloat32(f2c);
    stream.GetFloat32(f30);
    stream.GetUint32(u34);
    stream.GetUint32(u40);
    stream.GetUint32(u44);
    stream.GetUint32(u48);
    stream.GetFloat32(f4c);
    stream >> v50;
    stream.GetFloat32(f58);
    stream.GetFloat32(f5c);
    if (version != 0) {
        stream.GetUint32(u38);
        stream.GetUint32(u3c);
    }
}
void cSC4ScrubberDescription::Write(cIGZOStream& stream) const {
    stream.SetUint16(1);
    stream.SetUint32(u18);
    stream.SetUint32(u20);
    stream.SetUint32(u24);
    stream.SetUint32(u28);
    stream.SetFloat32(f2c);
    stream.SetFloat32(f30);
    stream.SetUint32(u34);
    stream.SetUint32(u40);
    stream.SetUint32(u44);
    stream.SetUint32(u48);
    stream.SetFloat32(f4c);
    stream << v50;
    stream.SetFloat32(f58);
    stream.SetFloat32(f5c);
    stream.SetUint32(u38);
    stream.SetUint32(u3c);
}

// -------------------------------------------------------------------------------------------- helpers

// cS3DTransform::operator*= reduced to the translation: with both flags set the parent translation is rotated
// and scaled by the local transform and added to its own.
cS3DVector3 ComposeTranslation(const cS3DTransform& parent, const cS3DTransform& local) {
    cS3DVector3 t{local.translation[0], local.translation[1], local.translation[2]};
    if (parent.flags == 0) return t;
    if (local.flags == 0) return {parent.translation[0], parent.translation[1], parent.translation[2]};
    const float a = parent.translation[0], b = parent.translation[1], c = parent.translation[2];
    t.x = (local.rotation[0][2] * c + local.rotation[0][1] * b + local.rotation[0][0] * a) * local.scale + local.translation[0];
    t.y = (c * local.rotation[1][2] + b * local.rotation[1][1] + a * local.rotation[1][0]) * local.scale + local.translation[1];
    t.z = (c * local.rotation[2][2] + b * local.rotation[2][1] + a * local.rotation[2][0]) * local.scale + local.translation[2];
    return t;
}

// ------------------------------------------------------------------------------------------ camera

cSC4CameraEffect::cSC4CameraEffect(cSC4CameraDescription* description) : description_(description) {
    if (description_) description_->AddRef();
}

cSC4CameraEffect::~cSC4CameraEffect() {
    if (description_) description_->Release();
}

bool cSC4CameraEffect::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != 0x452294aa && iid != 1) return false;
    *out = static_cast<cIGZMessageTarget2*>(this);
    ++refs_;
    return true;
}

void cSC4CameraEffect::Start(int) {
    if (started_) return;
    IEffectsHost* host = g_host;
    ICameraControl* camera = host ? host->Camera() : nullptr;
    if (camera) {
        const std::uint32_t flags = description_->flags;
        if ((flags & 1) && host->CameraEffectsEnabled() &&
            static_cast<float>(camera->ZoomLevel()) < static_cast<float>(description_->zoom - 1))
            camera->SetZoom(static_cast<float>(description_->zoom));
        if ((flags & 2) && host->CameraEffectsEnabled()) camera->SetRotation(description_->rotation);
        if (flags & 4) {
            applying_ = true;
            camera->SetViewTargetPosition(position_);
            applying_ = false;
        }
        if ((flags & 8) || 0.0f < description_->radius) {
            host->AddNotification(static_cast<cIGZMessageTarget2*>(this), kMessageCameraMoved);
            registered_ = true;
            follow_ = ((description_->flags >> 3) & 1) != 0;
        } else {
            follow_ = false;
            registered_ = false;
        }
    }
    started_ = true;
}

void cSC4CameraEffect::Stop(int) {
    if (!started_) return;
    started_ = false;
    if (registered_) {
        if (g_host) g_host->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kMessageCameraMoved);
        registered_ = false;
    }
}

void cSC4CameraEffect::SetTransforms(const cS3DTransform& parent, const cS3DTransform& local) {
    position_ = ComposeTranslation(parent, local);
}

// While following, every update puts the camera back on the effect.
void cSC4CameraEffect::ApplyEffect(float) {
    if (!follow_) return;
    applying_ = true;
    if (g_host) {
        if (ICameraControl* camera = g_host->Camera()) camera->SetViewTargetPosition(position_);
    }
    applying_ = false;
}

// Parameter 0xe is a ray (origin + direction); within the radius of the ray the effect starts following.
bool cSC4CameraEffect::SetFloatParams(int type, const float* p, int) {
    if (type != 0xe) return false;
    const float radius = description_->radius;
    if (0.0f < radius) {
        const float dx = p[0] - position_.x;
        const float dy = p[1] - position_.y;
        const float dz = p[2] - position_.z;
        const float along = dz * p[5] + dy * p[4] + dx * p[3];
        const float px = -along * p[3] + dx;
        const float py = dy - p[4] * along;
        const float pz = -along * p[5] + dz;
        if (std::sqrt(px * px + py * py + pz * pz) < radius) follow_ = true;
    }
    return true;
}

// The user moving the camera (while we are not the one moving it) stops the following.
bool cSC4CameraEffect::DoMessage(cIGZMessage2* message) {
    if (message->GetType() != kMessageCameraMoved) return false;
    if (!applying_) follow_ = false;
    return true;
}

// ------------------------------------------------------------------------------------------- sound

cSC4SoundEffect::cSC4SoundEffect(cSC4SoundDescription* description) : description_(description) {
    if (description_) description_->AddRef();
}

cSC4SoundEffect::~cSC4SoundEffect() {
    if (description_) description_->Release();
}

void cSC4SoundEffect::Start(int) {
    if (started_) return;
    soundSystem_ = g_host && g_host->HasSoundSystem();
    elapsed_ = 0.0f;
    next_ = 0.0f;
    instanceId_ = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(this));
    if (soundSystem_) g_host->PostSoundEvent(kSoundEventPlay, description_->soundId, instanceId_, 0, 0);
    started_ = true;
}

void cSC4SoundEffect::Stop(int) {
    if (!started_) return;
    started_ = false;
    if (soundSystem_ && g_host) {
        g_host->PostSoundEvent(kSoundEventStop, instanceId_, 0, 0, 0);
        soundSystem_ = false;
    }
}

void cSC4SoundEffect::SetTransforms(const cS3DTransform& parent, const cS3DTransform& local) {
    position_ = ComposeTranslation(parent, local);
}

void cSC4SoundEffect::ApplyEffect(float elapsed) {
    elapsed_ += elapsed;
    if (0.0f < description_->interval && next_ < elapsed_) {
        next_ = elapsed_ + description_->interval;
        if (soundSystem_ && g_host)
            g_host->PostSoundEvent(kSoundEventMove, instanceId_, static_cast<std::int64_t>(position_.x),
                                   static_cast<std::int64_t>(position_.y), static_cast<std::int64_t>(position_.z));
    }
    if (0.0f < description_->duration && description_->duration < elapsed_) Stop(1);
}

// ------------------------------------------------------------------------------------------- brush

cSC4BrushEffect::~cSC4BrushEffect() {
    if (description_) description_->Release();
}

// Each effect gets its own random brush parameters, drawn from the description's two ranges.
void cSC4BrushEffect::SetDescription(cSC4BrushDescription* description) {
    if (description_ != description) {
        if (description) description->AddRef();
        cSC4BrushDescription* old = description_;
        description_ = description;
        if (old) old->Release();
    }
    if (ITerrainBrush* brush = g_terrainBrush) {
        randomA_ = static_cast<float>(brush->RandomRange(description_->v28.x, description_->v28.y));
        randomB_ = static_cast<float>(brush->RandomRange(description_->v30.x, description_->v30.y));
    }
}

void cSC4BrushEffect::Start(int) {
    started_ = true;
    brushBegun_ = false;
    previous_ = position_;
    accumulator_ = 0.0f;
    elapsed_ = 0.0f;
}

void cSC4BrushEffect::Stop(int) {
    if (brushBegun_) {
        if (g_terrainBrush) g_terrainBrush->EndBrush();
        brushBegun_ = false;
    }
    started_ = false;
}

void cSC4BrushEffect::SetTransforms(const cS3DTransform& parent, const cS3DTransform& local) {
    const cS3DVector3 t = ComposeTranslation(parent, local);
    position_.x = t.x;
    position_.y = t.z;
}

void cSC4BrushEffect::ApplyBrush(int count) {
    ITerrainBrush* terrain = g_terrainBrush;
    if (!terrain) return;
    if (!terrain->PositionInCity(previous_.x, previous_.y) || !terrain->PositionInCity(position_.x, position_.y)) return;
    for (; count != 0; --count) {
        TerrainBrushParams params;
        params.u24 = description_->u24;
        params.last = count == 1;
        params.randomA = randomA_;
        params.randomB = randomB_;
        params.mode = 3;
        params.amount = description_->f38;
        params.from = previous_;
        params.to = position_;
        terrain->ApplyBrush(description_->id, params);
    }
    previous_ = position_;
}

void cSC4BrushEffect::ApplyEffect(float elapsed) {
    if (description_->flag != 0) {                 // one shot
        started_ = false;
        ApplyBrush(static_cast<int>(description_->f18));
        return;
    }
    if (!brushBegun_) {
        if (g_terrainBrush) g_terrainBrush->BeginBrush();
        brushBegun_ = true;
    }
    accumulator_ = description_->f18 * elapsed + accumulator_;
    elapsed_ += elapsed;
    // floor via the float-add rounding trick of the original (round(x - 0.5) with ties to even, then x == r + 1)
    const float biased = accumulator_ - 0.5f + 12582912.0f;
    std::int32_t bits;
    std::memcpy(&bits, &biased, sizeof bits);
    std::int32_t whole = bits - 0x4b400000;
    const std::int32_t next = bits - 0x4b3fffff;
    if (accumulator_ == static_cast<float>(next)) whole = next;
    ApplyBrush(whole);
    accumulator_ -= static_cast<float>(whole);
    if (0.0f < description_->f1c && description_->f1c < elapsed_) started_ = false;
}

}  // namespace sc4
