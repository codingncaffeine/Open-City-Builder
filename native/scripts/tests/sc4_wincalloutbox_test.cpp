// cSC4WinCalloutBox tests: COM lifetime through the two bases, Init's layout of a parent-less-constraint box, the
// setters' clamping / reference counting, and GZPaint's three stages (measure, place, draw) observed on recording
// buffers / fonts / draw context.
#include <cstdio>
#include <string>
#include <vector>

#include "fake_winmgr.h"
#include "gz/Font.h"
#include "sc4/WinCalloutBox.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECK_EQ(a, b) do { const long long va = (a), vb = (b); if (va != vb) { \
    std::printf("FAIL line %d: %s == %s (%lld vs %lld)\n", __LINE__, #a, #b, va, vb); ++failures; } } while (0)
#define CHECK_RECT(r, l, t, rr, b) do { if ((r).left != (l) || (r).top != (t) || (r).right != (rr) || (r).bottom != (b)) { \
    std::printf("FAIL line %d: %s = {%d,%d,%d,%d}\n", __LINE__, #r, (r).left, (r).top, (r).right, (r).bottom); \
    ++failures; } } while (0)

using namespace sc4;
using namespace gz;

namespace {

// ---- a group font: one char is 8 x 16 --------------------------------------------------------------
struct FakeGZFont final : cIGZFont {
    int refs = 0;
    int charWidth = 8;
    int lineHeight = 16;
    int areaCalls = 0;
    int lockCalls = 0;
    int unlockCalls = 0;
    std::vector<cRZRect> measured;
    std::vector<cRZRect> paragraphs;
    std::vector<std::string> drawn;

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    void GetName(cIGZString&) const override {}
    std::uint32_t GetStyle() const override { return 0; }
    bool GetStyle(int) const override { return false; }
    bool SetStyle(int, bool) override { return true; }
    std::uint32_t GetType() const override { return 0; }
    std::uint32_t GetSize() const override { return 16; }
    bool SetSize(std::uint32_t) override { return true; }
    std::uint32_t GetFontColor() const override { return 0; }
    std::uint32_t GetFontBackgroundColor() const override { return 0; }
    void SetFontColor(std::uint32_t, std::uint32_t) override {}
    int GetAntialiasing() override { return 0; }
    void SetAntialiasing(int) override {}
    float GetAntialiasingGammaCorrection() override { return 1; }
    void SetAntialiasingGammaCorrection(float) override {}
    float GetAntialiasingSharpness() override { return 1; }
    void SetAntialiasingSharpness(float) override {}
    void GetFilePath(cIGZString&) const override {}
    void SetFilePath(const cIGZString&) override {}
    bool GetCodePage(int&) const override { return true; }
    bool SetCodePage(int) override { return true; }
    bool IsCodePageAvailable(int) override { return true; }
    bool IsCharEncodingAvailable(int) override { return true; }
    bool IsCharAvailable(void*, std::uint32_t) override { return true; }
    bool PreloadChars(void*, std::uint32_t) override { return true; }
    void GetScaling(float& x, float& y) const override { x = y = 1; }
    void SetScaling(float, float) override {}
    void GetAdvanceScaling(float& x, float& y) const override { x = y = 1; }
    void SetAdvanceScaling(float, float) override {}
    void GetSpacingAdjustment(int& x, int& y) const override { x = y = 0; }
    void SetSpacingAdjustment(int, int) override {}
    int GetLineHeight() override { return lineHeight; }
    int CharWidth(const void*, std::uint32_t n) override { return int(n) * charWidth; }
    int CharHeight(const void*, std::uint32_t) override { return lineHeight; }
    int GetSizeAboveBaseline() override { return 12; }
    int GetSizeBelowBaseline() override { return 4; }
    int GetAdvanceWidth(const void*, std::uint32_t n) override { return int(n) * charWidth; }
    std::uint32_t CalculateWordsToFitInWidth(const void* text, std::uint32_t length, std::uint32_t width,
                                             std::uint32_t*, WordWrapMode) override {
        const std::uint32_t fit = width / std::uint32_t(charWidth);
        for (std::uint32_t i = 0; i < length && i < fit; ++i)
            if (static_cast<const char*>(text)[i] == '\n') return i + 1;
        return length <= fit ? length : fit;
    }
    std::uint32_t CalculateCharsToFitInWidth(const void*, std::uint32_t length, std::uint32_t width,
                                             std::uint32_t*) override {
        const std::uint32_t fit = width / std::uint32_t(charWidth);
        return fit < length ? fit : length;
    }
    std::uint32_t CalculateNumberOfLines(const void*, std::uint32_t, std::uint32_t) override { return 1; }
    std::uint32_t CalculateWidthOfLines(const void*, std::uint32_t, std::uint32_t) override { return 0; }
    bool CalculateTextArea(const void*, std::uint32_t, cRZRect&, bool, bool) override { return true; }
    // The callout's measuring call: the text box at the origin, as wide as the text and one line high.
    bool CalculateTextArea(const void*, std::uint32_t length, cRZRect& area, int, int, int) override {
        ++areaCalls;
        area.left = 0;
        area.top = 0;
        area.right = int(length) * charWidth;
        area.bottom = lineHeight;
        measured.push_back(area);
        return true;
    }
    bool TextArea(const void*, std::uint32_t, cRZRect&) override { return true; }
    int GetStringWidth(const void*, std::uint32_t n) override { return int(n) * charWidth; }
    int GetStringVisibleWidth(const void*, std::uint32_t n, std::uint32_t& visible) override {
        visible = n;
        return int(n) * charWidth;
    }
    bool FormatForEllipsis(cIGZString&, const cIGZString&, const cRZRect&, bool) override { return true; }
    bool DrawTextLine(void*, int, int, const void*, std::uint32_t, std::uint32_t, std::uint32_t, const cRZRect*,
                      cRZPoint*) override {
        return true;
    }
    bool DrawTextParagraph(void*, const cRZRect& area, const void* text, std::uint32_t length, int, std::uint32_t,
                           std::uint32_t, const cRZRect*, cRZPoint*) override {
        paragraphs.push_back(area);
        drawn.emplace_back(static_cast<const char*>(text), length);
        return true;
    }
    cIGZFontRenderer* GetRenderer() const override { return nullptr; }
    void SetRenderer(cIGZFontRenderer*) override {}
    bool LockSurfaceForFontUse(void*) override { return ++lockCalls > 0; }
    bool UnlockSurfaceForFontUse(void*) override { return ++unlockCalls > 0; }
};

// The port's font seam (gz::IWinFont) over the group font above.
struct FakeWinFont final : IWinFont {
    FakeGZFont font;
    int refs = 0;
    cIGZFont* AsFont() override { return &font; }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    std::uint32_t DefaultTextColor() override { return 0; }
    std::uint32_t DefaultShadowColor() override { return 0; }
    std::int16_t LineHeight(int, int) override { return std::int16_t(font.lineHeight); }
    int CountLines(const char*, std::uint32_t, int) override { return 1; }
    int WrappedWidth(const char*, std::uint32_t, int) override { return 0; }
    void MeasureText(const char*, std::uint32_t length, cRZRect& out) override {
        out = cRZRect{0, 0, int(length) * font.charWidth, font.lineHeight};
    }
    int TextWidth(const char*, std::uint32_t length) override { return int(length) * font.charWidth; }
    void DrawText(IWinBuffer*, int, int, const char*, std::uint32_t, std::uint32_t, std::uint32_t, const cRZRect*,
                  int) override {}
    void DrawTextBox(IWinBuffer*, const cRZRect&, const char*, std::uint32_t, std::uint32_t, std::uint32_t,
                     std::uint32_t, const cRZRect*, int) override {}
};

struct FontSource final : IWinFontSource {
    FakeWinFont* byStyle[8] = {};
    std::uint32_t styles[8] = {};
    int count = 0;
    void Add(std::uint32_t style, FakeWinFont* font) {
        styles[count] = style;
        byStyle[count] = font;
        ++count;
    }
    IWinFont* Font(std::uint32_t style) override {
        for (int i = 0; i < count; ++i)
            if (styles[i] == style) return byStyle[i];
        return nullptr;
    }
    bool Available() override { return true; }
};

// ---- a recording buffer ------------------------------------------------------------------------------
struct Ctx;   // the recording draw context (defined below the buffer)

// A buffer's draw target (iid 0xab300b2b): RZDrawBltTiled's hardware path uses it, so the tiled blits of a buffer that
// has one land in a draw context instead of being expanded into per-tile Blt calls.
struct FakeDrawTarget final : cIGZBufferDrawTarget {
    cIGZBuffer* buffer = nullptr;
    Ctx* context = nullptr;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    cIGZBuffer* AsIGZBuffer() override { return buffer; }
    gz::cIGZDrawContext* GetDrawContext() override;
    gz::cIGZDrawContext* LockContext() override { return nullptr; }
    void UnlockContext() override {}
    void Init(gz::cIGZDrawContext*, cIGZBuffer*) override {}
    void ResetSize() override {}
    void SetBlitSource(cIGZBuffer*) override {}
};

struct BufferOp {
    enum Kind { kBlt, kBltTiled } kind;
    cRZRect source;
    cRZRect dest;
};

struct FakeBuffer;
std::vector<FakeBuffer*>& AllBuffers() {
    static std::vector<FakeBuffer*> all;   // every buffer made; freed at the end of main
    return all;
}

struct FakeBuffer final : cIGZBuffer {
    int refs = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int inks = 0;
    int locks = 0;
    int unlocks = 0;
    std::uint32_t transparent = 0;
    bool hasTransparent = false;
    std::vector<BufferOp> ops;
    cRZRect area;
    FakeDrawTarget target;
    Ctx* targetContext = nullptr;   // when set, the buffer advertises the draw-target interface

