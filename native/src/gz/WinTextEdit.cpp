#include "gz/WinTextEdit.h"

#include <algorithm>
#include <cstring>

#include "gz/RZDraw.h"
#include "gz/WinBtn.h"
#include "gz/WinCtrlFactory.h"
#include "gz/WinStyle.h"
#include "rz/CharLength.h"
#include "rz/TextEncoding.h"
#include "rz/UTF16Text.h"

namespace gz {

namespace {
IWinTextEditHost* g_host = nullptr;
constexpr std::uint32_t kCursorIBeam = 3;
constexpr std::uint32_t kNone = 0xffffffffu;
bool IsBlankChar(char c) { return c == ' ' || c == '\n'; }
}  // namespace

IWinTextEditHost* WinTextEditHost() { return g_host; }
void SetWinTextEditHost(IWinTextEditHost* host) { g_host = host; }

// ---- construction -------------------------------------------------------------------------------------

cGZWinTextEdit::cGZWinTextEdit(cIGZWinMgr* mgr, int flags) : cGZWin(mgr) { InitializeMemberVariables(flags); }

cGZWinTextEdit::~cGZWinTextEdit() {
    cGZWinTextEdit::Shutdown();
    if (font_) font_->Release();
}

bool cGZWinTextEdit::QueryInterface(std::uint32_t iid, void** out) {   // 0x10025f7d2
    if (iid == kGZIID_cIGZWinTextEdit) {
        *out = static_cast<cIGZWinTextEdit*>(this);
        AddRef();
        return true;
    }
    return cGZWin::QueryInterface(iid, out);
}

void cGZWinTextEdit::InitializeMemberVariables(int flags) {   // 0x10025f872
    maskChar_ = '*';
    codePage_ = GetCurrentFrameworkCodePage();
    gutterX_ = 5;
    maxText_ = 0xffffffffu;
    firstLine_ = 0;
    visibleLines_ = 0;
    wantedPixel_ = -1;
    hScrollPixels_ = 0;
    if (background_) { background_->Release(); background_ = nullptr; }
    stretchBackground_ = true;
    maxUndo_ = 0x4000;
    caretVisible_ = false;
    colors_[6] = 0xff000000;
    colors_[8] = 0xff000000;
    colors_[7] = 0xff000000;
    if (font_) { font_->Release(); font_ = nullptr; }
    fontStyle_ = 0;
    pos_ = TextEditPosition{};
    hScrollChars_ = 0;
    lineHeight_ = 1;
    SetFillColor(0xff, 0xff, 0xff);
    for (int i = 0; i < 6; ++i) ClearFontColor(static_cast<ColorIndex>(i));
    clickTimer_.SetUnits(cRZTimer::kMilliseconds);
    doubleClickMs_ = 500;
    caretPeriod_ = 1000;
    caretTimer_.SetCallbackFunction(&CaretTimerCallback, this);
    scrollTimer_.SetCallbackFunction(&ScrollTimerCallback, this);
    SetFlags(static_cast<std::uint32_t>(flags));
}

void cGZWinTextEdit::CaretTimerCallback(void* self) {   // 0x10025fad6
    auto* edit = static_cast<cGZWinTextEdit*>(self);
    if (cIGZWinMgr* mgr = edit->GetWindowManager()) {
        cGZMessage message;
        message.type = kTextEditMsgCaretBlink;
        mgr->PostMessageToTarget(edit, message);
    }
}

void cGZWinTextEdit::ScrollTimerCallback(void* self) {   // 0x10025fb5e
    auto* edit = static_cast<cGZWinTextEdit*>(self);
    if (cIGZWinMgr* mgr = edit->GetWindowManager()) {
        cGZMessage message;
        message.type = kTextEditMsgScrollTick;
        mgr->PostMessageToTarget(edit, message);
    }
}

void cGZWinTextEdit::Notify(std::uint32_t type, std::uint64_t p2, std::uint64_t p3) {
    SendMsg(notify_, type, reinterpret_cast<std::uint64_t>(static_cast<cIGZWinTextEdit*>(this)), p2, p3);
}

// ---- lifecycle ----------------------------------------------------------------------------------------

bool cGZWinTextEdit::Init() {   // 0x10025fb9c
    if (get_init_flag()) return true;
    cGZWin::Init();
    if (!font_) {
        if (IWinStyleManager* styles = WinStyleManager()) {
            auto* font = static_cast<cIGZFont*>(styles->GetFontStyle(fontStyle_ ? fontStyle_ : kTextEditDefaultFontStyle));
            if (!font) font = static_cast<cIGZFont*>(styles->GetFontStyle(kTextEditDefaultFontStyle));
            SetFont(font);
        }
    }
    lineStarts_.reserve(0x10);
    visibleWidth_ = CalculateVisibleWidth();
    if (flags_ & kTEEditable) SetCursor(GetCursorManager()->LoadCursor(kCursorIBeam), false);
    if (IsVisible() && (flags_ & 0x41) == 0x41 && !caretTimer_.IsRunning())
        caretTimer_.StartPeriodic(caretPeriod_ >> 1, static_cast<cIGZWinTextEdit*>(this));
    codePage_ = GetCurrentFrameworkCodePage();
    RecalculateAll();
    return true;
}

bool cGZWinTextEdit::Shutdown() {   // 0x10025feb0
    if (font_) { cIGZFont* f = font_; font_ = nullptr; f->Release(); }
    if (caretTimer_.IsRunning()) caretTimer_.Stop();
    if (scrollTimer_.IsRunning()) scrollTimer_.Stop();
    if (vScroll_) { vScroll_->Release(); vScroll_ = nullptr; }
    if (hScroll_) { hScroll_->Release(); hScroll_ = nullptr; }
    if (vImage_) { vImage_->Release(); vImage_ = nullptr; }
    if (hImage_) { hImage_->Release(); hImage_ = nullptr; }
    return cGZWin::Shutdown();
}

void cGZWinTextEdit::ShowWindow() {   // 0x100266ed2
    UpdateScrollBarsAndWindowPosition();
    cGZWin::ShowWindow();
}

std::uint32_t cGZWinTextEdit::SetFlag(std::uint32_t flag, bool on) {   // 0x10026300a
    if (flag == kWinVisible) {
        if (!on) {
            if (caretTimer_.IsRunning()) caretTimer_.Stop();
        } else if ((flags_ & 0x41) == 0x41 && !caretTimer_.IsRunning()) {
            caretTimer_.StartPeriodic(caretPeriod_ >> 1, static_cast<cIGZWinTextEdit*>(this));
        }
    }
    return cGZWin::SetFlag(flag, on);
}

void cGZWinTextEdit::SetFlags(std::uint32_t flags) {   // 0x100263148
    const std::uint32_t old = flags_;
    if (old == flags) return;
    if (flags & kTEPassword) flags = (flags & 0xfffffdf9u) | kTESingleLine;
    if (flags & kTESingleLine) flags &= 0xfffffff9u;
    if (flags & kTEWrap) flags &= 0xfffffffbu;
    flags_ = flags;
    if (!get_init_flag()) return;
    const std::uint32_t changed = old ^ flags;
    if (changed & 0x41) {
        if (!IsVisible() || (flags_ & 0x41) != 0x41) {
            if (caretTimer_.IsRunning()) caretTimer_.Stop();
        } else if (!caretTimer_.IsRunning()) {
            caretTimer_.StartPeriodic(caretPeriod_ >> 1, static_cast<cIGZWinTextEdit*>(this));
        }
    }
    if (changed & 0xb) {
        RecalculateAll();
        InvalidateSelfAndParents();
        SetCursor((flags & 1) ? GetCursorManager()->LoadCursor(kCursorIBeam) : nullptr, false);
    }
}

bool cGZWinTextEdit::SetCaretPeriod(std::uint32_t ms) {   // 0x10026650c
    std::uint32_t period = 0xfa;
    if (0xfa < ms) period = ms;
    if (9999 < period) period = 10000;
    caretPeriod_ = period;
    if (get_init_flag() && (flags_ & kTECaretBlink)) {
        ClearFlag(kTECaretBlink);
        SetFlag(kTECaretBlink);
    }
    return true;
}

// ---- appearance ---------------------------------------------------------------------------------------

void cGZWinTextEdit::GetTextArea(cRZRect& out) {   // 0x100260226
    out.left = static_cast<int>(gutterX_);
    out.top = static_cast<int>(gutterY_);
    out.right = visibleWidth_ + static_cast<int>(gutterX_);
    out.bottom = visibleHeight_ + static_cast<int>(gutterY_);
}

void cGZWinTextEdit::SetFont(cIGZFont* font) {   // 0x1002636aa
    cIGZFont* old = font_;
    if (old == font) return;
    if (font) font->AddRef();
    font_ = font;
    if (old) old->Release();
    if (get_init_flag()) {
        RecalculateAll();
        InvalidateSelfAndParents();
    }
}

bool cGZWinTextEdit::SetFontStyle(std::uint32_t styleId) {   // 0x100263730
    fontStyle_ = styleId;
    if (get_init_flag()) {
        if (IWinStyleManager* styles = WinStyleManager()) {
            auto* font = static_cast<cIGZFont*>(styles->GetFontStyle(fontStyle_ ? fontStyle_ : kTextEditDefaultFontStyle));
            if (font) SetFont(font);
        }
    }
    return true;
}

bool cGZWinTextEdit::GetFontStyle(std::uint32_t& styleId) {   // 0x100263798
    if (fontStyle_ == 0) return false;
    styleId = fontStyle_;
    return true;
}

bool cGZWinTextEdit::SetFontColor(std::uint32_t packed) {   // 0x1002637c8
    colors_[0] = ConvertNativeToPackedRGB(packed);
    return true;
}

bool cGZWinTextEdit::SetFontColor(ColorIndex index, std::uint32_t packed) {   // 0x100263816
    return SetColor(index, ConvertNativeToPackedRGB(packed));
}

bool cGZWinTextEdit::GetFontColor(ColorIndex index, std::uint32_t& native) const {   // 0x100263898
    std::uint32_t color = 0;
    const bool ok = GetColor(index, color);
    if (ok) native = ConvertPackedRGBToNative(color);
    return ok;
}

bool cGZWinTextEdit::ClearFontColor(ColorIndex index) {   // 0x100263986
    std::uint32_t value = 0x40000000u;
    if (index == 4) value = 0xffffffffu;
    if ((index | 2) == 2) value = 0xff000000u;
    colors_[index] = value;
    return true;
}

void cGZWinTextEdit::SetFillColor(std::uint32_t nativeColor) {   // 0x100263518
    cGZWin::SetFillColor(nativeColor);
    if (flags_ & kTEFilled) InvalidateSelfAndParents();
}

void cGZWinTextEdit::SetBackgroundImage(cIGZBuffer* image, bool stretch) {   // 0x100263aa6
    bool changed = false;
    if (background_ != image) {
        if (image) image->AddRef();
        if (background_) background_->Release();
        background_ = image;
        changed = true;
    }
    if (stretchBackground_ != stretch) stretchBackground_ = stretch;
    else if (!changed) return;
    InvalidateSelfAndParents();
}

void cGZWinTextEdit::SetOutlineUse(bool on, std::uint32_t packed) {   // 0x100263b42
    flags_ = on ? (flags_ | kTEOutline) : (flags_ & ~static_cast<std::uint32_t>(kTEOutline));
    colors_[8] = ConvertNativeToPackedRGB(packed);
}

void cGZWinTextEdit::GetOutlineUse(bool& on, std::uint32_t& native) const {   // 0x100263bc4
    on = (flags_ & kTEOutline) != 0;
    native = ConvertPackedRGBToNative(colors_[8]);
}

void cGZWinTextEdit::SetTextGutterX(std::uint32_t gutter) {   // 0x100263ffe
    if (gutterX_ == gutter) return;
    gutterX_ = gutter;
    visibleWidth_ = CalculateVisibleWidth();
    RecalculateAll();
    if (get_init_flag()) InvalidateSelfAndParents();
}

void cGZWinTextEdit::SetTextGutterY(std::uint32_t gutter) {   // 0x100264062
    if (gutterY_ == gutter) return;
    gutterY_ = gutter;
    if (get_init_flag()) InvalidateSelfAndParents();
}

bool cGZWinTextEdit::ResizeWindowForExactLineHeights() {   // 0x1002640e2
    const std::uint32_t lineHeight = static_cast<std::uint32_t>(lineHeight_);
    const std::uint32_t inner = static_cast<std::uint32_t>(GetH() - static_cast<int>(gutterY_) * 2);
    std::uint32_t lines = 1;
    if (0 < static_cast<int>(inner / lineHeight)) lines = inner / lineHeight;
    if (lines * lineHeight != inner) SetH(static_cast<int>(lines * lineHeight + gutterY_ * 2));
    SetScrollBarSizes();
    SetScrollBarValues(true);
    InvalidateSelfAndParents();
    return true;
}

int cGZWinTextEdit::CalculateVisibleWidth() {   // 0x10025fd46
    int bar = vScroll_ ? vScroll_->AsIGZWin()->GetW() : 0;
    const std::uint32_t width = static_cast<std::uint32_t>(GetW());
    const std::uint32_t used = static_cast<std::uint32_t>(bar + static_cast<int>(gutterX_) * 2);
    return width < used ? 0 : static_cast<int>(width - used);
}

int cGZWinTextEdit::CalculateVisibleHeight() {   // 0x1002666a0
    int bar = hScroll_ ? hScroll_->AsIGZWin()->GetH() : 0;
    const std::uint32_t height = static_cast<std::uint32_t>(GetH());
    const std::uint32_t used = static_cast<std::uint32_t>(bar + static_cast<int>(gutterY_) * 2);
    return height < used ? 0 : static_cast<int>(height - used);
}

void cGZWinTextEdit::RecalculateAll() {   // 0x10025fdaa
    static int nestedCalls = 0;
    if (!get_init_flag()) return;
    lineHeight_ = font_ ? font_->GetLineHeight() : 0x10;
    visibleHeight_ = CalculateVisibleHeight();
    const std::uint32_t height = static_cast<std::uint32_t>(visibleHeight_);
    std::uint32_t lines = height / static_cast<std::uint32_t>(lineHeight_);
    if (height < static_cast<std::uint32_t>(lineHeight_) && 0 < visibleHeight_) lines = 1;
    visibleLines_ = static_cast<int>(lines);
    visibleWidth_ = CalculateVisibleWidth();
    CalculateAllLineLengths(0);
    SetScrollBarValues(true);
    FixInsertionPositionIfBroken();
    if (!lineStarts_.empty()) {
        if (!(static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(firstLine_))) < lineStarts_.size()))
            firstLine_ = static_cast<std::uint32_t>(lineStarts_.size()) - 1;
    } else {
        firstLine_ = 0;
    }
    if (nestedCalls < 1) {
        ++nestedCalls;
        TestForPossibleScrollBarAdditionOrRemoval();
        --nestedCalls;
    }
}

