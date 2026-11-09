#include "gz/WinBtn.h"

#include <algorithm>

#include "gz/RZDraw.h"

namespace gz {

static IWinButtonHost* g_host = nullptr;
IWinButtonHost* WinButtonHost() { return g_host; }
void SetWinButtonHost(IWinButtonHost* host) { g_host = host; }

cGZWinBtn::cGZWinBtn(cIGZWinMgr* mgr) : cGZWin(mgr) {
    std::fill(fontColors_, fontColors_ + 6, kBtnColorUnset);
    cGZWin::SetFlag(kWinFlag800, false);
    fillColor_ = 0xffc0c0c0;
}

cGZWinBtn::~cGZWinBtn() {
    if (font_) font_->Release();
    if (image_) image_->Release();
}

bool cGZWinBtn::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cIGZWinBtn) {
        *out = static_cast<cIGZWinBtn*>(this);
        AddRef();
        return true;
    }
    return cGZWin::QueryInterface(iid, out);
}

void cGZWinBtn::ReplaceFont(IWinButtonFont* font) {
    if (font_ == font) return;
    if (font) font->AddRef();
    IWinButtonFont* old = font_;
    font_ = font;
    if (old) old->Release();
}

bool cGZWinBtn::Init() {
    if (get_init_flag()) return true;
    cGZWin::Init();
    IWinButtonHost* host = WinButtonHost();
    const bool haveColors = host && host->HasWindowColorManager();
    if ((btnFlags_ & kBtnHasCaption) && haveColors && !font_) {
        SetFont(host->Font(fontStyle_ != 0 ? fontStyle_ : kBtnFontDefaultStyle));
    }
    if (style_ == 0) {
        cIGZBuffer* image = haveColors ? host->Image(kBtnDefaultImageId) : nullptr;
        SetStyle(2, image, image ? image->GetBufferArea() : nullptr);
    }
    if (btnFlags_ & kBtnFitToContent) FitSizeToContent();
    if (haveColors) {
        for (int i = 0; i < 3; ++i) {
            if (soundIds_[i] == 0) SetSoundID(i, host->DefaultSoundId(i));
        }
    }
    return true;
}

bool cGZWinBtn::Shutdown() {
    if (!get_init_flag()) return true;
    if (flashTimer_.IsRunning()) flashTimer_.Stop();
    HideTip();
    HideWindow();
    if (image_) {
        cIGZBuffer* image = image_;
        image_ = nullptr;
        image->Release();
    }
    if (font_) {
        IWinButtonFont* font = font_;
        font_ = nullptr;
        font->Release();
    }
    cGZWin::Shutdown();
    return true;
}

void cGZWinBtn::HideTip() {
    if (IWinButtonHost* host = WinButtonHost()) host->HideTip();
}

std::uint32_t cGZWinBtn::SetFlag(std::uint32_t flag, bool on) {
    const std::uint32_t result = cGZWin::SetFlag(flag, on);
    if (flag == kWinEnabled) {
        OnLayout(GetW(), GetH());
        InvalidateSelfAndParents();
    } else if (flag == kWinVisible) {
        InvalidateSelfAndParents();
    }
    return result;
}

void cGZWinBtn::SetKeyEquivalent(std::uint32_t key) {
    cGZWin::SetKeyEquivalent(key);
    SetCaption(GetCaption());       // refresh the caption (the accelerator is part of what is shown)
}

void cGZWinBtn::SetArea(int l, int t, int r, int b) {
    const int w = std::max(0, r - l), h = std::max(0, b - t);
    cGZWin::SetArea(l, t, l + w, t + h);
    OnLayout(w, h);
}

void cGZWinBtn::SetCaption(const cIGZString& caption) {
    cGZWin::SetCaption(caption);
    if (caption.Strlen() == 0) {
        SetBtnFlagInternal(kBtnHasCaption, false);
    } else {
        SetBtnFlagInternal(kBtnHasCaption, true);
        UpdateTextSize();
    }
    OnLayout(GetW(), GetH());
}

// ---- flags ----------------------------------------------------------------------------------------------

