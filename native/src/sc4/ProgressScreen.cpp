#include "sc4/ProgressScreen.h"

#include <utility>

#include "gz/RZDraw.h"
#include "rz/cRZRandom.h"

namespace sc4 {

namespace {
// The lock flags the original passes (the write bit 0x8000 with a region code).
constexpr auto kLockFill = static_cast<cIGZBuffer::eLockFlags>(0x8001);
constexpr auto kLockTile = static_cast<cIGZBuffer::eLockFlags>(0x8010);
constexpr auto kLockBlend = static_cast<cIGZBuffer::eLockFlags>(0x8080);
constexpr auto kUnlockBlend = static_cast<cIGZBuffer::eLockFlags>(0x8040);   // the original unlocks with a different code
constexpr auto kLockSource = static_cast<cIGZBuffer::eLockFlags>(0x40);
constexpr int kThumbnailLift = 0x3c;   // the thumbnail sits this much above the vertical middle (half of it)

cRZRect AreaOf(cIGZBuffer* buffer) {
    cRZRect area;
    buffer->GetBufferArea(area);
    return area;
}
}  // namespace

void cSC4ProgressScreen::Assign(cIGZBuffer*& slot, cIGZBuffer* value) {
    if (value) value->AddRef();
    cIGZBuffer* old = slot;
    slot = value;
    if (old) old->Release();
}
void cSC4ProgressScreen::Assign(cIGZFont*& slot, cIGZFont* value) {
    if (value) value->AddRef();
    cIGZFont* old = slot;
    slot = value;
    if (old) old->Release();
}

cSC4ProgressScreen::cSC4ProgressScreen(IProgressScreenHost* host, const cRZRect& area, bool obliterating)
    : host_(host), area_(area), textArea_(area), obliterating_(obliterating) {
    textArea_.top = area.top + 0xc;
    textArea_.bottom = area.top + 100;
}

cSC4ProgressScreen::~cSC4ProgressScreen() {
    Shutdown();
    Assign(background_, nullptr);
    Assign(titleFont_, nullptr);
    Assign(lineFont_, nullptr);
    Assign(thumbnailMask_, nullptr);
    Assign(thumbnail_, nullptr);
    Assign(screen_, nullptr);
}

bool cSC4ProgressScreen::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != 1 && iid != 0x452294aa) return false;   // the object itself for both (cIGZMessageTarget2)
    *out = static_cast<cIGZMessageTarget2*>(this);
    AddRef();
    return true;
}

// The fonts, a screen-sized buffer in the display format, the colours, the background tile and the shuffled tips. Without
// either font nothing else is set up; a buffer or a background that cannot be made shuts the screen down again.
void cSC4ProgressScreen::Init() {
    if (initialized_) return;
    initialized_ = true;
    host_->AddNotification(this, kMsgCityInitBegins);
    Assign(lineFont_, host_->Font(kProgressLineFontId));
    Assign(titleFont_, host_->Font(kProgressTitleFontId));
    if (!lineFont_ || !titleFont_) return;
    textArea_.bottom = textArea_.top + lineFont_->GetLineHeight() * 2 + 0xf + titleFont_->GetLineHeight();
    Assign(screen_, nullptr);
    cIGZBuffer* buffer = host_->CreateScreenBuffer();
    screen_ = buffer;   // the creator's reference
    if (!screen_ || !screen_->Init(static_cast<std::uint32_t>(area_.right - area_.left), static_cast<std::uint32_t>(area_.bottom - area_.top),
                                   cGZBufferColorType{kBufferColorDisplay, 0})) {
        Shutdown();
        return;
    }
    if (!obliterating_) {
        textColor_ = screen_->ConvertRGBValueToNative(0xff, 0xff, 0xff);
        blue1_ = screen_->ConvertRGBValueToNative(0x8a, 0x96, 0xbe);
        blue2_ = screen_->ConvertRGBValueToNative(0x76, 0x81, 0xa3);
        backgroundColor_ = screen_->ConvertRGBValueToNative(0xbb, 0xcc, 0xcc);
    } else {
        textColor_ = screen_->ConvertRGBValueToNative(0xff, 0, 0);
        backgroundColor_ = screen_->ConvertRGBValueToNative(0xff, 0xff, 0xff);
    }
    cIGZBuffer* background = host_->LoadImage(kProgressImageGroup, kProgressBackgroundInstance);
    if (!background) {
        Shutdown();
        return;
    }
    Assign(background_, background);
    background->Release();
    cRZString tip;
    for (std::uint32_t instance = kProgressFirstTipInstance; host_->LoadString(kProgressTipGroup, instance, tip); ++instance) tips_.push_back(tip);
    // std::random_shuffle(tips, cRZRandom(-1)): every element from the second on swaps with one at or before it
    cRZRandom random(0xffffffffu);
    for (std::size_t i = 1; i < tips_.size(); ++i) std::swap(tips_[i], tips_[random.RandomUint32Uniform(static_cast<std::uint32_t>(i + 1))]);
    tipTimer_.Start();
    tipTimer_.SetElapsedTimeFloat(1000.0f);   // the first caption shows a tip at once
}

