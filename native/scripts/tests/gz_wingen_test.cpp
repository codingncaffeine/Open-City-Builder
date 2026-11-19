#include <cstdio>
#include <string>
#include <vector>

#include "fake_winmgr.h"
#include "gz/WinGen.h"
#include "gz/WinFlatRect.h"
#include "gz/WinBtn.h"
#include "gz/Buffer.h"
#include "gz/WinBtn.h"
#include "gz/WinCtrlFactory.h"
#include "gz/WinStyle.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace gz;

namespace {
struct Ctx final : cIGZDrawContext {
    std::vector<std::string> log;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    void PushTransform() override {} void ClipReset() override {} void Clip(const cRZRect&) override {}
    void ClipAbsolute(const cRZRect&) override {} void Offset(const cRZPoint&) override {}
    void OffsetAbsolute(const cRZPoint&) override {} void PopTransform() override {} void PushState() override { log.push_back("push"); }
    void PopState() override { log.push_back("pop"); } cRZRect GetCanvasArea() override { return {}; } cRZRect GetClipRect() override { return {}; }
    cRZPoint GetOrigin() override { return {}; } int GetTransformStackDepth() override { return 0; }
    bool IsTargetAttached() override { return true; } void SetTarget(IDrawTargetBuffer*) override {}
    cIGZDrawEngine* GetEngine() override { return nullptr; } void ResetToDefaultState() override {}
    void SetColor(std::uint32_t rgb) override { log.push_back("color " + std::to_string(rgb)); }
    void SetColor(std::uint32_t, std::uint32_t) override {} void SetAlphaBlend(int) override {}
    void SetBltModulate(bool) override {} void SetAntialiasing(bool) override {} void SetLineThickness(float) override {}
    void ClearGradient() override {} void SetGradientTransform(const float (*)[4], float, float) override {}
    void ClearBlt() override {} void SetBlt(cIGZUnknown*, float, float, float, float) override {}
    void ClearBltTiling() override {} void SetBltTiling(const cRZRect&) override {} void Clear() override {}
    void Point(int, int) override {}
    void Line(int a, int b, int c, int d) override { log.push_back("line " + std::to_string(a) + "," + std::to_string(b) + "," + std::to_string(c) + "," + std::to_string(d)); }
    void Fill(const cRZRect& r) override { log.push_back("fill " + std::to_string(r.left) + "," + std::to_string(r.top) + "," + std::to_string(r.right) + "," + std::to_string(r.bottom)); }
    void Ellipse(int, int, int, int) override {} void FillEllipse(int, int, int, int) override {}
    void Blt(IDrawTargetBuffer*, const cRZRect& s, const cRZRect& d) override {
        log.push_back("blt " + std::to_string(s.left) + "," + std::to_string(s.top) + "," + std::to_string(s.right) + "," + std::to_string(s.bottom) +
                      ">" + std::to_string(d.left) + "," + std::to_string(d.top) + "," + std::to_string(d.right) + "," + std::to_string(d.bottom)); }
    void BltTiled(IDrawTargetBuffer*, const cRZRect&, const cRZRect&, int, int) override { log.push_back("tile"); }
    void Flush() override {} bool IsSuspended() override { return false; } void Suspend() override {} void Resume() override {}
    void Draw(eDrawingOp, const float*, int) override {} void DrawNoTransform(eDrawingOp, const float*, int) override {}
    bool Lock(cGZBufferSurfaceDesc&, bool) override { return true; } void Unlock() override {}
};


// Every glyph is 8 pixels wide, the line is 16 pixels high.
struct FakeFont final : cIGZFont {
    int refs = 0;
    std::vector<std::string> drawn;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Init() override { return true; } bool Shutdown() override { return true; }
    void GetName(cIGZString&) const override {} std::uint32_t GetStyle() const override { return 0; }
    bool GetStyle(int) const override { return false; } bool SetStyle(int, bool) override { return true; }
    std::uint32_t GetType() const override { return 0; } std::uint32_t GetSize() const override { return 16; }
    bool SetSize(std::uint32_t) override { return true; } std::uint32_t GetFontColor() const override { return 0; }
    std::uint32_t GetFontBackgroundColor() const override { return 0; } void SetFontColor(std::uint32_t, std::uint32_t) override {}
    int GetAntialiasing() override { return 0; } void SetAntialiasing(int) override {}
    float GetAntialiasingGammaCorrection() override { return 1; } void SetAntialiasingGammaCorrection(float) override {}
    float GetAntialiasingSharpness() override { return 1; } void SetAntialiasingSharpness(float) override {}
    void GetFilePath(cIGZString&) const override {} void SetFilePath(const cIGZString&) override {}
    bool GetCodePage(int&) const override { return true; } bool SetCodePage(int) override { return true; }
    bool IsCodePageAvailable(int) override { return true; } bool IsCharEncodingAvailable(int) override { return true; }
    bool IsCharAvailable(void*, std::uint32_t) override { return true; } bool PreloadChars(void*, std::uint32_t) override { return true; }
    void GetScaling(float& x, float& y) const override { x = y = 1; } void SetScaling(float, float) override {}
    void GetAdvanceScaling(float& x, float& y) const override { x = y = 1; } void SetAdvanceScaling(float, float) override {}
    void GetSpacingAdjustment(int& x, int& y) const override { x = y = 0; } void SetSpacingAdjustment(int, int) override {}
    int GetLineHeight() override { return 16; }
    int CharWidth(const void*, std::uint32_t n) override { return int(n) * 8; }
    int CharHeight(const void*, std::uint32_t) override { return 16; }
    int GetSizeAboveBaseline() override { return 12; } int GetSizeBelowBaseline() override { return 4; }
    int GetAdvanceWidth(const void*, std::uint32_t n) override { return int(n) * 8; }
    std::uint32_t CalculateWordsToFitInWidth(const void* text, std::uint32_t length, std::uint32_t width, std::uint32_t*, WordWrapMode) override {
        const char* t = static_cast<const char*>(text);
        std::uint32_t fit = width / 8;
        for (std::uint32_t i = 0; i < length && i < fit; ++i) if (t[i] == '\n') return i + 1;
        if (length <= fit) return length;
        std::uint32_t at = fit;
        while (at > 0 && t[at - 1] != ' ') --at;     // break after the last space that fits
        return at ? at : fit;
    }
    std::uint32_t CalculateCharsToFitInWidth(const void*, std::uint32_t length, std::uint32_t width, std::uint32_t*) override {
        const std::uint32_t fit = width / 8; return fit < length ? fit : length; }
    std::uint32_t CalculateNumberOfLines(const void*, std::uint32_t, std::uint32_t) override { return 1; }
    std::uint32_t CalculateWidthOfLines(const void*, std::uint32_t, std::uint32_t) override { return 0; }
    bool CalculateTextArea(const void*, std::uint32_t, cRZRect&, bool, bool) override { return true; }
    bool CalculateTextArea(const void*, std::uint32_t, cRZRect&, int, int, int) override { return true; }
    bool TextArea(const void*, std::uint32_t, cRZRect&) override { return true; }
    int GetStringWidth(const void*, std::uint32_t n) override { return int(n) * 8; }
    int GetStringVisibleWidth(const void*, std::uint32_t n, std::uint32_t& v) override { v = n; return int(n) * 8; }
    bool FormatForEllipsis(cIGZString&, const cIGZString&, const cRZRect&, bool) override { return true; }
    bool DrawTextLine(void*, int x, int y, const void* text, std::uint32_t length, std::uint32_t color, std::uint32_t,
                      const cRZRect*, cRZPoint*) override {
        drawn.push_back(std::to_string(x) + "," + std::to_string(y) + ":" + std::string(static_cast<const char*>(text), length) + ":" + std::to_string(color));
        return true; }
    bool DrawTextParagraph(void*, const cRZRect&, const void*, std::uint32_t, int, std::uint32_t, std::uint32_t, const cRZRect*, cRZPoint*) override { return true; }
    cIGZFontRenderer* GetRenderer() const override { return nullptr; } void SetRenderer(cIGZFontRenderer*) override {}
    bool LockSurfaceForFontUse(void*) override { return true; } bool UnlockSurfaceForFontUse(void*) override { return true; }
};

struct Styles final : IWinStyleManager {
    FakeFont font;
    cIGZUnknown* GetFontStyle(std::uint32_t) override { return &font; }
};
struct Factory final : IWinCtrlFactory {
    cGZBuffer* image = nullptr;
    int buttons = 0;
    bool CreateCtrl(std::uint32_t cls, std::uint32_t, void** out) override {
        if (cls != 0xc2afa76e) return false;
        auto* rect = new cGZWinFlatRect(mgr);
        rect->AddRef();
        *out = static_cast<cIGZWinFlatRect*>(rect);
        return true;
    }
    cIGZWinScrollbar* CreateScrollbar(std::uint32_t, int, int, int, int) override { return nullptr; }
    std::uint32_t DefaultColor(int id) override { return 0x1000u + std::uint32_t(id & 0xff); }
    std::uint32_t DoubleClickMilliseconds() override { return 700; }
    cIGZWinBtn* CreateButton(std::uint32_t, std::uint32_t id, const char*, cIGZBuffer*, const cRZRect*, std::uint32_t) override {
        ++buttons;
        auto* button = new cGZWinBtn(mgr);
        button->AsIGZWin()->SetID(id);
        button->AsIGZWin()->SetArea(0, 0, 12, 12);
        button->AddRef();
        return button;
    }
    cIGZBuffer* Image(std::uint32_t id) override { return id == 0xe2b14589 || id == 0xe2b14588 ? image : nullptr; }
    cIGZUnknown* Font(std::uint32_t) override { return nullptr; }
    cIGZWinMgr* mgr = nullptr;
};
struct Cursors final : cIGZCursorManager {
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; } std::uint32_t Release() override { return 1; }
    cIGZCursor* LoadCursor(std::uint32_t) override { return nullptr; }
    void SetCursor(cIGZCursor*) override {}
};
struct Target final : cGZWin {
    explicit Target(cIGZWinMgr* mgr) : cGZWin(mgr) {}
    std::vector<cGZMessage> got;
    bool DoMessage(cGZMessage& m) override { got.push_back(m); return true; }
};


