#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "fake_drawcontext.h"
#include "fake_winmgr.h"
#include "gz/WinCtrlMgr.h"
#include "gz/WinGenDlg.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace gz;

namespace {
struct FakeFont final : IWinFont {
    int refs = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    std::uint32_t DefaultTextColor() override { return 0xff102030; }
    std::uint32_t DefaultShadowColor() override { return 0; }
    std::int16_t LineHeight(int, int) override { return 10; }
    int CountLines(const char*, std::uint32_t, int) override { return 1; }
    int WrappedWidth(const char*, std::uint32_t, int) override { return 0; }
    void MeasureText(const char*, std::uint32_t length, cRZRect& out) override { out = {0, 0, int(length) * 5, 10}; }
    int TextWidth(const char*, std::uint32_t length) override { return int(length) * 5; }
    void DrawText(IWinBuffer*, int, int, const char*, std::uint32_t, std::uint32_t, std::uint32_t, const cRZRect*, int) override {}
    void DrawTextBox(IWinBuffer*, const cRZRect&, const char*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, const cRZRect*, int) override {}
};
struct Host final : IWinButtonHost {
    FakeFont font;
    IWinButtonFont* Font(std::uint32_t) override { return &font; }
    cIGZBuffer* Image(std::uint32_t) override { return nullptr; }
    std::uint32_t DefaultSoundId(int) override { return 0; }
    void PlaySound(std::uint32_t) override {}
    void ShowTip(const BtnTipRequest&) override {}
    void HideTip() override {}
    bool HasWindowColorManager() override { return true; }
};
struct Sound final : IWinSoundSettings {
    int volume = 5; std::map<int, std::uint32_t> ids;
    void SetVolume(int v) override { volume = v; }
    int GetVolume() override { return volume; }
    std::uint32_t SoundId(int w) override { return ids[w]; }
    void SetSoundId(int w, std::uint32_t id) override { ids[w] = id; }
};
struct Creator final : cIGZCtrlCreator {
    int refs = 1; bool accept = true; int inits = 0, shutdowns = 0; std::vector<std::uint32_t> asked; bool answers = false;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZCtrlCreator) return false;
        *out = static_cast<cIGZCtrlCreator*>(this); AddRef(); return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Initialize() override { ++inits; return accept; }
    void Shutdown() override { ++shutdowns; }
    bool CreateCtrl(std::uint32_t classId, std::uint32_t, void**) override { asked.push_back(classId); return answers; }
};
struct Com final : IWinComSource {
    std::vector<std::uint32_t> asked;
    bool GetCOMObject(std::uint32_t classId, std::uint32_t, void**) override { asked.push_back(classId); return false; }
};
}  // namespace

namespace {
struct Dlg final : cGZWinGenDlg {
    using cGZWinGenDlg::cGZWinGenDlg;
    void SetCtx(cIGZDrawContext* c) { drawContext_ = c; }
};
struct Fonts final : IWinFontSource {
    IWinFont* font = nullptr;
    IWinFont* Font(std::uint32_t) override { return font; }
    bool Available() override { return true; }
};
std::vector<std::uint32_t> g_commands;
void OnCommand(std::uint32_t command, std::uint32_t param, void* context) { g_commands.push_back(command); g_commands.push_back(param); g_commands.push_back(*static_cast<std::uint32_t*>(context)); }
}  // namespace

