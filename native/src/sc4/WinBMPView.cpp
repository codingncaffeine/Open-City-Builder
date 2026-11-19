#include "sc4/WinBMPView.h"

#include <cmath>
#include <cstddef>

#include "gz/DrawContext.h"
#include "gz/GimexFactory.h"
#include "gz/RZDraw.h"
#include "rz/cRZCallbackTimer.h"
#include "rz/cRZFile.h"
#include "rz/cRZString.h"

namespace sc4 {

namespace {

// The one Gimex factory (the original's file-static cWinBMPView::mpGimexFactory), owned by the views through the host
// and released by Shutdown and by the destructor.
cGZGimexFactory* g_gimexFactory = nullptr;

void ReleaseGimexFactory() {
    if (g_gimexFactory) {
        g_gimexFactory->Release();
        g_gimexFactory = nullptr;
    }
}

// A host that paints nothing and has no image codec; tests install their own.
class cNullWinBMPViewHost final : public IWinBMPViewHost {
public:
    cIGZBuffer* DrawBuffer(cGZWin& window) override { (void)window; return nullptr; }
    cGZGimexFactory* CreateGimexFactory() override { return nullptr; }
};
cNullWinBMPViewHost g_nullHost;
IWinBMPViewHost* g_host = &g_nullHost;

std::uint64_t SystemNow() { return cRZCallbackTimer::GetCurrentSystemMillisecondTime(); }
WinBMPViewClockFn g_clock = &SystemNow;

// The original's signed halving: (int)(((unsigned)v >> 31) + (int)v) >> 1 (0x1003c8b1a / 0x1003c8b32).
int HalfTowardZero(int v) {
    return static_cast<int>((static_cast<std::uint32_t>(v) >> 31) + static_cast<std::uint32_t>(v)) >> 1;
}

// The one-step scroll amount for an axis: image size / 32, at least 1 (0x1003c8fb7..0x1003c8fe8).
int ScrollStep(int direction, int imageSize) {
    const std::uint32_t step = static_cast<std::uint32_t>(imageSize) >> 5;
    if (direction < 0) return step == 0 ? -1 : -static_cast<int>(step);
    return step == 0 ? 1 : static_cast<int>(step);
}

}  // namespace

IWinBMPViewHost* WinBMPViewHost() { return g_host; }
void SetWinBMPViewHost(IWinBMPViewHost* host) { g_host = host ? host : &g_nullHost; }
void SetWinBMPViewClock(WinBMPViewClockFn clock) { g_clock = clock ? clock : &SystemNow; }

// ---- construction -------------------------------------------------------------------------------------------------

cWinBMPView::cWinBMPView(cIGZWinMgr* mgr) : cGZWinBMP(mgr) {}

cWinBMPView::~cWinBMPView() {
    ReleaseGimexFactory();
    cGZWinBMP::Shutdown();   // the original shuts the base down before ~cGZWinBMP
}

bool cWinBMPView::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cIWinBMPView) {
        *out = static_cast<cIWinBMPView*>(this);
        AddRef();
        return true;
    }
    if (iid == gz::kGZIID_cIGZWinBMP) return cGZWinBMP::QueryInterface(iid, out);
    if (iid == kGZIID_cGZWinAlt || iid == 1 || iid == kGZIID_cIGZWin) {
        *out = static_cast<cGZWin*>(this);
        AddRef();
        return true;
    }
    return false;
}

bool cWinBMPView::Init() {
    if (!cGZWinBMP::Init()) return false;
    CalculateAllMetrics();
    // The factory is created even when the window cannot use it; Init still succeeds (0x1003c83a8).
    if (!g_gimexFactory) g_gimexFactory = WinBMPViewHost()->CreateGimexFactory();
    return true;
}

bool cWinBMPView::Shutdown() {
    ReleaseGimexFactory();
    return cGZWinBMP::Shutdown();
}

// ---- geometry -----------------------------------------------------------------------------------------------------