void cGZWinTextEdit::SetArea(int l, int t, int r, int b) {   // 0x100262c30
    cGZWin::SetArea(l, t, r, b);
    visibleWidth_ = CalculateVisibleWidth();
    RecalculateAll();
    SetScrollBarSizes();
    SetScrollBarValues(true);
    InvalidateSelfAndParents();
}

// ---- line layout --------------------------------------------------------------------------------------

void cGZWinTextEdit::AddFirstLineLength() {   // 0x1002657b0
    if (lineStarts_.empty()) lineStarts_.push_back(0);
}

void cGZWinTextEdit::AdjustLinePositionArray(std::uint32_t length, std::uint32_t lineNumber) {   // 0x100267086
    if (lineStarts_.size() < lineNumber) {
        lineStarts_.resize(lineNumber, length + lineStarts_[lineNumber - 2]);
        return;
    }
    lineStarts_[lineNumber < 2 ? 0 : lineNumber - 1] = lineNumber < 2 ? 0 : length + lineStarts_[lineNumber - 2];
}

void cGZWinTextEdit::CalculateAllLineLengths(std::uint32_t fromLine) {   // 0x100264efa
    if (!font_) return;
    std::uint32_t line = 0;
    if (fromLine <= lineStarts_.size() + 1) line = fromLine;
    if (Size() == 0) {
        lineStarts_.clear();
        pos_.line = 0;
        pos_.column = 0;
        pos_.index = 0;
        return;
    }
    const char* base = Chars();
    const char* p;
    if (line == 0) {
        if (lineStarts_.empty()) AddFirstLineLength();
        p = base;
    } else {
        p = base + lineStarts_[line];
    }
    const char* end = base + Size();
    std::uint32_t lineNumber = line + 1;
    std::uint32_t length = 0;
    while (p < end) {
        if (visibleWidth_ == 0 || (flags_ & kTESingleLine)) {
            length = static_cast<std::uint32_t>(end - p);
        } else if (!(flags_ & kTEWrap)) {
            const char* q = p;
            while (q < end && *q != '\n') ++q;
            const char* limit = end;
            if (q != end) limit = (*q == '\n') ? q + 1 : q;
            length = static_cast<std::uint32_t>(limit - p);
        } else {
            length = font_->CalculateWordsToFitInWidth(p, static_cast<std::uint32_t>(end - p), static_cast<std::uint32_t>(visibleWidth_),
                                                       nullptr, static_cast<cIGZFont::WordWrapMode>(2));
        }
        p += length;
        if (end <= p) {
            if (p[-1] == '\n') {
                ++lineNumber;
                AdjustLinePositionArray(length, lineNumber);
            }
            break;
        }
        ++lineNumber;
        AdjustLinePositionArray(length, lineNumber);
    }
    if (lineStarts_.size() != lineNumber) lineStarts_.resize(lineNumber);
}

std::uint32_t cGZWinTextEdit::LineLength(std::uint32_t line) { return GetCharacterCountOfLine(line); }

std::uint32_t cGZWinTextEdit::GetCharacterCountOfLine(std::uint32_t line) {   // 0x1002646dc
    if (Size() == 0) return 0;
    std::uint32_t last = static_cast<std::uint32_t>(lineStarts_.size()) - 1;
    std::uint32_t next;
    if (line < last) next = lineStarts_[line + 1];
    else { next = Size(); line = last; }
    return next - lineStarts_[line];
}

void cGZWinTextEdit::GetLineAndColumnFromIndex(std::uint32_t& line, std::uint32_t& column, std::uint32_t index) {   // 0x100264752
    if (!lineStarts_.empty() && index != 0 && Size() != 0) {
        const std::uint32_t last = static_cast<std::uint32_t>(lineStarts_.size()) - 1;
        std::uint32_t i = 0;
        while (i < last) {
            if (lineStarts_[i] <= index && index < lineStarts_[i + 1]) {
                line = i;
                column = index - lineStarts_[i];
                return;
            }
            ++i;
        }
        if (Chars()[index - 1] == '\n') {
            line = i;
            column = 0;
        } else {
            line = last;
            column = index - lineStarts_[last];
        }
        return;
    }
    column = 0;
    line = 0;
}

bool cGZWinTextEdit::GetIndexFromLineAndColumn(std::uint32_t line, std::uint32_t column, std::uint32_t& index) {   // 0x100264832
    index = 0;
    if (!lineStarts_.empty() && Size() != 0) {
        if (lineStarts_.size() <= line) line = static_cast<std::uint32_t>(lineStarts_.size()) - 1;
        index = column + lineStarts_[line];
        if (index <= Size()) return true;
        index = Size();
    }
    return false;
}

void cGZWinTextEdit::FixInsertionPositionIfBroken() {   // 0x100266eee
    if (Size() == 0) {
        pos_.index = 0;
        pos_.line = 0;
        pos_.column = 0;
        pos_.selectionB = 0;
        return;
    }
    if (Size() < pos_.index) pos_.index = Size();
    GetLineAndColumnFromIndex(pos_.line, pos_.column, pos_.index);
    if (Size() < pos_.selectionB) pos_.selectionB = Size();
}

void cGZWinTextEdit::MoveInsertionPositionToBeforeReturnIfNecessary() {   // 0x10025ffb2
    if (pos_.index != 0 && pos_.column != 0) {
        const std::uint32_t before = pos_.index - 1;
        if (Chars()[before] == '\n') {
            --pos_.column;
            pos_.index = before;
        }
    }
}

int cGZWinTextEdit::GetHorizontalScrollPixels() {   // 0x1002629fc
    if (!(flags_ & kTESingleLine)) return hScrollPixels_;
    return font_->GetStringWidth(Chars(), static_cast<std::uint32_t>(hScrollChars_));
}

