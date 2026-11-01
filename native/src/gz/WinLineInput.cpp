#include "gz/WinLineInput.h"

#include <algorithm>
#include <string>

#include "gz/DrawContext.h"
#include "gz/RZDraw.h"
#include "gz/WinBtn.h"
#include "gz/WinCtrlFactory.h"
#include "gz/WinStyle.h"
#include "rz/CharLength.h"
#include "rz/TextEncoding.h"

namespace gz {

namespace {
constexpr std::uint32_t kCursorIBeam = 3;
constexpr int kOutlineInset[3] = {2, 2, 4};   // table at 0x100a1bc14: space the outline takes per type
cIGZFont* FontForStyle(std::uint32_t styleId) {
    IWinStyleManager* styles = WinStyleManager();
    return styles ? static_cast<cIGZFont*>(styles->GetFontStyle(styleId)) : nullptr;
}
std::uint32_t DefaultColor(std::uint32_t id) {
    IWinCtrlFactory* factory = WinCtrlFactory();
    return factory ? factory->DefaultColor(static_cast<int>(id)) : 0;
}
}  // namespace

cGZWinLineInput::cGZWinLineInput(cIGZWinMgr* mgr, bool editable) : cGZWin(mgr), editable_(editable) {
    SetArea(0, 0, 0x80, 0x40);
    SetFillColor(DefaultColor(kLineInputColorFill));
}

cIGZWinLineInput* cGZWinLineInput::CreateNewObject(cIGZWinMgr* mgr) {
    return new cGZWinLineInput(mgr, true);
}

cGZWinLineInput::~cGZWinLineInput() {
    HideWindow();
    SetFont(nullptr);
    cGZWin::Shutdown();
}

bool cGZWinLineInput::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cIGZWinLineInput) {
        *out = static_cast<cIGZWinLineInput*>(this);
        AddRef();
        return true;
    }
    return cGZWin::QueryInterface(iid, out);
}

void cGZWinLineInput::NotifyTextChanged() {
    SendMsg(notify_, kGZMsgCommand, kLineInputTextChanged, GetID(), 0);
}

// ---- lifecycle ---------------------------------------------------------------------------------------

bool cGZWinLineInput::Init() {
    if (get_init_flag()) return true;
    viewStart_ = 0;
    if (WinStyleManager()) {
        if (!font_) SetFont(FontForStyle(kLineInputDefaultFont));
        if (fontColors_[1] == kWinUnsetColor) SetFontColor(kColorSecond, DefaultColor(kLineInputColorSecond));
        if (fontColors_[2] == kWinUnsetColor) SetFontColor(kColorSelected, DefaultColor(kLineInputColorSelected));
        if (GetFillColor() == kWinUnsetColor) SetFillColor(DefaultColor(kLineInputColorFill));
        if (highlightColor_ == kWinUnsetColor) SetHighlightFillColor(DefaultColor(kLineInputColorSelectedBack));
    }
    if (editable_) SetCursor(GetCursorManager()->LoadCursor(kCursorIBeam), false);
    codePage_ = GetCurrentFrameworkCodePage();
    SetSize(GetW(), GetH());
    return cGZWin::Init();
}

bool cGZWinLineInput::Shutdown() {
    HideWindow();
    SetFont(nullptr);
    return cGZWin::Shutdown();
}

void cGZWinLineInput::SetArea(int l, int t, int r, int b) {
    cRZRect minimum;
    GetMinMaxInfo(minimum);
    const int width = std::max(r - l, minimum.left);
    const int height = std::max(b - t, minimum.top);
    cGZWin::SetArea(l, t, l + width, t + height);
}

std::uint32_t cGZWinLineInput::SetFlag(std::uint32_t flag, bool on) {
    if (flag == kWinEnabled) {
        if (!on) AsIGZWin()->SetCursor(nullptr, true);
        else if (editable_) SetCursor(GetCursorManager()->LoadCursor(kCursorIBeam), false);
    }
    return cGZWin::SetFlag(flag, on);
}

// ---- appearance --------------------------------------------------------------------------------------

