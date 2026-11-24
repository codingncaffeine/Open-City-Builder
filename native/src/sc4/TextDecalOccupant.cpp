#include "sc4/TextDecalOccupant.h"

#include <cmath>

namespace sc4 {

namespace {
ITextDecalHost* g_host = nullptr;

// UpdateDecal()::kTurnsByRotationAndOrientation @ 0x100a294a0: [view rotation][orientation] -> decal turns
const float kTurnsByRotationAndOrientation[16] = {
    0.25f, 0.0f, 0.125f, 0.875f,
    0.75f, 0.0f, 0.625f, 0.875f,
    0.75f, 0.5f, 0.625f, 0.375f,
    0.25f, 0.5f, 0.125f, 0.375f,
};

void ReplaceOverlay(ITextDecalOverlay*& slot, ITextDecalOverlay* fresh) {
    ITextDecalOverlay* old = slot;
    if (old == fresh) return;
    if (fresh) fresh->AddRef();
    slot = fresh;
    if (old) old->Release();
}
}  // namespace

ITextDecalHost* TextDecalHost() { return g_host; }
void SetTextDecalHost(ITextDecalHost* host) { g_host = host; }

// 0x1007114c6 (the wrapper at 0x10071148e just calls it)
cSC4TextDecalOccupant::cSC4TextDecalOccupant() {
    const float lo[3] = {0.0f, 0.0f, 0.0f}, hi[3] = {0.1f, 0.1f, 0.1f};   // the original writes only +0x84..+0x8c
    SetBoundingBox(lo, hi);
}

// 0x100712800 (+ wrappers 0x10071267c, 0x100712686 and the thunks 0x10071270e..0x1007127ca)
cSC4TextDecalOccupant::~cSC4TextDecalOccupant() {
    if (overlayB_) overlayB_->Release();
    if (overlayA_) overlayA_->Release();
    if (texture_) texture_->Release();
}

// 0x1007119d2
bool cSC4TextDecalOccupant::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cISC4TextDecalOccupant) {
        *out = static_cast<cISC4TextDecalOccupant*>(this);
        AddRef();
        return true;
    }
    return cSC4PropOccupant::QueryInterface(iid, out);
}

// 0x1007116b2
bool cSC4TextDecalOccupant::Init() {
    if (!IsInitialized()) {
        SetPropType(kPropTypeTextDecal);
        cSC4PropOccupant::Init();
        ITextDecalHost* host = TextDecalHost();
        if ((cSC4Occupant::state_ & 2) == 0) {         // not loaded from a save
            UpdateOverlayManager();
            ITextDecalViewSet* view = host ? host->AcquireViewSet() : nullptr;
            if (view) {
                if (view->ContainsOccupant(static_cast<cSC4Occupant*>(this)) && host->PropDeveloperAllowsDecals())
                    SetFlag(kTextDecalFlagShown);
                view->Release();
            }
        }
        if (host && host->Messages()) {
            cIGZMessageServer2* server = host->Messages();
            cIGZMessageTarget2* target = this;
            server->AddNotification(target, kMessageTextDecalRefresh);
            server->AddNotification(target, kMessageTextDecalVisibility);
            server->AddNotification(target, kMessageTextDecalViewChanged);
        }
    }
    return true;
}

// 0x100711892
bool cSC4TextDecalOccupant::Shutdown() {
    if (!IsInitialized()) return true;
    ITextDecalHost* host = TextDecalHost();
    if (host && host->Messages()) {
        cIGZMessageServer2* server = host->Messages();
        cIGZMessageTarget2* target = this;
        server->RemoveNotification(target, kMessageTextDecalRefresh);
        server->RemoveNotification(target, kMessageTextDecalVisibility);
        server->RemoveNotification(target, kMessageTextDecalViewChanged);
    }
    if (overlayA_) {
        if (decalA_ != -1) {
            overlayA_->RemoveDecal(decalA_);
            decalA_ = -1;
        }
        ITextDecalOverlay* o = overlayA_;
        overlayA_ = nullptr;
        o->Release();
    }
    if (overlayB_) {
        if (decalB_ != -1) {
            overlayB_->RemoveDecal(decalB_);
            decalB_ = -1;
        }
        ITextDecalOverlay* o = overlayB_;
        overlayB_ = nullptr;
        o->Release();
    }
    if (texture_) {
        ITextTexture* t = texture_;
        texture_ = nullptr;
        t->Release();
    }
    return cSC4PropOccupant::Shutdown();
}

// 0x1007117ae
void cSC4TextDecalOccupant::UpdateOverlayManager() {
    RemoveDecals();
    ITextDecalHost* host = TextDecalHost();
    const std::uint32_t dyn = dynamic_ ? 1u : 0u;
    ReplaceOverlay(overlayB_, host ? host->OverlayManager(dyn + 1 + dyn) : nullptr);
    ReplaceOverlay(overlayA_, host ? host->OverlayManager(dyn * 2) : nullptr);
    UpdateDecal();
}