// Recomputes the image rectangle (+0x1a0) from the zoom, the origin, the image size and the window size. The zoom is
// clamped to [minimum, maximum] in place first.
void cWinBMPView::CalculateAllMetrics() {
    const std::uint32_t imageW = static_cast<std::uint32_t>(GetImageWidth());
    const std::uint32_t imageH = static_cast<std::uint32_t>(GetImageHeight());
    const std::uint32_t viewW = static_cast<std::uint32_t>(GetW());
    const std::uint32_t viewH = static_cast<std::uint32_t>(GetH());
    if (imageW == 0 || imageH == 0) return;
    if (zoom_ < zoomMin_) zoom_ = zoomMin_;
    else if (zoomMax_ < zoom_) zoom_ = zoomMax_;
    const float scaledWidth = static_cast<float>(imageW) * zoom_;
    const float scaledHeight = zoom_ * static_cast<float>(imageH);
    const std::uint32_t scaledW = static_cast<std::uint32_t>(scaledWidth);
    const std::uint32_t scaledH = static_cast<std::uint32_t>(scaledHeight);

    std::uint32_t originX = static_cast<std::uint32_t>(originX_);
    if (viewW < scaledW) {   // the scaled image is wider than the window: clamp the origin
        if (alignment_ == kAlignTopLeft) {
            if (scaledWidth - static_cast<float>(viewW) < static_cast<float>(originX_)) {
                originX = static_cast<std::uint32_t>(scaledWidth - static_cast<float>(viewW));
                originX_ = static_cast<int>(originX);
            }
            if (static_cast<std::int32_t>(originX) < 0) {
                originX_ = 0;
                originX = 0;
            }
        } else {
            const std::uint32_t half = viewW >> 1;
            if (scaledWidth - static_cast<float>(half) < static_cast<float>(originX_)) {
                originX = static_cast<std::uint32_t>(scaledWidth - static_cast<float>(half));
                originX_ = static_cast<int>(originX);
            }
            if (originX < half) {
                originX_ = static_cast<int>(half);
                originX = half;
            }
        }
    } else {                 // it fits: centre it when asked to, otherwise start at the left edge
        originX = imageW >> 1;
        if (alignment_ == kAlignTopLeft) originX = 0;
        originX_ = static_cast<int>(originX);
    }

    std::uint32_t originY = static_cast<std::uint32_t>(originY_);
    if (viewH < scaledH) {
        if (alignment_ == kAlignTopLeft) {
            if (scaledHeight - static_cast<float>(viewH) < static_cast<float>(originY_)) {
                originY = static_cast<std::uint32_t>(scaledHeight - static_cast<float>(viewH));
                originY_ = static_cast<int>(originY);
            }
            if (static_cast<std::int32_t>(originY) < 0) {
                originY_ = 0;
                originY = 0;
            }
        } else {
            const std::uint32_t half = viewH >> 1;
            if (scaledHeight - static_cast<float>(half) < static_cast<float>(originY_)) {
                originY = static_cast<std::uint32_t>(scaledHeight - static_cast<float>(half));
                originY_ = static_cast<int>(originY);
            }
            if (originY < half) {
                originY_ = static_cast<int>(half);
                originY = half;
            }
        }
    } else {
        originY = imageH >> 1;
        if (alignment_ == kAlignTopLeft) originY = 0;
        originY_ = static_cast<int>(originY);
    }

    if (alignment_ == kAlignTopLeft) {
        imageDest_.left = -static_cast<int>(originX);
        imageDest_.top = -static_cast<int>(originY);
        imageDest_.right = static_cast<int>(scaledW - originX);
        imageDest_.bottom = static_cast<int>(scaledH - originY);
    } else {
        // Centred: the origin is the image pixel at the window centre, so the rectangle also carries half the
        // difference between the image and scaled sizes.
        const std::int32_t insetX = static_cast<std::int32_t>(viewW >> 1) - static_cast<std::int32_t>(scaledW) / 2;
        const std::int32_t insetY = static_cast<std::int32_t>(viewH >> 1) - static_cast<std::int32_t>(scaledH) / 2;
        const std::int32_t deltaX = static_cast<std::int32_t>(imageW >> 1) - static_cast<std::int32_t>(originX);
        const std::int32_t deltaY = static_cast<std::int32_t>(imageH >> 1) - static_cast<std::int32_t>(originY);
        const std::uint32_t right = static_cast<std::uint32_t>(scaledWidth + static_cast<float>(insetX));
        const std::uint32_t bottom = static_cast<std::uint32_t>(scaledHeight + static_cast<float>(insetY));
        imageDest_.left = insetX + deltaX;
        imageDest_.top = insetY + deltaY;
        imageDest_.right = static_cast<int>(right) + deltaX;
        imageDest_.bottom = static_cast<int>(bottom) + deltaY;
    }
}