void cGZWinTextEdit::GetHorizontalLineMetrics(std::uint32_t line, std::uint32_t scrollPixels, std::uint32_t& start, std::uint32_t& end, int& offset) {   // 0x100262a4a
    const std::uint32_t length = GetCharacterCountOfLine(line);
    const std::uint32_t lineStart = lineStarts_[line];
    const char* text = Chars() + lineStart;
    std::uint32_t skipped, consumed;
    std::uint32_t lastWidth;
    if (scrollPixels == 0) {
        lastWidth = 0;
        consumed = 0;
        skipped = 0;
    } else {
        consumed = static_cast<std::uint32_t>(GetCharLength(text, length, codePage_));
        skipped = consumed;
        if (consumed < length) {
            std::uint32_t width = 0;
            do {
                lastWidth = width;
                width = static_cast<std::uint32_t>(font_->GetStringWidth(text, skipped));
                if (scrollPixels < width) break;
                consumed = static_cast<std::uint32_t>(GetCharLength(text + skipped, length - skipped, codePage_));
                skipped += consumed;
            } while (skipped < length);
        } else {
            lastWidth = 0;
        }
    }
    start = (lineStart - consumed) + skipped;
    offset = static_cast<int>(lastWidth - scrollPixels);
    std::uint32_t from = start;
    if (from < lineStart + length) {
        from -= lineStart;
        const int room = visibleWidth_;
        std::uint32_t to = static_cast<std::uint32_t>(GetCharLength(text + from, length - from, codePage_)) + from;
        if (to < length) {
            do {
                const std::uint32_t width = static_cast<std::uint32_t>(font_->GetStringWidth(text + from, to - from));
                if (static_cast<std::uint32_t>(room - (static_cast<int>(lastWidth) - static_cast<int>(scrollPixels))) <= width) break;
                to += static_cast<std::uint32_t>(GetCharLength(text + to, length - to, codePage_));
            } while (to < length);
        }
        from = lineStart + to;
    }
    end = from;
}

int cGZWinTextEdit::GetPixelWidthOfLongestLine(bool all) {   // 0x100266fbc
    std::uint32_t first, last;
    if (all) {
        first = 0;
        last = static_cast<std::uint32_t>(lineStarts_.size());
    } else {
        first = firstLine_;
        last = static_cast<std::uint32_t>(visibleLines_) + first;
    }
    std::uint32_t widest = 0;
    for (std::uint32_t i = first; i < last; ++i) {
        const std::uint32_t width = static_cast<std::uint32_t>(
            font_->GetStringWidth(Chars() + lineStarts_[i], GetCharacterCountOfLine(i)));
        if (widest < width) widest = width;
    }
    return static_cast<int>(widest);
}

// ---- position conversions -----------------------------------------------------------------------------

void cGZWinTextEdit::GetIndexFromPosition(std::uint32_t& index, int x, int y) {   // 0x10026416c
    if (Size() == 0) { index = 0; return; }
    std::uint32_t line, column;
    GetLineAndColumnFromPosition(line, column, x, y);
    index = column + lineStarts_[line];
}

void cGZWinTextEdit::GetColumnFromPosition(std::uint32_t line, std::uint32_t& column, int x) {   // 0x1002641f4
    const int pixel = GetHorizontalScrollPixels() + (x - static_cast<int>(gutterX_));
    column = 0;
    if (0 < pixel && !lineStarts_.empty()) {
        const std::uint32_t length = GetCharacterCountOfLine(line);
        const char* base = Chars() + lineStarts_[line];
        const char* textEnd = Chars() + Size();
        std::uint32_t c = column;
        if (c < length) {
            do {
                const int charLength = GetCharLength(base + c, static_cast<std::uint32_t>(textEnd - (base + c)), codePage_);
                const int width = font_->GetStringWidth(base, c + static_cast<std::uint32_t>(charLength));
                if (pixel <= width) return;
                c += static_cast<std::uint32_t>(charLength);
                column = c;
            } while (c < length);
        }
        column = length;
    }
}

void cGZWinTextEdit::GetLineAndColumnFromPosition(std::uint32_t& line, std::uint32_t& column, int x, int y) {   // 0x10026432a
    int codePage = codePage_;
    const bool password = (flags_ & kTEPassword) != 0;
    const cRZString* source = &text_;
    if (password) {
        const std::uint32_t glyphs = GetGlyphCountFromString(Chars(), Size(), codePage);
        if (display_.Strlen() != glyphs) display_.Resize(glyphs), display_.str().assign(glyphs, maskChar_);
        source = &display_;
        codePage = 0;
    }
    if (y < 0) { y = 0; x = 0; }
    if (x < 0) x = 0;
    const int scroll = GetHorizontalScrollPixels();
    line = firstLine_ + static_cast<std::uint32_t>(y) / static_cast<std::uint32_t>(lineHeight_);
    if (lineStarts_.size() <= line) line = static_cast<std::uint32_t>(lineStarts_.size()) - 1;
    if (source->Strlen() == 0) {
        column = 0;
        line = 0;
        return;
    }
    const std::uint32_t start = lineStarts_[line];
    std::uint32_t length = GetCharacterCountOfLine(line);
    const char* data = source->Data();
    column = 0;
    if (length != 0) {
        const int pixel = (x - static_cast<int>(gutterX_)) + scroll;
        int previous = 0;
        while (column < length) {
            const int charLength = GetCharLength(data + start + column, source->Strlen() - (column + start), codePage);
            column += static_cast<std::uint32_t>(charLength);
            const int width = font_->GetStringWidth(data + start, column);
            if (pixel < width) {
                if (pixel - previous <= width - pixel) column -= static_cast<std::uint32_t>(charLength);
                if (!password) return;
                length = static_cast<std::uint32_t>(LengthFromGlyphCount(Chars(), column, codePage_));
                break;
            }
            previous = width;
        }
    }
    column = length;
}

void cGZWinTextEdit::GetPositionFromLineAndColumn(std::uint32_t line, std::uint32_t column, int& x, int& y) {   // 0x1002645d0
    if (lineStarts_.empty()) {
        x = static_cast<int>(gutterX_);
        y = static_cast<int>(gutterY_);
    } else {
        if (lineStarts_.size() <= line) line = static_cast<std::uint32_t>(lineStarts_.size()) - 1;
        const std::uint32_t start = lineStarts_[line];
        std::uint32_t length = column;
        if (Size() <= start + column) length = Size() - start;
        x = font_->GetStringWidth(Chars() + start, length) + static_cast<int>(gutterX_);
        y = (static_cast<int>(line) - static_cast<int>(firstLine_)) * lineHeight_ + static_cast<int>(gutterY_);
    }
    x -= GetHorizontalScrollPixels();
}

// ---- scrolling ----------------------------------------------------------------------------------------

void cGZWinTextEdit::SetScrollBarSizes() {   // 0x100262c7c
    if (!vScroll_ && !hScroll_) return;
    if (!vScroll_) {
        cIGZWin* bar = hScroll_->AsIGZWin();
        bar->SetArea(0, GetH() - hScroll_->AsIGZWin()->GetH(), GetW(), GetH());
    } else if (!hScroll_) {
        cIGZWin* bar = vScroll_->AsIGZWin();
        bar->SetArea((GetW() + 1) - vScroll_->AsIGZWin()->GetW(), 0, GetW(), GetH());
    } else {
        hScroll_->AsIGZWin()->SetArea(0, GetH() - hScroll_->AsIGZWin()->GetH(), GetW() - vScroll_->AsIGZWin()->GetW(), GetH());
        vScroll_->AsIGZWin()->SetArea((GetW() + 1) - vScroll_->AsIGZWin()->GetW(), 0, GetW(), GetH() - hScroll_->AsIGZWin()->GetH());
    }
}

void cGZWinTextEdit::SetScrollBarValues(bool resetRange) {   // 0x100262ece
    if (vScroll_) {
        if (resetRange && visibleLines_ != 0) {
            int range = static_cast<int>(GetLineCount()) - visibleLines_;
            if (range < 0) range = 0;
            vScroll_->SetMinMax(0, range);
            vScroll_->SetLineAndPageCount(static_cast<std::uint32_t>(range), GetLineCount() / static_cast<std::uint32_t>(visibleLines_) + 1);
        }
        vScroll_->SetValue(static_cast<int>(firstLine_));
    }
    if (hScroll_) {
        if (resetRange && visibleLines_ != 0) {
            const int widest = GetPixelWidthOfLongestLine(true);
            int range = widest - visibleWidth_;
            if (range < 0) range = 0;
            hScroll_->SetMinMax(0, range);
            hScroll_->SetLineAndPageCount(static_cast<std::uint32_t>(range), static_cast<std::uint32_t>(widest / visibleWidth_ + 1));
        }
        hScroll_->SetValue(hScrollPixels_);
    }
}

bool cGZWinTextEdit::AddScrollBar(bool vertical, bool horizontal) {   // 0x1002670ee
    bool addedVertical = false;
    IWinCtrlFactory* factory = WinCtrlFactory();
    if (vertical && (flags_ & kTEVScroll) && !vScroll_) {
        if (factory) vScroll_ = factory->CreateScrollbar(0, 2, 0, 100, 0);
        if (!vScroll_) {
            ClearFlag(kTEVScroll);
        } else {
            if (vImage_) {
                cRZRect area = vImageRect_;
                if (!(area.left < area.right && area.top < area.bottom)) vImage_->GetBufferArea(area);
                vScroll_->ImageSetup(area, vImage_, 0);
            }
            ChildAdd(vScroll_->AsIGZWin());
        }
        addedVertical = true;
    }
    if (horizontal && (flags_ & kTEHScroll) && !hScroll_) {
        if (factory) hScroll_ = factory->CreateScrollbar(1, 1, 0, 100, 0);
        if (!hScroll_) {
            ClearFlag(kTEHScroll);
        } else {
            if (hImage_) {
                cRZRect area = hImageRect_;
                if (!(area.left < area.right && area.top < area.bottom)) hImage_->GetBufferArea(area);
                hScroll_->ImageSetup(area, hImage_, 0);
            }
            ChildAdd(hScroll_->AsIGZWin());
        }
    } else if (!addedVertical) {
        return true;
    }
    RecalculateAll();
    SetScrollBarSizes();
    SetScrollBarValues(true);
    return true;
}

void cGZWinTextEdit::RemoveScrollBar(bool vertical, bool horizontal) {   // 0x100267348
    bool removed = false;
    if (vertical && vScroll_) {
        ChildDelete(vScroll_->AsIGZWin());
        vScroll_->Release();
        vScroll_ = nullptr;
        removed = true;
    }
    if (horizontal && hScroll_) {
        ChildDelete(hScroll_->AsIGZWin());
        hScroll_->Release();
        hScroll_ = nullptr;
    } else if (!removed) {
        return;
    }
    RecalculateAll();
}

bool cGZWinTextEdit::TestForPossibleScrollBarAdditionOrRemoval() {   // 0x1002650d8
    if (flags_ & kTEVScroll) {
        if (static_cast<std::uint64_t>(static_cast<std::int64_t>(visibleLines_)) < lineStarts_.size() && visibleWidth_ != 0 && visibleHeight_ != 0) {
            if (!vScroll_) AddScrollBar(true, false);
        } else if (vScroll_) {
            RemoveScrollBar(true, false);
        }
    }
    if ((flags_ & 6) == 4) {
        const int widest = GetPixelWidthOfLongestLine(true);
        if (visibleWidth_ < widest && visibleWidth_ != 0 && visibleHeight_ != 0) {
            if (!hScroll_) AddScrollBar(false, true);
        } else if (hScroll_) {
            RemoveScrollBar(false, true);
        }
    }
    return true;
}