// 0x100711a4c
bool cSC4TextDecalOccupant::DoMessage(cIGZMessage2* message) {
    ITextDecalHost* host = TextDecalHost();
    cISC4Occupant* self = static_cast<cSC4Occupant*>(this);
    const std::uint32_t type = message->GetType();
    if (type == kMessageTextDecalViewChanged) {
        UpdateOverlayManager();
        ITextDecalViewSet* view = host ? host->AcquireViewSet() : nullptr;
        if (view) {
            if (view->ContainsOccupant(self) && host->PropDeveloperAllowsDecals()) SetFlag(kTextDecalFlagShown);
            view->Release();
        }
    } else if (type == kMessageTextDecalVisibility) {
        ITextDecalViewSet* view = *static_cast<ITextDecalViewSet**>(static_cast<cIGZMessage2Standard*>(message)->GetVoid2());
        if (view->ContainsOccupant(self) && host && host->PropDeveloperAllowsDecals()) {
            if (!IsFlagSet(kTextDecalFlagShown)) SetFlag(kTextDecalFlagShown);
            return cSC4PropOccupant::DoMessage(message);
        }
        if (IsFlagSet(kTextDecalFlagShown)) ClearFlag(kTextDecalFlagShown);
    } else if (type == kMessageTextDecalRefresh) {
        UpdateDecal();
    }
    return cSC4PropOccupant::DoMessage(message);
}

// 0x100711ba0
void cSC4TextDecalOccupant::UpdateDecal() {
    RemoveDecals();
    if (!texture_) return;
    if (!IsFlagSet(kTextDecalFlagShown)) return;
    ITextDecalHost* host = TextDecalHost();
    std::int32_t size[2] = {0, 0};
    texture_->GetSize(size);
    // width = aspect * height * 16 (world units); the half extents follow
    float w = static_cast<float>(size[0]) / static_cast<float>(size[1]);
    w = w * height_;
    w = w * 16.0f;
    width_ = w;
    const float halfW = w * 0.5f;
    float halfH = height_ * 16.0f;
    halfH = halfH * 0.5f;
    const float ground = host->TerrainHeight(pos_[0], pos_[1]);
    float halfX = halfW, halfZ = halfH;
    if (orientation_ == 0) {                  // the extents swap
        halfX = halfH;
        halfZ = halfW;
    }
    // index not range-checked in the original; the table has 16 entries (rotation 0..3, orientation 0..3)
    const float turns = kTurnsByRotationAndOrientation[(static_cast<unsigned>(host->ViewRotation()) * 4u +
                                                        static_cast<unsigned>(orientation_)) & 15u];
    const float lo[3] = {pos_[0] - halfX, ground - 0.1f, pos_[1] - halfZ};
    const float hi[3] = {pos_[0] + halfX, ground + 0.1f, pos_[1] + halfZ};
    SetBoundingBox(lo, hi);

    decalA_ = overlayA_->AddDecal(width_, turns, texture_, pos_);   // both managers get the turns (i386 0x004c73f4)
    if (decalA_ != -1) {
        overlayA_->SetDecalColor(decalA_, color_);
        overlayA_->SetDecalAlpha(decalA_, alpha_);
    }
    decalB_ = overlayB_->AddDecal(width_, turns, texture_, pos_);
    if (decalB_ != -1) {
        overlayB_->SetDecalColor(decalB_, color_);
        overlayB_->SetDecalAlpha(decalB_, alpha_);
    }
    if (host->OccupantManager()) host->OccupantManager()->MoveOccupant(static_cast<cSC4Occupant*>(this), false);
}

// 0x100711e46
void cSC4TextDecalOccupant::RemoveDecals() {
    if (decalA_ != -1) {
        overlayA_->RemoveDecal(decalA_);
        decalA_ = -1;
    }
    if (decalB_ != -1) {
        overlayB_->RemoveDecal(decalB_);
        decalB_ = -1;
    }
}

// 0x100711e9a
void cSC4TextDecalOccupant::MoveDecal() {
    if (!dynamic_) return;
    if (decalA_ != -1) overlayA_->MoveDecal(decalA_, pos_);
    if (decalB_ != -1) overlayB_->MoveDecal(decalB_, pos_);
}

// 0x100711efe (thunks 0x100711f6e, 0x100711f7c)
void cSC4TextDecalOccupant::SetPosition(const float* xyz) {
    pos_[0] = xyz[0];
    pos_[1] = xyz[2];
    cSC4PropOccupant::SetPosition(xyz);
    int cx = 0, cz = 0;                        // the cell lookup result is not used
    if (TextDecalHost()) TextDecalHost()->PositionToCell(pos_[0], pos_[1], cx, cz);
    if (!dynamic_) UpdateDecal();
    else MoveDecal();
}

// 0x100711f8a
void cSC4TextDecalOccupant::SetText(cIGZString& text) {
    text_.AssignFromIGZString(text);
    UpdateTexture();
}

// 0x100712654
void cSC4TextDecalOccupant::GetText(cIGZString& out) { out.AssignFromIGZString(text_); }

