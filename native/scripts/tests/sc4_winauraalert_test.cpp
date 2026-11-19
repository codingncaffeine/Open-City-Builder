#include <cstdio>
#include <string>
#include <vector>

#include "fake_winmgr.h"
#include "sc4/WinAuraAlert.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;
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
    void BltTiled(IDrawTargetBuffer*, const cRZRect& s, const cRZRect& d, int, int) override {
        log.push_back("tile " + std::to_string(s.left) + "," + std::to_string(s.top) + "," + std::to_string(s.right) + "," + std::to_string(s.bottom) +
                      ">" + std::to_string(d.left) + "," + std::to_string(d.top) + "," + std::to_string(d.right) + "," + std::to_string(d.bottom)); }
    void Flush() override {} bool IsSuspended() override { return false; } void Suspend() override {} void Resume() override {}
    void Draw(eDrawingOp, const float*, int) override {} void DrawNoTransform(eDrawingOp, const float*, int) override {}
    bool Lock(cGZBufferSurfaceDesc&, bool) override { return true; } void Unlock() override {}
};
struct Probe final : cGZWin {
    using cGZWin::cGZWin;
    bool DoMessage(cGZMessage&) override { return true; }
};
struct Aura final : cSC4WinAuraBar {
    using cSC4WinAuraBar::cSC4WinAuraBar;
    void SetCtx(cIGZDrawContext* c) { drawContext_ = c; }
};
struct Alert final : cSC4WinAlertBorder {
    using cSC4WinAlertBorder::cSC4WinAlertBorder;
    void SetCtx(cIGZDrawContext* c) { drawContext_ = c; }
};
struct Properties final : IRenderProperties {
    int refs = 0;
    bool enabled = true;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool AlertBordersEnabled() override { return enabled; }
};
}  // namespace

int main() {
    FakeMgr mgr; FakeWinGraphics graphics; Ctx ctx;
    SetWinGraphics(&graphics);
    auto* parent = new Probe(&mgr);
    parent->AddRef();
    parent->SetArea(0, 0, 400, 400);

    // ---- aura bar
    auto* bar = new Aura(&mgr);
    bar->SetArea(10, 20, 110, 40);                        // 100 x 20
    parent->ChildAdd(bar);
    bar->SetCtx(&ctx);
    bar->CalcAbsoluteArea();
    bar->SetFillColorRGB(0x336699);
    CHECK(bar->GetValue() == 0.5);
    void* v = nullptr;
    CHECK(bar->QueryInterface(1, &v) && v == static_cast<cISC4WinAuraBar*>(bar));
    bar->Release();
    CHECK(bar->QueryInterface(kGZIID_cISC4WinAuraBar, &v) && v == static_cast<cISC4WinAuraBar*>(bar));
    bar->Release();
    CHECK(bar->QueryInterface(0x22ba0121, &v) && v == static_cast<cIGZWin*>(bar));
    bar->Release();
    CHECK(!bar->QueryInterface(77, &v));
    bar->GZPaint();
    CHECK(ctx.log.size() == 2 && ctx.log[0] == "color " + std::to_string(0x336699));
    const cRZRect area = bar->GetAreaAbsolute();
    CHECK(ctx.log[1] == "fill " + std::to_string(area.left) + "," + std::to_string(area.top) + "," + std::to_string(area.left + 50) + "," + std::to_string(area.bottom));
    bar->SetValue(2.0);
    CHECK(bar->GetValue() == 1.0);
    bar->SetValue(-1.0);
    CHECK(bar->GetValue() == 0.0);
    bar->SetValue(0.333);
    ctx.log.clear();
    bar->GZPaint();
    CHECK(ctx.log[1] == "fill " + std::to_string(area.left) + "," + std::to_string(area.top) + "," + std::to_string(area.left + 33) + "," + std::to_string(area.bottom));
    // with an image: a one-row strip, centred, at row value * (height - 1)
    auto* image = new cGZBuffer();
    image->AddRef();
    image->Init(120, 11, {kBufferColorARGB8888, 32});
    bar->SetValue(0.5);
    bar->SetImage(image);
    CHECK(image->Release() == 1);
    ctx.log.clear();
    bar->GZPaint();
    CHECK(ctx.log.size() == 1 && ctx.log[0] == "tile 10,5,110,6>" + std::to_string(area.left) + "," + std::to_string(area.top) + "," + std::to_string(area.right) + "," + std::to_string(area.bottom));
    bar->SetImage(nullptr);
    ctx.log.clear();
    bar->GZPaint();
    CHECK(ctx.log.size() == 2);

    // ---- alert border
    Properties properties;
    SetWinRenderProperties(&properties);
    auto* border = new Alert(&mgr);
    CHECK(properties.refs == 1);
    border->SetArea(0, 0, 90, 60);
    parent->ChildAdd(border);
    border->SetCtx(&ctx);
    border->CalcAbsoluteArea();
    CHECK(border->QueryInterface(1, &v) && v == static_cast<cISC4WinAlertBorder*>(border));
    border->Release();
    CHECK(border->QueryInterface(kGZIID_cISC4WinAlertBorder, &v) && v == static_cast<cISC4WinAlertBorder*>(border));
    border->Release();
    CHECK(border->QueryInterface(0xe98b2f57, &v) && v == static_cast<cIGZWin*>(border));
    border->Release();
    CHECK(border->AsIGZWin() == static_cast<cIGZWin*>(border));
    ctx.log.clear();
    border->GZPaint();
    CHECK(ctx.log.empty());                               // no image
    auto* frame = new cGZBuffer();
    frame->AddRef();
    frame->Init(30, 30, {kBufferColorARGB8888, 32});
    border->SetImage(frame);
    cIGZBuffer* asBuffer = frame;
    std::uint32_t key = 0;
    CHECK(asBuffer->GetTransparentColor(key) && key == asBuffer->ConvertRGBValueToNative(0xff, 0, 0xff));
    ctx.log.clear();
    border->GZPaint();
    CHECK(!ctx.log.empty());                              // the nine-slice frame was drawn
    properties.enabled = false;
    ctx.log.clear();
    border->GZPaint();
    CHECK(ctx.log.empty());
    parent->Release();   // deletes the tree (bar and border: the parent holds their only references)
    CHECK(properties.refs == 0);
    frame->Release();
    SetWinRenderProperties(nullptr);
    std::printf(failures ? "FAILED\n" : "sc4_winauraalert ok\n");
    return failures != 0;
}