bool cGZWinLineInput::SetFont(cIGZFont* font) {
    if (font) font->AddRef();
    if (font_) font_->Release();
    font_ = font;
    lineHeight_ = font ? font->CharHeight(nullptr, 0) : 1;
    return true;
}

bool cGZWinLineInput::SetFontColor(tFontColorIdx index, std::uint32_t native) {
    if (index >= 3) return false;
    const std::uint32_t old = fontColors_[index];
    fontColors_[index] = native;
    if (old != native) InvalidateSelfAndParents();
    return true;
}

bool cGZWinLineInput::GetFontColor(tFontColorIdx index, std::uint32_t& native) const {
    if (index >= 3 || fontColors_[index] == kWinUnsetColor) return false;
    native = fontColors_[index];
    return true;
}

bool cGZWinLineInput::SetFilled(bool filled) {
    filled_ = filled;
    InvalidateSelfAndParents();
    return true;
}

bool cGZWinLineInput::SetOutlineColor(tOutlineColorIdx index, std::uint32_t native) {
    const std::uint32_t packed = ConvertNativeToPackedRGB(native);
    if (index >= 4) return false;
    const std::uint32_t old = outlineColors_[index];
    outlineColors_[index] = packed;
    if (old != packed) InvalidateSelfAndParents();
    return true;
}

bool cGZWinLineInput::GetOutlineColor(tOutlineColorIdx index, std::uint32_t& native) const {
    if (index >= 4) return false;
    native = ConvertPackedRGBToNative(outlineColors_[index]);
    return true;
}

bool cGZWinLineInput::SetHighlightFillColor(std::uint32_t native) {
    highlightColor_ = ConvertNativeToPackedRGB(native);
    InvalidateSelfAndParents();
    return true;
}

bool cGZWinLineInput::GetHighlightFillColor(std::uint32_t& native) const {
    native = ConvertPackedRGBToNative(highlightColor_);
    return true;
}

bool cGZWinLineInput::SetHighlightFillAlpha(float alpha) {
    float clamped = 0.0f;
    if (0.0f <= alpha) clamped = alpha;
    float result = 1.0f;
    if (clamped <= 1.0f) result = clamped;
    highlightAlpha_ = result;
    InvalidateSelfAndParents();
    return true;
}

bool cGZWinLineInput::SetGutters(std::int8_t left, std::int8_t top, std::int8_t right, std::int8_t bottom) {
    gutterLeft_ = left;
    gutterTop_ = top;
    gutterRight_ = right;
    gutterBottom_ = bottom;
    InvalidateSelfAndParents();
    return true;
}

bool cGZWinLineInput::GetGutters(std::int8_t& left, std::int8_t& top, std::int8_t& right, std::int8_t& bottom) const {
    left = gutterLeft_;
    top = gutterTop_;
    right = gutterRight_;
    bottom = gutterBottom_;
    return true;
}

bool cGZWinLineInput::GetMinMaxInfo(cRZRect& info) {
    int inset = 0;
    if (static_cast<std::uint32_t>(outlineType_) - 1u < 3u) inset = kOutlineInset[outlineType_ - 1];
    info.left = gutterRight_ + gutterLeft_ + inset;
    info.top = gutterBottom_ + gutterTop_ + inset + lineHeight_;
    info.right = 0x7fffffff;
    info.bottom = 0x7fffffff;
    return true;
}

// ---- text --------------------------------------------------------------------------------------------

std::uint32_t cGZWinLineInput::GetText(cIGZString& out) const {
    out.Copy(text_);
    return Length();
}

bool cGZWinLineInput::SetText(const cIGZString* text) {
    if (!text) {
        text_.Erase(0, 0xffffffffu);
    } else {
        text_.Copy(*text);
        if (characterLimit_ < Length()) text_.Resize(characterLimit_);
    }
    if (!changing_) {
        changing_ = true;
        NotifyTextChanged();
        changing_ = false;
    }
    changing_ = false;
    selectionEnd_ = 0;
    viewStart_ = 0;
    cursor_ = 0;
    selectionStart_ = 0;
    InvalidateSelfAndParents();
    return true;
}

bool cGZWinLineInput::SetText(const char* text) {
    cRZString string(text);
    return SetText(&string);
}

