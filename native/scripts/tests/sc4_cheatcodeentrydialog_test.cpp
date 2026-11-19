// cSC4CheatCodeEntryDialog: the box's layout, what Return does with a cheat, a command and an unknown line, the session
// history walk (cases CCD-C1 / C2), the keys that close it, Ctrl held, and the sorted name list (CCD-C4).
#include <cstdio>
#include <string>
#include <vector>

#include "fake_drawcontext.h"
#include "fake_winmgr.h"
#include "gz/CheatCodeManager.h"
#include "gz/WinCtrlMgr.h"
#include "rz/cRZKeyboard.h"
#include "sc4/CheatCodeEntryDialog.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace gz;
using sc4::cSC4CheatCodeEntryDialog;

namespace {
struct Keyboard final : cIGZKeyboard, cRZKeyboard {
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
};

struct Host final : sc4::ICheatCodeEntryDialogHost {
    cGZCheatCodeManager* cheats = nullptr;
    std::uint32_t commandStatus = 2;
    std::vector<std::string> commands;
    bool debug = false;
    cIGZWin* main = nullptr;
    std::vector<std::string> menu;
    std::uint32_t pick = 99;
    cGZCheatCodeManager* CheatCodeManager() override { return cheats; }
    std::uint32_t ExecuteCommandText(const cIGZString& text) override {
        commands.emplace_back(text.Data(), text.Strlen());
        return commandStatus;
    }
    bool DebugFlag() override { return debug; }
    cIGZWin* MainWindow() override { return main; }
    std::uint32_t PopupStringListMenu(cIGZWin*, int, int, const std::vector<cRZString>& items, cRZString& choice) override {
        menu.clear();
        for (const cRZString& item : items) menu.emplace_back(item.Data(), item.Strlen());
        if (pick < items.size()) choice = items[pick];
        return pick;
    }
};

// Hears every cheat (the old-style target list), as the application does.
struct Listener final : cIGZMessageTarget {
    std::vector<std::uint32_t> heard;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool DoMessage(cGZMessage& m) override { heard.push_back(static_cast<std::uint32_t>(m.param1)); return true; }
};

std::string Line(cSC4CheatCodeEntryDialog* d) {
    cRZString text;
    d->Edit()->GetText(text);
    return std::string(text.Data(), text.Strlen());
}
void Type(cSC4CheatCodeEntryDialog* d, const char* text) { d->Edit()->SetText(cRZString(text), false); }
bool Return(cSC4CheatCodeEntryDialog* d) {
    cGZMessage m;
    m.type = kTextEditMsgKey;
    m.param2 = 0x0d;
    return d->DoMessage(m);
}
cSC4CheatCodeEntryDialog* Open(FakeMgr& mgr) {
    auto* d = new cSC4CheatCodeEntryDialog(&mgr);
    d->AddRef();
    d->Init();
    return d;
}
// What the game's DestroyWindow does: each child is shut down (the edit's caret timer lets go of it), then the box.
void Close(cSC4CheatCodeEntryDialog* d) {
    if (d->Edit()) d->Edit()->AsIGZWin()->Shutdown();
    d->Shutdown();
    d->Release();
}
}  // namespace