struct Proc final : cIGZWinProc {
    std::vector<std::uint32_t> seen;
    bool handleCreate = true, limits = false;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool DoWinProcMessage(cIGZWin*, cGZMessage& m) override {
        seen.push_back(m.type);
        if (m.type == kWinProcCreated) return handleCreate;
        if (m.type == kWinProcSizeLimits && limits) {
            auto* l = reinterpret_cast<cRZRect*>(m.param1);
            *l = cRZRect{50, 40, 120, 90};     // min 50x40, max 120x90 for the user area
            return true;
        }
        return false;
    }
};
struct Gen final : cGZWinGen {
    using cGZWinGen::cGZWinGen;
    void SetCtx(cIGZDrawContext* c) { drawContext_ = c; }
};
}  // namespace

int main() {
    FakeMgr mgr;
    FakeWinGraphics graphics;
    SetWinGraphics(&graphics);
    Factory factory;
    factory.mgr = &mgr;
    auto* imagePtr = new cGZBuffer;
    imagePtr->AddRef();
    cGZBuffer& image = *imagePtr;
    image.Init(32, 64, cGZBufferColorType{kBufferColorRGB565, 16});
    factory.image = &image;
    SetWinCtrlFactory(&factory);
    Cursors cursors;
    mgr.cursors = &cursors;

    auto* gen = new Gen(&mgr);
    gen->AddRef();
    gen->SetArea(10, 10, 210, 160);
    gen->SetCaption(cRZString("Window"));
    CHECK(gen->GetCLSID() == 0x4386d516 && gen->GetWinGenFlag(1) && gen->GetWinGenFlag(4) && gen->GetWinGenFlag(0x10));
    CHECK(gen->Init());
    CHECK(gen->TitleBar() != nullptr);
    gen->SetArea(10, 10, 210, 160);
    CHECK(factory.buttons == 3);                        // close, zoom, minimise
    CHECK(gen->GetW() == 200 && gen->GetH() == 150);
    CHECK(gen->MinWidth() >= 0x80 + 8);                 // the title bar's minimum plus the gutters
    cRZRect user;
    gen->GetUserArea(user);
    CHECK(user.left == 4 && user.right == 196 && user.top == 4 + gen->TitleBar()->GetH() && user.bottom == 146);
    CHECK(std::string(gen->GetCaption().Data()) == "Window");
    gen->TitleBar()->SetArea(4, 4, 196, 24);   // no font in the fake: the bar keeps whatever area the test gives it
    gen->SetArea(10, 10, 211, 160); gen->SetArea(10, 10, 210, 160);

    // Size modes: the title bar moves, the edges resize.
    CHECK(gen->calc_size_mode(100, 100) == 0);          // inside the user area
    CHECK(gen->calc_size_mode(100, 5) == 1);            // on the title bar
    gen->SetWinGenFlag(2, true);
    CHECK(gen->calc_size_mode(2, 100) == 2);            // left edge
    CHECK(gen->calc_size_mode(198, 100) == 3);          // right edge
    CHECK(gen->calc_size_mode(100, 148) == 5);          // bottom edge
    CHECK(gen->calc_size_mode(2, 148) == 8);            // bottom-left corner

    // Dragging moves the window.
    gen->GZOnMouseDownL(100, 5, 0);
    CHECK(gen->SizeMode() == 1);
    gen->GZOnMouseMove(110, 15, 0);
    CHECK(gen->GetL() == 20 && gen->GetT() == 20);
    CHECK(gen->GZOnMouseUpL(110, 15, 0) && gen->SizeMode() == 0);
    gen->GZWinMoveTo(10, 10);

    // Flags and the title bar.
    gen->SetWinGenFlag(0x1000, true);
    CHECK(gen->TitleBar() == nullptr);                  // a dialog has no title bar
    gen->SetWinGenFlag(0x1000, false);
    CHECK(gen->TitleBar() != nullptr);
    gen->SetWinGenFlag(0x10000, true);                  // side bar
    CHECK(gen->ChildExists(kWinGenSideBarId));
    gen->GetUserArea(user);
    CHECK(user.left == 4 + 8 + 4);
    gen->SetWinGenFlag(0x10000, false);
    CHECK(!gen->ChildExists(kWinGenSideBarId));

    // Escape closes (posts a delete-child to the parent for a plain window, ends the dialog for a dialog).
    mgr.dialogs.clear();
    gen->SetWinGenFlag(0x1000, true);
    CHECK(gen->GZOnKeyDown(0x1b, 0));
    CHECK(mgr.dialogs.size() == 1 && mgr.dialogs[0].code == 0xf && mgr.dialogs[0].result == 0x42b7c353);
    CHECK(!gen->GZOnKeyUp(0x41, 0));
    gen->SetWinGenFlag(0x1000, false);

    // Window procedure: it gets the creation message and can veto Init's success.
    Proc proc;
    gen->SetWinProc(&proc);
    CHECK(gen->GetWinProc() == &proc);
    CHECK(proc.seen.size() >= 2 && proc.seen[0] == kWinProcInit);
    proc.limits = true;
    gen->SetArea(10, 10, 211, 160); gen->SetArea(10, 10, 210, 160);
    CHECK(gen->GetW() >= 50 + 8 && gen->GetH() >= 40);   // limits applied to the user area, window = user + frame
    gen->SetWinProc(nullptr);

    // Colour bars and dividers.
    CHECK(gen->AddColorBar(0x77, 3, 0xe, cRZRect{0, 20, 30, 28}));
    CHECK(gen->ChildExists(0x77));
    CHECK(!gen->AddColorBar(0x77, 3, 0xe, cRZRect{0, 20, 30, 28}));    // the id is taken
    CHECK(gen->RemoveColorBar(0x77) && !gen->RemoveColorBar(0x77));
    gen->AddDividerH(0x78, 40);
    CHECK(gen->ChildExists(0x78));

    // User data and backgrounds.
    CHECK(gen->SetUserData(0, 42) && gen->GetUserData() == 42);
    std::uint32_t slot = 0;
    CHECK(gen->GetUserData(0, slot) && slot == 42 && !gen->GetUserData(1, slot) && !gen->SetUserData(1, 5));
    gen->SetBackgroundBlitType(kBGStretched);
    BGBltType type; cIGZBuffer* bg = nullptr;
    CHECK(gen->GetBackgroundBitmap(type, bg) && type == kBGStretched);
    int gx, gy; gen->GetGutters(gx, gy); CHECK(gx == 4 && gy == 4);
    gen->SetGutters(std::uint8_t(6), std::uint8_t(7));
    gen->GetGutters(gx, gy); CHECK(gx == 6 && gy == 7);
    gen->SetGutters(std::uint8_t(4), std::uint8_t(4));

    // Painting runs without a draw context only through the proc hooks; with one it draws the frame.
    Ctx ctx;
    gen->SetCtx(&ctx);
    CHECK(gen->GZPaint());
    CHECK(!ctx.log.empty());

    void* raw = nullptr;
    CHECK(gen->QueryInterface(kGZIID_cIGZWinGen, &raw) && raw == static_cast<cIGZWinGen*>(gen)); gen->Release();
    gen->Release();
    imagePtr->Release();
    std::printf(failures ? "FAILED (%d)\n" : "OK\n", failures);
    return failures ? 1 : 0;
}