bool cGZWinLineInput::SetCursorLoc(std::uint32_t location) {
    if (Length() < location) location = Length();
    cursor_ = selectionStart_ = selectionEnd_ = location;
    return true;
}

bool cGZWinLineInput::SetEditable(bool editable) {
    editable_ = editable;
    SetCursor(editable ? GetCursorManager()->LoadCursor(kCursorIBeam) : nullptr, !editable);
    return true;
}

bool cGZWinLineInput::SetSelection(std::uint32_t start, std::uint32_t count) {
    const std::uint32_t length = Length();
    if (length <= start) start = length == 0 ? 0 : length - 1;
    if (length < start + count) count = length - start;
    selectionStart_ = start;
    selectionEnd_ = count + start;
    cursor_ = count + start;
    UpdateViewLocation();
    InvalidateSelfAndParents();
    return true;
}

bool cGZWinLineInput::GetSelection(std::uint32_t& start, std::uint32_t& count) const {
    start = selectionStart_;
    count = selectionEnd_ - selectionStart_;
    return true;
}

bool cGZWinLineInput::SetSoundID(int which, int soundId) {
    if (which >= 3) return false;
    soundIds_[which] = soundId;
    return true;
}

bool cGZWinLineInput::PlaySoundA(int which) {
    if (soundIds_[which] == 0) return false;
    IWinButtonHost* host = WinButtonHost();
    if (!host) return false;
    host->PlaySound(static_cast<std::uint32_t>(soundIds_[which]));
    return true;
}

// ---- editing -----------------------------------------------------------------------------------------

bool cGZWinLineInput::InsertChar(char c, std::uint32_t position) {
    const std::uint32_t length = Length();
    if (characterLimit_ <= length) {
        PlaySoundA(1);
    } else {
        text_.InsertChars(position, &c, 1);
        NotifyTextChanged();
    }
    return characterLimit_ > length;
}

bool cGZWinLineInput::DeleteChar(std::uint32_t oneBasedIndex, std::uint32_t count) {
    if (oneBasedIndex == 0) return false;
    if (count == 0 || oneBasedIndex > Length()) return false;
    text_.Erase(oneBasedIndex - 1, count);
    NotifyTextChanged();
    return true;
}

std::uint32_t cGZWinLineInput::FindTextIndexFromCursorLocation(int x) {
    const std::uint32_t length = Length();
    std::uint32_t result = viewStart_;
    const int relative = x - gutterLeft_;
    if (relative != 0 && gutterLeft_ <= x) {
        const int remaining = static_cast<int>(length - viewStart_);
        if (remaining > 0) {
            const char* text = text_.Data();
            int adjust = 0, offset = 0, previousWidth = 0;
            for (;;) {
                const int width = font_->GetStringWidth(text + viewStart_, static_cast<std::uint32_t>(offset));
                if (relative < width) {
                    if (width - relative < relative - previousWidth) adjust = 0;
                    return static_cast<std::uint32_t>(adjust + offset + static_cast<int>(viewStart_));
                }
                const int charLength = GetCharLength(text + viewStart_ + offset, length - (viewStart_ + offset), codePage_);
                offset += charLength;
                if (remaining <= offset) break;
                adjust = -charLength;
                previousWidth = width;
            }
        }
        result = length;
    }
    return result;
}

bool cGZWinLineInput::UpdateViewLocation() {
    const std::uint32_t length = Length();
    if (length == 0) {
        cursor_ = 0;
        viewStart_ = 0;
        return true;
    }
    if (viewStart_ < cursor_) {
        for (;;) {
            const int width = font_->GetStringWidth(text_.Data() + viewStart_, cursor_ - viewStart_);
            if (width <= ((drawArea_.right - drawArea_.left) - gutterLeft_) - gutterRight_) break;
            viewStart_ += static_cast<std::uint32_t>(
                GetCharLength(text_.Data() + viewStart_, Length() - viewStart_, codePage_));
        }
    } else {
        viewStart_ = std::min<std::uint32_t>(cursor_, length - 1);
    }
    return true;
}

// ---- painting ----------------------------------------------------------------------------------------

