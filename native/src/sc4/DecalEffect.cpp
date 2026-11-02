#include "sc4/DecalEffect.h"

#include <cmath>

#include "rz/cRZRandom.h"
#include "s3d/S3DMath.h"
#include "sc4/OverlayManager.h"

namespace sc4 {
namespace {
IDecalEffectHost* g_host = nullptr;
// cSC4DecalEffect::mRandom (Windows 0x00B22C10, static-init seed 0xAC09C20E: foundations.md).
cRZRandom& DecalRandom() {
    static cRZRandom random(0xac09c20eu);
    return random;
}
bool Flag(const cSC4DecalDescription& description, int bit) { return (description.flags >> bit & 1u) != 0; }
// The magic-constant floor of CurveLookup: (v - 0.5) + 1.5 * 2^23, the integer bits back.
std::uint32_t KeyIndex(float position) { return static_cast<std::uint32_t>(std::floor(position)); }
}  // namespace

IDecalEffectHost* DecalEffectHost() { return g_host; }
void SetDecalEffectHost(IDecalEffectHost* host) { g_host = host; }

float DecalCurve(const std::vector<float>& curve, float phase) {   // i386 0x0078f298
    if (curve.size() == 1) return curve[0];
    const float position = static_cast<float>(curve.size() - 1) * phase;
    const std::uint32_t index = KeyIndex(position);
    const float fraction = position - static_cast<float>(index);
    if (0.0f < fraction) return (curve[index + 1] - curve[index]) * fraction + curve[index];
    return curve[index];
}

cS3DVector3 DecalCurve(const std::vector<cS3DVector3>& curve, float phase) {   // i386 0x0078ed4e
    if (curve.size() == 1) return curve[0];
    const float position = static_cast<float>(curve.size() - 1) * phase;
    const std::uint32_t index = KeyIndex(position);
    const float fraction = position - static_cast<float>(index);
    if (0.0f < fraction) {
        const cS3DVector3& b = curve[index + 1];
        const cS3DVector3& a = curve[index];
        const float rest = 1.0f - fraction;
        return cS3DVector3{fraction * b.x + rest * a.x, fraction * b.y + rest * a.y, fraction * b.z + rest * a.z};
    }
    return curve[index];
}

cSC4DecalEffect::~cSC4DecalEffect() {   // 0x004130e6
    if (overlays_) overlays_->Release();
    if (texture_) texture_->Release();
}

void cSC4DecalEffect::Init() {   // 0x00412690
    started_ = false;
    handle_ = 0xffffffffu;
}

void cSC4DecalEffect::Shutdown() {   // 0x00412620
    if (started_) Stop(1);
}

void cSC4DecalEffect::SetDescription(const cSC4DecalDescription& description) {   // 0x0041318e
    description_ = description;
    inverseLife_ = 1.0f / (description.life <= 0.0f ? 0.0001f : description.life);
    if (texture_) {
        texture_->Release();
        texture_ = nullptr;
    }
    externalOpacity_ = 1.0f;
    externalSize_ = 1.0f;
    cRZRandom& random = DecalRandom();
    randomOpacity_ = static_cast<float>(random.RandomDoubleRangeUniform(1.0 - static_cast<double>(description_.opacityVariance),
                                                                        static_cast<double>(description_.opacityVariance) + 1.0));
    randomSize_ = static_cast<float>(random.RandomDoubleRangeUniform(1.0 - static_cast<double>(description_.sizeVariance),
                                                                     static_cast<double>(description_.sizeVariance) + 1.0));
    if (description_.rotationVariance <= 0.0f)
        randomRotation_ = 1.0f;
    else
        randomRotation_ = static_cast<float>(random.RandomDoubleRangeUniform(static_cast<double>(-description_.rotationVariance),
                                                                             static_cast<double>(description_.rotationVariance)));
    mappingScale_ = 1.0f;
    aspect_ = 1.0f;
    x_ = 0.0f;
    z_ = 0.0f;
    size_ = randomSize_;
    angle_ = 0.0f;
    explicitPlacement_ = false;
    if (Flag(description_, 4)) {   // the whole city
        explicitPlacement_ = true;
        const float sizeZ = g_host ? g_host->CityWorldSizeZ() : 0.0f;
        const float sizeX = g_host ? g_host->CityWorldSizeX() : 0.0f;
        x_ = sizeX * 0.5f;
        z_ = 0.5f * sizeZ;
        size_ = g_host ? g_host->CityWorldSizeX() : 0.0f;
    }
}

void cSC4DecalEffect::Start(int transition) {   // 0x00412b26
    if (started_) return;
    float phase = 0.0f;
    if (transition != 0) {
        if (description_.lifetime == 2) return;
        if (description_.lifetime == 3) phase = 1.0f;
    }
    if (!g_host || !g_host->HasTerrainView()) return;
    if (!overlays_) {
        const bool water = Flag(description_, 2);
        const int index = Flag(description_, 6) ? (water ? 1 : 0) : (water ? 3 : 2);
        overlays_ = g_host->OverlayManager(index);
        if (!overlays_) return;
        overlays_->AddRef();
    }
    const float size = size_ * DecalCurve(description_.sizeCurve, phase);
    const float turns = angle_ + randomRotation_ * DecalCurve(description_.rotationCurve, phase);
    const cS3DVector2 centre{x_, z_};
    if (Flag(description_, 5))
        handle_ = overlays_->AddRingDecal(description_.textureId, centre, size, turns);   // vslot 0x1c
    else if (texture_)
        handle_ = overlays_->AddDecal(texture_, centre, size, turns);                      // vslot 0x14
    else
        handle_ = overlays_->AddDecal(description_.textureId, centre, size, turns);        // vslot 0x0c
    if (handle_ == 0xffffffffu) return;

    cDecalInfo info;
    overlays_->DecalInfo(handle_, &info);
    info.aspectAdjust = aspect_ * DecalCurve(description_.aspectCurve, phase);
    if (Flag(description_, 3)) {
        info.uRepeat = mappingScale_ * description_.mapping;
        info.vRepeat = info.uRepeat * info.aspectAdjust;
    } else {
        info.uRepeat = 1.0f;
        info.vRepeat = 1.0f;
    }
    info.uvOffset = description_.offset.x;
    info.userValue = description_.offset.y;
    overlays_->UpdateDecalInfo(handle_, info);
    if (Flag(description_, 1)) overlays_->SetOverlayFlags(handle_, 1);
    overlays_->SetOverlayAlpha(handle_, randomOpacity_ * DecalCurve(description_.opacityCurve, phase) * externalOpacity_);
    overlays_->SetOverlayColor(handle_, DecalCurve(description_.colorCurve, phase));
    overlays_->SetOverlayDrawMode(handle_, static_cast<tOverlayDrawMode>(description_.layer));
    elapsed_ = phase;
    started_ = true;
}

void cSC4DecalEffect::Stop(int) {   // 0x004126b6
    if (!started_) return;
    started_ = false;
    if (handle_ == 0xffffffffu) return;
    overlays_->RemoveOverlay(handle_);
    handle_ = 0xffffffffu;
    overlays_->Release();
    overlays_ = nullptr;
}

void cSC4DecalEffect::SetTransforms(const cS3DTransform& local, const cS3DTransform& world) {   // 0x00412f4a
    if (explicitPlacement_) return;
    cS3DTransform combined = world;   // world * local: the local transform applies first
    nS3D::MultiplyTransform(combined, local);
    x_ = combined.translation[0];
    z_ = combined.translation[2];
    size_ = combined.scale * randomSize_;
    angle_ = std::atan2(-combined.rotation[2][0], combined.rotation[0][0]) * 0.15915494f;   // nS3D::TurnsFromTransform
    if (Flag(description_, 6) && handle_ != 0xffffffffu) {
        cDecalInfo info;
        overlays_->DecalInfo(handle_, &info);
        info.x = x_;
        info.z = z_;
        info.width = size_ * DecalCurve(description_.sizeCurve, 0.0f) * externalSize_;
        info.turns = angle_ + randomRotation_ * DecalCurve(description_.rotationCurve, 0.0f);
        overlays_->UpdateDecalInfo(handle_, info);
    }
}

bool cSC4DecalEffect::SetFloatParams(int type, const float* values, int) {   // 0x0041270a
    if (type == 0xc) {   // an explicit cell rectangle (x0, z0, x1, z1)
        x_ = (values[0] + values[2]) * 8.0f;
        z_ = (values[1] + values[3]) * 8.0f;
        const float width = values[2] - values[0];
        size_ = width * 16.0f;
        angle_ = 0.0f;
        aspect_ = (values[3] - values[1]) / width;
        if (Flag(description_, 3)) mappingScale_ = width;
        explicitPlacement_ = true;
        return true;
    }
    if (type == 0x102) {
        externalSize_ = values[1];
        return true;
    }
    if (type == 3) {
        externalOpacity_ = values[0];
        return true;
    }
    return false;
}

bool cSC4DecalEffect::SetVoidParams(int type, void** values, int) {   // 0x00412660
    if (type != 6) return false;
    auto* texture = static_cast<IOverlayTexture*>(values[0]);
    if (texture != texture_) {
        if (texture) texture->AddRef();
        if (texture_) texture_->Release();
        texture_ = texture;
    }
    return true;
}

void cSC4DecalEffect::SendPlacement(float phase) {
    cDecalInfo info;
    info.x = x_;
    info.z = z_;
    info.width = size_ * DecalCurve(description_.sizeCurve, phase) * externalSize_;
    info.turns = angle_ + randomRotation_ * DecalCurve(description_.rotationCurve, phase);
    info.aspectAdjust = aspect_ * DecalCurve(description_.aspectCurve, phase);
    if (Flag(description_, 3)) {
        info.uRepeat = mappingScale_ * description_.mapping;
        info.vRepeat = info.uRepeat * info.aspectAdjust;
    } else {
        info.uRepeat = 1.0f;
        info.vRepeat = 1.0f;
    }
    info.uvOffset = description_.offset.x;
    info.userValue = description_.offset.y;
    overlays_->UpdateDecalInfo(handle_, info);
}

void cSC4DecalEffect::Update(float seconds) {   // 0x004127fa
    if (handle_ == 0xffffffffu) return;
    const float elapsed = seconds + elapsed_;
    elapsed_ = elapsed;
    float phase = elapsed * inverseLife_;
    if (description_.lifetime == 1) {
        phase -= std::floor(phase);
    } else if (1.0f < phase) {
        if (description_.lifetime == 2) {
            Stop(0);
            return;
        }
        phase = 1.0f;
    }
    if (!Flag(description_, 6)) {
        SendPlacement(phase);
        overlays_->SetOverlayAlpha(handle_, randomOpacity_ * DecalCurve(description_.opacityCurve, phase) * externalOpacity_);
        if (description_.colorCurve.size() > 1) overlays_->SetOverlayColor(handle_, DecalCurve(description_.colorCurve, phase));
    } else {
        overlays_->SetOverlayAlpha(handle_, randomOpacity_ * DecalCurve(description_.opacityCurve, phase) * externalOpacity_);
    }
}

}  // namespace sc4