void cSC4ProgressScreen::Shutdown() {
    if (!initialized_) return;
    initialized_ = false;
    if (screen_) {
        screen_->Shutdown();
        Assign(screen_, nullptr);
    }
    tips_.clear();
    tipTimer_.Stop();
    Assign(background_, nullptr);
    Assign(lineFont_, nullptr);
    Assign(titleFont_, nullptr);
    host_->RemoveNotification(this, kMsgCityInitBegins);
}

// The city's own region-view pictures, or the default snapshot for its terrain type and size.
bool cSC4ProgressScreen::DoMessage(cIGZMessage2* message) {
    if (message->GetType() != kMsgCityInitBegins) return false;
    Assign(thumbnailMask_, nullptr);
    Assign(thumbnail_, nullptr);
    if (!host_->ReadCityThumbnails(&thumbnail_, &thumbnailMask_)) {
        const int index = host_->BaseTerrainType() * 3 + host_->CitySizeType();
        if (index >= 0 && index < 6) {   // the original indexes the table unchecked
            cIGZBuffer* image = host_->LoadImage(kProgressImageGroup, kDefaultSnapInstances[index][0]);
            cIGZBuffer* mask = host_->LoadImage(kProgressImageGroup, kDefaultSnapInstances[index][1]);
            if (image) { Assign(thumbnail_, image); image->Release(); }
            if (mask) { Assign(thumbnailMask_, mask); mask->Release(); }
        }
    }
    return true;
}

// Line 1 takes the next tip when a second has passed since the last one (or it is empty); index 0 sets the title and clears the
// two lines under it. The text of any other index is not used: those calls only move the tips on and repaint.
void cSC4ProgressScreen::SetCaption(std::uint32_t index, const cIGZString& text) {
    const float elapsed = static_cast<float>(tipTimer_.GetElapsedTimeU32());
    if (elapsed >= kProgressTipPeriodMs || captions_[1].Strlen() == 0) {
        if (!tips_.empty()) {
            tipTimer_.SetElapsedTimeFloat(0.0f);
            captions_[1] = tips_.back();
            tips_.pop_back();
        }
    }
    if (index == 0) {
        captions_[0].Copy(text);
        captions_[1].Erase(0, 0xffffffffu);
        captions_[2].Erase(0, 0xffffffffu);
    }
    Repaint();
}