void cGZWinLineInput::draw_text(int x, int y, std::uint32_t offset, std::uint32_t count, int colorIndex) {
    const std::uint32_t color = fontColors_[colorIndex];
    if (color == kWinUnsetColor) {
        font_->DrawTextLine(FontTarget(drawBuffer_), x, y, text_.Data() + offset, count, kFontDefaultColor, kFontDefaultColor,
                            nullptr, nullptr);
        return;
    }
    const std::string part(text_.Data() + offset, count);
    font_->DrawTextLine(FontTarget(drawBuffer_), x, y, part.c_str(), static_cast<std::uint32_t>(part.size()), color,
                        kFontDefaultColor, nullptr, nullptr);
}

void cGZWinLineInput::paint_outline() {
    auto* context = static_cast<cIGZDrawContext*>(drawContext_);
    if (outlineType_ == kOutlineFlat) {
        RZDrawRectangleOutline(context, drawArea_, ConvertNativeToPackedRGB(outlineColors_[0]), 1);
    } else if (outlineType_ == kOutlineDouble) {
        cRZRect rect = drawArea_;
        RZDraw3DRectangleOutline(context, rect, outlineColors_[0], outlineColors_[1]);
        rect.left += 1;
        rect.top += 1;
        rect.right -= 1;
        rect.bottom -= 1;
        RZDraw3DRectangleOutline(context, rect, outlineColors_[2], outlineColors_[3]);
    } else if (outlineType_ == kOutlineRaised) {
        RZDraw3DRectangleOutline(context, drawArea_, outlineColors_[0], outlineColors_[1]);
    }
}

bool cGZWinLineInput::GZPaint() {
    auto* context = static_cast<cIGZDrawContext*>(drawContext_);
    if (filled_) {
        context->SetColor(GetFillColorRGB());
        context->Fill(drawArea_);
    }
    if (!font_) return false;

    const int left = gutterLeft_ + drawArea_.left;
    const int top = gutterTop_ + drawArea_.top;
    const int right = drawArea_.right - gutterRight_;
    const int bottom = drawArea_.bottom - gutterBottom_;
    const int textY = (bottom - (lineHeight_ + top)) / 2 + top;
    const std::uint32_t length = Length();
    const char* text = text_.Data();

    if (GetFocus() == static_cast<cIGZWin*>(this)) {
        if (selectionStart_ == selectionEnd_) {
            if (length != 0) {
                const std::uint32_t fit =
                    font_->CalculateCharsToFitInWidth(text + viewStart_, length - viewStart_, right - left, nullptr);
                draw_text(left, textY, viewStart_, fit, GetFlag(kWinEnabled) ? 0 : 1);
            }
            if (editable_ && viewStart_ <= cursor_) {
                const int thickness = 2 - (lineHeight_ < 0x18 ? 1 : 0);
                int x = left;
                if (cursor_ != 0) {
                    const int width = font_->GetStringWidth(text + viewStart_, cursor_ - viewStart_);
                    x = ((left - 1) - (thickness >> 1)) + width;
                }
                context->SetColor(0xff000000);
                context->SetLineThickness(static_cast<float>(thickness));
                context->Line(x, thickness + textY, x, (textY - thickness) + lineHeight_);
                context->SetLineThickness(1.0f);
            }
        } else {
            const std::uint32_t fit =
                font_->CalculateCharsToFitInWidth(text + viewStart_, length - viewStart_, right - left, nullptr);
            std::uint32_t low = static_cast<std::uint32_t>(std::min<int>(selectionStart_, selectionEnd_));
            if (static_cast<int>(low) < 0) low = 0;
            if (static_cast<int>(low) < static_cast<int>(viewStart_)) low = viewStart_;
            std::uint32_t high = static_cast<std::uint32_t>(std::max<int>(selectionStart_, selectionEnd_));
            if (static_cast<int>(length) < static_cast<int>(high)) high = length;
            std::uint32_t end = viewStart_ + fit;
            if (static_cast<int>(high) < static_cast<int>(end)) end = high;
            int selectionX0 = low == 0 ? 0 : font_->GetStringWidth(text, low);
            int selectionX1 = font_->GetStringWidth(text, end);
            if (viewStart_ != 0) {
                const int base = font_->GetStringWidth(text, viewStart_);
                selectionX0 -= base;
                selectionX1 -= base;
            }
            if (highlightAlpha_ != 0.0f) {
                cRZRect highlight{selectionX0 + left, top, selectionX1 + left, bottom};
                if (highlightAlpha_ == 1.0f) {   // only an opaque highlight is drawn
                    context->SetColor(highlightColor_);
                    context->Fill(highlight);
                }
            }
            if (fit != 0) draw_text(left, textY, viewStart_, fit, 0);
            const int selected = static_cast<int>(end) - static_cast<int>(low);
            if (selected != 0 && static_cast<int>(low) <= static_cast<int>(end))
                draw_text(selectionX0 + left, textY, low, static_cast<std::uint32_t>(selected), 2);
        }
    } else if (length != 0) {
        const std::uint32_t fit =
            font_->CalculateCharsToFitInWidth(text + viewStart_, length - viewStart_, right - left, nullptr);
        if (fit != 0) draw_text(left, textY, viewStart_, fit, GetFlag(kWinEnabled) ? 0 : 1);
    }
    if (outlineType_ != kOutlineNone) paint_outline();
    return true;
}