bool cGZWinBtn::SetBtnFlagInternal(int flag, bool on) {
    const std::uint32_t old = btnFlags_;
    const std::uint32_t updated = on ? (old | flag) : (old & ~static_cast<std::uint32_t>(flag));
    if (updated == old) return true;
    btnFlags_ = updated;
    std::uint32_t command = 0;
    switch (flag) {
        case kBtnOn: command = kBtnCmdOnChanged; break;
        case kBtnPressed: command = kBtnCmdPressedChanged; break;
        case kBtnHover: command = kBtnCmdHoverChanged; break;
        case kBtnRepaint: InvalidateSelfAndParents(); return true;
        case kBtnHasCaption: case kBtnFitToContent: case kBtnWrap: OnLayout(GetW(), GetH()); return true;
        default: return true;
    }
    SendMsg(notify_, kGZMsgCommand, command, GetID(), on);
    OnLayout(GetW(), GetH());
    return true;
}

// ---- style and image -------------------------------------------------------------------------------------

bool cGZWinBtn::SetStyle(int style, cIGZBuffer* image, const cRZRect* area) {
    if (style == 0) return false;
    style_ = style;
    int frames;
    if (style == 2 || style == 3) {
        layoutMode_ = 1;
        frames = 4;
    } else if (style == 4) {
        layoutMode_ = 2;
        SetBtnFlag(kBtnShiftWhenPressed, false);   // i386 0x001de7f0: vslot 0x14 of the button's own interface (was read as the window's enabled flag)
        frames = 8;
    } else if (style == 1) {
        layoutMode_ = 0;
        return true;
    } else {
        return false;
    }
    if (image_ != image) {
        if (image) image->AddRef();
        cIGZBuffer* old = image_;
        image_ = image;
        if (old) old->Release();
    }
    if (!area) {
        if (!image) {
            layoutMode_ = 0;
            return false;
        }
        imageArea_.left = 0;
        imageArea_.top = 0;
        imageArea_.right = static_cast<int>(static_cast<std::uint32_t>(image->Width()) / static_cast<std::uint32_t>(frames));
        imageArea_.bottom = image->Height();
    } else {
        imageArea_.left = area->left;
        imageArea_.top = area->top;
        imageArea_.right = (area->right - area->left) / frames + area->left;
        imageArea_.bottom = area->top + area->bottom - area->top;
    }
    return OnLayout(GetW(), GetH());
}

bool cGZWinBtn::SetButtonStyle(int style) {
    SetStyle(style, image_, nullptr);
    InvalidateSelfAndParents();
    return true;
}

bool cGZWinBtn::SetBuffer(cIGZBuffer* image) {
    SetStyle(style_, image, nullptr);
    InvalidateSelfAndParents();
    return true;
}

// ---- font ------------------------------------------------------------------------------------------------

bool cGZWinBtn::SetFont(IWinButtonFont* font) {
    if (font_ == font) return true;
    ReplaceFont(font);
    if (font_) {
        const std::uint32_t a = font_->DefaultTextColor(), b = font_->DefaultShadowColor();
        const std::uint32_t defaults[6] = {a, b, a, b, a, b};
        for (int i = 0; i < 6; ++i) {
            if (fontColors_[i] == kBtnColorUnset) fontColors_[i] = defaults[i];
        }
    }
    OnLayout(GetW(), GetH());
    return true;
}

bool cGZWinBtn::SetFontStyle(std::uint32_t styleId) {
    fontStyle_ = styleId;
    if (get_init_flag() || font_) {
        if (IWinButtonHost* host = WinButtonHost()) SetFont(host->Font(fontStyle_ != 0 ? fontStyle_ : kBtnFontDefaultStyle));
    }
    return true;
}

bool cGZWinBtn::GetFontStyle(std::uint32_t& styleId) {
    if (fontStyle_ == 0) return false;
    styleId = fontStyle_;
    return true;
}

bool cGZWinBtn::SetFontColor(int index, std::uint32_t native) {
    if (fontColors_[index] != native) {
        fontColors_[index] = native;
        InvalidateSelfAndParents();
    }
    return true;
}

bool cGZWinBtn::SetFontColor(int index, cRZColor color) {
    const std::uint32_t native = ConvertPackedRGBToNative(color.rgb);
    if (fontColors_[index] != native) {
        fontColors_[index] = native;
        InvalidateSelfAndParents();
    }
    return true;
}

bool cGZWinBtn::GetFontColor(int index, std::uint32_t& native) const {
    if (fontColors_[index] == kBtnColorUnset) return false;
    native = fontColors_[index];
    return true;
}