void cGZWinTextEdit::UpdateScrollBarsAndWindowPosition() {   // 0x100265800
    if (get_init_flag()) {
        PutInsertionPositionInView();
        TestForPossibleScrollBarAdditionOrRemoval();
    }
    InvalidateSelfAndParents();
}

void cGZWinTextEdit::PutInsertionPositionInView() {   // 0x1002618de
    bool changed = false;
    std::uint32_t line = pos_.line;
    if (line < firstLine_) {
        firstLine_ = line;
        changed = true;
    } else if (firstLine_ + static_cast<std::uint32_t>(visibleLines_) <= line) {
        firstLine_ = (line + 1) - static_cast<std::uint32_t>(visibleLines_);
        changed = true;
    }
    const std::uint32_t f = flags_;
    if (hScroll_ || (f & 0x202) != 2) {
        if (f & kTESingleLine) {
            if (pos_.column < static_cast<std::uint32_t>(hScrollChars_)) {
                hScrollChars_ = static_cast<std::int32_t>(pos_.column);
            } else {
                const int width = font_->GetStringWidth(Chars(), pos_.column);
                int shown = GetHorizontalScrollPixels();
                if (visibleWidth_ < width - shown) {
                    do {
                        const int at = hScrollChars_;
                        if (Size() <= static_cast<std::uint64_t>(static_cast<std::int64_t>(at))) break;
                        hScrollChars_ += GetCharLength(Chars() + at, Size() - static_cast<std::uint32_t>(at), codePage_);
                        shown = GetHorizontalScrollPixels();
                    } while ((visibleWidth_ * 3) / 4 <= width - shown);
                }
            }
            SetScrollBarValues(false);
            return;
        }
        if (hScroll_ || (f & kTEWrap) == 0) {
            int width = font_->GetStringWidth(Chars() + lineStarts_[line], pos_.column);
            width -= GetHorizontalScrollPixels();
            if (width < 0 || visibleWidth_ <= width) {
                hScrollPixels_ += width < 0 ? width : width - visibleWidth_;
                SetScrollBarValues(false);
                return;
            }
        }
    }
    if (!changed) return;
    SetScrollBarValues(false);
}

void cGZWinTextEdit::MakeCaretVisible() {   // 0x1002601ea
    if (GetFocus() == static_cast<cIGZWin*>(this) && !caretVisible_) {
        caretVisible_ = true;
        InvalidateSelfAndParents();
    }
}

void cGZWinTextEdit::DoCursorScroll() {   // 0x1002600c2
    if (!GZAmCapturing()) return;
    const int width = GetW(), height = GetH();
    int x = 0, y = 0;
    GetWindowManager()->GetCursorPos(this, x, y);
    if (y < -2) {
        if (!ScrollLineUp()) return;
        y = lineHeight_ >> 1;
    } else if (y < height + 2) {
        if (-3 < x) {
            if (x < width + 2) return;
            if (!ScrollRight()) return;
            GZOnMouseMove(GetW(), y, 0);
            return;
        }
        if (!ScrollLeft()) return;
        x = 0;
    } else {
        if (!ScrollLineDown()) return;
        y = height - (lineHeight_ >> 1);
    }
    GZOnMouseMove(x, y, 0);
}

bool cGZWinTextEdit::ScrollLineUp() {   // 0x100263c2c
    const bool can = 0 < static_cast<int>(firstLine_);
    if (can) {
        --firstLine_;
        SetScrollBarValues(false);
        InvalidateSelfAndParents();
    }
    return can;
}

bool cGZWinTextEdit::ScrollLineDown() {   // 0x100263c7c
    const bool can = visibleLines_ < static_cast<int>(lineStarts_.size()) - static_cast<int>(firstLine_);
    if (can) {
        ++firstLine_;
        SetScrollBarValues(false);
        InvalidateSelfAndParents();
    }
    return can;
}

bool cGZWinTextEdit::ScrollLeft() {   // 0x100263ce4
    if (!(flags_ & kTESingleLine)) {
        if ((flags_ & kTEWrap) && !hScroll_) return true;
        if (0 < hScrollPixels_) hScrollPixels_ -= lineHeight_;
        SetScrollBarValues(false);
    } else {
        const std::uint32_t at = static_cast<std::uint32_t>(hScrollChars_);
        if (0 < static_cast<std::int64_t>(static_cast<std::int32_t>(at)))
            hScrollChars_ -= static_cast<std::int32_t>(GetPreviousCharLength(Chars() + at, at, codePage_));
    }
    InvalidateSelfAndParents();
    return true;
}

bool cGZWinTextEdit::ScrollRight() {   // 0x100263d7c
    if (!(flags_ & kTESingleLine)) {
        if ((flags_ & kTEWrap) && !hScroll_) return true;
        int value = hScrollPixels_;
        if (value <= visibleWidth_) {
            value += lineHeight_;
            hScrollPixels_ = value;
        }
        if (visibleWidth_ < value) hScrollPixels_ = visibleWidth_;
        SetScrollBarValues(false);
    } else {
        const int at = hScrollChars_;
        if (static_cast<std::uint64_t>(static_cast<std::int64_t>(at)) < Size())
            hScrollChars_ += GetCharLength(Chars() + at, Size() - static_cast<std::uint32_t>(at), codePage_);
    }
    InvalidateSelfAndParents();
    return true;
}

bool cGZWinTextEdit::ScrollToStart() {   // 0x100263e4c
    if (firstLine_ == 0 && hScrollChars_ == 0 && hScrollPixels_ == 0) return false;
    firstLine_ = 0;
    hScrollChars_ = 0;
    hScrollPixels_ = 0;
    SetScrollBarValues(false);
    InvalidateSelfAndParents();
    return true;
}

bool cGZWinTextEdit::ScrollToEnd() {   // 0x100263eb2
    const int count = static_cast<int>(lineStarts_.size());
    const bool can = visibleLines_ < count - static_cast<int>(firstLine_);
    if (can) {
        firstLine_ = static_cast<std::uint32_t>(count - visibleLines_);
        hScrollChars_ = 0;
        hScrollPixels_ = 0;
        SetScrollBarValues(false);
        InvalidateSelfAndParents();
    }
    return can;
}

bool cGZWinTextEdit::ScrollPageUp() {   // 0x100263f2a
    if (firstLine_ == 0) return false;
    firstLine_ = firstLine_ < static_cast<std::uint32_t>(visibleLines_) ? 0 : firstLine_ - static_cast<std::uint32_t>(visibleLines_);
    SetScrollBarValues(false);
    InvalidateSelfAndParents();
    return true;
}

bool cGZWinTextEdit::ScrollPageDown() {   // 0x100263f80
    const std::int64_t count = static_cast<std::int64_t>(lineStarts_.size());
    if (visibleLines_ < static_cast<int>(count) - static_cast<int>(firstLine_)) {
        std::uint32_t next = firstLine_ + static_cast<std::uint32_t>(visibleLines_);
        if (static_cast<std::uint64_t>(count - visibleLines_) <= firstLine_) next = static_cast<std::uint32_t>(count) - 1;
        firstLine_ = next;
        SetScrollBarValues(false);
        InvalidateSelfAndParents();
        return true;
    }
    return false;
}

bool cGZWinTextEdit::GetScrollBarImageVertical(cIGZBuffer*& image, cRZRect& area) const {   // 0x100263388
    image = vImage_;
    area = vImageRect_;
    return image != nullptr;
}

bool cGZWinTextEdit::GetScrollBarImageHorizontal(cIGZBuffer*& image, cRZRect& area) const {   // 0x1002634c0
    image = hImage_;
    area = hImageRect_;
    return image != nullptr;
}

bool cGZWinTextEdit::SetScrollBarImageVertical(cIGZBuffer* image, const cRZRect& area) {   // 0x1002632a8
    if (image) image->AddRef();
    if (vImage_) vImage_->Release();
    vImage_ = image;
    vImageRect_ = area;
    if (image && vScroll_) {
        cRZRect use = area;
        if (!(area.left < area.right && area.top < area.bottom)) image->GetBufferArea(use);
        vScroll_->ImageSetup(use, image, 0);
    }
    return true;
}

bool cGZWinTextEdit::SetScrollBarImageHorizontal(cIGZBuffer* image, const cRZRect& area) {   // 0x1002633e0
    if (image) image->AddRef();
    if (hImage_) hImage_->Release();
    hImage_ = image;
    hImageRect_ = area;
    if (image && hScroll_) {
        cRZRect use = area;
        if (!(area.left < area.right && area.top < area.bottom)) image->GetBufferArea(use);
        hScroll_->ImageSetup(use, image, 0);
    }
    return true;
}

// ---- sounds -------------------------------------------------------------------------------------------

bool cGZWinTextEdit::SetSoundID(int which, int soundId) {   // 0x100266dd6
    if (which >= 4) return false;
    soundIds_[which] = soundId;
    return true;
}

bool cGZWinTextEdit::PlaySoundA(int which) {   // 0x100266e0e
    if (soundIds_[which] == 0) return false;
    IWinButtonHost* host = WinButtonHost();
    if (!host) return false;
    host->PlaySound(static_cast<std::uint32_t>(soundIds_[which]));
    return true;
}

// ---- text editing -------------------------------------------------------------------------------------

void cGZWinTextEdit::GetText(char* buffer, std::uint32_t& length) {   // 0x100264bb2
    if (Size() < length) length = Size();
    std::memcpy(buffer, Chars(), length);
}

bool cGZWinTextEdit::RemoveBadCharacters(cRZString& text) {   // 0x1002648d4
    std::uint32_t length = text.Strlen();
    std::string copy = text.str();
    const std::uint32_t removed = RemoveBadCharacters(copy.data(), length);
    if (text.Strlen() != length) {
        copy.resize(length);
        text.str() = copy;
    } else if (removed) {
        text.str() = copy;
    }
    return removed != 0;
}

std::uint32_t cGZWinTextEdit::RemoveBadCharacters(char* text, std::uint32_t& length) {   // 0x10026494a
    if (!(flags_ & kTESingleLine)) return 0;
    std::string kept;
    std::uint32_t removed = 0;
    std::uint32_t at = 0;
    while (at < length) {
        const std::uint32_t charLength = static_cast<std::uint32_t>(GetCharLength(text + at, length - at, codePage_));
        if (text[at] == '\n') removed += charLength;
        else kept.append(text + at, charLength);
        at += charLength;
    }
    if (removed == 0) return 0;
    std::strncpy(text, kept.c_str(), length);
    length = static_cast<std::uint32_t>(kept.size());
    return removed;
}