int main() {
    FakeMgr mgr;
    FakeWinGraphics graphics;
    SetWinGraphics(&graphics);
    Keyboard keyboard;
    mgr.keyboard = &keyboard;
    auto* controls = new cGZWinCtrlMgr(&mgr);
    controls->AddRef();
    controls->Init();
    auto* frame = new cGZBuffer;
    frame->AddRef();
    frame->Init(24, 24, cGZBufferColorType{kBufferColorRGB565, 16});
    controls->SetSystemBMP(static_cast<int>(0xc8963ed3), frame);
    SetWinCtrlMgr(controls);

    auto* cheats = new cGZCheatCodeManager();
    cheats->AddRef();
    constexpr std::uint32_t kWeaknessPays = 0x4ab87766;
    cheats->RegisterCheatCode(kWeaknessPays, cRZString("WeaknessPays"));
    Listener listener;
    cheats->AddNotification(&listener, 0);
    cGZWin mainWindow(&mgr);
    Host host;
    host.cheats = cheats;
    host.main = &mainWindow;
    sc4::SetCheatCodeEntryDialogHost(&host);

    // Layout: the 312 x 28 box at 8,8, the edit at 4,6..308,26 with flags 0x17c1 (Return notifies), focus on the edit,
    // the filter view, the magenta fill.
    {
        cSC4CheatCodeEntryDialog* d = Open(mgr);
        CHECK(d->Edit() != nullptr);
        CHECK(d->GetL() == 8 && d->GetT() == 8 && d->GetW() == 312 && d->GetH() == 28);
        cIGZWin* edit = d->Edit()->AsIGZWin();
        CHECK(edit->GetL() == 4 && edit->GetT() == 6 && edit->GetR() == 308 && edit->GetB() == 26);
        CHECK(d->Edit()->GetFlags() == 0x17c1);
        CHECK(mgr.focus == edit);
        CHECK(d->Background() == frame);
        void* filter = nullptr;
        CHECK(d->QueryInterface(sc4::kCheatDialogFilterIid, &filter) && filter == static_cast<cIGZWinMessageFilter*>(d));
        d->Release();
        std::uint8_t r = 0, g = 0, b = 0;
        d->GetFillColor(r, g, b);
        CHECK(r == 255 && g == 0 && b == 255);
        Close(d);
    }

    // An empty Return closes the box with 0; a line nobody knows keeps it open with the text; a registered cheat runs,
    // enters the history, closes the box with 1 and gives the app window the keys back.
    {
        cSC4CheatCodeEntryDialog::History().clear();
        cSC4CheatCodeEntryDialog* d = Open(mgr);
        mgr.dialogs.clear();
        CHECK(Return(d) && mgr.dialogs.size() == 1 && mgr.dialogs[0].code == 0x0f /* EndModal */ && mgr.dialogs[0].result == 0);
        mgr.dialogs.clear();
        Type(d, "zzz");
        host.commandStatus = 2;
        CHECK(Return(d) && mgr.dialogs.empty() && Line(d) == "zzz");
        CHECK(host.commands.size() == 1 && host.commands[0] == "zzz");
        CHECK(cSC4CheatCodeEntryDialog::History().empty());
        Type(d, "weaknesspays");
        listener.heard.clear();
        CHECK(Return(d));
        CHECK(listener.heard.size() == 1 && listener.heard[0] == kWeaknessPays);
        CHECK(host.commands.size() == 1);   // a cheat does not reach the command server
        CHECK(mgr.dialogs.size() == 1 && mgr.dialogs[0].result == 1 && mgr.dialogs[0].window == static_cast<cIGZWin*>(d));
        CHECK(mgr.focus == &mainWindow);
        CHECK(cSC4CheatCodeEntryDialog::History().size() == 1);
        Close(d);
    }

    // A line the command server runs (any status but 2) counts as run: history + close.
    {
        cSC4CheatCodeEntryDialog* d = Open(mgr);
        mgr.dialogs.clear();
        host.commandStatus = 0;
        Type(d, "some command");
        CHECK(Return(d) && mgr.dialogs.size() == 1 && mgr.dialogs[0].result == 1);
        CHECK(cSC4CheatCodeEntryDialog::History().size() == 2 && std::string(cSC4CheatCodeEntryDialog::History()[1].Data()) == "some command");
        host.commandStatus = 2;
        Close(d);
    }

    // Ctrl (or Shift) held: the cheat runs, the line clears and the box stays open for more.
    {
        cSC4CheatCodeEntryDialog* d = Open(mgr);
        mgr.dialogs.clear();
        keyboard.ProcessKeyDown(0x11);
        Type(d, "WeaknessPays");
        listener.heard.clear();
        CHECK(Return(d) && listener.heard.size() == 1 && mgr.dialogs.empty() && Line(d).empty());
        keyboard.ProcessKeyUp(0x11);
        keyboard.ProcessKeyDown(0x10);
        Type(d, "weaknesspays");
        CHECK(Return(d) && listener.heard.size() == 2 && mgr.dialogs.empty() && Line(d).empty());
        keyboard.ProcessKeyUp(0x10);
        Close(d);
    }

    // CCD-C1 / C2: the history [a, b]; a new box starts past the end. Up, Up, Up: b, a, then a stays. Down from a: b,
    // then the empty line, then nothing more.
    {
        auto& history = cSC4CheatCodeEntryDialog::History();
        history.clear();
        history.emplace_back("a");
        history.emplace_back("b");
        cSC4CheatCodeEntryDialog* d = Open(mgr);
        CHECK(d->GZOnKeyDown(0x26, 0) && Line(d) == "b");
        CHECK(d->GZOnKeyDown(0x26, 0) && Line(d) == "a");
        CHECK(d->GZOnKeyDown(0x26, 0) && Line(d) == "a");
        CHECK(d->GZOnKeyDown(0x28, 0) && Line(d) == "b");
        CHECK(d->GZOnKeyDown(0x28, 0) && Line(d).empty());
        Type(d, "x");
        CHECK(d->GZOnKeyDown(0x28, 0) && Line(d) == "x");   // already past the end: nothing changes
        // the keys that close it: Esc, Alt+F4 (not a plain F4); the edit's key-downs come through the filter
        mgr.dialogs.clear();
        CHECK(!d->GZOnKeyDown(0x73, 0) && mgr.dialogs.empty());
        CHECK(d->GZOnKeyDown(0x73, 4) && mgr.dialogs.size() == 1 && mgr.dialogs[0].result == 0);
        cGZMessage key;
        key.type = kGZMsgKeyDown;
        key.param1 = 0x1b;
        CHECK(d->DoMessage(d->Edit()->AsIGZWin(), key) && mgr.dialogs.size() == 2 && mgr.dialogs[1].result == 0);
        key.param1 = 0x41;
        CHECK(!d->DoMessage(d->Edit()->AsIGZWin(), key) && mgr.dialogs.size() == 2);
        Close(d);
        history.clear();
    }

    // CCD-C4: F1 lists every cheat name in byte order, only with the debug flag; a pick goes on the line.
    {
        cheats->RegisterCheatCode(0x1001, cRZString("zoo"));
        cheats->RegisterCheatCode(0x1002, cRZString("Abc"));
        cheats->RegisterCheatCode(0x1003, cRZString("abc"));
        cSC4CheatCodeEntryDialog* d = Open(mgr);
        host.debug = false;
        host.menu.clear();
        CHECK(d->GZOnKeyDown(0x70, 0) && host.menu.empty());
        host.debug = true;
        host.pick = 1;
        CHECK(d->GZOnKeyDown(0x70, 0));
        CHECK((host.menu == std::vector<std::string>{"Abc", "WeaknessPays", "abc", "zoo"}));
        CHECK(Line(d) == "WeaknessPays");
        host.pick = 99;
        Type(d, "keep");
        CHECK(d->GZOnMouseDownR(1, 1, 0) && Line(d) == "keep");   // nothing picked: the line stays
        Close(d);
    }

    // Without the cheat code manager Return just closes the box (0) and answers false.
    {
        host.cheats = nullptr;
        cSC4CheatCodeEntryDialog* d = Open(mgr);
        mgr.dialogs.clear();
        Type(d, "weaknesspays");
        CHECK(!Return(d) && mgr.dialogs.size() == 1 && mgr.dialogs[0].result == 0);
        Close(d);
        host.cheats = cheats;
    }

    sc4::SetCheatCodeEntryDialogHost(nullptr);
    cheats->RemoveNotification(&listener, 0);
    cheats->Release();
    SetWinCtrlMgr(nullptr);
    controls->Shutdown();   // the service lets go of its bitmaps
    controls->Release();
    frame->Release();
    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("sc4_cheatcodeentrydialog: all checks passed\n");
    return failures ? 1 : 0;
}