    FakeBuffer() { target.buffer = this; AllBuffers().push_back(this); }
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cIGZBufferDrawTarget && targetContext) {
            target.context = targetContext;
            *out = &target;
            return true;
        }
        return false;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Init(std::uint32_t w, std::uint32_t h, cGZBufferColorType) override {
        width = w;
        height = h;
        area = cRZRect{0, 0, int(w), int(h)};
        ++inks;
        return true;
    }
    bool Shutdown() override { return true; }
    bool Uninitialize() override { return true; }
    bool Lock(eLockFlags) override { return ++locks > 0; }
    bool Unlock(eLockFlags) override { return ++unlocks > 0; }
    bool IsLocked() const override { return locks > unlocks; }
    std::uint32_t Width() const override { return width; }
    std::uint32_t Height() const override { return height; }
    const cRZRect* GetBufferArea() const override { return &area; }
    void GetBufferArea(cRZRect& out) const override { out = area; }
    std::uint32_t GetMemoryUsage() const override { return width * height * 4; }
    cGZBufferColorType GetColorType() const override { return cGZBufferColorType{1, 32}; }
    void GetColorType(cGZBufferColorType& out) const override { out = cGZBufferColorType{1, 32}; }
    std::uint32_t GetBitsPerPixel() const override { return 32; }
    std::uint32_t GetBytesPerPixel() const override { return 4; }
    bool Fill(std::uint32_t, const cRZRect&, const cRZRect*) override { return true; }
    bool Fill(std::uint32_t, int, int, int, int, const cRZRect*) override { return true; }
    bool Fill(std::uint32_t, const cRZRect*) override { return true; }
    std::uint32_t GetPixel(std::uint32_t, std::uint32_t) const override { return 0; }
    void SetPixel(std::uint32_t, std::uint32_t, std::uint32_t) override {}
    void SetTransparency(std::uint32_t color) override {
        transparent = color;
        hasTransparent = true;
    }
    bool GetTransparency() const override { return hasTransparent; }
    bool IsPixelTransparent(std::uint32_t, std::uint32_t) override { return false; }
    bool GetTransparentColor(std::uint32_t& color) const override {
        color = transparent;
        return hasTransparent;
    }
    void RemoveTransparency() override {}
    bool DrawLine(int, int, int, int, int, std::uint32_t) override { return true; }
    bool Blt(cIGZBuffer*, const cRZRect& source, const cRZRect& dest, const cRZRect*) override {
        ops.push_back(BufferOp{BufferOp::kBlt, source, dest});
        return true;
    }
    std::uint32_t ConvertRGBValueToNative(std::uint8_t r, std::uint8_t g, std::uint8_t b) override {
        return 0xff000000u | (std::uint32_t(r) << 16) | (std::uint32_t(g) << 8) | b;
    }
    bool ConvertNativeValueToRGB(std::uint32_t, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) override {
        r = g = b = 0;
        return true;
    }
    std::uint32_t ConvertRGBAValueToNative(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t) override {
        return 0xff000000u | (std::uint32_t(r) << 16) | (std::uint32_t(g) << 8) | b;
    }
    bool ConvertNativeValueToRGBA(std::uint32_t, std::uint8_t&, std::uint8_t&, std::uint8_t&, std::uint8_t&) override {
        return true;
    }
    void* GetColorSurfaceBits() override { return nullptr; }
    std::uint32_t GetColorSurfaceStride() override { return width * 4; }
    std::uint32_t GetChangeCounter() override { return 0; }
    cIGZBufferHardwareCache* GetHardwareCache() override { return nullptr; }
    bool SetHardwareCache(cIGZBufferHardwareCache*) override { return true; }
    bool IsReady() override { return true; }
};