// 0x100711fae
void cSC4TextDecalOccupant::UpdateTexture() {
    if (texture_) {                              // drop the old texture first
        ITextTexture* old = texture_;
        texture_ = nullptr;
        old->Release();
    }
    ITextTexture* fresh = nullptr;
    if (TextDecalHost()) TextDecalHost()->CreateTextTexture(text_, fresh);
    texture_ = fresh;
    if (texture_) UpdateDecal();
}

// 0x10071206a
void cSC4TextDecalOccupant::SetTextHeight(float height) {
    if (0.0f < height && height < 8.0f) {
        height_ = height;
        UpdateDecal();
    }
}

// 0x100712094
void cSC4TextDecalOccupant::SetDynamic(bool dynamic) {
    if (dynamic_ == dynamic) return;
    dynamic_ = dynamic;
    UpdateOverlayManager();
}

// 0x1007120b6
void cSC4TextDecalOccupant::SetTextColor(const float* rgb) {
    const bool changed = 0.0001f <= std::fabs(rgb[0] - color_[0]) || 0.0001f <= std::fabs(rgb[1] - color_[1]) ||
                         0.0001f <= std::fabs(rgb[2] - color_[2]);
    if (!changed) return;
    color_[0] = rgb[0];
    color_[1] = rgb[1];
    color_[2] = rgb[2];
    if (!dynamic_) {
        UpdateDecal();
        return;
    }
    if (decalA_ != -1) overlayA_->SetDecalColor(decalA_, color_);
    if (decalB_ != -1) overlayB_->SetDecalColor(decalB_, color_);
}

// 0x1007121ea
void cSC4TextDecalOccupant::SetAlpha(float alpha) {
    if (alpha_ == alpha) return;                 // (NaN counts as different)
    alpha_ = alpha;
    if (!dynamic_) {
        UpdateDecal();
        return;
    }
    if (decalA_ != -1) overlayA_->SetDecalAlpha(decalA_, alpha_);
    if (decalB_ != -1) overlayB_->SetDecalAlpha(decalB_, alpha_);
}

// 0x100712270
bool cSC4TextDecalOccupant::SetOrientation(std::int32_t orientation) {
    if (orientation_ == orientation) return true;
    orientation_ = orientation;
    UpdateDecal();
    return true;
}

// 0x10071228a (thunk 0x100712352)
void cSC4TextDecalOccupant::SetAllFlags(std::uint32_t flags) {
    std::uint32_t changed = static_cast<std::uint32_t>(flags_) ^ flags;
    if ((flags & changed & kTextDecalFlagShown) != 0) {
        // turning "shown" on is refused unless the prop developer allows it
        if (!TextDecalHost() || !TextDecalHost()->PropDeveloperAllowsDecals()) {
            flags &= ~kTextDecalFlagShown;
            changed = flags_;                    // quirk: the original reloads the old flag byte here, not the xor
        }
    }
    cSC4Occupant::SetAllFlags(flags);
    if ((changed & kTextDecalFlagShown) == 0) return;
    if ((flags & kTextDecalFlagShown) == 0) {
        RemoveDecals();
        return;
    }
    if (!overlayA_ || !overlayB_) UpdateOverlayManager();
    else UpdateDecal();
    if (!texture_ && text_.Strlen() != 0) UpdateTexture();
}

// 0x100712360 (thunks 0x100712478..0x1007124a2)
bool cSC4TextDecalOccupant::Write(cIGZOStream& stream) const {
    stream.SetUint16(3);
    if (cSC4PropOccupant::Write(stream)) {
        stream.SetGZStr(text_);
        stream.SetFloat32(pos_[0]);
        stream.SetFloat32(pos_[1]);
        stream.SetFloat32(height_);
        stream.SetFloat32(width_);
        stream.SetFloat32(pos_[0]);
        stream.SetFloat32(pos_[1]);
        stream.SetFloat32(color_[0]);
        stream.SetFloat32(color_[1]);
        stream.SetFloat32(color_[2]);
        stream.SetFloat32(alpha_);
        stream.SetUint8(static_cast<std::uint8_t>(orientation_));
    }
    return !stream.GetError();
}

// 0x1007124b0 (thunks 0x100712612..0x10071263c)
bool cSC4TextDecalOccupant::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (cSC4PropOccupant::Read(stream)) {
        stream.GetGZStr(text_);
        if (version < 3) {
            std::uint32_t unused = 0;            // a field since dropped
            stream.GetUint32(unused);
        }
        stream.GetFloat32(pos_[0]);
        stream.GetFloat32(pos_[1]);
        if (version == 1) {
            std::uint32_t whole = 0;             // version 1 stored the height as an integer
            stream.GetUint32(whole);
            height_ = static_cast<float>(whole);
        } else {
            stream.GetFloat32(height_);
        }
        stream.GetFloat32(width_);
        stream.GetFloat32(pos_[0]);              // the position is stored twice
        stream.GetFloat32(pos_[1]);
        stream.GetFloat32(color_[0]);
        stream.GetFloat32(color_[1]);
        stream.GetFloat32(color_[2]);
        stream.GetFloat32(alpha_);
        std::uint8_t orientation = 0;
        stream.GetUint8(orientation);
        orientation_ = orientation;
    }
    return !stream.GetError();
}

}  // namespace sc4