void cWinBMPView::SetAlignment(Alignment alignment) {
    if (alignment_ == alignment) return;
    alignment_ = alignment;
    CalculateAllMetrics();
    InvalidateSelfAndParents();
}

bool cWinBMPView::SetZoomRangeMinMax(float minZoom, float maxZoom) {
    zoomMin_ = 0.25f <= minZoom ? minZoom : 0.25f;
    zoomMax_ = maxZoom <= 256.0f ? maxZoom : 256.0f;
    return true;
}

bool cWinBMPView::SetZoom(float zoom) {
    const std::uint64_t savedOrigin = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(originY_)) << 32) |
                                      static_cast<std::uint32_t>(originX_);
    const float oldZoom = zoom_;
    float newZoom = zoomMin_;
    if (zoomMin_ <= zoom) {
        newZoom = zoom;
        if (zoomMax_ < zoom) newZoom = zoomMax_;
    }
    if (oldZoom == newZoom && !std::isnan(oldZoom) && !std::isnan(newZoom)) return true;
    // Keep the image pixel under the window centre in place while the scale changes.
    int imageX = 0, imageY = 0;
    WindowPixelToImagePixel(HalfTowardZero(GetW()), HalfTowardZero(GetH()), imageX, imageY);
    zoom_ = newZoom;
    CalculateAllMetrics();
    CenterOnImagePixel(imageX, imageY);
    CalculateAllMetrics();
    const bool sameOrigin = static_cast<std::uint32_t>(originX_) == static_cast<std::uint32_t>(savedOrigin) &&
                            static_cast<std::uint32_t>(originY_) == static_cast<std::uint32_t>(savedOrigin >> 32);
    if (sameOrigin && zoom_ == oldZoom && !std::isnan(zoom_) && !std::isnan(oldZoom)) return true;
    InvalidateSelfAndParents();
    return true;
}

void cWinBMPView::ZoomIn(float factor) {
    // No zooming-enabled check here; GZOnKeyDown does it (kept quirk).
    SetZoom(factor * zoom_);
}

bool cWinBMPView::ZoomOut(float factor) {
    if (factor < zoomMin_) return false;
    return SetZoom(zoom_ / factor);
}

bool cWinBMPView::SetOrigin(int x, int y) {
    originX_ = x;
    originY_ = y;
    CalculateAllMetrics();
    InvalidateSelfAndParents();
    return true;
}

bool cWinBMPView::CenterOnImagePixel(int x, int y) {
    const std::uint32_t imageW = static_cast<std::uint32_t>(GetImageWidth());
    const std::uint32_t imageH = static_cast<std::uint32_t>(GetImageHeight());
    if (imageW > static_cast<std::uint32_t>(GetW())) {
        if (x < 0) return false;
        if (imageH < static_cast<std::uint32_t>(GetH())) return false;   // kept quirk (0x1003c8d3e)
    } else if (x < 0) {
        return false;
    }
    if (y < 0) return false;
    if (static_cast<std::uint32_t>(x) >= static_cast<std::uint32_t>(GetImageWidth())) return false;
    if (static_cast<std::uint32_t>(y) >= static_cast<std::uint32_t>(GetImageHeight())) return false;
    const std::uint32_t viewW = static_cast<std::uint32_t>(GetW());
    const std::uint32_t viewH = static_cast<std::uint32_t>(GetH());
    float originX = static_cast<float>(x) * zoom_;
    float originY = zoom_ * static_cast<float>(y);
    if (alignment_ == kAlignTopLeft) {
        originX -= static_cast<float>(viewW >> 1);
        originY -= static_cast<float>(viewH >> 1);
    }
    originX_ = static_cast<int>(originX);
    originY_ = static_cast<int>(originY);
    CalculateAllMetrics();
    InvalidateSelfAndParents();
    return true;
}