int cGZWinTextEdit::FixBadCharacterComposition(cRZString& text, const cRZString& existing, std::uint32_t at) {   // 0x100264a74
    std::uint32_t available = at;
    std::uint32_t previous = GetPreviousUTF16Character(existing.Data() + at, available, codePage_);
    std::string& s = text.str();
    std::uint32_t pos = 0;
    int dropped = 0;
    while (pos < s.size()) {
        std::uint32_t consumed = static_cast<std::uint32_t>(s.size()) - pos;
        const std::uint32_t character = GetUTF16Character(s.data() + pos, consumed, codePage_);
        if (consumed == 0) {
            consumed = static_cast<std::uint32_t>(s.size()) - pos;
            dropped += static_cast<int>(consumed);
        } else {
            if (UTF16GetThaiCharLevel(character & 0xffff, previous & 0xffff, 0) != 0) {
                pos += consumed;
                previous = character;
                continue;
            }
            dropped += static_cast<int>(consumed);
        }
        s.erase(pos, consumed);
    }
    return dropped;
}

bool cGZWinTextEdit::SetText(const char* text, std::uint32_t length, bool notify) {   // 0x100264c16
    if (!text) text = "";
    if (length == 0) length = static_cast<std::uint32_t>(std::strlen(text));
    text_.str().erase();
    cRZString buffer;
    buffer.str().assign(text, length);
    const int bad = RemoveBadCharacters(buffer) ? 1 : 0;
    const int fixed = FixBadCharacterComposition(buffer, text_, 0);
    if (bad + fixed != 0) PlaySoundA(0);
    std::uint32_t size = buffer.Strlen();
    if (size == 0) {
        RecalculateAll();
        InvalidateSelfAndParents();
    } else {
        if (maxText_ < size) {
            std::uint32_t allowed = maxText_;
            if (codePage_ == 100 || codePage_ == 8) {
                allowed = 0;
                if (maxText_ != 0) {
                    for (;;) {
                        allowed += static_cast<std::uint32_t>(GetCharLength(text + allowed, size - allowed, codePage_));
                        if (maxText_ <= allowed) break;
                    }
                }
            }
            PlaySoundA(1);
            size = allowed;
        }
        text_.str().assign(buffer.Data(), size);
        CalculateAllLineLengths(0);
        SetScrollBarValues(true);
        pos_.line = 0;
        pos_.column = 0;
        pos_.index = 0;
        pos_.selectionA = 0;
        pos_.selectionB = 0;
        TestForPossibleScrollBarAdditionOrRemoval();
        InvalidateSelfAndParents();
    }
    undo_.clear();
    redo_.clear();
    if (notify) Notify(kTextEditMsgChanged);
    return true;
}

std::uint32_t cGZWinTextEdit::InsertText(const char* text, std::uint32_t length, std::uint32_t index) {   // 0x10026545c
    if (Size() < index) index = Size();
    cRZString buffer;
    buffer.str().assign(text, length);
    const int bad = RemoveBadCharacters(buffer) ? 1 : 0;
    const int fixed = FixBadCharacterComposition(buffer, text_, index);
    if (bad + fixed != 0) PlaySoundA(0);
    std::uint32_t count = buffer.Strlen();
    if (count == 0) return 0;
    if (maxText_ < count + Size()) {
        std::uint32_t allowed;
        if (codePage_ == 100 || codePage_ == 8) {
            allowed = 0;
            while (Size() + allowed < maxText_)
                allowed += static_cast<std::uint32_t>(GetCharLength(text + allowed, count, codePage_));
        } else {
            allowed = maxText_ - Size();
        }
        PlaySoundA(1);
        count = allowed;
    }
    if (lineStarts_.empty()) AddFirstLineLength();
    text_.str().insert(index, buffer.Data(), count);
    IncrementInsertionPosition(static_cast<int>(GetGlyphCountFromString(buffer.Data(), count, codePage_)));
    if (!GetFlag(kWinVisible)) return count;
    bool recalc = true;
    if (!(flags_ & kTEWrap)) {
        recalc = index < Size();
        if (!recalc) {
            for (std::uint32_t i = 0; i < count; ++i)
                if (buffer.Data()[i] == '\n') { recalc = true; break; }
        }
    }
    if (recalc) {
        std::uint32_t line = 0, column = 0;
        GetLineAndColumnFromIndex(line, column, index);
        CalculateAllLineLengths(line - 1);
        GetLineAndColumnFromIndex(pos_.line, pos_.column, pos_.index);
        SetScrollBarValues(true);
    }
    UpdateScrollBarsAndWindowPosition();
    return count;
}

void cGZWinTextEdit::InsertText(const char* text, std::uint32_t length, std::uint32_t line, std::uint32_t column) {   // 0x100265846
    std::uint32_t index = 0;
    GetIndexFromLineAndColumn(line, column, index);
    InsertText(text, length, index);
}

bool cGZWinTextEdit::RemoveChar(std::uint32_t index, bool plain, cIGZString* removed) {   // 0x1002658fa
    if (Size() <= index) index = Size() - 1;
    const std::uint32_t available = Size() - index;
    const std::uint32_t length = plain ? static_cast<std::uint32_t>(GetCharLength(Chars() + index, available, codePage_))
                                       : GetComposedCharLength(Chars() + index, available, codePage_);
    if (removed) removed->AssignFromChars(Chars() + index, length);
    text_.str().erase(index, length);
    if (plain) IncrementInsertionPosition(-1);
    pos_.index = index;
    GetLineAndColumnFromIndex(pos_.line, pos_.column, index);
    CalculateAllLineLengths(pos_.line - 1);
    SetScrollBarValues(true);
    GetLineAndColumnFromIndex(pos_.line, pos_.column, pos_.index);
    const int first = static_cast<int>(firstLine_);
    if (0 < first) {
        int excess = (first - static_cast<int>(lineStarts_.size())) + visibleLines_;
        if (0 < excess) {
            if (first <= excess) excess = first;
            firstLine_ = static_cast<std::uint32_t>(first - excess);
            SetScrollBarValues(false);
        }
    }
    PutInsertionPositionInView();
    TestForPossibleScrollBarAdditionOrRemoval();
    if (lineStarts_.size() <= static_cast<std::uint64_t>(static_cast<std::int64_t>(visibleLines_))) {
        firstLine_ = 0;
        SetScrollBarValues(false);
    }
    InvalidateSelfAndParents();
    return true;
}

std::uint32_t cGZWinTextEdit::RemoveText(std::uint32_t index, std::uint32_t length, cIGZString* removed) {   // 0x100265b08
    if (index >= Size()) return 0;
    if (Size() < length + index) length = Size() - index;
    if (removed) removed->AssignFromChars(Chars() + index, length);
    text_.str().erase(index, length);
    pos_.index = index;
    GetLineAndColumnFromIndex(pos_.line, pos_.column, index);
    CalculateAllLineLengths(pos_.line - 1);
    SetScrollBarValues(true);
    const int first = static_cast<int>(firstLine_);
    if (0 < first) {
        int excess = (first - static_cast<int>(lineStarts_.size())) + visibleLines_;
        if (0 < excess) {
            if (first <= excess) excess = first;
            firstLine_ = static_cast<std::uint32_t>(first - excess);
            SetScrollBarValues(false);
        }
    }
    PutInsertionPositionInView();
    TestForPossibleScrollBarAdditionOrRemoval();
    InvalidateSelfAndParents();
    return length;
}

void cGZWinTextEdit::RemoveText(std::uint32_t line, std::uint32_t column, std::uint32_t length, cIGZString* removed) {   // 0x100265c7c
    std::uint32_t index = 0;
    GetIndexFromLineAndColumn(line, column, index);
    RemoveText(index, length, removed);
}

void cGZWinTextEdit::RemoveText(std::uint32_t line0, std::uint32_t column0, std::uint32_t line1, std::uint32_t column1, cIGZString*) {   // 0x100265d26
    std::uint32_t first = 0, second = 0;
    GetIndexFromLineAndColumn(line0, column0, first);
    GetIndexFromLineAndColumn(line1, column1, second);
    // Removes |second - first| characters from the FIRST index, even when that is the later one.
    const std::uint32_t distance = first < second || first - second == 0 ? second - first : first - second;
    RemoveText(first, distance, nullptr);
}

std::uint32_t cGZWinTextEdit::FindText(const char* text, std::uint32_t length, std::uint32_t start, bool forward) {   // 0x100265da8
    std::size_t at;
    if (forward) at = text_.str().find(text, start, length);
    else at = text_.str().rfind(text, start);
    const std::uint32_t found = static_cast<std::uint32_t>(at);
    return found < Size() ? found : kNone;
}

bool cGZWinTextEdit::SetInsertMode(bool insert) {   // 0x100265e12
    flags_ = insert ? flags_ & 0xffffbfffu : flags_ | 0x4000u;
    return true;
}

bool cGZWinTextEdit::SetInsertionIndex(std::uint32_t index) {   // 0x100265e82
    if (index == kNone || Size() < index) index = Size();
    pos_.index = index;
    GetLineAndColumnFromIndex(pos_.line, pos_.column, index);
    InvalidateSelfAndParents();
    return true;
}

bool cGZWinTextEdit::SetInsertionLineAndColumn(std::uint32_t line, std::uint32_t column) {   // 0x100265f34
    if (lineStarts_.empty()) {
        pos_.index = 0;
        pos_.line = 0;
        pos_.column = 0;
    } else {
        std::uint32_t last = static_cast<std::uint32_t>(lineStarts_.size()) - 1;
        const std::uint32_t use = line < lineStarts_.size() ? line : last;
        const std::uint32_t available = GetCharacterCountOfLine(use);
        if (available < column) column = available;
        pos_.line = use;
        pos_.column = column;
        GetIndexFromLineAndColumn(use, column, pos_.index);
    }
    return true;
}

namespace {
void ClampIndex(std::uint32_t& index, std::uint32_t size) {
    if (static_cast<std::int32_t>(index) < 0) index = 0;
    else if (size < index) index = size;
}
}  // namespace

void cGZWinTextEdit::IncrementInsertionPosition(int characters) {   // 0x100266194
    if (codePage_ == 100 || codePage_ == 8) {
        if (0 < characters) {
            for (int i = 0; i < characters; ++i)
                pos_.index += static_cast<std::uint32_t>(GetCharLength(Chars() + pos_.index, Size() - pos_.index, codePage_));
        } else {
            for (int i = 0; i < -characters; ++i)
                pos_.index -= GetPreviousCharLength(Chars() + pos_.index, pos_.index, codePage_);
        }
    } else {
        pos_.index += static_cast<std::uint32_t>(characters);
    }
    ClampIndex(pos_.index, Size());
    GetLineAndColumnFromIndex(pos_.line, pos_.column, pos_.index);
}

