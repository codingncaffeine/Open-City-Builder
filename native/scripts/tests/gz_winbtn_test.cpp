#include <cstdio>
#include <string>
#include <vector>

#include "fake_drawcontext.h"
#include "fake_winmgr.h"
#include "gz/WinBtn.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace gz;
using fake_dc::Ctx;

namespace {
struct FakeFont final : IWinFont {
    int refs = 0; std::vector<std::string> drawn;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    std::uint32_t DefaultTextColor() override { return 0x111111; }
    std::uint32_t DefaultShadowColor() override { return 0x222222; }
    std::int16_t LineHeight(int, int) override { return 12; }
    int CountLines(const char*, std::uint32_t length, int width) override { return width > 0 ? int((length * 6 + width - 1) / width) : 1; }
    int WrappedWidth(const char*, std::uint32_t, int) override { return 40; }
    void MeasureText(const char*, std::uint32_t length, cRZRect& out) override { out = {0, 0, int(length) * 6, 12}; }
    int TextWidth(const char*, std::uint32_t length) override { return int(length) * 6; }
    void DrawText(IWinBuffer*, int x, int y, const char*, std::uint32_t, std::uint32_t c1, std::uint32_t c2, const cRZRect*, int) override {
        drawn.push_back("text " + std::to_string(x) + "," + std::to_string(y) + " " + std::to_string(c1) + "/" + std::to_string(c2));
    }
    void DrawTextBox(IWinBuffer*, const cRZRect&, const char*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, const cRZRect*, int) override { drawn.push_back("box"); }
};
struct Host final : IWinButtonHost {
    FakeFont font; std::vector<std::uint32_t> sounds; int tips = 0, hides = 0; BtnTipRequest lastTip;
    cIGZBuffer* image = nullptr;
    IWinButtonFont* Font(std::uint32_t) override { return &font; }
    cIGZBuffer* Image(std::uint32_t) override { return image; }
    std::uint32_t DefaultSoundId(int which) override { return 100 + which; }
    void PlaySound(std::uint32_t id) override { sounds.push_back(id); }
    void ShowTip(const BtnTipRequest& r) override { ++tips; lastTip = r; }
    void HideTip() override { ++hides; }
    bool HasWindowColorManager() override { return true; }
};
struct Target final : cGZWin {
    using cGZWin::cGZWin;
    std::vector<cGZMessage> got;
    bool DoMessage(cGZMessage& m) override { got.push_back(m); return true; }
};
struct Button final : cGZWinBtn {
    using cGZWinBtn::cGZWinBtn;
    void SetCtx(cIGZDrawContext* c) { drawContext_ = c; }
};
}  // namespace