void cWinBMPView::CenterOnWindowPixel(int x, int y) {
    int imageX = 0, imageY = 0;
    WindowPixelToImagePixel(x, y, imageX, imageY);
    CenterOnImagePixel(imageX, imageY);
}

bool cWinBMPView::WindowPixelToImagePixel(int windowX, int windowY, int& imageX, int& imageY) {
    imageX = windowX;
    imageY = windowY;
    if (!bmp_) return false;
    imageX = static_cast<int>(static_cast<float>(imageX - imageDest_.left) / zoom_);
    imageY = static_cast<int>(static_cast<float>(imageY - imageDest_.top) / zoom_);
    if (imageX < 0 || static_cast<std::uint32_t>(imageX) >= static_cast<std::uint32_t>(GetImageWidth())) return false;
    if (imageY < 0 || static_cast<std::uint32_t>(imageY) >= static_cast<std::uint32_t>(GetImageHeight())) return false;
    return true;
}

bool cWinBMPView::Scroll(int dx, int dy) {
    if (dx == 0 && dy == 0) return true;
    const std::uint64_t savedOrigin = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(originY_)) << 32) |
                                      static_cast<std::uint32_t>(originX_);
    if (dx == -1) dx = ScrollStep(-1, GetImageWidth());
    else if (dx == 1) dx = ScrollStep(1, GetImageWidth());
    if (dy == -1) dy = ScrollStep(-1, GetImageHeight());
    else if (dy == 1) dy = ScrollStep(1, GetImageHeight());
    originX_ += dx;
    originY_ += dy;
    CalculateAllMetrics();
    if (static_cast<std::uint32_t>(originX_) != static_cast<std::uint32_t>(savedOrigin) ||
        static_cast<std::uint32_t>(originY_) != static_cast<std::uint32_t>(savedOrigin >> 32)) {
        InvalidateSelfAndParents();
    }
    return true;
}

// ---- painting -----------------------------------------------------------------------------------------------------

bool cWinBMPView::GZPaint() {
    cIGZBuffer* target = WinBMPViewHost()->DrawBuffer(*this);
    if (!target) {
        // The port's windows draw through their draw context (cGZWinBMP::GZPaint does the same): the fill where the image
        // does not cover the window, then the image stretched into its destination.
        auto* context = static_cast<gz::cIGZDrawContext*>(drawContext_);
        if (!context) return true;
        if (!(flags_ & gz::kWinBMPNoFill) &&
            (!bmp_ || imageDest_.right - imageDest_.left < drawArea_.right - drawArea_.left ||
             imageDest_.bottom - imageDest_.top < drawArea_.bottom - drawArea_.top)) {
            context->SetColor(GetFillColorRGB());
            context->Fill(drawArea_);
        }
        if (bmp_) {
            const cRZRect source{0, 0, static_cast<int>(bmp_->Width()), static_cast<int>(bmp_->Height())};
            const cRZRect destination{drawArea_.left + imageDest_.left, drawArea_.top + imageDest_.top,
                                      drawArea_.left + imageDest_.right, drawArea_.top + imageDest_.bottom};
            context->Blt(bmp_, source, destination);
        }
        return true;
    }
    // The fill is skipped by flag 2 and only drawn when there is no image or the image does not cover the window.
    if (!(flags_ & gz::kWinBMPNoFill) &&
        (!bmp_ || imageDest_.right - imageDest_.left < drawArea_.right - drawArea_.left ||
         imageDest_.bottom - imageDest_.top < drawArea_.bottom - drawArea_.top)) {
        if (target && target->Lock(static_cast<cIGZBuffer::eLockFlags>(0x8001))) {
            target->Fill(GetFillColor(), drawArea_, nullptr);
            target->Unlock(static_cast<cIGZBuffer::eLockFlags>(0x8001));
        }
    }
    if (bmp_ && target && target->Lock(static_cast<cIGZBuffer::eLockFlags>(0x8010))) {
        cRZRect source;
        source.left = 0;
        source.top = 0;
        source.right = static_cast<int>(bmp_->Width());
        source.bottom = static_cast<int>(bmp_->Height());
        cRZRect destination;
        destination.left = drawArea_.left + imageDest_.left;
        destination.top = drawArea_.top + imageDest_.top;
        destination.right = drawArea_.left + imageDest_.right;
        destination.bottom = drawArea_.top + imageDest_.bottom;
        if (zoom_ != 1.0f || std::isnan(zoom_)) {
            RZDrawBltStretch(target, bmp_, source, destination, &drawArea_);
        } else {
            target->Blt(bmp_, source, destination, nullptr);
        }
        target->Unlock(static_cast<cIGZBuffer::eLockFlags>(0x8010));
    }
    return true;
}

