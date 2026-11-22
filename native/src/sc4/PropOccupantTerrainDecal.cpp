#include "sc4/PropOccupantTerrainDecal.h"

#include <cmath>
#include <cstring>

#include "gz/cIGZIStream.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZOStream.h"
#include "sc4/OverlayManager.h"

namespace sc4 {

namespace {
ITerrainDecalHost* g_host = nullptr;

constexpr float kMillisecondsToSeconds = 0.001f;   // cSC4TrafficAutomaton::kfMinTravelDistance, the same constant
constexpr double kStaticFadeRate = 0.1;            // DOUBLE_00a70da8: at least this many alpha units a second = the land overlay
constexpr std::uint8_t kLoadedFromSave = 0x2;      // cSC4Occupant +0x52 (the original prop +0x36): read from a save
constexpr std::uint8_t kFlagShown = 0x2;           // cSC4Occupant +0x53 flags (the original prop +0x37): flagged by the view

// The animation tick's elapsed milliseconds: its third data word as a float (cIGZMessage2 slot 0x4c in the original).
float TickMilliseconds(cIGZMessage2* message) {
    void* raw = nullptr;
    if (!message->QueryInterface(kGZIID_cIGZMessage2Standard, &raw) || !raw) return 0.0f;
    auto* standard = static_cast<cIGZMessage2Standard*>(raw);
    const auto bits = static_cast<std::uint32_t>(standard->GetData3());
    standard->Release();
    float milliseconds = 0.0f;
    std::memcpy(&milliseconds, &bits, sizeof milliseconds);
    return milliseconds;
}
}  // namespace

void SetTerrainDecalHost(ITerrainDecalHost* host) { g_host = host; }
ITerrainDecalHost* TerrainDecalHost() { return g_host; }

cSC4PropOccupantTerrainDecal::cSC4PropOccupantTerrainDecal() = default;

cSC4PropOccupantTerrainDecal::~cSC4PropOccupantTerrainDecal() {
    if (manager_) manager_->Release();
}

// @ 0x001059fe: the message target (0x452294aa), the decal interface and the class id answer here; the rest is the prop's.
bool cSC4PropOccupantTerrainDecal::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 0x452294aa) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else if (iid == kGZIID_cISC4PropOccupantTerrainDecal || iid == kGZCLSID_cSC4PropOccupantTerrainDecal) {
        *out = static_cast<cISC4PropOccupantTerrainDecal*>(this);
    } else {
        return cSC4PropOccupant::QueryInterface(iid, out);
    }
    AddRef();
    return true;
}

// @ 0x00103146: the texture and its size; the box becomes size x 0.2 x size about the origin (SetPosition moves it).
void cSC4PropOccupantTerrainDecal::SetDecalTexture(const cGZPersistResourceKey& key, float size) {
    size_ = size;
    key_ = key;
    const float half = size * 0.5f;
    const float min[3] = {-half, -0.1f, -half}, max[3] = {half, 0.1f, half};
    SetBoundingBox(min, max);
}

// @ 0x00103714
void cSC4PropOccupantTerrainDecal::SetOpacity(float opacity) {
    opacity_ = opacity;
    if (manager_) manager_->SetOverlayAlpha(overlayId_, opacity);
}

// @ 0x00102b60: the fade from `from` to `to` over `seconds`; action 2 keeps the prop alive (its own reference) until the fade
// ends. The decal ticks from now on (priority 0xc0).
void cSC4PropOccupantTerrainDecal::SetOpacityFade(float seconds, tCompletionAction action, float from, float to) {
    fadeValue_ = from;
    fadeTarget_ = to;
    fadeRate_ = (to - from) / seconds;
    action_ = action;
    if (action == kRemoveOccupant && !selfReference_) {
        AddRef();
        selfReference_ = true;
    }
    if (g_host && !ticking_) {
        g_host->AddTick(TickTarget());
        ticking_ = true;
    }
}

// @ 0x00103750
void cSC4PropOccupantTerrainDecal::SetDecalSize(float size) {
    size_ = size;
    if (!manager_) return;
    cDecalInfo info;
    manager_->DecalInfo(overlayId_, &info);
    info.width = size_;
    manager_->UpdateDecalInfo(overlayId_, info);
}

// @ 0x00103b84: a new decal (not read from a save) gets a 32 m box and no fade; a loaded one that was fading ticks again.
bool cSC4PropOccupantTerrainDecal::Init() {
    if (IsInitialized()) return true;
    const bool loaded = (cSC4Occupant::state_ & kLoadedFromSave) != 0;
    if (!loaded) {
        const float min[3] = {-16.0f, -16.0f, -16.0f}, max[3] = {16.0f, 16.0f, 16.0f};
        SetBoundingBox(min, max);
        fadeRate_ = 0.0f;
    }
    cSC4PropOccupant::Init();
    if (loaded && ticking_ && g_host) g_host->AddTick(TickTarget());
    return true;
}

void cSC4PropOccupantTerrainDecal::DropOverlay() {
    if (!manager_) return;
    manager_->RemoveOverlay(overlayId_);
    overlayId_ = 0xffffffffu;
    manager_->Release();
    manager_ = nullptr;
}