void cGZWinTextEdit::IncrementInsertionPositionByComposedCharacter(int characters) {   // 0x10026600a
    if (codePage_ == 8 || codePage_ > 99) {
        for (int i = 0; i < characters; ++i)
            pos_.index += GetComposedCharLength(Chars() + pos_.index, Size() - pos_.index, codePage_);
        for (int i = 0; i < -characters; ++i)
            pos_.index -= GetPreviousComposedCharLength(Chars() + pos_.index, pos_.index, codePage_);
    } else {
        pos_.index += static_cast<std::uint32_t>(characters);
    }
    ClampIndex(pos_.index, Size());
    GetLineAndColumnFromIndex(pos_.line, pos_.column, pos_.index);
}

void cGZWinTextEdit::IncrementInsertionPositionByWord(int words) {   // 0x10026632c
    if (Size() == 0) return;
    const char* t = Chars();
    const int size = static_cast<int>(Size());
    std::int32_t index = static_cast<std::int32_t>(pos_.index);
    if (0 < words) {
        for (int n = 0; n < words; ++n) {
            bool found = false;
            for (int i = index; i < size - 1; ++i) {
                if (IsBlankChar(t[i]) && !IsBlankChar(t[i + 1])) {
                    index = i + 1;
                    found = true;
                    break;
                }
            }
            if (!found) { index = size; break; }
            pos_.index = static_cast<std::uint32_t>(index);
        }
    } else if (words < 0) {
        for (int n = 0; n > words; --n) {
            bool found = false;
            for (int j = index - 1; 0 < j; --j) {
                if (IsBlankChar(t[j - 1]) && !IsBlankChar(t[j])) {
                    index = j;
                    found = true;
                    break;
                }
            }
            if (!found) { index = 0; break; }
            pos_.index = static_cast<std::uint32_t>(index);
        }
    } else {
        return;
    }
    pos_.index = static_cast<std::uint32_t>(index);
    ClampIndex(pos_.index, Size());
    GetLineAndColumnFromIndex(pos_.line, pos_.column, pos_.index);
}

bool cGZWinTextEdit::SetSelection(std::uint32_t start, std::uint32_t end) {   // 0x1002665d0
    if (Size() < start) start = Size();
    if (Size() < end) end = Size();
    pos_.selectionA = start;
    pos_.selectionB = end;
    pos_.index = end;
    GetLineAndColumnFromIndex(pos_.line, pos_.column, end);
    InvalidateSelfAndParents();
    return true;
}

bool cGZWinTextEdit::SelectCurrentWord() {   // 0x100261c10
    if (Size() == 0) return false;
    const char* t = Chars();
    std::uint32_t at = pos_.index;
    if (t[at] == '\n' || t[at] == ' ') {
        if (at == 0) { PutInsertionPositionInView(); return true; }
        --at;
        if (t[at] == '\n' || t[at] == ' ') { PutInsertionPositionInView(); return true; }
        pos_.index = pos_.selectionA = pos_.selectionB = at;
    } else {
        at = pos_.selectionA;
    }
    if (at != 0) {
        std::int32_t remaining = -static_cast<std::int32_t>(at);
        std::uint32_t i = at - 1;
        do {
            if (t[i] == '\n' || t[i] == ' ') break;
            pos_.selectionA = i;
            --i;
            ++remaining;
        } while (remaining != 0);
    }
    std::uint32_t end = pos_.selectionB;
    while (end < Size() && t[end] != '\n' && t[end] != ' ') {
        ++end;
        pos_.selectionB = end;
        pos_.index = end;
    }
    FixInsertionPositionIfBroken();
    PutInsertionPositionInView();
    return true;
}

// ---- undo / redo --------------------------------------------------------------------------------------

bool cGZWinTextEdit::AddUndoRecordForTextInsertion(const TextEditPosition& before, std::uint32_t at, std::uint32_t length,
                                                   const char* replaced, std::uint32_t replacedLength) {   // 0x1002651d6
    if (!(flags_ & kTEUndo)) return false;
    if (maxUndo_ <= undo_.size()) undo_.erase(undo_.begin(), undo_.begin() + static_cast<std::ptrdiff_t>(undo_.size() / 4));
    UndoRedoAction action;
    action.inserted = true;
    action.insertAt = at;
    action.insertLength = length;
    if (replaced && replacedLength) {
        action.deleted = true;
        action.deleteAt = at;
        action.deletedText.assign(replaced, replacedLength);
    }
    action.position = before;
    undo_.push_back(std::move(action));
    redo_.clear();
    return true;
}

bool cGZWinTextEdit::AddUndoRecordForTextDeletion(const TextEditPosition& before, std::uint32_t at, const char* text, std::uint32_t length) {   // 0x1002616dc
    if (!(flags_ & kTEUndo)) return false;
    UndoRedoAction action;
    action.deleted = true;
    action.deleteAt = at;
    action.deletedText.assign(text, length);
    action.position = before;
    undo_.push_back(std::move(action));
    redo_.clear();
    return true;
}

bool cGZWinTextEdit::ApplyAction(std::vector<UndoRedoAction>& from, std::vector<UndoRedoAction>& to) {
    if (!(flags_ & kTEUndo) || from.empty()) return false;
    const UndoRedoAction action = from.back();
    UndoRedoAction reverse;
    if (action.deleted) {
        reverse.inserted = true;
        reverse.insertAt = action.deleteAt;
        reverse.insertLength = static_cast<std::uint32_t>(action.deletedText.size());
    }
    if (action.inserted) {
        reverse.deleted = true;
        reverse.deleteAt = action.insertAt;
        reverse.deletedText.assign(text_.str(), action.insertAt, action.insertLength);
    }
    reverse.position = pos_;
    to.push_back(std::move(reverse));
    if (action.inserted) text_.str().erase(action.insertAt, action.insertLength);
    if (action.deleted) text_.str().insert(action.deleteAt, action.deletedText);
    pos_ = action.position;
    from.pop_back();
    RecalculateAll();
    PutInsertionPositionInView();
    InvalidateSelfAndParents();
    return true;
}

bool cGZWinTextEdit::Undo() { return ApplyAction(undo_, redo_); }   // 0x1002666fe
bool cGZWinTextEdit::Redo() { return ApplyAction(redo_, undo_); }   // 0x100266a10

// ---- messages and input -------------------------------------------------------------------------------

bool cGZWinTextEdit::DoMessage(cGZMessage& message) {   // 0x10025fffe
    if (message.type == kGZMsgCommand) {
        if (message.param1 == kTextEditCmdScrollbarChanged) {
            if (message.param2 == 0) {
                if (!vScroll_) return true;
                const int value = vScroll_->GetValue();
                if (value == static_cast<int>(firstLine_)) return true;
                firstLine_ = static_cast<std::uint32_t>(value);
            } else if (message.param2 == 1) {
                if (!hScroll_) return true;
                const int value = hScroll_->GetValue();
                if (value == hScrollPixels_) return true;
                hScrollPixels_ = value;
            } else {
                return cGZWin::DoMessage(message);
            }
            InvalidateSelfAndParents();
            return true;
        }
    } else {
        if (message.type == kTextEditMsgScrollTick) {
            DoCursorScroll();
            return true;
        }
        if (message.type == kTextEditMsgCaretBlink) {
            if (GetFocus() != static_cast<cIGZWin*>(this)) return true;
            caretVisible_ = !caretVisible_;
            InvalidateSelfAndParents();
            return true;
        }
    }
    return cGZWin::DoMessage(message);
}

bool cGZWinTextEdit::GZOnSetFocus(cIGZWin* previous) {   // 0x100261f44
    InvalidateSelfAndParents();
    if (flags_ & kTESingleLine) Notify(kTextEditMsgSetFocus);
    return cGZWin::GZOnSetFocus(previous);
}

bool cGZWinTextEdit::GZOnKillFocus(cIGZWin* next) {   // 0x100261f94
    InvalidateSelfAndParents();
    if (static_cast<std::int16_t>(flags_) < 0) Notify(kTextEditMsgKillFocus);
    return cGZWin::GZOnKillFocus(next);
}

bool cGZWinTextEdit::GZOnKeyUp(std::uint32_t key, std::uint32_t modifiers) {   // 0x100261ac4
    if (key == 0x1b) return SendMsg(notify_, kGZMsgKeyUp, 0x1b, modifiers, 0), true;
    return true;
}

bool cGZWinTextEdit::GZOnMouseDownR(int x, int y, std::uint32_t modifiers) {   // 0x100261dac
    if (!(flags_ & kTEEditable))
        SendMsg(notify_, kTextEditMsgMouseDownR, reinterpret_cast<std::uint64_t>(static_cast<cIGZWinTextEdit*>(this)), static_cast<std::uint64_t>(x), static_cast<std::uint64_t>(y));
    (void)modifiers;
    return true;
}

bool cGZWinTextEdit::GZOnMouseDownL(int x, int y, std::uint32_t modifiers) {   // 0x100261af2
    if (!(flags_ & kTEEditable)) {
        SendMsg(notify_, kTextEditMsgMouseDownL, reinterpret_cast<std::uint64_t>(static_cast<cIGZWinTextEdit*>(this)),
                static_cast<std::uint64_t>(x), static_cast<std::uint64_t>(y));
        return true;
    }
    bool doubleClick;
    if (!clickTimer_.IsTimerRunning() || doubleClickMs_ <= clickTimer_.GetElapsedTimeU32()) {
        clickTimer_.Restart();
        doubleClick = false;
    } else {
        clickTimer_.Reset();
        doubleClick = true;
    }
    GZSetCapture();
    std::uint32_t line = 0, column = 0;
    GetLineAndColumnFromPosition(line, column, x, y);
    SetInsertionLineAndColumn(line, column);
    MoveInsertionPositionToBeforeReturnIfNecessary();
    pos_.selectionB = pos_.index;
    if (!(modifiers & 1)) pos_.selectionA = pos_.index;
    if (doubleClick) SelectCurrentWord();
    InvalidateSelfAndParents();
    wantedPixel_ = -1;
    return true;
}

bool cGZWinTextEdit::GZOnMouseUpL(int, int, std::uint32_t) {   // 0x100261d58
    if ((flags_ & kTEEditable) && GZAmCapturing()) {
        GZReleaseCapture();
        if (scrollTimer_.IsRunning()) scrollTimer_.Stop();
    }
    return true;
}