// ---- window overrides ---------------------------------------------------------------------------------------------

void cWinBMPView::SetArea(int l, int t, int r, int b) {
    cGZWinBMP::SetArea(l, t, r, b);
    CalculateAllMetrics();
}

void cWinBMPView::SetShouldFitImageInView(bool fit) { fitImageInView_ = fit; }
void cWinBMPView::EnableZooming(bool enable) { zoomingEnabled_ = enable; }
void cWinBMPView::EnableScrolling(bool enable) { scrollingEnabled_ = enable; }

bool cWinBMPView::GZOnKeyDown(std::uint32_t key, std::uint32_t flags) {
    (void)flags;
    switch (key) {
        case 0x25:   // left
            if (scrollingEnabled_) Scroll(-1, 0);
            break;
        case 0x26:   // up
            if (scrollingEnabled_) Scroll(0, -1);
            break;
        case 0x27:   // right
            if (scrollingEnabled_) Scroll(1, 0);
            break;
        case 0x28:   // down
            if (scrollingEnabled_) Scroll(0, 1);
            break;
        case 0x6b:   // 'k': zoom in by the fixed factor 2.0 (0x100a16690)
            if (zoomingEnabled_) ZoomIn(2.0f);
            break;
        case 0x6d:   // 'm': zoom out by 2.0
            if (zoomingEnabled_) ZoomOut(2.0f);
            break;
        default:
            break;
    }
    return true;
}

bool cWinBMPView::GZOnKeyUp(std::uint32_t key, std::uint32_t flags) {
    (void)key;
    (void)flags;
    return true;
}

bool cWinBMPView::GZOnMouseDownL(int x, int y, std::uint32_t flags) {
    (void)flags;
    if (scrollingEnabled_) CenterOnWindowPixel(x, y);
    return true;
}

bool cWinBMPView::GZOnMouseDownR(int x, int y, std::uint32_t flags) {
    (void)flags;
    // The original only keeps the anchor when the manager's GZSetCapture returns true; the port's cIGZWinMgr seam
    // has no result for it, so the capture is taken as granted.
    GZSetCapture();
    anchorX_ = x;
    anchorY_ = y;
    PostMsg(this, kWinBMPViewScrollMsg, 0, 0, 0);   // manager slot 0x78, the 5-argument post
    return true;
}

bool cWinBMPView::GZOnMouseUpR(int x, int y, std::uint32_t flags) {
    (void)flags;
    if (GZAmCapturing()) {
        anchorX_ = x;
        anchorY_ = y;
        GZReleaseCapture();
    }
    return true;
}

bool cWinBMPView::GZOnMouseMove(int x, int y, std::uint32_t flags) {
    (void)x;
    (void)y;
    (void)flags;
    return true;
}

bool cWinBMPView::GZOnMouseWheel(int x, int y, std::uint32_t flags, int delta) {
    (void)x;
    (void)y;
    (void)flags;
    (void)delta;
    return true;
}