int main() {
    FakeMgr mgr; FakeWinGraphics graphics; Ctx ctx; Host host;
    SetWinGraphics(&graphics);
    SetWinButtonHost(&host);
    auto* target = new Target(&mgr);
    target->AddRef();
    target->SetArea(0, 0, 300, 300);
    auto* btn = new Button(&mgr);
    btn->SetID(5);
    btn->SetArea(10, 10, 74, 34);                       // 64 x 24
    btn->Init();                                         // style 2 without an image, default sounds, a font
    target->ChildAdd(btn);
    btn->SetCtx(&ctx);
    btn->CalcAbsoluteArea();
    CHECK(btn->GetStyle() == 2 && btn->GetSoundID(0) == 100 && btn->GetSoundID(2) == 102);
    CHECK(!btn->GetBtnFlag(kBtnHasCaption) && btn->GetBtnFlag(kBtnFitToContent) && !btn->GetBtnFlag(kBtnActOnDown));

    // images: a 3-frame strip, 30x10 -> frames of 10
    auto* strip = new cGZBuffer();
    strip->AddRef();
    strip->Init(40, 10, {kBufferColorARGB8888, 32});     // 4 frames of 10
    btn->SetStyle(2, strip, nullptr);
    CHECK(btn->GetImageW() == 10 && btn->GetImageH() == 10 && btn->CurrentFrameX() == 10);   // enabled: frame 1
    btn->GZOnMouseEnter(nullptr);
    CHECK(btn->CurrentFrameX() == 30);                   // hover: frame 3
    btn->GZOnMouseDownL(1, 1, 0);
    CHECK(btn->GetBtnFlag(kBtnPressed) && btn->CurrentFrameX() == 20 && host.sounds.back() == 100);
    target->got.clear();
    btn->GZOnMouseUpL(2, 2, 0);                          // released inside: activates and toggles on
    CHECK(btn->IsOn() && !btn->GetBtnFlag(kBtnPressed));
    bool sawActivated = false;
    for (const auto& m : target->got) if (m.type == kGZMsgCommand && m.param1 == kBtnCmdActivated && m.param2 == 5 && m.param3 == 1) sawActivated = true;
    CHECK(sawActivated);
    CHECK(host.sounds.size() == 3 && host.sounds[1] == 101 && host.sounds[2] == 102);

    // caption layout and painting
    cRZString caption("Hello");
    btn->SetCaption(caption);
    int x, y, w, h;
    btn->TextPosition(x, y, w, h);
    CHECK(w == 30 && h == 12);
    btn->SetCaptionStyle(1);                             // left
    btn->TextPosition(x, y, w, h);
    CHECK(x == 0);
    ctx.log.clear(); host.font.drawn.clear();
    btn->GZPaint();
    bool drewFrame = false;
    for (const auto& line : ctx.log) if (line.rfind("blt", 0) == 0) drewFrame = true;
    CHECK(drewFrame);                                    // the frame is stretched over the wide button with the nine-slice blit
    CHECK(host.font.drawn.size() == 1);

    // min / max size follow the caption and the image
    cRZRect info;
    CHECK(btn->GetMinMaxInfo(info) && info.left == 30 && info.top == 12 && info.right == 0x7fffffff);

    // tips
    btn->SetTipText(cRZString("Click me"));
    CHECK(btn->GetBtnFlag(kBtnTipEnabled));
    btn->GZOnMouseExit(nullptr);
    btn->GZOnMouseEnter(nullptr);
    CHECK(host.tips == 1 && std::string(host.lastTip.text->ToChar()) == "Click me");
    btn->ClearTipText();
    CHECK(host.hides > 0);

    // flashing counts down on each tick message
    btn->Flash(3, 100000);
    CHECK(btn->FlashCount() == 3);
    cGZMessage tick{kBtnMsgFlashTick, 0, 0, 0};
    btn->DoMessage(tick);
    CHECK(btn->FlashCount() == 2);
    btn->DoMessage(tick); btn->DoMessage(tick); btn->DoMessage(tick);
    CHECK(btn->FlashCount() == 0);
    btn->DoMessage(tick);                                // reaching 0 stops the timer
    btn->SetFontColor(0, 0x123456);
    std::uint32_t native = 0;
    CHECK(btn->GetFontColor(0, native) && native == 0x123456);
    btn->ClearFontColor(0);
    CHECK(!btn->GetFontColor(0, native));
    btn->SetGutters(2, 3);
    int a, b, c, d;
    btn->GetGutters(a, b, c, d);
    CHECK(a == 2 && b == 3 && c == 2 && d == 3);
    // style 4 (a check button, i386 SetStyle 0x001de7f0): frames of 8, "shift when pressed" off, the window still enabled,
    // and a click toggles it
    {
        auto* check = new Button(&mgr);
        check->SetID(6);
        check->SetArea(100, 10, 110, 20);
        check->Init();
        target->ChildAdd(check);
        check->SetCtx(&ctx);
        check->SetBtnFlag(kBtnShiftWhenPressed, true);
        check->SetStyle(4, strip, nullptr);
        CHECK(check->GetImageW() == 5);
        CHECK(!check->GetBtnFlag(kBtnShiftWhenPressed));
        CHECK(check->GetFlag(kWinEnabled));
        check->GZOnMouseDownL(1, 1, 0);
        check->GZOnMouseUpL(1, 1, 0);
        CHECK(check->IsChecked());
    }
    strip->Release();
    target->Release();
    std::printf(failures ? "FAILED\n" : "gz_winbtn ok\n");
    return failures != 0;
}