bool cGZWinBtn::GetFontColor(int index, cRZColor& color) const {
    if (fontColors_[index] == kBtnColorUnset) return false;
    color.rgb = ConvertNativeToPackedRGB(fontColors_[index]);
    return true;
}

bool cGZWinBtn::SetCaptionStyle(int style) {
    captionStyle_ = style;
    if (static_cast<std::uint32_t>(style) < 3) {
        const std::uint16_t alignments[3] = {1, 0, 2};
        captionAlign_ = alignments[style];
    }
    OnLayout(GetW(), GetH());
    return true;
}

bool cGZWinBtn::SetGutters(std::uint8_t l, std::uint8_t t, std::uint8_t r, std::uint8_t b) {
    gutters_[0] = l; gutters_[1] = t; gutters_[2] = r; gutters_[3] = b;
    OnLayout(GetW(), GetH());
    return true;
}

// ---- text metrics ------------------------------------------------------------------------------------------

void cGZWinBtn::UpdateTextSize() {
    if (!font_) {
        IWinButtonHost* host = WinButtonHost();
        if (!host || !host->HasWindowColorManager()) {
            textW_ = textH_ = 0;
            return;
        }
        if (fontStyle_ != 0) ReplaceFont(host->Font(fontStyle_));
        if (!font_) ReplaceFont(host->Font(kBtnFontFallbackStyle));
        if (!font_) ReplaceFont(host->Font(kBtnFontDefaultStyle));
        if (!font_) {
            textW_ = textH_ = 0;
            return;
        }
    }
    if (!(btnFlags_ & kBtnHasCaption)) {
        textW_ = textH_ = 0;
        return;
    }
    const char* text = caption_.Data();
    const std::uint32_t length = caption_.Strlen();
    cRZRect measured;
    if (!(btnFlags_ & kBtnWrap)) {
        font_->MeasureText(text, length, measured);
        textW_ = static_cast<std::int16_t>(measured.right - measured.left);
        textH_ = static_cast<std::int16_t>(measured.bottom - measured.top);
        return;
    }
    const int imageSpan = style_ == 4 ? imageArea_.right - imageArea_.left : 0;
    const int lines = font_->CountLines(text, length, (GetW() - (gutters_[0] + imageSpan)) - gutters_[2]);
    textH_ = static_cast<std::int16_t>(font_->LineHeight(0, 0) * static_cast<std::int16_t>(lines));
    if (lines == 1) {
        font_->MeasureText(text, length, measured);
        textW_ = static_cast<std::int16_t>(measured.right - measured.left);
    } else {
        textW_ = static_cast<std::int16_t>(font_->WrappedWidth(text, length, textH_));
    }
}

void cGZWinBtn::UpdateTextPos(const cRZRect& rect) {
    switch (captionStyle_) {
        case 2: textX_ = static_cast<std::int16_t>(rect.right - textW_); break;
        case 1: textX_ = static_cast<std::int16_t>(rect.left); break;
        case 0: textX_ = static_cast<std::int16_t>((rect.right - rect.left - textW_) / 2 + rect.left); break;
        default: textX_ = 0; textY_ = 0; return;
    }
    textY_ = static_cast<std::int16_t>((rect.bottom - rect.top - textH_) / 2 + rect.top);
}

// ---- layout ----------------------------------------------------------------------------------------------