// ---- input -------------------------------------------------------------------------------------------

bool cGZWinLineInput::GZOnSetFocus(cIGZWin*) {
    if (editable_) InvalidateSelfAndParents();
    return editable_;
}

bool cGZWinLineInput::GZOnKillFocus(cIGZWin*) {
    InvalidateSelfAndParents();
    return true;
}

bool cGZWinLineInput::GZOnMouseDownL(int x, int y, std::uint32_t flags) {
    if (!editable_) {
        void* raw = nullptr;
        if (parent_ && parent_->QueryInterface(0x412ce496, &raw)) {
            SendMsg(notify_, kGZMsgMouseDownL, static_cast<std::uint64_t>(x), static_cast<std::uint64_t>(y), flags);
            static_cast<cIGZUnknown*>(raw)->Release();
        }
        return true;
    }
    GZSetCapture();
    std::uint32_t index;
    if ((flags & 1) == 0) {
        SetCursorLoc(FindTextIndexFromCursorLocation(x));
        index = cursor_;
        selectionStart_ = index;
    } else {
        index = FindTextIndexFromCursorLocation(x);
        cursor_ = index;
    }
    selectionEnd_ = index;
    UpdateViewLocation();
    InvalidateSelfAndParents();
    return true;
}

bool cGZWinLineInput::GZOnMouseUpL(int x, int, std::uint32_t) {
    if (GZAmCapturing()) {
        GZReleaseCapture();
        selectionEnd_ = FindTextIndexFromCursorLocation(x);
    }
    return true;
}

bool cGZWinLineInput::GZOnMouseMove(int x, int, std::uint32_t) {
    if (GZAmCapturing()) {
        const std::uint32_t index = FindTextIndexFromCursorLocation(x);
        if (selectionEnd_ != index) {
            selectionEnd_ = index;
            InvalidateSelfAndParents();
        }
    }
    return true;
}

bool cGZWinLineInput::GZOnCharacter(char c) {
    if (!editable_) return false;
    if (static_cast<unsigned char>(c) < 0x20) {
        PlaySoundA(0);
        return false;
    }
    if (characterLimit_ != 0xffffffffu) {
        std::uint32_t length = Length();
        const int difference = static_cast<int>(selectionEnd_) - static_cast<int>(selectionStart_);
        if (difference != 0) length -= static_cast<std::uint32_t>(difference < 0 ? -difference : difference);
        if (characterLimit_ <= length) {
            PlaySoundA(1);
            return false;
        }
    }
    if (selectionStart_ != selectionEnd_) GZOnKeyDown(8, 0);
    InsertChar(c, cursor_);
    cursor_ += 1;
    selectionStart_ = selectionEnd_ = cursor_;
    UpdateViewLocation();
    InvalidateSelfAndParents();
    return true;
}