void cSC4ProgressScreen::Repaint() {
    if (obliterating_) {
        RepaintForObliterationProgress();
        return;
    }
    host_->BeginScreenDraw();
    if (!screen_ || !thumbnail_ || !thumbnailMask_) return;
    // the background colour, then the tile over the whole buffer
    if (screen_->Lock(kLockFill)) {
        screen_->Fill(backgroundColor_, nullptr);
        screen_->Unlock(kLockFill);
    }
    if (screen_->Lock(kLockTile)) {
        const cRZRect tile = AreaOf(background_), whole = AreaOf(screen_);
        RZDrawBltTiled(screen_, background_, tile, whole, 0, 0, nullptr);
        screen_->Unlock(kLockTile);
    }
    // the thumbnail, centred across and a little above the middle, clipped to itself and to the screen; the image is
    // premultiplied, so each covered pixel is the screen scaled by (255 - mask) plus the image
    const cRZRect thumb = AreaOf(thumbnail_);
    const int dx = (area_.right - (thumb.right - thumb.left)) / 2 - thumb.left;
    const int dy = ((area_.bottom - (thumb.bottom - thumb.top)) - kThumbnailLift) / 2 - thumb.top;
    cRZRect source = thumb;
    cRZRect target{thumb.left + dx, thumb.top + dy, thumb.right + dx, thumb.bottom + dy};
    auto clip = [&](const cRZRect& bounds, const cRZRect& against) {
        int d = bounds.left - against.left;
        if (d > 0) { source.left += d; target.left += d; }
        d = bounds.top - against.top;
        if (d > 0) { source.top += d; target.top += d; }
        d = bounds.right - against.right;
        if (d < 0) { source.right += d; target.right += d; }
        d = bounds.bottom - against.bottom;
        if (d < 0) { source.bottom += d; target.bottom += d; }
    };
    clip(AreaOf(thumbnail_), source);
    clip(AreaOf(screen_), target);
    if (target.right - target.left > 0 && target.bottom - target.top > 0 && screen_->Lock(kLockBlend)) {
        if (thumbnail_->Lock(kLockSource)) {
            if (thumbnailMask_->Lock(kLockSource)) {
                auto* imageBits = static_cast<const std::uint8_t*>(thumbnail_->GetColorSurfaceBits());
                auto* maskBits = static_cast<const std::uint8_t*>(thumbnailMask_->GetColorSurfaceBits());
                const std::uint32_t imageStride = thumbnail_->GetColorSurfaceStride(), maskStride = thumbnailMask_->GetColorSurfaceStride();
                int y = target.top;
                for (int row = source.top; row < source.bottom; ++row, ++y) {
                    auto* image = reinterpret_cast<const std::uint32_t*>(imageBits + row * imageStride + source.left * 4);
                    const std::uint8_t* mask = maskBits + maskStride * row + source.left * 4;
                    int x = target.left;
                    for (int column = source.left; column < source.right; ++column, ++x, ++image, mask += 4) {
                        const std::uint8_t alpha = *mask;
                        if (!alpha) continue;
                        const std::uint32_t pixel = *image;
                        std::uint8_t r = 0, g = 0, b = 0;
                        screen_->ConvertNativeValueToRGB(screen_->GetPixel(x, y), r, g, b);
                        const std::uint32_t keep = 0xffu - alpha;
                        r = static_cast<std::uint8_t>(((r * keep + 0x80) >> 8) + (pixel >> 16));
                        g = static_cast<std::uint8_t>(((g * keep + 0x80) >> 8) + (pixel >> 8));
                        b = static_cast<std::uint8_t>(((b * keep + 0x80) >> 8) + pixel);
                        screen_->SetPixel(x, y, screen_->ConvertRGBValueToNative(r, g, b));
                    }
                }
                thumbnailMask_->Unlock(kLockSource);
            }
            thumbnail_->Unlock(kLockSource);
        }
        screen_->Unlock(kUnlockBlend);
    }
    // the title in the title font, the two lines under it in the line font, each centred in the text area
    if (lineFont_ && titleFont_ && lineFont_->LockSurfaceForFontUse(screen_)) {
        int y = textArea_.top;
        for (int i = 0; i < 3; ++i) {
            cIGZFont* font = i == 0 ? titleFont_ : lineFont_;
            const cRZString& line = captions_[i];
            if (line.Strlen() != 0) {
                const int width = font->GetStringWidth(line.ToChar(), line.Strlen());
                font->DrawTextLine(screen_, ((textArea_.right - textArea_.left) - width) / 2, y, line.ToChar(), line.Strlen(), textColor_,
                                   backgroundColor_, nullptr, nullptr);
            }
            y += font->GetLineHeight() + 2;
        }
        lineFont_->UnlockSurfaceForFontUse(screen_);
    }
    host_->ShowBuffer(screen_, AreaOf(screen_), area_);
    host_->EndScreenDraw();
}

// A white frame and buffer with the "obliterating" line in red.
void cSC4ProgressScreen::RepaintForObliterationProgress() {
    if (!screen_) return;
    host_->ClearScreen(1.0f, 1.0f, 1.0f, 1.0f);
    if (screen_->Lock(kLockFill)) {
        screen_->Fill(backgroundColor_, nullptr);
        screen_->Unlock(kLockFill);
    }
    if (lineFont_ && lineFont_->LockSurfaceForFontUse(screen_)) {
        cRZString text;
        host_->LoadString(kProgressTextGroup, kProgressObliteratingInstance, text);
        const int width = lineFont_->GetStringWidth(text.ToChar(), text.Strlen());
        lineFont_->DrawTextLine(screen_, ((textArea_.right - textArea_.left) - width) / 2, textArea_.top, text.ToChar(), text.Strlen(),
                                textColor_, backgroundColor_, nullptr, nullptr);
        lineFont_->UnlockSurfaceForFontUse(screen_);
        host_->ShowBuffer(screen_, AreaOf(screen_), area_);
    }
    host_->EndScreenDraw();
}

}  // namespace sc4