// Picks the frame of the image strip for the current state and positions the caption.
bool cGZWinBtn::OnLayout(int w, int h) {
    InvalidateSelfAndParents();
    UpdateTextSize();
    if (static_cast<unsigned>(layoutMode_) > 4) return false;
    const int iw = imageArea_.right - imageArea_.left, ih = imageArea_.bottom - imageArea_.top;
    const int flash = flashCount_;
    const std::uint32_t flags = btnFlags_;
    const bool enabled = GetFlag(kWinEnabled);
    const bool hasCaption = (flags & kBtnHasCaption) != 0;
    sourceX_ = imageArea_.left;
    cRZRect box;
    auto captionBoxFromWindow = [&] { box = {gutters_[0], gutters_[1], w - gutters_[2], h - gutters_[3]}; };
    switch (layoutMode_) {
        case 1: {
            if (enabled) {
                int frame;
                if (style_ == 2) {
                    if (flash & 1) frame = 3;
                    else if (flags & kBtnPressed) frame = 2;
                    else frame = (flags & kBtnHover) ? 3 : 1;
                } else if (flash & 1) {
                    frame = 3;
                } else if ((flags & (kBtnPressed | kBtnOn)) == 0) {
                    frame = (flags & kBtnHover) ? 3 : 1;
                } else {
                    frame = 2;
                }
                sourceX_ += frame * iw;
            }
            imageOffsetX_ = imageOffsetY_ = 0;
            if (!hasCaption) return true;
            captionBoxFromWindow();
            UpdateTextPos(box);
            return true;
        }
        case 2:
        case 4: {
            int frame;
            if (layoutMode_ == 2) {
                if (!enabled) {
                    frame = (flags & kBtnOn) ? 6 : 7;
                } else if (flash & 1) {
                    frame = (flags & kBtnOn) ? 2 : 3;
                } else if (flags & kBtnPressed) {
                    frame = (flags & kBtnOn) ? 4 : 5;
                } else if (flags & kBtnHover) {
                    frame = (flags & kBtnOn) ? 2 : 3;
                } else {
                    frame = (flags & kBtnOn) ? 0 : 1;
                }
                sourceX_ += frame * iw;
            } else if (enabled) {
                if (flags & kBtnPressed) frame = 2;
                else frame = (flags & kBtnOn) ? 3 : 1;
                sourceX_ += frame * iw;
            }
            if (captionStyle_ == 1) {
                imageOffsetX_ = w - iw;
                imageOffsetY_ = (h - ih) / 2;
                if (!hasCaption) return true;
                box = {gutters_[0], gutters_[1], textW_ + gutters_[0], textH_ + gutters_[1]};
                UpdateTextPos(box);
                return true;
            }
            if (captionStyle_ != 2 && captionStyle_ != 0) return false;
            imageOffsetX_ = 0;
            imageOffsetY_ = (h - ih) / 2;
            if (!hasCaption) return true;
            box = {iw + gutters_[0], gutters_[1], textW_ + iw + gutters_[0], textH_ + gutters_[1]};
            UpdateTextPos(box);
            return true;
        }
        case 3: {
            int span = iw;
            if (enabled) {
                if (!(flash & 1)) span <<= (flags >> 12) & 1;
                sourceX_ += span;
            }
            imageOffsetX_ = 0;
            imageOffsetY_ = (h - ih) / 2;
            break;
        }
        default:
            break;
    }
    if (hasCaption) {
        box = {gutters_[0], gutters_[1], GetW() - gutters_[2], GetH() - gutters_[3]};
        UpdateTextPos(box);
    }
    return true;
}

// ---- min / max size -----------------------------------------------------------------------------------------

bool cGZWinBtn::GetMinMaxInfo(cRZRect& out) {
    constexpr int kHuge = 0x7fffffff;
    int iw = imageArea_.right - imageArea_.left, ih = imageArea_.bottom - imageArea_.top;
    const int textMinW = gutters_[2] + gutters_[0] + textW_, textMinH = gutters_[3] + gutters_[1] + textH_;
    const int areaW = drawArea_.right - drawArea_.left, areaH = drawArea_.bottom - drawArea_.top;
    if (style_ == 2 || style_ == 3) {
        if (!(btnFlags_ & kBtnHasCaption)) {
            out.left = iw;
            out.top = ih;
        } else {
            out.left = (btnFlags_ & kBtnWrap) ? areaW : std::max(textMinW, iw);
            out.top = std::max(ih, textMinH);
        }
        out.right = kHuge;
        out.bottom = kHuge;
        return true;
    }
    if (style_ == 4) {
        if (btnFlags_ & kBtnHasCaption) {
            out.left = iw + textW_ + gutters_[2] + gutters_[0];
            out.top = std::max(ih, textMinH);
            out.right = kHuge;
            out.bottom = out.top < 1 ? kHuge : out.top;
        } else {
            out.left = iw;
            out.top = ih;
            out.right = iw < 1 ? kHuge : iw;
            out.bottom = ih < 1 ? kHuge : ih;
        }
        return true;
    }
    if (style_ == 1) {
        out.left = textMinW;
        out.top = textMinH;
        out.right = kHuge;
        out.bottom = kHuge;
        return true;
    }
    if (!image_) {
        if (!(btnFlags_ & kBtnHasCaption)) {
            out.left = iw = areaW;
            out.top = ih = areaH;
        } else {
            out.left = iw = textW_;
            out.top = ih = textH_;
        }
    } else {
        out.left = iw;
        out.top = ih;
        if (btnFlags_ & kBtnHasCaption) {
            if (iw < textW_) { out.left = textW_; iw = textW_; }
            if (ih < textH_) { out.top = textH_; ih = textH_; }
        }
    }
    out.right = iw < 1 ? kHuge : iw;
    out.bottom = ih < 1 ? kHuge : ih;
    return true;
}