// RZ::GS() vslot 0x20 for the callout: hands out recording buffers and keeps them.
struct Services final : IWinCalloutBoxServices {
    std::vector<FakeBuffer*> created;
    Ctx* context = nullptr;
    cIGZBuffer* CreateBuffer() override {
        auto* buffer = new FakeBuffer();
        buffer->AddRef();
        buffer->targetContext = context;
        created.push_back(buffer);
        return buffer;
    }
};

// ---- a recording draw context -------------------------------------------------------------------------
struct ContextOp {
    enum Kind { kBlt, kBltTiled } kind;
    cRZRect source;
    cRZRect dest;
};

struct Ctx final : cIGZDrawContext {
    std::vector<ContextOp> ops;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    void PushTransform() override {}
    void ClipReset() override {}
    void Clip(const cRZRect&) override {}
    void ClipAbsolute(const cRZRect&) override {}
    void Offset(const cRZPoint&) override {}
    void OffsetAbsolute(const cRZPoint&) override {}
    void PopTransform() override {}
    void PushState() override {}
    void PopState() override {}
    cRZRect GetCanvasArea() override { return cRZRect{0, 0, 1024, 768}; }
    cRZRect GetClipRect() override { return cRZRect{}; }
    cRZPoint GetOrigin() override { return cRZPoint{}; }
    int GetTransformStackDepth() override { return 0; }
    bool IsTargetAttached() override { return true; }
    void SetTarget(IDrawTargetBuffer*) override {}
    cIGZDrawEngine* GetEngine() override { return nullptr; }
    void ResetToDefaultState() override {}
    void SetColor(std::uint32_t) override {}
    void SetColor(std::uint32_t, std::uint32_t) override {}
    void SetAlphaBlend(int) override {}
    void SetBltModulate(bool) override {}
    void SetAntialiasing(bool) override {}
    void SetLineThickness(float) override {}
    void ClearGradient() override {}
    void SetGradientTransform(const float (*)[4], float, float) override {}
    void ClearBlt() override {}
    void SetBlt(cIGZUnknown*, float, float, float, float) override {}
    void ClearBltTiling() override {}
    void SetBltTiling(const cRZRect&) override {}
    void Clear() override {}
    void Point(int, int) override {}
    void Line(int, int, int, int) override {}
    void Fill(const cRZRect&) override {}
    void Ellipse(int, int, int, int) override {}
    void FillEllipse(int, int, int, int) override {}
    void Blt(IDrawTargetBuffer*, const cRZRect& source, const cRZRect& dest) override {
        ops.push_back(ContextOp{ContextOp::kBlt, source, dest});
    }
    void BltTiled(IDrawTargetBuffer*, const cRZRect& tile, const cRZRect& dest, int, int) override {
        ops.push_back(ContextOp{ContextOp::kBltTiled, tile, dest});
    }
    void Flush() override {}
    bool IsSuspended() override { return false; }
    void Suspend() override {}
    void Resume() override {}
    void Draw(eDrawingOp, const float*, int) override {}
    void DrawNoTransform(eDrawingOp, const float*, int) override {}
    bool Lock(cGZBufferSurfaceDesc&, bool) override { return true; }
    void Unlock() override {}
};