int main() {
    FakeMgr mgr; FakeWinGraphics graphics; Sound sound; Host host; Com com; FakeFont font, boldFont;
    SetWinGraphics(&graphics); SetWinSoundSettings(&sound); SetWinButtonHost(&host); SetWinComSource(&com);
    auto* m = new cGZWinCtrlMgr(&mgr);
    m->AddRef();
    m->Init();
    m->SetSystemFont(0x68963c4c, &font);
    m->SetSystemFont(static_cast<int>(0xc8963c5c), &boldFont);
    m->SetSystemFont(0x68963c54, &font);
    m->SetSystemFont(0x28963c70, &boldFont);
    auto* edge = new cGZBuffer; edge->AddRef();
    edge->Init(24, 24, cGZBufferColorType{kBufferColorRGB565, 16});
    m->SetSystemBMP(static_cast<int>(0xc8963ed3), edge);
    SetWinCtrlMgr(m);
    SetWinCtrlFactory(m);
    Fonts fonts;
    fonts.font = &font;
    SetWinFontSource(&fonts);
    FakeCursors cursors;
    mgr.cursors = &cursors;

    // without a control manager Init "succeeds" and does nothing
    SetWinCtrlMgr(nullptr);
    auto* idle = new Dlg(&mgr);
    idle->AddRef();
    CHECK(idle->Init() && idle->Background() == nullptr && idle->ControlFont() == nullptr);
    CHECK(idle->AddBtn(cRZRect{0, 0, 10, 10}, cRZString("x")) == nullptr && idle->AddText(cRZRect{}, cRZString("x"), 0, false) == nullptr);
    idle->Release();
    SetWinCtrlMgr(m);

    // a bevelled dialog takes the stock background and reserves room for the frame
    auto* dlg = new Dlg(&mgr);
    dlg->AddRef();
    dlg->SetArea(0, 0, 300, 200);
    dlg->SetCaption(cRZString("Options"));
    CHECK(dlg->GetStyle() == kWindowBackgroundBevelled);
    CHECK(dlg->Init());
    CHECK(dlg->Background() == edge && dlg->ControlFont() == &font && dlg->TitleFont() == &boldFont);
    const cRZRect borders = dlg->GetBorderWidths();
    CHECK(borders.left == 6 && borders.top == 0x17 && borders.right == 6 && borders.bottom == 8);
    CHECK(dlg->GetFlag(kWinPrivateBuffer));
    void* raw = nullptr;
    CHECK(dlg->QueryInterface(kGZIID_cIGZWinGenDlg, &raw) && raw == static_cast<cIGZWinGenDlg*>(dlg)); dlg->Release();
    CHECK(dlg->QueryInterface(1, &raw) && raw == static_cast<cIGZWinGenDlg*>(dlg)); dlg->Release();
    CHECK(dlg->QueryInterface(kGZIID_cIGZWin, &raw) && raw == static_cast<cIGZWin*>(dlg)); dlg->Release();

    // painting: the bevelled style needs the background and the title is drawn with the title font
    fake_dc::Ctx ctx;
    dlg->SetCtx(&ctx);
    CHECK(dlg->GZPaint());

    // control factories
    const cRZRect area{10, 30, 110, 50};
    cIGZWin* list = dlg->AddListBox(area);
    CHECK(list && dlg->ChildExists(list) && list->GetL() == 10 && list->GetR() == 110 && list->GetID() == static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(list)));
    cRZString caption("Go");
    cIGZWin* btn = dlg->AddBtn(area, caption);
    CHECK(btn && dlg->ChildExists(btn) && std::string(btn->GetCaption().Data()) == "Go" && btn->GetID() != 0);
    cIGZWin* check = dlg->AddCheck(area, caption);
    CHECK(check && dlg->ChildExists(check) && check->GetT() == 30);
    cIGZWin* label = dlg->AddText(area, cRZString("Name"), 1, false);
    CHECK(label && dlg->ChildExists(label) && std::string(label->GetCaption().Data()) == "Name");
    cIGZWin* slider = dlg->AddSlider(area, 0, 10, 5, 1);
    CHECK(slider && dlg->ChildExists(slider) && slider->GetID() == static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(slider)));
    cIGZWin* bar = dlg->Scrollbar(area, 0, 100, 50, 2);
    CHECK(bar && dlg->ChildExists(bar));
    cIGZWin* spinner = dlg->AddSpinner(area, 1, 9, 3);
    CHECK(spinner && dlg->ChildExists(spinner));
    cIGZWin* group = dlg->AddOptionGroup(area, caption);
    CHECK(group && dlg->ChildExists(group) && group->GetL() == 10 && group->GetT() == 30);   // the group lays itself out inside the area
    cIGZWin* input = dlg->AddLineInput(area);
    CHECK(input && dlg->ChildExists(input));
    cIGZWin* combo = dlg->AddCombo(area, true);
    CHECK(combo != nullptr);   // the fake font has no full font, so the combo cannot finish Init and is not a child
    cIGZWin* edit = dlg->AddTextEdit(area, cRZString("hello"));
    CHECK(edit && dlg->ChildExists(edit) && std::string(edit->GetCaption().Data()) == "hello");
    auto* picture = new cGZBuffer; picture->AddRef();
    picture->Init(8, 8, cGZBufferColorType{kBufferColorRGB565, 16});
    cIGZWin* bmp = dlg->AddBMP(area, picture);
    CHECK(bmp && dlg->ChildExists(bmp));
    picture->Release();
    cIGZWin* line = dlg->AddLineGraph(area);
    cIGZWin* bars = dlg->AddBarGraph(area);
    cIGZWin* scatter = dlg->AddScatterGraph(area);
    CHECK(line && bars && scatter && dlg->ChildExists(line) && dlg->ChildExists(bars) && dlg->ChildExists(scatter));

    // the label is sent to the back with the "do not redraw" flag restored
    CHECK(!label->GetFlag(kWinFlag800));

    // commands: a child's command reaches the callback, a command from a sender with id 0 ends the dialog
    std::uint32_t context = 77;
    dlg->SetCommandCallback(&OnCommand, &context);
    CHECK(!dlg->GZOnCommand(5, 123) && g_commands.size() == 3 && g_commands[0] == 5 && g_commands[1] == 123 && g_commands[2] == 77);
    g_commands.clear();
    CHECK(!dlg->GZOnCommand(0x287259f6, reinterpret_cast<std::uint64_t>(btn)) && g_commands.size() == 3 && g_commands[0] == 0x287259f6);
    g_commands.clear();
    mgr.dialogs.clear();
    btn->SetID(0);
    CHECK(!dlg->GZOnCommand(0x287259f6, reinterpret_cast<std::uint64_t>(btn)) && mgr.dialogs.size() == 1 && mgr.dialogs[0].code == 0xf && mgr.dialogs[0].result == 0 && g_commands.empty());
    cGZWin stranger(&mgr);
    CHECK(!dlg->GZOnCommand(0x287259f6, reinterpret_cast<std::uint64_t>(&stranger)) && g_commands.empty() && mgr.dialogs.size() == 1);
    dlg->SetCommandCallback(nullptr, nullptr);
    CHECK(!dlg->GZOnCommand(9, 9) && g_commands.empty());

    // mouse: a movable dialog drags, any other forwards the mouse to its parent
    auto* desktop = new cGZWin(&mgr);
    desktop->AddRef();
    desktop->SetArea(0, 0, 800, 600);
    CHECK(desktop->ChildAdd(dlg));
    dlg->SetFlag(kWinFlag100, true);
    const int l = dlg->GetL(), t = dlg->GetT();
    CHECK(dlg->GZOnMouseDownL(5, 5, 0) && mgr.capture == dlg);
    CHECK(dlg->GZOnMouseMove(15, 25, 0) && dlg->GetL() == l + 10 && dlg->GetT() == t + 20);
    CHECK(dlg->GZOnMouseUpL(15, 25, 0) && mgr.capture == nullptr);
    CHECK(dlg->GZOnMouseMove(50, 50, 0) && dlg->GetL() == l + 10);                // released: no more dragging
    dlg->SetFlag(kWinFlag100, false);
    CHECK(dlg->GZOnMouseDownL(5, 5, 0) && dlg->GZOnMouseMove(1, 1, 0) && dlg->GetL() == l + 10);

    // a new background replaces the old one
    auto* wall = new cGZBuffer; wall->AddRef();
    wall->Init(8, 8, cGZBufferColorType{kBufferColorRGB565, 16});
    CHECK(dlg->SetBackground(wall) && dlg->Background() == wall);
    wall->Release();

    // tiled and filled dialogs
    auto* tiled = new Dlg(&mgr, kWindowBackgroundTiled);
    tiled->AddRef();
    tiled->SetBackground(edge);
    CHECK(tiled->GetStyle() == kWindowBackgroundTiled && tiled->Init() && tiled->GetBorderWidths().top == 0);
    tiled->SetCtx(&ctx);
    ctx.log.clear();
    CHECK(tiled->GZPaint());
    CHECK(!ctx.log.empty() && ctx.log[0].rfind("blt", 0) == 0);
    tiled->Shutdown();   // drops the background reference the destructor leaves alone
    tiled->Release();
    auto* filled = new Dlg(&mgr, kWindowBackgroundFilled);
    filled->AddRef();
    CHECK(filled->Init());
    filled->SetCtx(&ctx);
    ctx.log.clear();
    CHECK(filled->GZPaint() && ctx.log.size() == 2 && ctx.log[0].rfind("color", 0) == 0 && ctx.log[1].rfind("fill", 0) == 0);
    filled->Release();
    auto* bare = new Dlg(&mgr, kWindowBackgroundTiled);       // tiled without a background cannot start
    bare->AddRef();
    CHECK(!bare->Init());
    bare->Release();

    // The manager's Create* calls hand out a reference the dialog's child list does not own; the combo never became a
    // child, so the test holds its only reference.
    for (cIGZWin* w : {btn, check, label, slider, bar, spinner, group, input, bmp}) w->Release();
    combo->Release();
    dlg->Shutdown();
    CHECK(dlg->Background() == nullptr && dlg->ControlFont() == nullptr);
    desktop->Release();
    dlg->Release();
    SetWinCtrlMgr(nullptr);
    edge->Release();
    m->Shutdown();   // drops the manager's font and bitmap references; its destructor does not
    m->Release();
    std::printf(failures ? "FAILED (%d)\n" : "OK\n", failures);
    return failures ? 1 : 0;
}
