#include "gz/DrawContext.h"

#include <algorithm>
#include <utility>

#include "platform/Unwired.h"

namespace gz {

static IDrawContextHost* g_host = nullptr;
IDrawContextHost* DrawContextHost() { return g_host; }
void SetDrawContextHost(IDrawContextHost* host) { g_host = host; }

namespace {
bool Overlaps(const cRZRect& a, const cRZRect& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}
cRZRect Intersect(const cRZRect& a, const cRZRect& b) {
    return {std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
}
bool NonEmpty(const cRZRect& r) { return r.left < r.right && r.top < r.bottom; }
}  // namespace

cGZDrawContext::cGZDrawContext() = default;

cGZDrawContext::~cGZDrawContext() {
    if (engine_) engine_->Shutdown();
}

bool cGZDrawContext::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != kGZIID_cIGZUnknown && iid != kGZIID_cIGZDrawContext) return false;
    *out = this;
    AddRef();
    return true;
}

std::uint32_t cGZDrawContext::Release() {
    if (--refCount_ == 0) {
        delete this;
        return 0;
    }
    return refCount_;
}

void cGZDrawContext::PushTransform() { transforms_.push_back({originX_, originY_, clip_}); }

void cGZDrawContext::PopTransform() {
    if (transforms_.empty()) return;
    const cTransform& saved = transforms_.back();
    originX_ = saved.x;
    originY_ = saved.y;
    clip_ = saved.clip;
    transforms_.pop_back();
    engine_->SetClipRect(clip_);
}

void cGZDrawContext::ClipReset() { clip_ = canvas_; }

// The new clip is the intersection with the (origin-translated) rectangle, or empty when they miss.
void cGZDrawContext::Clip(const cRZRect& rect) {
    const int dx = static_cast<int>(originX_), dy = static_cast<int>(originY_);
    const cRZRect moved{rect.left + dx, rect.top + dy, rect.right + dx, rect.bottom + dy};
    clip_ = Overlaps(moved, clip_) ? Intersect(moved, clip_) : cRZRect{0, 0, 0, 0};
    RecomputeClipRect();
}

void cGZDrawContext::ClipAbsolute(const cRZRect& rect) {
    clip_ = Overlaps(rect, clip_) ? Intersect(rect, clip_) : cRZRect{0, 0, 0, 0};
    engine_->SetClipRect(clip_);
}

void cGZDrawContext::Offset(const cRZPoint& offset) {
    originX_ += static_cast<float>(offset.x);
    originY_ += static_cast<float>(offset.y);
}

void cGZDrawContext::OffsetAbsolute(const cRZPoint& origin) {
    originX_ = static_cast<float>(origin.x);
    originY_ = static_cast<float>(origin.y);
}

void cGZDrawContext::PushState() { states_.push_back({antialias_, bltModulate_, alphaBlend_, lineThickness_, thicknessPixels_}); }

void cGZDrawContext::PopState() {
    if (states_.empty()) return;
    const cState saved = states_.back();
    SetAntialiasing(saved.antialias);
    SetBltModulate(saved.modulate);
    SetAlphaBlend(saved.alphaBlend);
    SetLineThickness(saved.thickness);
    states_.pop_back();
}

// Attaching a target creates a fresh engine for it and pushes the whole state into it.
void cGZDrawContext::SetTarget(IDrawTargetBuffer* target) {
    if (target_ == target) return;
    if (engine_) {
        engine_->Shutdown();
        cIGZDrawEngine* old = engine_;
        engine_ = nullptr;
        old->Release();
    }
    IDrawTargetBuffer* previous = target_;
    if (target_ != target) {
        if (target) target->AddRef();
        target_ = target;
        if (previous) previous->Release();
    }
    if (!target) return;
    clip_ = *target->GetBufferArea();
    originX_ = originY_ = 0.0f;
    engine_ = g_host->CreateEngine(target);
    engine_->Init(target);
    engine_->SetClipRect(clip_);
    engine_->SetColorTable(0, 4, colors_);
    engine_->SetAlphaBlend(alphaBlend_);
    engine_->SetBltModulate(bltModulate_);
    engine_->SetAntialiasing(antialias_);
    engine_->SetLineThickness(lineThickness_);
    cRZRect scratch;
    engine_->SetBltSource(nullptr, 0.0f, 0.0f, 0.0f, 0.0f, scratch);
    engine_->SetGradientTransform(nullptr, 0.0f, 0.0f);
    engine_->GetCanvasArea(canvas_);
    bltActive_ = false;
    tiling_ = false;
    suspendCount_ = 0;
}

