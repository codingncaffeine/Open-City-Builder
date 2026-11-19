#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "fake_drawcontext.h"
#include "fake_winmgr.h"
#include "gz/WinCtrlMgr.h"
#include "gz/WinMsgBox.h"

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
// A message box whose background the test can set.
struct Box final : cGZWinMsgBox {
    using cGZWinMsgBox::cGZWinMsgBox;
    void SetBackground(cIGZBuffer* b) { background_ = b; }
    void SetCtx(cIGZDrawContext* c) { drawContext_ = c; }
};
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
    SetWinCtrlMgr(m);
    SetWinCtrlFactory(m);

    auto* desktop = new cGZWin(&mgr);
    desktop->AddRef();
    desktop->SetArea(0, 0, 800, 600);
    mgr.mainWin = desktop;

    cRZString message("Save changes?"), title("Quit");
    auto* box = new Box(&mgr);
    box->AddRef();
    CHECK(box->TitleFillColor() == 0xff7c90ffu);
    CHECK(box->CreateDlg(message, title, kMsgBoxYesNoCancel));
    CHECK(box->Init());
    CHECK(box->GetDefaultButton() && box->GetCancelButton() && box->GetTitleText() && box->GetMessageText());
    CHECK(box->GetDefaultButton()->AsIGZWin()->GetID() == kMsgBoxYes && box->GetCancelButton()->AsIGZWin()->GetID() == kMsgBoxCancel);
    CHECK(box->GetID() == kWinMsgBoxDialogId && box->GetFlag(kWinPrivateBuffer));
    CHECK(box->GetW() >= 0x13c && box->GetL() == (800 - box->GetW()) / 2 && box->GetT() == (600 - box->GetH()) / 2);
    CHECK(box->GetTitleText()->AsIGZWin()->GetL() == 8 && box->GetTitleText()->AsIGZWin()->GetT() == 8);
    CHECK(box->GetMessageText()->AsIGZWin()->GetT() == box->GetTitleText()->AsIGZWin()->GetB() + 8);
    // the buttons sit in a row, 8 apart, the first at (width - 0x13c) / 2 + 8
    cIGZWin* yes = box->GetDefaultButton()->AsIGZWin();
    cIGZWin* no = box->GetChildWindowFromID(kMsgBoxNo);
    cIGZWin* cancel = box->GetCancelButton()->AsIGZWin();
    CHECK(no && yes->GetL() >= 8);
    CHECK(no && no->GetL() == yes->GetR() + 8 && cancel->GetL() == no->GetR() + 8);
    CHECK(yes->GetT() == no->GetT() && no->GetT() == cancel->GetT());

    // keyboard: Enter is OK, Escape and Alt+F4 are Cancel
    mgr.dialogs.clear();
    CHECK(box->GZOnKeyDown(0xd, 0) && mgr.dialogs.size() == 1 && mgr.dialogs[0].code == 0xf && mgr.dialogs[0].result == kMsgBoxOK);
    CHECK(box->GZOnKeyDown(0x1b, 0) && mgr.dialogs.back().result == kMsgBoxCancel);
    mgr.dialogs.clear();
    CHECK(box->GZOnKeyDown(0x73, 0) && mgr.dialogs.empty());
    CHECK(box->GZOnKeyDown(0x73, 4) && mgr.dialogs.size() == 1 && mgr.dialogs[0].result == kMsgBoxCancel);
    CHECK(box->GZOnKeyDown('A', 0) && mgr.dialogs.size() == 1 && box->GZOnKeyUp(0xd, 0));
    CHECK(!box->GZOnCommand(1, 2) && box->GZOnCommand(kWinMsgBoxCommand, 77) && mgr.dialogs.back().result == 77);

    // dragging
    const int l = box->GetL(), t = box->GetT();
    CHECK(box->GZOnMouseMove(500, 500, 0) && box->GetL() == l);                 // not dragging yet
    CHECK(box->GZOnMouseDownL(10, 10, 0) && mgr.capture == box && box->GetFlag(kWinFlagDragging));
    CHECK(box->GZOnMouseMove(15, 22, 0) && box->GetL() == l + 5 && box->GetT() == t + 12);
    CHECK(box->GZOnMouseUpL(15, 22, 0) && mgr.capture == nullptr && !box->GetFlag(kWinFlagDragging));
    CHECK(!box->GZOnMouseUpL(0, 0, 0));

    // painting: a plain fill plus the outline, or the tiled background
    fake_dc::Ctx ctx;
    box->SetCtx(&ctx);
    CHECK(box->GZPaint());
    CHECK(ctx.log.size() >= 2 && ctx.log[0].rfind("color", 0) == 0 && ctx.log[1].rfind("fill", 0) == 0);
    ctx.log.clear();
    auto* wall = new cGZBuffer; wall->AddRef();
    wall->Init(8, 8, cGZBufferColorType{kBufferColorRGB565, 16});
    box->SetBackground(wall);
    CHECK(box->GZPaint() && ctx.log[0] == "tile");
    box->SetBackground(nullptr);
    wall->Release();
    box->DestroyDlg();
    CHECK(!box->GetDefaultButton() && !box->GetCancelButton() && !box->GetTitleText() && !box->GetMessageText());
    void* raw = nullptr;
    CHECK(box->QueryInterface(kGZIID_cIGZWinMsgBox, &raw) && raw == static_cast<cIGZWinMsgBox*>(box)); box->Release();
    CHECK(box->QueryInterface(kGZIID_cIGZWinDlg, &raw) && raw == static_cast<cIGZWinDlg*>(box)); box->Release();
    CHECK(box->QueryInterface(1, &raw) && raw == static_cast<cIGZWin*>(box)); box->Release();
    CHECK(!box->QueryInterface(0x1234, &raw));
    box->Release();

    // every style
    struct Case { std::uint32_t style; std::vector<std::uint32_t> ids; };
    const Case cases[] = {
        {kMsgBoxOK_, {kMsgBoxOK}},
        {kMsgBoxOKCancel, {kMsgBoxOK, kMsgBoxCancel}},
        {kMsgBoxRetryCancel, {kMsgBoxRetry, kMsgBoxCancel}},
        {kMsgBoxYesNo, {kMsgBoxYes, kMsgBoxNo}},
        {kMsgBoxYesNoCancel, {kMsgBoxYes, kMsgBoxNo, kMsgBoxCancel}},
        {kMsgBoxAbortRetryIgnore, {kMsgBoxAbort, kMsgBoxRetry, kMsgBoxIgnore}},
    };
    for (const Case& c : cases) {
        auto* b = new Box(&mgr);
        b->AddRef();
        b->CreateDlg(message, c.style);                      // no title
        CHECK(b->TitleFillColor() == 0);
        CHECK(b->Init());
        CHECK(b->GetTitleText() == nullptr && b->GetMessageText() != nullptr);
        CHECK(b->GetMessageText()->AsIGZWin()->GetT() == 8);
        for (std::uint32_t id : c.ids) CHECK(b->GetChildWindowFromID(id) != nullptr);
        CHECK(b->GetChildCount() == c.ids.size() + 1);
        CHECK(b->GetDefaultButton()->AsIGZWin()->GetID() == c.ids.front() && b->GetCancelButton()->AsIGZWin()->GetID() == c.ids.back());
        // the button captions are the stock English ones
        CHECK(std::string(b->GetChildWindowFromID(c.ids.front())->GetCaption().Data()).size() >= 2);
        b->Release();
    }

    // an unknown style builds the text and then fails
    auto* bad = new Box(&mgr);
    bad->AddRef();
    bad->CreateDlg(message, 9);
    CHECK(!bad->Init() && bad->GetMessageText() != nullptr);
    bad->Release();

    // default texts from the manager win over the built-in ones
    cRZString* okText = new cRZString("Okay"); okText->AddRef();
    m->SetDefaultText(static_cast<int>(kMsgBoxOK), okText);
    auto* ok = new Box(&mgr);
    ok->AddRef();
    ok->CreateDlg(message, kMsgBoxOK_);
    CHECK(ok->Init() && std::string(ok->GetChildWindowFromID(kMsgBoxOK)->GetCaption().Data()) == "Okay");
    ok->Release();

    // without a main window the dialog cannot be centred
    mgr.mainWin = nullptr;
    auto* lost = new Box(&mgr);
    lost->AddRef();
    lost->CreateDlg(message, kMsgBoxOK_);
    CHECK(!lost->Init());
    lost->Release();

    // a fixed title colour
    auto* colored = new Box(&mgr);
    colored->AddRef();
    colored->SetTitleFillColor(1, 2, 3);
    CHECK(colored->TitleFillColor() == 0xff010203u);
    colored->SetTitleFillColor(0xff102030u);
    CHECK(colored->TitleFillColor() == 0xff102030u);
    colored->Release();

    SetWinCtrlMgr(nullptr);
    desktop->Release();
    okText->Release(); okText->Release();  // plus the reference SetDefaultText keeps
    m->Release();
    std::printf(failures ? "FAILED (%d)\n" : "OK\n", failures);
    return failures ? 1 : 0;
}