bool cGZWinTextEdit::GZOnMouseMove(int x, int y, std::uint32_t) {   // 0x100261de0
    if (!(flags_ & kTEEditable)) return true;
    if (!GZAmCapturing()) {
        if (scrollTimer_.IsRunning()) scrollTimer_.Stop();
        return true;
    }
    if (!scrollTimer_.IsRunning()) scrollTimer_.StartPeriodic(100, nullptr);
    std::uint32_t line = 0, column = 0;
    GetLineAndColumnFromPosition(line, column, x, y);
    SetInsertionLineAndColumn(line, column);
    MoveInsertionPositionToBeforeReturnIfNecessary();
    if (pos_.selectionB != pos_.index) {
        pos_.selectionB = pos_.index;
        InvalidateSelfAndParents();
    }
    return true;
}

bool cGZWinTextEdit::GZOnMouseWheel(int, int, std::uint32_t, int delta) {   // 0x100261eb8
    if (GZAmCapturing()) return true;
    if (0 < delta) {
        if (99 < delta) for (int i = 0; i < delta / 100; ++i) ScrollLineUp();
    } else if (delta < -99) {
        for (int i = 0; i < -delta / 100; ++i) ScrollLineDown();
    }
    return true;
}

bool cGZWinTextEdit::GZOnCharacter(char c) {   // 0x100260282
    IWinTextEditHost* host = WinTextEditHost();
    const bool shift = host && host->ShiftDown();
    const bool caps = host && host->CapsLockOn();
    unsigned char used;
    if (static_cast<unsigned char>(c + 0xbf) < 0x1a && !caps && !shift) {
        c = static_cast<char>(c + 0x20);
        used = static_cast<unsigned char>(c);
    } else {
        used = static_cast<unsigned char>(c);
        if (shift) used = static_cast<unsigned char>(c - 0x20);
        if (!(static_cast<unsigned char>(c + 0x9f) < 0x1a)) used = static_cast<unsigned char>(c);
    }
    if (flags_ & kTEEditable) {
        unsigned char ch = used == 0xd ? 10 : used;
        if (ch < 0x20 && ch != 10) return true;
        if (codePage_ == 8 || codePage_ > 99) {
            if (pendingChar_.empty() && IsLeadByte(static_cast<char>(ch), codePage_)) {
                pendingChar_.push_back(static_cast<char>(ch));
                return true;
            }
            pendingChar_.push_back(static_cast<char>(ch));
            const int follow = GetFollowByteCount(pendingChar_[0], codePage_);
            if (pendingChar_.size() < static_cast<std::size_t>(follow + 1)) return true;
        } else {
            pendingChar_.assign(1, static_cast<char>(ch));
        }
        if (!GZAmCapturing()) {
            cRZString typed;
            typed.str() = pendingChar_;
            InsertTextHighLevel(typed);
        }
    }
    pendingChar_.clear();
    return false;
}

bool cGZWinTextEdit::InsertTextHighLevel(const cRZString& text) {   // 0x10026047e
    if ((flags_ & kTESingleLine) && text.Data()[0] == '\n') {
        pos_.selectionA = pos_.selectionB = pos_.index;
        InvalidateSelfAndParents();
        return true;
    }
    cRZString removed;
    const TextEditPosition before = pos_;
    std::uint32_t at;
    if (pos_.selectionA == pos_.selectionB) {
        at = pos_.index;
    } else {
        at = std::min(pos_.selectionA, pos_.selectionB);
        const std::uint32_t high = std::max(pos_.selectionA, pos_.selectionB);
        RemoveText(at, high - at, &removed);
        SetInsertionIndex(at);
        pos_.selectionA = pos_.selectionB = pos_.index;
    }
    if (flags_ & kTEOverwrite) {
        if (removed.Strlen() == 0 && pos_.index < Size()) RemoveChar(pos_.index, false, &removed);
    }
    const std::uint32_t inserted = InsertText(text.Data(), text.Strlen(), pos_.index);
    if (inserted == 0) PlaySoundA(1);
    else AddUndoRecordForTextInsertion(before, at, inserted, removed.Data(), removed.Strlen());
    pos_.selectionA = pos_.selectionB = pos_.index;
    wantedPixel_ = -1;
    PutInsertionPositionInView();
    if (flags_ & kTEUpDownNotify) Notify(kTextEditMsgChanged);
    return true;
}

bool cGZWinTextEdit::GZOnKeyDown(std::uint32_t key, std::uint32_t modifiers) {   // 0x1002606ee
    IWinTextEditHost* host = WinTextEditHost();
    const std::uint32_t mods = host ? host->ModifierState() : 0;
    const bool ctrl = (mods & 0xc0) != 0;
    const bool shift = (modifiers & 1) != 0;
    bool result = true;

    // Copy / cut (also reached by Escape on a read-only field, as in the original).
    auto copyOrCut = [&]() {
        if ((modifiers & 5) == 0 && ctrl && (flags_ & 0x3000) == 0x1000 && !GZAmCapturing() && host) {
            if (pos_.selectionA != pos_.selectionB) {
                const std::uint32_t low = std::min(pos_.selectionA, pos_.selectionB);
                const std::uint32_t high = std::max(pos_.selectionA, pos_.selectionB);
                cRZString copy;
                copy.str().assign(Chars() + low, high - low);
                host->ClipboardSetText(copy);
            }
            if (key == 0x58) GZOnKeyDown(0x2e, 0);
        }
    };

    auto finishAfterEdit = [&]() {
        pos_.selectionA = pos_.selectionB = pos_.index;
        wantedPixel_ = -1;
        if (flags_ & kTEUpDownNotify) Notify(kTextEditMsgChanged);
    };
    auto removeSelection = [&](cRZString& removed) -> bool {
        const std::uint32_t low = std::min(pos_.selectionA, pos_.selectionB);
        const std::uint32_t high = std::max(pos_.selectionA, pos_.selectionB);
        const TextEditPosition before = pos_;
        const std::uint32_t count = RemoveText(low, high - low, &removed);
        if (count == 0) PlaySoundA(1);
        else AddUndoRecordForTextDeletion(before, low, removed.Data(), removed.Strlen());
        SetInsertionIndex(low);
        return true;
    };
    auto commonTail = [&]() {   // the "selection follows the caret" tail shared by movement keys
        pos_.selectionB = pos_.index;
        if (!shift) pos_.selectionA = pos_.index;
        PutInsertionPositionInView();
        MakeCaretVisible();
        InvalidateSelfAndParents();
        wantedPixel_ = -1;
    };

    if (key > 0x42) {
        switch (key) {
            case 0x56:   // paste
                if ((modifiers & 5) == 0 && ctrl && !GZAmCapturing() && (flags_ & kTEClipboard) && host) {
                    cRZString pasted;
                    if (host->ClipboardGetText(pasted)) InsertTextHighLevel(pasted);
                }
                return true;
            case 0x57: return true;
            case 0x58: copyOrCut(); return true;
            case 0x59:
                if ((modifiers & 4) != 0 || !ctrl) return true;
                if (!redo_.empty()) Redo(); else PlaySoundA(3);
                return true;
            case 0x5a:
                if ((modifiers & 4) != 0 || !ctrl) return true;
                if (!undo_.empty()) Undo(); else PlaySoundA(3);
                return true;
            default:
                if (key != 0x43) return true;
                copyOrCut();
                return true;
        }
    }

    switch (key) {
        case 0x1b:
            if (flags_ & kTEEditable) {
                if (pos_.selectionA != pos_.selectionB) {
                    pos_.selectionA = pos_.selectionB = pos_.index;
                    InvalidateSelfAndParents();
                }
                SendMsg(notify_, kGZMsgKeyDown, 0x1b, modifiers, 0);
                return true;
            }
            copyOrCut();
            return true;
        case 0x1c: case 0x1d: case 0x1e: case 0x1f: case 0x20:
        case 0x29: case 0x2a: case 0x2b: case 0x2c:
            return true;
        case 0x21:
        case 0x22:
        case 0x26:
        case 0x28: {
            const bool pageKey = key == 0x21 || key == 0x22;
            const bool up = key == 0x21 || key == 0x26;
            if ((flags_ & 0x201) != 1) {
                if (pageKey) return true;
                if (!(flags_ & kTEUpDownNotify)) return true;
                Notify(kTextEditMsgKey, key, 0);
                return true;
            }
            const std::uint32_t count = static_cast<std::uint32_t>(lineStarts_.size());
            const bool canMove = up ? pos_.line != 0 : pos_.line < count - 1;
            if (!canMove) {
                if (pos_.selectionA == pos_.selectionB) {
                    PlaySoundA(2);
                    wantedPixel_ = -1;
                    return true;
                }
            } else {
                int x = 0, y = 0;
                GetPositionFromLineAndColumn(pos_.line, pos_.column, x, y);
                const std::uint32_t step = pageKey ? static_cast<std::uint32_t>(visibleLines_) : 1;
                if (up) pos_.line = pos_.line >= step ? pos_.line - step : 0;
                else pos_.line = (count - step <= pos_.line) ? count - 1 : pos_.line + step;
                if (wantedPixel_ == -1) wantedPixel_ = x;
                GetColumnFromPosition(pos_.line, pos_.column, wantedPixel_);
                GetIndexFromLineAndColumn(pos_.line, pos_.column, pos_.index);
                MoveInsertionPositionToBeforeReturnIfNecessary();
            }
            pos_.selectionB = pos_.index;
            if (!shift) pos_.selectionA = pos_.index;
            PutInsertionPositionInView();
            if (key == 0x22) MakeCaretVisible();
            InvalidateSelfAndParents();
            return true;
        }
        case 0x23: {   // End
            if (!(flags_ & kTEEditable)) return true;
            const std::uint32_t end = GetCharacterCountOfLine(pos_.line);
            if (pos_.column == end && pos_.selectionA == pos_.selectionB) { wantedPixel_ = -1; return true; }
            if (!ctrl) {
                if (pos_.column != end) {
                    pos_.column = end;
                    GetIndexFromLineAndColumn(pos_.line, end, pos_.index);
                    MoveInsertionPositionToBeforeReturnIfNecessary();
                }
            } else {
                pos_.index = Size();
                GetLineAndColumnFromIndex(pos_.line, pos_.column, Size());
            }
            commonTail();
            return true;
        }
        case 0x24: {   // Home
            if (!(flags_ & kTEEditable)) return true;
            if (pos_.column == 0 && pos_.selectionA == pos_.selectionB) { wantedPixel_ = -1; return true; }
            if (ctrl) {
                pos_.index = 0;
                GetLineAndColumnFromIndex(pos_.line, pos_.column, 0);
            } else if (pos_.column != 0) {
                pos_.column = 0;
                GetIndexFromLineAndColumn(pos_.line, 0, pos_.index);
            }
            commonTail();
            return true;
        }
        case 0x25: {   // Left
            if (!(flags_ & kTEEditable)) return true;
            if (pos_.index == 0) {
                if (pos_.selectionA == pos_.selectionB) { PlaySoundA(2); wantedPixel_ = -1; return true; }
            } else if ((modifiers & 4) == 0) {
                if (!ctrl) IncrementInsertionPositionByComposedCharacter(-1);
                else IncrementInsertionPositionByWord(-1);
            } else if (pos_.column != 0) {
                pos_.column = 0;
                GetIndexFromLineAndColumn(pos_.line, 0, pos_.index);
            }
            commonTail();
            return true;
        }
        case 0x27: {   // Right
            if (!(flags_ & kTEEditable)) return true;
            if (Size() <= pos_.index && pos_.selectionA == pos_.selectionB) { PlaySoundA(2); wantedPixel_ = -1; return true; }
            if (pos_.index < Size()) {
                if (modifiers & 4) {
                    const std::uint32_t end = GetCharacterCountOfLine(pos_.line);
                    if (pos_.column != end) {
                        pos_.column = end;
                        GetIndexFromLineAndColumn(pos_.line, end, pos_.index);
                        MoveInsertionPositionToBeforeReturnIfNecessary();
                    }
                }
                if (!ctrl) IncrementInsertionPositionByComposedCharacter(1);
                else IncrementInsertionPositionByWord(1);
            }
            commonTail();
            return true;
        }
        case 0x2d:   // Insert
            if ((flags_ & 1) && (flags_ & kTEInsertKey)) flags_ ^= kTEOverwrite;
            return true;
        case 0x2e:   // Delete
        case 8: {    // Backspace
            if (!(flags_ & kTEEditable)) return true;
            cRZString removed;
            if (pos_.selectionA == pos_.selectionB) {
                if (key == 0x2e) {
                    if (Size() <= pos_.index) { PlaySoundA(1); finishAfterEdit(); return true; }
                    const TextEditPosition before = pos_;
                    const std::uint32_t at = pos_.index;
                    if (!RemoveChar(at, false, &removed)) PlaySoundA(1);
                    else AddUndoRecordForTextDeletion(before, at, removed.Data(), removed.Strlen());
                } else {
                    if (pos_.index == 0) { PlaySoundA(1); finishAfterEdit(); return true; }
                    const TextEditPosition before = pos_;
                    const std::uint32_t previous = GetPreviousCharLength(Chars() + pos_.index, pos_.index, codePage_);
                    const std::uint32_t at = pos_.index - previous;
                    if (!RemoveChar(at, true, &removed)) PlaySoundA(1);
                    else AddUndoRecordForTextDeletion(before, at, removed.Data(), removed.Strlen());
                }
            } else {
                removeSelection(removed);
            }
            finishAfterEdit();
            return true;
        }
        case 0xd:
            if (flags_ & kTEReturnNotify) Notify(kTextEditMsgKey, 0xd, 0);
            return true;
        default:
            return true;
    }
    return result;
}