void cGZDrawContext::ResetToDefaultState() {
    SetAlphaBlend(0);
    SetBltModulate(false);
    SetAntialiasing(false);
    SetLineThickness(1.0f);
    ClearBlt();
    ClearBltTiling();
    ClearGradient();
}

void cGZDrawContext::SetColor(std::uint32_t index, std::uint32_t rgb) {
    colors_[index] = rgb;
    nativeColors_[index] = target_->ConvertRGBValueToNative((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
    if (engine_) engine_->SetColorTable(0, 4, colors_);
}

void cGZDrawContext::SetAlphaBlend(int blend) {
    if (alphaBlend_ == blend) return;
    alphaBlend_ = blend;
    if (engine_) engine_->SetAlphaBlend(blend);
}

void cGZDrawContext::SetBltModulate(bool on) {
    if (bltModulate_ == on) return;
    bltModulate_ = on;
    if (engine_) engine_->SetBltModulate(on);
}

void cGZDrawContext::SetAntialiasing(bool on) {
    if (antialias_ == on) return;
    antialias_ = on;
    if (engine_) engine_->SetAntialiasing(on);
}

void cGZDrawContext::SetLineThickness(float thickness) {
    if (lineThickness_ == thickness) return;     // NaN never compares equal, so it always goes through
    lineThickness_ = thickness <= 0.0f ? 0.0f : thickness;
    const int pixels = static_cast<int>(lineThickness_ + 0.5f);
    thicknessPixels_ = pixels != 0 ? pixels : 1;
    if (engine_) engine_->SetLineThickness(lineThickness_);
}

void cGZDrawContext::ClearBlt() {
    SetBlt(nullptr, 0.0f, 0.0f, 1.0f, 1.0f);
    bltActive_ = false;
    RecomputeClipRect();
}

void cGZDrawContext::SetBlt(cIGZUnknown* source, float x, float y, float sx, float sy) {
    bltActive_ = engine_->SetBltSource(source, x + originX_, y + originY_, sx, sy, bltClip_);
    RecomputeClipRect();
}

void cGZDrawContext::ClearBltTiling() {
    tiling_ = false;
    engine_->ClearBltTiling();
    RecomputeClipRect();
}

void cGZDrawContext::SetBltTiling(const cRZRect& tile) {
    tiling_ = true;
    engine_->SetBltTiling(tile);
    RecomputeClipRect();
}

// While a blit source is active (and not tiled) the engine's clip is the user clip intersected with the blit's.
void cGZDrawContext::RecomputeClipRect() {
    if (!bltActive_ || tiling_) {
        engine_->SetClipRect(clip_);
        return;
    }
    const cRZRect effective = Overlaps(bltClip_, clip_) ? Intersect(bltClip_, clip_) : cRZRect{0, 0, 0, 0};
    engine_->SetClipRect(effective);
}

void cGZDrawContext::Suspend() {
    ++suspendCount_;
    engine_->Flush();
    engine_->Suspend();
}

void cGZDrawContext::Resume() {
    if (suspendCount_ != 0 && --suspendCount_ == 0) engine_->Resume();
}

void cGZDrawContext::Clear() {
    const float rect[4] = {static_cast<float>(clip_.left), static_cast<float>(clip_.top), static_cast<float>(clip_.right),
                           static_cast<float>(clip_.bottom)};
    DrawNoTransform(eDrawingOp::FillRect, rect, 2);
}

void cGZDrawContext::Point(int x, int y) {
    const float point[2] = {static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
    Draw(eDrawingOp::Point, point, 1);
}

void cGZDrawContext::Line(int x0, int y0, int x1, int y1) {
    const float points[4] = {static_cast<float>(x0) + 0.5f, static_cast<float>(y0) + 0.5f, static_cast<float>(x1) + 0.5f,
                             static_cast<float>(y1) + 0.5f};
    Draw(eDrawingOp::Line, points, 2);
}

void cGZDrawContext::Ellipse(int l, int t, int r, int b) {
    // The original asks for op 5 (the *filled* ellipse) here, exactly like FillEllipse.
    const float box[4] = {static_cast<float>(l), static_cast<float>(t), static_cast<float>(r), static_cast<float>(b)};
    Draw(eDrawingOp::FillEllipse, box, 2);
}

void cGZDrawContext::FillEllipse(int l, int t, int r, int b) {
    const float box[4] = {static_cast<float>(l), static_cast<float>(t), static_cast<float>(r), static_cast<float>(b)};
    Draw(eDrawingOp::FillEllipse, box, 2);
}

void cGZDrawContext::Fill(const cRZRect& rect) {
    const float box[4] = {static_cast<float>(rect.left), static_cast<float>(rect.top), static_cast<float>(rect.right),
                          static_cast<float>(rect.bottom)};
    Draw(eDrawingOp::FillRect, box, 2);
}

void cGZDrawContext::Blt(IDrawTargetBuffer* source, const cRZRect& sourceRect, const cRZRect& destRect) {
    const int sw = sourceRect.right - sourceRect.left, sh = sourceRect.bottom - sourceRect.top;
    const int dw = destRect.right - destRect.left, dh = destRect.bottom - destRect.top;
    if (sw == 0 || sourceRect.left > sourceRect.right || sh == 0 || sourceRect.top > sourceRect.bottom) return;
    if (dw == 0 || destRect.left > destRect.right || dh == 0 || destRect.top > destRect.bottom) return;
    SetBlt(source, static_cast<float>(destRect.left - sourceRect.left), static_cast<float>(destRect.top - sourceRect.top),
           static_cast<float>(dw) / static_cast<float>(sw), static_cast<float>(dh) / static_cast<float>(sh));
    ClearBltTiling();
    Fill(destRect);
    ClearBlt();
}

void cGZDrawContext::BltTiled(IDrawTargetBuffer* source, const cRZRect& tile, const cRZRect& destRect, int dx, int dy) {
    if (!NonEmpty(tile) || !NonEmpty(destRect)) return;
    SetBlt(source, static_cast<float>(destRect.left + dx), static_cast<float>(destRect.top + dy), 1.0f, 1.0f);
    SetBltTiling(tile);
    Fill(destRect);
    ClearBltTiling();
    ClearBlt();
}

void cGZDrawContext::Draw(eDrawingOp op, const float* points, int count) {
    transformed_.resize(static_cast<std::size_t>(count) * 2);
    for (int i = 0; i < count; ++i) {
        transformed_[i * 2] = points[i * 2] + originX_;
        transformed_[i * 2 + 1] = points[i * 2 + 1] + originY_;
    }
    DrawNoTransform(op, transformed_.data(), count);
}

void cGZDrawContext::DrawNoTransform(eDrawingOp op, const float* points, int count) {
    std::swap(scratchA_, scratchB_);
    DirectDraw(op, points, count);
}

void cGZDrawContext::DirectDraw(eDrawingOp op, const float* points, int count) {
    const eEngineDrawResult how = engine_->Draw(op, points, count);
    // Tesselate* / Rasterize* fallbacks are not recovered
    if (how != eEngineDrawResult::Done)
        SC4_UNWIRED("cGZDrawContext::DirectDraw: the software Tesselate*/Rasterize* fallback for a primitive the engine refused is not ported");
}

}  // namespace gz