bool cGZWinLineInput::GZOnKeyDown(std::uint32_t key, std::uint32_t modifiers) {
    const bool shift = (modifiers & 1) != 0;
    const char* text = text_.Data();
    // deletes the selection (shared by Backspace and Delete) and parks the cursor at its start
    auto deleteSelection = [&] {
        const int low = std::min<int>(selectionStart_, selectionEnd_);
        if (editable_) {
            const int high = std::max<int>(selectionStart_, selectionEnd_);
            DeleteChar(static_cast<std::uint32_t>(low + 1), static_cast<std::uint32_t>(high - low));
        }
        SetCursorLoc(static_cast<std::uint32_t>(low));
    };
    auto collapseSelection = [&] { selectionStart_ = selectionEnd_ = cursor_; };

    if (key < 0x1b) {
        if (key == 8) {   // Backspace
            if (selectionStart_ != selectionEnd_) {
                deleteSelection();
            } else {
                if (cursor_ == 0 || !editable_) return true;
                const std::uint32_t previous = GetPreviousCharLength(text + cursor_, cursor_, codePage_);
                DeleteChar(cursor_ - previous, previous);
                cursor_ -= previous;
                selectionStart_ = selectionEnd_ = cursor_;
                if (viewStart_ == cursor_) viewStart_ = cursor_ - 10 < 0 ? 0 : cursor_ - 10;
            }
        } else if (key == 9) {   // Tab goes to the parent
            SendMsg(notify_, kGZMsgKeyDown, key, modifiers, 0);
            return true;
        } else if (key == 0xd) {   // Return
            if (editable_) SendMsg(notify_, kGZMsgCommand, kLineInputReturn, GetID(), 0);
            if (selectionStart_ == selectionEnd_) return true;
            collapseSelection();
        } else {
            return true;
        }
    } else {
        switch (key) {
        case 0x23: {   // End
            if (Length() == cursor_) return true;
            cursor_ = selectionStart_ = selectionEnd_ = Length();
            break;
        }
        case 0x24:   // Home
            if (cursor_ == 0) return true;
            cursor_ = 0;
            selectionStart_ = 0;
            selectionEnd_ = 0;
            break;
        case 0x25: {   // Left
            if (cursor_ == 0) {
                if (shift) return true;
                SetCursorLoc(0);
                selectionStart_ = selectionEnd_ = 0;
            } else {
                const std::uint32_t previous = GetPreviousCharLength(text + cursor_, cursor_, codePage_);
                if (!shift) {
                    SetCursorLoc(cursor_ - previous);
                    collapseSelection();
                } else {
                    if (cursor_ != 0) cursor_ -= previous;
                    if (0 < static_cast<int>(selectionEnd_)) selectionEnd_ -= previous;
                }
            }
            break;
        }
        case 0x27: {   // Right
            if (Length() <= cursor_) {
                if (shift) return true;
                SetCursorLoc(Length());
                selectionStart_ = Length();
                selectionEnd_ = Length();
                break;
            }
            const std::uint32_t next = static_cast<std::uint32_t>(GetCharLength(text + cursor_, Length() - cursor_, codePage_));
            if (!shift) {
                SetCursorLoc(cursor_ + next);
                collapseSelection();
            } else {
                if (cursor_ < Length()) cursor_ += next;
                if (static_cast<int>(selectionEnd_) < static_cast<int>(Length())) selectionEnd_ += next;
            }
            break;
        }
        case 0x2e: {   // Delete
            if (selectionStart_ == selectionEnd_) {
                if (!editable_ || Length() == 0) return true;
                const std::uint32_t next =
                    static_cast<std::uint32_t>(GetCharLength(text + cursor_, Length() - cursor_, codePage_));
                DeleteChar(cursor_ + 1, next);
                if (selectionStart_ != selectionEnd_) collapseSelection();
            } else {
                deleteSelection();
            }
            break;
        }
        case 0x1b:   // Escape goes to the parent
            SendMsg(notify_, kGZMsgKeyDown, key, modifiers, 0);
            return true;
        default:
            return true;
        }
    }
    UpdateViewLocation();
    InvalidateSelfAndParents();
    return true;
}

}  // namespace gz