// ---- painting -----------------------------------------------------------------------------------------

bool cGZWinTextEdit::GZPaint() {   // 0x100261fe6
    auto* context = static_cast<cIGZDrawContext*>(drawContext_);
    const std::uint32_t first = firstLine_;
    const cIGZWin* focus = GetFocus();
    const std::uint32_t textColor = ConvertPackedRGBToNative(colors_[0]);
    const std::uint32_t backColor = ConvertPackedRGBToNative(colors_[1]);
    const std::uint32_t selectedText = ConvertPackedRGBToNative(colors_[4]);
    const std::uint32_t selectedBack = ConvertPackedRGBToNative(colors_[5]);
    std::uint32_t selectionLow, selectionHigh;
    const cRZString* source;
    if (!(flags_ & kTEPassword)) {
        selectionLow = pos_.selectionA;
        selectionHigh = pos_.selectionB;
        source = &text_;
    } else {
        const std::uint32_t glyphs = GetGlyphCountFromString(Chars(), Size(), codePage_);
        if (display_.Strlen() != glyphs) display_.str().assign(glyphs, maskChar_);
        selectionLow = GetGlyphCountFromString(Chars(), pos_.selectionA, codePage_);
        selectionHigh = GetGlyphCountFromString(Chars(), pos_.selectionB, codePage_);
        source = &display_;
    }
    const int scroll = GetHorizontalScrollPixels();
    std::uint32_t lastLine = static_cast<std::uint32_t>(lineStarts_.size());
    if (first + static_cast<std::uint32_t>(visibleLines_) <= lastLine) lastLine = first + static_cast<std::uint32_t>(visibleLines_);

    const int barW = vScroll_ ? vScroll_->AsIGZWin()->GetW() : 0;
    const int barH = hScroll_ ? hScroll_->AsIGZWin()->GetH() : 0;
    if (flags_ & kTEFilled) {
        if (!background_) {
            context->SetColor(GetFillColorRGB());
            context->Fill(drawArea_);
        } else {
            cRZRect bounds;
            background_->GetBufferArea(bounds);
            if (!stretchBackground_) {
                context->BltTiled(static_cast<IDrawTargetBuffer*>(static_cast<void*>(background_)), bounds, drawArea_, 0, 0);
            } else {
                cRZRect tile{bounds.left, bounds.top, bounds.right / 3, bounds.bottom / 3};
                cRZRect dest = drawArea_;
                dest.right -= barW;
                dest.bottom -= barH;
                RZDrawBltEdge(context, background_, tile, dest, true);
            }
        }
    }
    bool unfocused = focus != static_cast<const cIGZWin*>(this);
    if (visibleWidth_ != 0) {
        const std::uint32_t selLow = std::min(selectionLow, selectionHigh);
        const std::uint32_t selHigh = std::max(selectionLow, selectionHigh);
        int y = static_cast<int>(gutterY_) + drawArea_.top;
        for (std::uint32_t line = first; line < lastLine; ++line) {
            int x = static_cast<int>(gutterX_) + drawArea_.left;
            std::uint32_t lineStart, lineEnd;
            int length;
            if (!hScroll_ && (flags_ & 0x202) == 2) {
                lineStart = lineStarts_[line];
                length = static_cast<int>(GetCharacterCountOfLine(line));
                lineEnd = lineStart + static_cast<std::uint32_t>(length);
            } else {
                int offset = 0;
                GetHorizontalLineMetrics(line, static_cast<std::uint32_t>(scroll), lineStart, lineEnd, offset);
                length = static_cast<int>(lineEnd - lineStart);
                x += offset;
            }
            if (length != 0) {
                const char* data = source->Data();
                length -= data[lineEnd] == '\n' ? 1 : 0;
                std::uint32_t partStart = 0, partEnd = 0;
                if (pos_.selectionA != pos_.selectionB && lineStart < selHigh) {
                    const std::uint32_t lineLimit = lineStart + static_cast<std::uint32_t>(length);
                    if (selLow < lineLimit) {
                        partStart = std::max(selLow, lineStart);
                        partEnd = std::min(selHigh, lineLimit);
                    }
                }
                cRZRect clip = drawArea_;
                clip.right -= barW;
                clip.bottom -= barH;
                clip.left += static_cast<int>(gutterX_);
                clip.top += static_cast<int>(gutterY_);
                clip.right -= static_cast<int>(gutterX_);
                clip.bottom -= static_cast<int>(gutterY_);
                bool selectionPath;
                if (!unfocused) selectionPath = partStart <= lineEnd && partEnd != partStart && lineStart < partEnd;
                else selectionPath = !(lineEnd < partStart || partEnd <= lineStart || partEnd == partStart || !(flags_ & kTEKeepHighlight));
                if (!selectionPath) {
                    font_->DrawTextLine(FontTarget(drawBuffer_), x, y, data + lineStart, static_cast<std::uint32_t>(length), textColor, backColor, &clip, nullptr);
                } else {
                    const int lineWidth = font_->GetStringWidth(data + lineStart, static_cast<std::uint32_t>(length));
                    if (lineStart < partStart || partEnd < lineStart + static_cast<std::uint32_t>(length)) {
                        font_->DrawTextLine(FontTarget(drawBuffer_), x, y, data + lineStart, static_cast<std::uint32_t>(length), textColor, backColor, &clip, nullptr);
                        int before = 0;
                        if (partStart != lineStart) before = font_->GetStringWidth(data + lineStart, partStart - lineStart);
                        const int selWidth = font_->GetStringWidth(data + partStart, partEnd - partStart);
                        cRZRect fill{x + before, y, x + before + selWidth, y + lineHeight_};
                        context->SetColor(colors_[7]);
                        context->Fill(fill);
                        font_->DrawTextLine(FontTarget(drawBuffer_), fill.left, y, data + partStart, partEnd - partStart, selectedText, selectedBack, &clip, nullptr);
                    } else {
                        cRZRect fill{x, y, x + lineWidth, y + lineHeight_};
                        context->SetColor(colors_[7]);
                        context->Fill(fill);
                        font_->DrawTextLine(FontTarget(drawBuffer_), x, y, data + lineStart, static_cast<std::uint32_t>(length), selectedText, selectedBack, &clip, nullptr);
                    }
                }
            }
            y += lineHeight_;
        }
        unfocused = focus != static_cast<const cIGZWin*>(this);
        if (caretVisible_ && !unfocused) {
            const std::uint32_t line = pos_.line;
            if (first <= line && line < first + static_cast<std::uint32_t>(visibleLines_)) {
                const char* at = source->Data();
                if (!lineStarts_.empty()) at += lineStarts_[line];
                int caretX = (drawArea_.left - scroll) + static_cast<int>(gutterX_) + font_->GetStringWidth(at, pos_.column);
                if (drawArea_.left + static_cast<int>(gutterX_) <= caretX) {
                    const int thickness = 2 - (static_cast<std::uint32_t>(lineHeight_) < 0x18 ? 1 : 0);
                    if (drawArea_.right <= caretX && (flags_ & kTEWrap)) caretX = (drawArea_.right - static_cast<int>(gutterX_)) - thickness;
                    if (caretX < drawArea_.right - static_cast<int>(gutterX_)) {
                        int top = (static_cast<int>(line) - static_cast<int>(first)) * lineHeight_ + static_cast<int>(gutterY_) + drawArea_.top;
                        const int topLimit = drawArea_.top + 1;
                        if (top < topLimit) top = topLimit;
                        int bottom = drawArea_.bottom - 1;
                        if (top + lineHeight_ <= bottom) bottom = top + lineHeight_;
                        context->SetColor(colors_[6]);
                        context->SetLineThickness(static_cast<float>(thickness));
                        context->Line(caretX, top, caretX, bottom);
                        context->SetLineThickness(1.0f);
                    }
                }
            }
            unfocused = false;
        }
    }
    if ((flags_ & kTEOutline) && !unfocused) RZDrawRectangleOutline(context, drawArea_, colors_[8], 1);
    return true;
}

}  // namespace gz