// @ 0x00103650: the overlay goes, the texture is forgotten, the tick stops; the reference a fade took is dropped last.
bool cSC4PropOccupantTerrainDecal::Shutdown() {
    if (!IsInitialized()) return true;
    DropOverlay();
    key_.instance = 0;
    cSC4PropOccupant::Shutdown();
    if (ticking_) {
        if (g_host) g_host->RemoveTick(TickTarget());
        ticking_ = false;
    }
    if (selfReference_) {
        selfReference_ = false;
        Release();
    }
    return true;
}

// @ 0x00105064 (lot-records.md 10.5): version 3, the prop record, then the decal's fields.
bool cSC4PropOccupantTerrainDecal::Write(cIGZOStream& stream) const {
    stream.SetUint16(kTerrainDecalRecordVersion);
    if (cSC4PropOccupant::Write(stream)) {
        stream.SetUint32(overlayId_);
        stream.SetUint32(key_.type);
        stream.SetUint32(key_.group);
        stream.SetUint32(key_.instance);
        stream.SetFloat32(size_);
        stream.SetFloat32(posX_);
        stream.SetFloat32(posZ_);
        stream.SetFloat32(opacity_);
        stream.SetFloat32(fadeValue_);
        stream.SetFloat32(fadeTarget_);
        stream.SetFloat32(fadeRate_);
        stream.SetSint8(static_cast<std::int8_t>(action_));
        stream.SetUint8(selfReference_ ? 1 : 0);
        stream.SetUint8(ticking_ ? 1 : 0);
    }
    return !stream.GetError();
}

// @ 0x001054b0: only version 3; the prop record first.
bool cSC4PropOccupantTerrainDecal::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != kTerrainDecalRecordVersion || !cSC4PropOccupant::Read(stream)) return false;
    std::uint32_t id = 0;
    stream.GetUint32(id);
    overlayId_ = id;
    stream.GetUint32(key_.type);
    stream.GetUint32(key_.group);
    stream.GetUint32(key_.instance);
    stream.GetFloat32(size_);
    stream.GetFloat32(posX_);
    stream.GetFloat32(posZ_);
    stream.GetFloat32(opacity_);
    stream.GetFloat32(fadeValue_);
    stream.GetFloat32(fadeTarget_);
    stream.GetFloat32(fadeRate_);
    std::int8_t action = 0;
    stream.GetSint8(action);
    action_ = static_cast<std::uint8_t>(action);
    std::uint8_t flag = 0;
    stream.GetUint8(flag);
    selfReference_ = flag != 0;
    stream.GetUint8(flag);
    ticking_ = flag != 0;
    return !stream.GetError();
}

// @ 0x00104b8e: the animation tick moves the fade; past its target the action runs (1 removes the overlay, 2 the prop; 0 keeps
// setting the last opacity, as compiled).
bool cSC4PropOccupantTerrainDecal::DoMessage(cIGZMessage2* message) {
    if (!message || message->GetType() != kTerrainDecalTickMessage) return false;
    const float milliseconds = TickMilliseconds(message);
    if (overlayId_ == 0xffffffffu || (cSC4Occupant::flags_ & kFlagShown) == 0 || fadeRate_ == 0.0f) return true;
    fadeValue_ = fadeRate_ * kMillisecondsToSeconds * milliseconds + fadeValue_;
    if ((fadeValue_ - fadeTarget_) * fadeRate_ < 0.0f) {
        opacity_ = fadeValue_;
        manager_->SetOverlayAlpha(overlayId_, fadeValue_);
        return true;
    }
    switch (action_) {
        case kRemoveDecal:
            DropOverlay();
            return true;
        case kRemoveOccupant:
            if (!IsInitialized()) return true;
            PlaceInOccupantManager(false);
            Shutdown();
            return true;
        case kKeepDecal:
            manager_->SetOverlayAlpha(overlayId_, opacity_);
            return true;
        default:
            return true;
    }
}

// @ 0x00104356: the decal follows the prop's x / z.
void cSC4PropOccupantTerrainDecal::SetPosition(const float* xyz) {
    posX_ = xyz[0];
    posZ_ = xyz[2];
    cSC4PropOccupant::SetPosition(xyz);
    if (!manager_) return;
    cDecalInfo info;
    manager_->DecalInfo(overlayId_, &info);
    info.x = posX_;
    info.z = posZ_;
    info.width = size_;
    manager_->UpdateDecalInfo(overlayId_, info);
}

// @ 0x00102e78: when the view's flag 2 changes, the decal joins the overlay manager (land when it fades at 0.1 a second or
// faster, else dynamic land) the first time it is shown with a texture and a size, or is enabled / disabled afterwards.
void cSC4PropOccupantTerrainDecal::SetAllFlags(std::uint32_t flags) {
    if (((cSC4Occupant::flags_ ^ flags) & kFlagShown) != 0) {
        if (!manager_) {
            if ((flags & kFlagShown) != 0 && key_.instance != 0 && 0.0f < size_ && g_host) {
                const int index = static_cast<double>(std::fabs(fadeRate_)) >= kStaticFadeRate ? 2 : 0;
                manager_ = g_host->OverlayManager(index);
                if (manager_) {
                    manager_->AddRef();
                    overlayId_ = manager_->AddDecal(key_, cS3DVector2{posX_, posZ_}, size_, 0.0f);
                    manager_->SetOverlayAlpha(overlayId_, opacity_);
                }
            }
        } else {
            manager_->SetOverlayEnabled(overlayId_, (flags & kFlagShown) != 0);
        }
    }
    cSC4Occupant::SetAllFlags(flags);
}

}  // namespace sc4
