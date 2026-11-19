#include "sc4/PropOccupantVisualEffect.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "s3d/S3DMath.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/ServiceLocator.h"

namespace sc4 {

namespace {
constexpr float kHalfPi = 1.5707964f;   // the original's 0x3fc90fdb
}  // namespace

IPropVisualEffectHost* PropVisualEffectHost() { return dynamic_cast<IPropVisualEffectHost*>(SL::spEffectsManager); }

cSC4PropOccupantVisualEffect::cSC4PropOccupantVisualEffect() = default;

cSC4PropOccupantVisualEffect::~cSC4PropOccupantVisualEffect() {
    // The original frees the name string first and then releases the effect (it never stops it).
    if (effect_) {
        cSC4VisualEffect* old = effect_;
        effect_ = nullptr;
        old->Release();
    }
}

std::uint32_t cSC4PropOccupantVisualEffect::AddRef() { return cSC4PropOccupant::AddRef(); }

std::uint32_t cSC4PropOccupantVisualEffect::Release() { return cSC4PropOccupant::Release(); }

bool cSC4PropOccupantVisualEffect::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZCLSID_cSC4PropOccupantVisualEffect || iid == kGZIID_cISC4PropOccupantVisualEffect) {
        *out = static_cast<cISC4PropOccupantVisualEffect*>(this);
        AddRef();
        return true;
    }
    return cSC4PropOccupant::QueryInterface(iid, out);
}

cISC4Occupant* cSC4PropOccupantVisualEffect::AsOccupant() {
    return static_cast<cISC4Occupant*>(static_cast<cSC4PropOccupant*>(this));
}

cSC4PropOccupant* cSC4PropOccupantVisualEffect::AsPropOccupant() {
    return static_cast<cSC4PropOccupant*>(this);
}

bool cSC4PropOccupantVisualEffect::Init() {
    if (IsInitialized()) return true;
    const cISCPropertyHolder* props = AsPropertyHolder();

    // The box size property (3 floats, default 16 m) becomes the box maximum; the box minimum is 0 and the old
    // centre is restored through SetPosition, exactly like the original's direct stores at prop +0x70..+0x7c.
    std::vector<float> size;
    float maximum[3] = {16.0f, 16.0f, 16.0f};
    if (cSCPropertyHelp::GetPropertyValue(props, kPropertyPropEffectBoxSize, size) && size.size() == 3) {
        maximum[0] = size[0];
        maximum[1] = size[1];
        maximum[2] = size[2];
    }
    float position[3] = {0.0f, 0.0f, 0.0f};
    GetPosition(position);
    const float minimum[3] = {0.0f, 0.0f, 0.0f};
    SetBoundingBox(minimum, maximum);
    SetPosition(position);   // virtual in the original (the PropOccupant table's +0xf8 -> this class's thunk)

    nameVariantCount_ = 0;
    typeVariantCount_ = 0;
    if (cSCPropertyHelp::GetPropertyValue(props, kPropertyPropEffectName, effectName_)) {
        nameVariantCount_ = 1;
        for (std::uint32_t id = kPropertyPropEffectName + 1;
             id < kPropertyPropEffectName + kPropEffectVariantCount && props->HasProperty(id); ++id)
            ++nameVariantCount_;
        effectActive_ = true;
    } else if (cSCPropertyHelp::GetPropertyValue(props, kPropertyPropEffectType, effectType_)) {
        typeVariantCount_ = 1;
        for (std::uint32_t id = kPropertyPropEffectType + 1;
             id < kPropertyPropEffectType + kPropEffectVariantCount && props->HasProperty(id); ++id)
            ++typeVariantCount_;
        effectActive_ = true;
    }

    if (!IsInitialized()) cSC4Occupant::Init();
    if (effectName_.Strlen() != 0 || effectType_ != 0) RestartEffect();
    return true;
}