gz::cIGZDrawContext* FakeDrawTarget::GetDrawContext() { return context; }

struct Probe final : cGZWin {
    explicit Probe(cIGZWinMgr* mgr) : cGZWin(mgr) {}
    void SetCtx(gz::cIGZDrawContext* context) { drawContext_ = context; }
};

struct Box final : cSC4WinCalloutBox {
    explicit Box(cIGZWinMgr* mgr) : cSC4WinCalloutBox(mgr) {}
    void SetCtx(gz::cIGZDrawContext* context) { drawContext_ = context; }
};

bool HasBlt(const FakeBuffer& buffer, const cRZRect& source) {
    for (const BufferOp& op : buffer.ops)
        if (op.kind == BufferOp::kBlt && op.source.left == source.left && op.source.top == source.top &&
            op.source.right == source.right && op.source.bottom == source.bottom)
            return true;
    return false;
}

int CountOps(const FakeBuffer& buffer, BufferOp::Kind kind) {
    int n = 0;
    for (const BufferOp& op : buffer.ops)
        if (op.kind == kind) ++n;
    return n;
}

}  // namespace

int main() {
    // ---- 1. COM through the two bases -----------------------------------------------------------------
    {
        FakeMgr mgr;
        FakeWinGraphics graphics;
        SetWinGraphics(&graphics);
        FakeWinFont font;
        FontSource source;
        source.Add(kWinDefaultFontStyle, &font);
        SetWinFontSource(&source);

        auto* parent = new Probe(&mgr);
        parent->AddRef();
        parent->SetArea(0, 0, 300, 300);
        parent->CalcAbsoluteArea();
        mgr.mainWin = parent;

        auto* box = new Box(&mgr);
        box->AddRef();                                        // 1
        CHECK_EQ(box->GetFlag(kWinIgnoreCursor), true);       // the constructor's direct flag write (quirk)
        CHECK(box->AsIGZWin() == static_cast<cGZWin*>(box));
        mgr.AddWindowToValidList(box);
        CHECK(parent->ChildAdd(box));                         // adds a reference and calls Init()
        CHECK(box->GetParentWin() == parent);

        cISC4WinCalloutBox* iface = box;
        void* out = nullptr;
        CHECK(iface->QueryInterface(kGZIID_cISC4WinCalloutBox, &out));
        CHECK(out == static_cast<cISC4WinCalloutBox*>(box));
        CHECK_EQ(box->AddRef(), 4u);                          // 1 (test) + 1 (ChildAdd) + QI
        CHECK_EQ(box->Release(), 3u);
        out = nullptr;
        CHECK(box->QueryInterface(kGZIID_cIGZWin, &out));
        CHECK(out == static_cast<cGZWin*>(box));
        CHECK_EQ(box->Release(), 3u);
        CHECK_EQ(box->Release(), 2u);                         // the interface query's reference
        CHECK(!box->QueryInterface(0xdeadbeef, &out));
        CHECK(iface->AsIGZWin() == static_cast<cGZWin*>(box));

        // Init: id, flags, fonts and the constraint rectangle taken from the main window.
        CHECK(box->Init());
        CHECK_EQ(box->GetID(), 0x2aab8cc1u);
        CHECK(box->GetFlag(kWinFlag4));
        CHECK(!box->GetFlag(kWinPrivateBuffer));
        CHECK(box->GetFlag(kWinInitialized));
        CHECK_RECT(box->GetArea(), 0, 0, 300, 300);
        CHECK_RECT(box->GetConstraintRect(), 16, 16, 284, 284);
        CHECK_EQ(font.refs, 2);                               // title font + text font are the same group font
        CHECK_EQ(box->Init(), true);                          // a second Init does nothing

        // The setters.
        cRZString title("T");
        cRZString text("hello");
        box->SetTitle(title);
        CHECK(box->GetTitle().Strlen() == 1 && box->GetTitle().Data()[0] == 'T');
        box->SetText(text);
        CHECK(box->GetText().Strlen() == 5);
        CHECK(box->GetPointOfInterest().x == 0 && box->GetPointOfInterest().y == 0);
        cRZPoint point{12, -7};
        iface->SetPointOfInterest(point);
        CHECK(box->GetPointOfInterest().x == 12 && box->GetPointOfInterest().y == -7);
        cRZRect avoid{-40, -40, 40, 40};
        iface->SetAvoidanceRect(avoid);
        CHECK_RECT(box->GetAvoidanceRect(), -40, -40, 40, 40);
        cRZRect constraint{1, 2, 3, 4};
        iface->SetConstraintRect(constraint);
        CHECK_RECT(box->GetConstraintRect(), 1, 2, 3, 4);

        auto* background = new FakeBuffer();
        background->AddRef();
        iface->SetBackgroundImage(background, 5, 6);
        CHECK(box->GetBackgroundImage() == background);
        CHECK_EQ(background->refs, 2);   // the test's reference + the callout's
        CHECK(background->hasTransparent && background->transparent == 0xffff00ffu);   // magenta key
        auto* background2 = new FakeBuffer();
        background2->AddRef();
        iface->SetBackgroundImage(background2, -1, -1);        // replaces, defaults the margins to 8 / 3
        CHECK_EQ(background->refs, 1);   // the callout let go
        CHECK_EQ(background2->refs, 2);

        auto* icon = new FakeBuffer();
        icon->AddRef();
        iface->SetIcon(icon);
        CHECK(box->GetIcon() == icon);
        CHECK_EQ(icon->refs, 2);
        CHECK(icon->hasTransparent && icon->transparent == 0xffff00ffu);

        // Shutdown releases everything but the meter image (quirk) and the destructor releases that.
        auto* meter = new FakeBuffer();
        meter->AddRef();
        iface->SetIconMeter(meter, 3, 0.5f);
        box->Shutdown();
        CHECK(!box->GetFlag(kWinInitialized));
        CHECK_EQ(background->refs, 1);            // the callout let go when the second image replaced it
        CHECK_EQ(background2->refs, 1);           // Shutdown released it
        CHECK_EQ(icon->refs, 1);
        CHECK_EQ(font.refs, 0);
        CHECK_EQ(meter->refs, 2);                 // quirk: Shutdown leaves the meter image alone
        CHECK(parent->ChildRemove(box));          // the child's reference
        CHECK_EQ(meter->refs, 2);
        box->Release();                           // destructor
        parent->Release();
        CHECK_EQ(meter->refs, 1);                 // only the test's reference is left
        CHECK_EQ(background->refs, 1);
        CHECK_EQ(background2->refs, 1);
        CHECK_EQ(icon->refs, 1);
        SetWinGraphics(nullptr);
        SetWinFontSource(nullptr);
    }

    // ---- 2. Init's constraint quirk: only an equal left/right counts as "empty" ------------------------------
    {
        FakeMgr mgr;
        FakeWinGraphics graphics;
        SetWinGraphics(&graphics);
        FakeWinFont font;
        FontSource source;
        source.Add(kWinDefaultFontStyle, &font);
        SetWinFontSource(&source);
        auto* parent = new Probe(&mgr);
        parent->AddRef();
        parent->SetArea(10, 20, 310, 320);
        parent->CalcAbsoluteArea();
        mgr.mainWin = parent;

        auto* box = new Box(&mgr);
        box->AddRef();
        mgr.AddWindowToValidList(box);
        cRZRect tall{16, 50, 16, 200};            // right == left: Init replaces it
        box->SetConstraintRect(tall);
        CHECK(parent->ChildAdd(box));             // ChildAdd runs Init()
        CHECK_RECT(box->GetConstraintRect(), 26, 36, 294, 304);
        CHECK_RECT(box->GetArea(), 0, 0, 300, 300);   // the main window's area in parent coordinates

        auto* box2 = new Box(&mgr);
        box2->AddRef();
        mgr.AddWindowToValidList(box2);
        cRZRect narrow{16, 50, 200, 50};          // right != left: kept even though it is empty vertically
        box2->SetConstraintRect(narrow);
        CHECK(parent->ChildAdd(box2));
        CHECK_RECT(box2->GetConstraintRect(), 16, 50, 200, 50);
        parent->ChildRemove(box2);
        box2->Release();
        parent->ChildRemove(box);
        box->Release();
        parent->Release();
        SetWinGraphics(nullptr);
        SetWinFontSource(nullptr);
    }

    // ---- 3. GZPaint without a skin: measure, place above, compose the text buffer --------------------------
    {
        FakeMgr mgr;
        FakeWinGraphics graphics;
        SetWinGraphics(&graphics);
        FakeWinFont shared;
        FakeWinFont other;
        FontSource source;
        source.Add(kWinDefaultFontStyle, &shared);
        source.Add(0x1234, &other);
        SetWinFontSource(&source);

        auto* parent = new Probe(&mgr);
        parent->AddRef();
        parent->SetArea(0, 0, 300, 300);
        parent->CalcAbsoluteArea();
        mgr.mainWin = parent;

        auto* box = new Box(&mgr);
        box->AddRef();
        mgr.AddWindowToValidList(box);
        CHECK(parent->ChildAdd(box));
        CHECK(box->Init());
        box->SetTitle(cRZString("T"));
        box->SetText(cRZString("hello"));
        box->SetPointOfInterest(cRZPoint{150, 150});   // 134 px of room each way -> the box goes above the point

        Ctx ctx;
        Ctx compose;
        box->SetCtx(&ctx);
        Services services;
        services.context = &compose;
        SetCalloutBoxServices(&services);
        CHECK(box->GZPaint());

        // measure: title 1 char -> 8 wide + 4, 16 high; text 5 chars -> 40 wide; 16 + 4 + 16 = 36 high.
        CHECK_EQ(services.created.size(), 2u);
        FakeBuffer* textBuffer = services.created[0];
        FakeBuffer* skinBuffer = services.created[1];
        CHECK_EQ(textBuffer->width, 40u);
        CHECK_EQ(textBuffer->height, 38u);            // 36 + 2
        // no background image: the text buffer gets no skin blits, but is locked (and never unlocked - quirk).
        CHECK_EQ(CountOps(*textBuffer, BufferOp::kBlt), 0);
        CHECK_EQ(CountOps(*textBuffer, BufferOp::kBltTiled), 0);
        CHECK_EQ(textBuffer->locks, 1);
        CHECK_EQ(textBuffer->unlocks, 0);
        // the two font paragraphs (title at the origin, the text below it)
        CHECK_EQ(shared.font.paragraphs.size(), 2u);
        CHECK_RECT(shared.font.paragraphs[0], 0, 0, 8, 16);
        CHECK_RECT(shared.font.paragraphs[1], 0, 20, 40, 36);
        CHECK(shared.font.drawn[0] == "T" && shared.font.drawn[1] == "hello");
        CHECK_EQ(shared.font.lockCalls, 1);
        CHECK_EQ(shared.font.unlockCalls, 1);
        // the skin buffer: 56 x 66 (the box above, grown by the beak), one blit: the text block on the left margins
        CHECK_EQ(skinBuffer->width, 56u);
        CHECK_EQ(skinBuffer->height, 66u);
        CHECK_EQ(CountOps(*skinBuffer, BufferOp::kBlt), 1);
        CHECK_RECT(skinBuffer->ops[0].source, 0, 0, 40, 38);
        CHECK_RECT(skinBuffer->ops[0].dest, 8, 2, 48, 40);
        CHECK_EQ(skinBuffer->locks, 1);
        CHECK_EQ(skinBuffer->unlocks, 1);
        // the draw context gets the finished box at the dirty rectangle, in parent coordinates
        CHECK_EQ(ctx.ops.size(), 1u);
        CHECK_RECT(ctx.ops[0].source, 0, 0, 56, 66);
        CHECK_RECT(ctx.ops[0].dest, 122, 80, 178, 146);

        // A clean repaint still blits but does not measure, lock or compose again.
        CHECK(box->GZPaint());
        CHECK_EQ(ctx.ops.size(), 2u);
        CHECK_EQ(textBuffer->locks, 1);
        CHECK_EQ(skinBuffer->locks, 1);
        CHECK_EQ(shared.font.areaCalls, 2);

        // Narrower text: the box gets narrower (32 x 42, widened to 48 by the placement's minimum), so both buffers
        // are made again and the old ones released.
        box->SetText(cRZString("hi"));
        CHECK(box->GZPaint());
        CHECK_EQ(shared.font.areaCalls, 4);
        CHECK_EQ(services.created.size(), 4u);
        FakeBuffer* narrowBuffer = services.created[2];
        CHECK_EQ(narrowBuffer->width, 16u);           // 2 chars * 8
        CHECK_EQ(narrowBuffer->height, 38u);
        CHECK_EQ(narrowBuffer->locks, 1);
        CHECK_EQ(narrowBuffer->unlocks, 0);           // quirk: the text buffer is never unlocked
        CHECK_EQ(textBuffer->refs, 0);                // the callout released the first one
        CHECK_EQ(services.created[3]->width, 48u);    // the box is at least three skin cells wide when placed above
        CHECK_EQ(services.created[3]->height, 66u);
        CHECK_EQ(ctx.ops.size(), 3u);

        // Quirk (kept): the size test compares the buffer against the box size but Init uses the content size, so a
        // measure with the same text always makes the text buffer again.
        box->SetText(cRZString("ho"));
        CHECK(box->GZPaint());
        CHECK_EQ(shared.font.areaCalls, 6);
        CHECK_EQ(services.created.size(), 5u);
        CHECK_EQ(services.created[4]->width, 16u);
        CHECK_EQ(services.created[4]->locks, 1);
        CHECK_EQ(narrowBuffer->refs, 0);              // the first "hi" buffer was released again

        // Same title: no measurement (the setter's equality test).
        box->SetTitle(cRZString("T"));
        CHECK(box->GZPaint());
        CHECK_EQ(shared.font.areaCalls, 6);

        parent->ChildRemove(box);
        box->Release();
        parent->Release();
        SetCalloutBoxServices(nullptr);
        SetWinGraphics(nullptr);
        SetWinFontSource(nullptr);
    }

    // ---- 4. GZPaint with the 5x5 skin, an icon and a meter ------------------------------------------------
    {
        FakeMgr mgr;
        FakeWinGraphics graphics;
        SetWinGraphics(&graphics);
        FakeWinFont shared;
        FakeWinFont other;
        FontSource source;
        source.Add(kWinDefaultFontStyle, &shared);
        source.Add(0x1234, &other);
        SetWinFontSource(&source);

        auto* parent = new Probe(&mgr);
        parent->AddRef();
        parent->SetArea(0, 0, 300, 300);
        parent->CalcAbsoluteArea();
        mgr.mainWin = parent;

        auto* box = new Box(&mgr);
        box->AddRef();
        mgr.AddWindowToValidList(box);
        CHECK(parent->ChildAdd(box));
        CHECK(box->Init());
        box->SetTextFontStyle(0x1234);                  // different text font from the title font
        CHECK_EQ(shared.refs, 1);
        CHECK_EQ(other.refs, 1);

        auto* background = new FakeBuffer();
        background->AddRef();
        background->Init(80, 80, cGZBufferColorType{1, 0});   // 16 x 16 skin cells
        auto* icon = new FakeBuffer();
        icon->AddRef();
        icon->Init(30, 20, cGZBufferColorType{1, 0});
        auto* meter = new FakeBuffer();
        meter->AddRef();
        meter->Init(10, 20, cGZBufferColorType{1, 0});        // two 10 px halves

        box->SetBackgroundImage(background, 5, 6);
        box->SetIcon(icon);
        box->SetIconMeter(meter, 3, 0.5f);                    // 30 px wide meter, half filled
        box->SetTitle(cRZString("T"));
        box->SetText(cRZString("hello"));
        box->SetPointOfInterest(cRZPoint{150, 150});

        Ctx ctx;
        Ctx compose;
        box->SetCtx(&ctx);
        Services services;
        services.context = &compose;                  // records the tiled blits (RZDrawBltTiled's context path)
        SetCalloutBoxServices(&services);
        CHECK(box->GZPaint());

        CHECK_EQ(services.created.size(), 2u);
        FakeBuffer* textBuffer = services.created[0];
        FakeBuffer* skinBuffer = services.created[1];
        // 40 x 56: the text (36) + 4 for the title gap + the meter (10 + 8) = 54, +2.
        CHECK_EQ(textBuffer->width, 40u);
        CHECK_EQ(textBuffer->height, 56u);
        CHECK_EQ(shared.font.areaCalls, 1);                   // title measured with the title font
        CHECK_EQ(other.font.areaCalls, 1);                    // text measured with the text font
        CHECK_EQ(shared.font.paragraphs.size(), 1u);
        CHECK_RECT(shared.font.paragraphs[0], 0, 0, 8, 16);
        CHECK_EQ(other.font.paragraphs.size(), 1u);
        CHECK_RECT(other.font.paragraphs[0], 0, 34, 40, 50);  // title height + 4, meter height/2 + 4 more
        // the text buffer's own background bands (the skin's title band, above and below the text): tiled, so they
        // go through the composition context.
        CHECK_EQ(compose.ops.size(), 10u);
        CHECK_EQ(compose.ops[0].kind, ContextOp::kBltTiled);
        CHECK_RECT(compose.ops[0].source, 16, 16, 32, 32);
        CHECK_RECT(compose.ops[0].dest, 0, 0, 40, 20);
        CHECK_RECT(compose.ops[1].source, 16, 32, 32, 48);
        CHECK_RECT(compose.ops[1].dest, 0, 20, 40, 56);

        // the skin: box 84 x 66 above the point, dirty 84 x 90 (the beak adds 24)
        CHECK_EQ(skinBuffer->width, 84u);
        CHECK_EQ(skinBuffer->height, 90u);
        // 3 title band + 3 body band + 2 edges are tiled (the composition context); 4 corners, the beak and the text
        // block are plain blits on the buffer.
        CHECK_EQ(skinBuffer->ops.size(), 6u);
        CHECK_EQ(CountOps(*skinBuffer, BufferOp::kBltTiled), 0);
        CHECK_EQ(CountOps(*skinBuffer, BufferOp::kBlt), 6);
        CHECK_RECT(compose.ops[2].source, 0, 16, 16, 32);         // title band, left
        CHECK_RECT(compose.ops[2].dest, 0, 16, 16, 25);
        CHECK_RECT(compose.ops[3].source, 48, 16, 64, 32);        // title band, right
        CHECK_RECT(compose.ops[3].dest, 68, 16, 84, 25);
        CHECK_RECT(compose.ops[4].source, 16, 16, 32, 32);        // title band, tiled middle
        CHECK_RECT(compose.ops[4].dest, 16, 16, 68, 25);
        CHECK_RECT(compose.ops[5].source, 0, 32, 16, 48);         // body band, left
        CHECK_RECT(compose.ops[5].dest, 0, 25, 16, 50);
        CHECK_RECT(compose.ops[6].source, 48, 32, 64, 48);        // body band, right
        CHECK_RECT(compose.ops[6].dest, 68, 25, 84, 50);
        CHECK_RECT(compose.ops[7].source, 16, 32, 32, 48);        // body band, tiled middle
        CHECK_RECT(compose.ops[7].dest, 16, 25, 68, 50);
        CHECK_RECT(compose.ops[8].source, 16, 0, 32, 16);         // top edge
        CHECK_RECT(compose.ops[8].dest, 16, 0, 68, 16);
        CHECK_RECT(compose.ops[9].source, 16, 48, 32, 64);        // bottom edge
        CHECK_RECT(compose.ops[9].dest, 16, 50, 68, 66);
        CHECK_RECT(skinBuffer->ops[0].source, 0, 0, 16, 16);      // top-left corner (plain blit)
        CHECK_RECT(skinBuffer->ops[0].dest, 0, 0, 16, 16);
        CHECK_RECT(skinBuffer->ops[1].source, 48, 0, 64, 16);     // top-right corner
        CHECK_RECT(skinBuffer->ops[1].dest, 68, 0, 84, 16);
        CHECK_RECT(skinBuffer->ops[2].source, 0, 48, 16, 64);     // bottom-left corner
        CHECK_RECT(skinBuffer->ops[2].dest, 0, 50, 16, 66);
        CHECK_RECT(skinBuffer->ops[3].source, 48, 48, 64, 64);    // bottom-right corner
        CHECK_RECT(skinBuffer->ops[3].dest, 68, 50, 84, 66);
        CHECK_RECT(skinBuffer->ops[4].source, 64, 32, 80, 64);    // the beak (box above: the "down" cell)
        CHECK_RECT(skinBuffer->ops[4].dest, 34, 58, 50, 90);
        CHECK_RECT(skinBuffer->ops[5].source, 0, 0, 40, 56);      // the text block
        CHECK_RECT(skinBuffer->ops[5].dest, 39, 5, 79, 61);
        CHECK_EQ(skinBuffer->unlocks, 1);

        // the draw context: the box, the icon and the two meter tiles
        CHECK_EQ(ctx.ops.size(), 4u);
        CHECK_RECT(ctx.ops[0].source, 0, 0, 84, 90);
        CHECK_RECT(ctx.ops[0].dest, 108, 56, 192, 146);
        CHECK_RECT(ctx.ops[1].source, 0, 0, 30, 20);                 // the icon, vertically centred in the box
        CHECK_RECT(ctx.ops[1].dest, 113, 79, 143, 99);
        CHECK_RECT(ctx.ops[2].source, 0, 10, 10, 20);                // the meter: the empty half tiled down
        CHECK_RECT(ctx.ops[2].dest, 152, 81, 182, 91);
        CHECK_RECT(ctx.ops[3].source, 0, 0, 10, 10);                 // the filled half up to the value
        CHECK_RECT(ctx.ops[3].dest, 152, 81, 167, 91);

        // The value clamps to 1: the filled tile covers the whole meter.
        box->SetIconMeter(meter, 3, 2.0f);
        CHECK(box->GZPaint());
        CHECK_EQ(ctx.ops.size(), 8u);
        CHECK_RECT(ctx.ops[7].source, 0, 0, 10, 10);
        CHECK_RECT(ctx.ops[7].dest, 152, 81, 182, 91);
        // The value clamps to 0: a degenerate filled rectangle.
        box->SetIconMeter(meter, 3, -3.0f);
        CHECK(box->GZPaint());
        CHECK_RECT(ctx.ops[11].dest, 152, 81, 152, 91);

        // ---- the four sides, recognised by the beak cell they blit ------------------------------------
        // box left of the point: the right-pointing cell
        box->SetConstraintRect(cRZRect{16, 16, 300, 300});
        box->SetPointOfInterest(cRZPoint{290, 150});
        std::size_t createdBefore = services.created.size();
        CHECK(box->GZPaint());
        if (services.created.size() != createdBefore) skinBuffer = services.created.back();
        CHECK(HasBlt(*skinBuffer, cRZRect{32, 64, 64, 80}));
        CHECK_EQ(skinBuffer->width, 108u);            // 84 + 24 for the beak on the right
        CHECK_EQ(skinBuffer->height, 66u);
        CHECK_RECT(ctx.ops[ctx.ops.size() - 4].dest, 178, 117, 286, 183);
        // box right of the point: the left-pointing cell
        box->SetPointOfInterest(cRZPoint{20, 150});
        createdBefore = services.created.size();
        CHECK(box->GZPaint());
        if (services.created.size() != createdBefore) skinBuffer = services.created.back();
        CHECK(HasBlt(*skinBuffer, cRZRect{0, 64, 32, 80}));
        CHECK_EQ(skinBuffer->width, 108u);            // 84 + 24 for the beak on the left
        CHECK_EQ(skinBuffer->height, 66u);
        CHECK_EQ(skinBuffer->locks, 2);               // same size again: the buffer is reused and locked again
        CHECK_EQ(skinBuffer->unlocks, 2);
        CHECK_RECT(ctx.ops[ctx.ops.size() - 4].dest, 24, 117, 132, 183);
        // box below the point: the up-pointing cell
        box->SetPointOfInterest(cRZPoint{150, 40});
        createdBefore = services.created.size();
        CHECK(box->GZPaint());
        if (services.created.size() != createdBefore) skinBuffer = services.created.back();
        CHECK(HasBlt(*skinBuffer, cRZRect{64, 0, 80, 32}));
        CHECK_EQ(skinBuffer->width, 84u);             // 66 + 24 for the beak below
        CHECK_EQ(skinBuffer->height, 90u);
        CHECK_RECT(ctx.ops[ctx.ops.size() - 4].dest, 108, 44, 192, 134);

        parent->ChildRemove(box);
        box->Release();
        parent->Release();
        CHECK_EQ(meter->refs, 1);            // the destructor released the meter image
        CHECK_EQ(icon->refs, 1);
        CHECK_EQ(background->refs, 1);
        SetCalloutBoxServices(nullptr);
        SetWinGraphics(nullptr);
        SetWinFontSource(nullptr);
    }

    for (FakeBuffer* buffer : AllBuffers()) delete buffer;

    if (failures == 0) std::printf("sc4_wincalloutbox_test: ok\n");
    else std::printf("sc4_wincalloutbox_test: %d failure(s)\n", failures);
    return failures != 0;
}