bool cGZWinBtn::FitSizeToContent() {
    cRZRect info;
    if (!GetMinMaxInfo(info)) return false;
    if (info.left != 0 || info.top != 0) SetSize(info.left, info.top);
    return true;
}

// ---- tool tips ---------------------------------------------------------------------------------------------

bool cGZWinBtn::SetTipText(const cIGZString& text) {
    if (text.Strlen() == 0) {
        SetBtnFlagInternal(kBtnTipEnabled, false);
        HideTip();
    } else {
        tipText_.Copy(text);
        SetBtnFlagInternal(kBtnTipEnabled, true);
    }
    return true;
}

bool cGZWinBtn::ClearTipText() {
    HideTip();
    tipText_.Erase(0, 0xffffffffu);
    return true;
}

bool cGZWinBtn::GetTipColor(int index, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) const {
    const std::uint32_t c = tipColors_[index];
    r = (c >> 16) & 0xff;
    g = (c >> 8) & 0xff;
    b = c & 0xff;
    return true;
}

bool cGZWinBtn::ShowTip() {
    if (!(btnFlags_ & kBtnTipEnabled) || tipText_.Strlen() == 0) return false;
    BtnTipRequest request;
    request.text = &tipText_;
    request.placement = ((btnFlags_ & 0x600) >> 9) | tipPlacement_;
    if (!(tipPlacement_ & 0x10000)) {
        request.anchor = absoluteArea_;
    } else {
        int x = tipOffsetX_, y = tipOffsetY_;
        WindowToScreenCoordinates(x, y);
        request.anchor = {x, y, x, y};
    }
    request.colors[0] = tipColors_[0];
    request.colors[1] = tipColors_[1];
    request.colors[2] = tipColors_[2];
    if (IWinButtonHost* host = WinButtonHost()) host->ShowTip(request);
    tipHandle_ = 0;
    return true;
}

// ---- sounds ------------------------------------------------------------------------------------------------

void cGZWinBtn::PlaySound(int which) {
    if (soundIds_[which] == 0) return;
    if (IWinButtonHost* host = WinButtonHost()) host->PlaySound(soundIds_[which]);
}

// ---- flashing ----------------------------------------------------------------------------------------------

void cGZWinBtn::FlashTimerCallback(void* button) {
    auto* self = static_cast<cGZWinBtn*>(button);
    cGZMessage message{kBtnMsgFlashTick, 0, 0, 0};
    self->mgr_->PostMessageToTarget(static_cast<cGZWin*>(self), message);
}

void cGZWinBtn::Flash(std::uint32_t count, std::uint32_t periodMs) {
    if (flashTimer_.IsRunning()) flashTimer_.Stop();
    flashCount_ = static_cast<int>(count);
    OnLayout(GetW(), GetH());
    flashTimer_.SetCallbackFunction(&FlashTimerCallback, this);
    flashTimer_.StartPeriodic(periodMs, nullptr);
}

// A flash tick: re-layout and count down (0 stops the timer; the negative counters flash forever).
bool cGZWinBtn::DoMessage(cGZMessage& message) {
    if (message.type != kBtnMsgFlashTick) return cGZWin::DoMessage(message);
    OnLayout(GetW(), GetH());
    int next;
    if (flashCount_ == -2) {
        next = -1;
    } else if (flashCount_ == 0) {
        if (flashTimer_.IsRunning()) flashTimer_.Stop();
        return true;
    } else if (flashCount_ == -1) {
        next = -1;     // the original writes -2 and then adds 1
    } else {
        next = flashCount_ - 1;
    }
    flashCount_ = next;
    return true;
}