// While the right button drags, follow the cursor in 8-pixel steps and re-post the message; at most one step every
// minimum-ms (default 20).
bool cWinBMPView::DoMessage(cGZMessage& message) {
    if (message.type == kWinBMPViewScrollMsg) {
        const std::uint64_t now = g_clock();
        if (lastScrollMs_ + minMsPerScroll_ < now) {
            int cursorX = 0, cursorY = 0;
            mgr_->GetCursorPos(this, cursorX, cursorY);   // manager slot 0x110
            int dx = 0, dy = 0;
            if (cursorX < anchorX_ - 8) dx = -1;
            if (anchorX_ + 8 < cursorX) dx = 1;
            if (cursorY < anchorY_ - 8) dy = -1;
            if (anchorY_ + 8 < cursorY) dy = 1;
            Scroll(dx, dy);
            lastScrollMs_ = now;
        }
        if (GZAmCapturing()) PostMsg(this, kWinBMPViewScrollMsg, 0, 0, 0);
        return true;
    }
    if (message.type - 7 < 8) return cGZWin::DoMessage(message);
    return cGZWinBMP::DoMessage(message);
}

// ---- bitmap access ------------------------------------------------------------------------------------------------

void cWinBMPView::SetBMP(cIGZBuffer* buffer) {
    cGZWinBMP::SetBMP(buffer);
    originX_ = 0;
    originY_ = 0;
    CalculateAllMetrics();
    InvalidateSelfAndParents();
}

void cWinBMPView::ClearBMP() { SetBMP(nullptr); }

int cWinBMPView::GetImageWidth() { return bmp_ ? static_cast<int>(bmp_->Width()) : 0; }
int cWinBMPView::GetImageHeight() { return bmp_ ? static_cast<int>(bmp_->Height()) : 0; }

void cWinBMPView::FreeMemory(void* memory) {
    if (memory) ::operator delete[](memory);
}

// Loads one image through the shared Gimex factory (colour type 9 = ARGB8888) without touching the window.
bool cWinBMPView::SetBMPFromFile(cIGZBuffer** out, const cIGZString& path) {
    if (!out) return false;
    *out = nullptr;
    if (!g_gimexFactory) {
        g_gimexFactory = WinBMPViewHost()->CreateGimexFactory();
        if (!g_gimexFactory) return false;
    }
    cIGZBuffer* image = nullptr;
    if (!g_gimexFactory->LoadFromFile(path, &image, 9)) return false;
    *out = image;
    return image != nullptr;
}

bool cWinBMPView::SetBMPFromFile(const cIGZString& path) {
    cIGZBuffer* image = nullptr;
    if (!SetBMPFromFile(&image, path)) {
        ClearBMP();
        return false;
    }
    // Fit the image in the window by scaling it down when it is larger, at most 1:1.
    if (fitImageInView_) {
        const float viewW = static_cast<float>(GetW());
        const float viewH = static_cast<float>(GetH());
        const float imageW = static_cast<float>(image->Width());
        const float imageH = static_cast<float>(image->Height());
        if (viewW / imageW < 1.0f || viewH / imageH < 1.0f) {
            if (cIGZBuffer* scaled = WinBMPViewHost()->DownscaleImageToFit(image, GetW(), GetH())) {
                image->Release();
                image = scaled;
            }
        }
    }
    SetBMP(image);
    image->Release();
    return true;
}

// Saves `buffer` through a fresh Gimex factory, adding the extension of `format` when the path does not have it. The
// extended name is written back into `path` (which is why the argument is non-const in the original).
bool cWinBMPView::WriteBMPToFile(cIGZBuffer* buffer, std::uint32_t format, const cIGZString& path) {
    cRZString finalPath(path);
    cRZString originalPath(path);
    const std::uint32_t fmt = format ? format : 2;
    const char* extension = nullptr;
    switch (fmt) {
        case 1: extension = ".png"; break;
        case 2: extension = ".jpg"; break;
        case 3: extension = ".gif"; break;
        case 4: extension = ".bmp"; break;
        default: break;
    }
    if (extension && !finalPath.EndsWith(extension, 4)) finalPath.AppendChars(extension, 4);
    if (finalPath.CompareToString(originalPath, true) != 0) {
        // Kept quirk: the original tests and removes the *original* name, not the extended one (0x1003ca02b).
        if (cRZFile::FileExists(originalPath)) cRZFile::Remove(originalPath);
        const_cast<cIGZString&>(path).AssignFromCString(finalPath.ToChar());
    }
    cGZGimexFactory* factory = new cGZGimexFactory;
    factory->AddRef();
    const bool result = factory->SaveToFile(finalPath, buffer, 100);
    factory->Release();
    return result;
}

}  // namespace sc4