bool cSC4PropOccupantVisualEffect::Shutdown() {
    if (IsInitialized()) {
        if (effect_) {
            effect_->Stop(0);
            cSC4VisualEffect* old = effect_;
            effect_ = nullptr;
            old->Release();
        }
        cSC4PropOccupant::Shutdown();
    }
    return true;
}

bool cSC4PropOccupantVisualEffect::Write(cIGZOStream& stream) const {
    stream.SetUint16(kPropVisualEffectRecordVersion);
    if (cSC4PropOccupant::Write(stream)) {
        stream.SetUint8(static_cast<std::uint8_t>(effectActive_));
        stream.SetUint8(static_cast<std::uint8_t>(defaultTransition_));
        if (nameVariantCount_ == 0 && typeVariantCount_ == 0) {
            stream.SetGZStr(effectName_);
        } else {
            // A record whose name comes from a variant property stores an empty name; Init re-reads it later.
            const cRZString empty;
            stream.SetGZStr(empty);
        }
        stream.SetUint32(effectType_);
    }
    return !stream.GetError();
}

bool cSC4PropOccupantVisualEffect::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != kPropVisualEffectRecordVersion) return false;
    if (cSC4PropOccupant::Read(stream)) {
        std::uint8_t active = 0, transition = 0;
        stream.GetUint8(active);
        stream.GetUint8(transition);
        effectActive_ = active != 0;
        defaultTransition_ = transition != 0;
        stream.GetGZStr(effectName_);
        stream.GetUint32(effectType_);
    }
    return !stream.GetError();
}

void cSC4PropOccupantVisualEffect::SetPosition(const float* xyz) {
    cSC4PropOccupant::SetPosition(xyz);
    UpdateVisualEffectTransform();
}

bool cSC4PropOccupantVisualEffect::SetOrientation(std::int32_t orientation) {
    cSC4PropOccupant::SetOrientation(orientation);   // same clamp to 0..3 as the original's inline copy
    UpdateVisualEffectTransform();
    return true;
}

void cSC4PropOccupantVisualEffect::SetState(std::uint32_t state) {
    if (GetState() == static_cast<std::uint8_t>(state)) return;
    cSC4PropOccupant::SetStateRaw(static_cast<std::uint8_t>(state));
    RestartEffect();
}

void cSC4PropOccupantVisualEffect::SetAllFlags(std::uint32_t flags) {
    if (effect_ && effectActive_ && ((cSC4Occupant::GetFlags() ^ flags) & kOccupantFlagShown) != 0) {
        if ((flags & kOccupantFlagShown) == 0) {
            effect_->Stop(1);
        } else {
            effect_->Start(defaultTransition_ ? 0 : 1);
            defaultTransition_ = false;
        }
    }
    cSC4Occupant::SetAllFlags(flags);
}

void cSC4PropOccupantVisualEffect::SetEffectType(const char* name) {
    const std::size_t length = std::strlen(name);
    if (length == effectName_.Strlen() && std::strcmp(effectName_.Data(), name) == 0) return;
    effectType_ = 0;
    effectName_.AssignFromCString(name);
    nameVariantCount_ = 0;
    typeVariantCount_ = 0;
    RestartEffect();
}

void cSC4PropOccupantVisualEffect::SetEffectType(std::uint32_t type) {
    if (effectType_ == type) return;
    effectName_.Erase(0, 0xffffffffu);
    effectType_ = type;
    nameVariantCount_ = 0;
    typeVariantCount_ = 0;
    RestartEffect();
}

bool cSC4PropOccupantVisualEffect::StartEffect(bool instant) {
    bool result = false;
    if (!effect_) {
        if (effectActive_) return false;
    } else {
        if ((cSC4Occupant::GetFlags() & kOccupantFlagShown) != 0) effect_->Start(instant ? 0 : 1);
        result = true;
    }
    effectActive_ = true;
    return result;
}

bool cSC4PropOccupantVisualEffect::StopEffect(bool instant) {
    if (!effect_ || !effectActive_) return false;
    if ((cSC4Occupant::GetFlags() & kOccupantFlagShown) != 0) effect_->Stop(instant ? 0 : 1);
    effectActive_ = false;
    return true;
}