// ---- painting ----------------------------------------------------------------------------------------------

void cGZWinBtn::PaintBitmap() {
    const int iw = imageArea_.right - imageArea_.left;
    cRZRect source{sourceX_, imageArea_.top, sourceX_ + iw, imageArea_.bottom};
    int left = drawArea_.left;
    if ((btnFlags_ & 0x24) != 0x20 && drawArea_.right - drawArea_.left != iw && style_ != 4) {
        cRZRect tile{sourceX_, imageArea_.top, iw / 3 + sourceX_, (imageArea_.bottom - imageArea_.top) / 3 + imageArea_.top};
        RZDrawBltEdge(Context(), image_, tile, drawArea_, true);
        return;
    }
    cRZRect dest{left + imageOffsetX_, imageOffsetY_ + drawArea_.top, left + imageOffsetX_ + iw,
                 imageOffsetY_ + drawArea_.top + (imageArea_.bottom - imageArea_.top)};
    Context()->Blt(image_, source, dest);
}

void cGZWinBtn::PaintFlat() {
    const std::uint32_t flags = btnFlags_;
    if (flags & 0x10) return;
    bool raised;
    if (style_ == 3 || style_ == 4) raised = (flags & kBtnPressed) ? false : !(flags & kBtnOn);
    else if (style_ == 2) raised = (flags & kBtnPressed) == 0;
    else raised = true;
    RZDrawRaisedRect(Context(), drawArea_, GetFillColorRGB(), raised, true);
    if (flags & kBtnHover) {
        cRZRect inner{drawArea_.left + 1, drawArea_.top + 1, drawArea_.right - 1, drawArea_.bottom - 1};
        RZDrawRectangleOutline(Context(), inner, 0xffffffffu, 1);
    }
}

void cGZWinBtn::PaintLabel() {
    if (!(btnFlags_ & kBtnHasCaption) || !font_) return;
    const std::uint32_t *c1, *c2;
    if (!GetFlag(kWinEnabled)) { c1 = &fontColors_[4]; c2 = &fontColors_[5]; }
    else if (!(btnFlags_ & kBtnHover)) { c1 = &fontColors_[0]; c2 = &fontColors_[1]; }
    else { c1 = &fontColors_[2]; c2 = &fontColors_[3]; }
    const int shift = (btnFlags_ & kBtnShiftWhenPressed) ? ((btnFlags_ >> 12) & 1) : 0;
    const char* text = caption_.Data();
    const std::uint32_t length = caption_.Strlen();
    IWinBuffer* target = drawBuffer_;
    if (!(btnFlags_ & kBtnWrap)) {
        font_->DrawText(target, drawArea_.left + shift + textX_, shift + drawArea_.top + textY_, text, length, *c1, *c2, &drawArea_, 0);
    } else {
        cRZRect box;
        box.left = drawArea_.left + shift + textX_;
        box.top = shift + drawArea_.top + textY_;
        box.right = textW_ + box.left;
        box.bottom = textH_ + box.top;
        if (box.top < drawArea_.top) box.top = drawArea_.top;
        font_->DrawTextBox(target, box, text, length, captionAlign_, *c1, *c2, &drawArea_, 0);
    }
}

bool cGZWinBtn::GZPaint() {
    if (!image_) PaintFlat();
    else PaintBitmap();
    if (btnFlags_ & kBtnHasCaption) PaintLabel();
    return true;
}

// ---- input -------------------------------------------------------------------------------------------------

bool cGZWinBtn::ButtonActivated() {
    SendMsg(notify_, kGZMsgCommand, kBtnCmdActivated, GetID(), btnFlags_ & kBtnOn);
    return true;
}

bool cGZWinBtn::ButtonRightClicked() {
    SendMsg(notify_, kGZMsgCommand, kBtnCmdRightClicked, GetID(), 0);
    return true;
}

bool cGZWinBtn::GZOnKeyDown(std::uint32_t key, std::uint32_t modifiers) {
    if (key == 0xd) {
        ButtonActivated();
        return true;
    }
    if (key != 0x20) {
        SendMsg(notify_, kGZMsgKeyDown, key, modifiers, 0);
        return true;
    }
    if (!(btnFlags_ & kBtnKeyPressed)) {
        btnFlags_ |= kBtnKeyPressed;
        SendMsg(this, kGZMsgMouseDownL, 1, 1, modifiers);
    }
    return true;
}

bool cGZWinBtn::GZOnKeyUp(std::uint32_t key, std::uint32_t modifiers) {
    if (key == 0x20) {
        btnFlags_ &= ~kBtnKeyPressed;
        SendMsg(this, kGZMsgMouseUpL, 1, 1, modifiers);
    } else {
        SendMsg(notify_, kGZMsgKeyUp, key, modifiers, 0);
    }
    return true;
}

bool cGZWinBtn::GZOnMouseDownL(int, int, std::uint32_t) {
    if (btnFlags_ & kBtnPressed) return true;
    PlaySound(0);
    GZSetCapture();
    HideTip();
    SetBtnFlagInternal(kBtnPressed, true);
    if ((btnFlags_ & (kBtnActOnDown | kBtnRightActsAsLeft)) == kBtnActOnDown) {
        PlaySound(2);
        SetBtnFlagInternal(kBtnOn, !(btnFlags_ & kBtnOn));
        ButtonActivated();
    }
    InvalidateSelfAndParents();
    return true;
}

bool cGZWinBtn::GZOnMouseDownR(int, int, std::uint32_t) {
    if (btnFlags_ & kBtnPressed) return true;
    PlaySound(0);
    GZSetCapture();
    HideTip();
    SetBtnFlagInternal(kBtnPressed, true);
    if (btnFlags_ & kBtnActOnDown) {
        if (!(btnFlags_ & kBtnRightActsAsLeft)) {
            ButtonRightClicked();
        } else {
            PlaySound(2);
            SetBtnFlagInternal(kBtnOn, !(btnFlags_ & kBtnOn));
            ButtonActivated();
        }
    }
    InvalidateSelfAndParents();
    return true;
}

bool cGZWinBtn::GZOnMouseUpL(int x, int y, std::uint32_t) {
    const std::uint32_t before = btnFlags_;
    if (before & kBtnPressed) {
        PlaySound(1);
        SetBtnFlagInternal(kBtnPressed, false);
    }
    if (GZAmCapturing()) {
        if (!(btnFlags_ & kBtnActOnDown)) {
            const bool inside = IsPointInWindowWindowCoordinates(x, y);
            if ((before & kBtnPressed) && inside) {
                PlaySound(2);
                SetBtnFlagInternal(kBtnOn, !(btnFlags_ & kBtnOn));
                ButtonActivated();
            }
        }
        GZReleaseCapture();
    }
    if (before & kBtnPressed) InvalidateSelfAndParents();
    return true;
}

bool cGZWinBtn::GZOnMouseUpR(int x, int y, std::uint32_t) {
    const std::uint32_t before = btnFlags_;
    if (before & kBtnPressed) {
        PlaySound(1);
        SetBtnFlagInternal(kBtnPressed, false);
    }
    if (GZAmCapturing()) {
        if (!(btnFlags_ & kBtnActOnDown)) {
            const bool inside = IsPointInWindowWindowCoordinates(x, y);
            if ((before & kBtnPressed) && inside) {
                if (!(btnFlags_ & kBtnRightActsAsLeft)) {
                    ButtonRightClicked();
                } else {
                    PlaySound(2);
                    SetBtnFlagInternal(kBtnOn, !(btnFlags_ & kBtnOn));
                    ButtonActivated();
                }
            }
        }
        GZReleaseCapture();
    }
    if (before & kBtnPressed) InvalidateSelfAndParents();
    return true;
}

bool cGZWinBtn::GZOnMouseEnter(cIGZWin*) {
    if (!(btnFlags_ & kBtnPressed)) {
        SetBtnFlagInternal(kBtnHover, true);
        ShowTip();
    }
    return true;
}

bool cGZWinBtn::GZOnMouseExit(cIGZWin*) {
    SetBtnFlagInternal(kBtnHover, false);
    HideTip();
    return true;
}

bool cGZWinBtn::GZOnCaptureChanged(cIGZWin*, cIGZWin* current) {
    if (static_cast<cIGZWin*>(this) != current) {
        SetBtnFlagInternal(kBtnHover, false);
        SetBtnFlagInternal(kBtnPressed, false);
        HideTip();
    }
    return true;
}

}  // namespace gz