void cSC4PropOccupantVisualEffect::UpdateVisualEffectTransform() {
    if (!effect_) return;
    float position[3] = {0.0f, 0.0f, 0.0f};
    GetPosition(position);
    const float orientation = static_cast<float>(GetOrientation());
    const float sine = std::sin(-orientation * kHalfPi);
    const float cosine = std::cos(-orientation * kHalfPi);

    // The original zeros the rotation, fills it from sincosf and then ORs flags 5 into the flags byte; the zero
    // entries come from products with 0.0f, so their signs are part of the result (r01 / r12 are -0.0f, r10 / r21
    // are +0.0f).
    cS3DTransform transform;
    transform.rotation[0][0] = cosine;
    transform.rotation[0][1] = -(cosine * 0.0f);
    transform.rotation[0][2] = sine;
    transform.rotation[1][0] = sine * 0.0f + 0.0f;
    transform.rotation[1][1] = 1.0f;
    transform.rotation[1][2] = -(cosine * 0.0f);
    transform.rotation[2][0] = -sine;
    transform.rotation[2][1] = sine * 0.0f + 0.0f;
    transform.rotation[2][2] = cosine;
    transform.translation[0] = position[0];
    transform.translation[1] = position[1];
    transform.translation[2] = position[2];
    transform.scale = 1.0f;
    transform.flags = nS3D::kTransformTranslation | nS3D::kTransformRotation;
    transform.changeCounter = 2;   // the original's stack store is changeCounter = 0 + 2
    effect_->SetEffectTransform(transform);
}

void cSC4PropOccupantVisualEffect::RestartEffect() {
    const bool wasActive = effectActive_;
    if (effect_) {
        StopEffect(true);
        if (effect_) {
            cSC4VisualEffect* old = effect_;
            effect_ = nullptr;
            old->Release();
        }
    }

    bool created = false;
    if (nameVariantCount_ != 0) {
        created = RestartFromNameVariant();
    } else if (typeVariantCount_ != 0) {
        created = RestartFromTypeVariant();
    } else if (effectName_.Strlen() != 0) {
        created = CreateByName();
    } else {
        created = CreateByType(effectType_);
    }

    if (created) {
        UpdateVisualEffectTransform();
        if (wasActive) StartEffect(true);
    } else {
        effectActive_ = true;
    }
}

bool cSC4PropOccupantVisualEffect::RestartFromNameVariant() {
    if (nameVariantCount_ <= GetState()) return false;
    cSCPropertyHelp::GetPropertyValue(AsPropertyHolder(), kPropertyPropEffectName + GetState(), effectName_);
    if (effectName_.Strlen() == 0) return false;
    if (effectName_.Strlen() == 3 && std::strcmp(effectName_.Data(), kPropEffectOffName) == 0) return false;
    return CreateByName();
}

bool cSC4PropOccupantVisualEffect::RestartFromTypeVariant() {
    if (typeVariantCount_ <= GetState()) return false;
    cSCPropertyHelp::GetPropertyValue(AsPropertyHolder(), kPropertyPropEffectType + GetState(), effectType_);
    return CreateByType(effectType_);
}

bool cSC4PropOccupantVisualEffect::CreateByName() {
    IPropVisualEffectHost* host = PropVisualEffectHost();
    if (!host) return false;   // port guard: the original dereferences SL::spEffectsManager unconditionally
    return host->CreateEffectByName(effectName_.Data(), &effect_);
}

bool cSC4PropOccupantVisualEffect::CreateByType(std::uint32_t typeId) {
    if (typeId == 0) return false;
    IPropVisualEffectHost* host = PropVisualEffectHost();
    if (!host) return false;   // port guard: see CreateByName
    return host->CreateEffectByType(kPropEffectGroupId, typeId, &effect_);
}

}  // namespace sc4
