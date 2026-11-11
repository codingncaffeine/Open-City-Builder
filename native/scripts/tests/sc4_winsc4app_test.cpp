// cWinSC4App test: a fake window manager, a fake IWinSC4AppHost (the process services the original reaches through
// SL:: / RZ:: globals) and the recovered screens. Covers the two-vtable QueryInterface, Init (caption / id / flags /
// area, the accelerator, the graphic system and its view, the fps overlay, the message and cheat notifications, the
// splash screen), the screen swap path (splash -> destroy -> region -> destroy -> city 3D view -> destroy), the key
// accelerator message, the cheat / focus / command messages, GZPaint's frame counter and fps text, Shutdown's release
// order and its init-flag quirk, and the SL::IWinSC4App view over the city screen.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "fake_winmgr2.h"
#include "gz/CheatCodeManager.h"
#include "gz/WinMgrBase.h"
#include "gz/WinCtrlFactory.h"
#include "gz/WinKeyAccelerator.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/View3DWin.h"
#include "sc4/WinRegionScreen.h"
#include "sc4/WinSC4App.h"
#include "sc4/WinSplashScreen.h"
#include "sc4/WinText.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {

constexpr std::uint32_t kIIDMessageTarget2 = 0x452294aa;
constexpr std::uint32_t kIIDWin = 0x22ba0121, kIIDWinAlt = 0xe98b2f57;
constexpr std::uint32_t kIIDGraphicsView = 0xc47b747c;
constexpr std::uint32_t kCheatEntryCommand = 0x2a9496c9;

// A graphic-system view (the 0xc47b747c interface) that does nothing.
struct FakeGraphicsView final : cIGZUnknown {
    int refs = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
};

struct FakeGraphics final : cIGZUnknown {
    int refs = 0, queries = 0;
    FakeGraphicsView view;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        ++queries;
        if (iid != kIIDGraphicsView) return false;
        *out = &view;
        view.AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
};

struct FakeRdd final : cIGZUnknown {
    int refs = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
};

// The message server: records the notification subscriptions and the messages sent.
struct FakeServer final : cIGZMessageServer2 {
    struct Notification { cIGZMessageTarget2* target; std::uint32_t type; int adds, removes; };
    std::vector<Notification> notifications;
    struct Sent { std::uint32_t type; std::uint64_t data1; bool hasData1; };
    std::vector<Sent> sent;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool MessageSend(cIGZMessage2* message) override {
        auto* standard = dynamic_cast<cRZMessage2Standard*>(message);
        sent.push_back({message->GetType(), standard ? standard->GetData1() : 0,
                        standard ? standard->GetHasData1() : false});
        return true;
    }
    bool MessagePost(cIGZMessage2*, bool) override { return true; }
    bool AddNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        for (auto& n : notifications) if (n.target == target && n.type == type) { ++n.adds; return true; }
        notifications.push_back({target, type, 1, 0});
        return true;
    }
    bool RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        for (auto& n : notifications) if (n.target == target && n.type == type) { ++n.removes; return true; }
        return false;
    }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 1; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

// The window manager keeps the two modal calls ProcessCommand makes countable.
struct FakeMgr final : FakeMgr2 {
    int popupCalls = 0;
    std::vector<std::uint32_t> destroyedIds;
    int RunPopup(cIGZWin* window) override { ++popupCalls; (void)window; return 7; }
    bool DestroyWindow(cIGZWin* window) override {
        destroyedIds.push_back(window->GetID());
        return FakeMgr2::DestroyWindow(window);
    }
};

struct FakeHost final : IWinSC4AppHost {
    FakeMgr* mgr = nullptr;
    FakeGraphics graphics;
    FakeRdd rdd;
    FakeServer server;
    gz::cRZWinKeyAccelerator* accelerator = nullptr;
    bool showFps = true;
    bool clockPreference = true;

    int readyToHearMusic = 0;
    int clockWindows = 0;
    int addDebugText = 0, removeDebugText = 0;
    std::vector<std::uint32_t> executed;
    std::vector<bool> appFocus;
    int cheatDialogs = 0;

    cIGZUnknown* GraphicSystem() override { return &graphics; }
    cIGZUnknown* RealtimeDebugDisplay() override { return &rdd; }
    void AddDebugTextDisplay(cIGZString*) override { ++addDebugText; }
    void RemoveDebugTextDisplay(cIGZString*) override { ++removeDebugText; }
    bool ShowFps() override { return showFps; }
    cIGZMessageServer2* MessageServer2() override { return &server; }
    cGZCheatCodeManager* CheatCodeManager() override { return cheats; }
    void ExecuteCommand(std::uint32_t command) override { executed.push_back(command); }
    bool ClockWindowPreference() override { return clockPreference; }
    void DoModelessClockWindow() override { ++clockWindows; }
    cIGZWin* CreateCheatCodeEntryDialog() override {
        ++cheatDialogs;
        auto* dialog = new cGZWin(mgr);   // reference count 0, like `new cSC4CheatCodeEntryDialog`
        dialog->SetID(0xc4ea71d1);
        return dialog;
    }
    void AppFocus(bool gained) override { appFocus.push_back(gained); }
    void ReadyToHearMusic() override { ++readyToHearMusic; }
    cIGZWinKeyAccelerator* CreateKeyAccelerator() override { accelerator->AddRef(); return accelerator; }
    cGZCheatCodeManager* cheats = nullptr;
    // the -intro switch and the intro screen (i386 cWinSC4App::Init / CreateIntroVideoScreen)
    bool hasIntroSwitch = false, provideIntro = false;
    std::string introSwitch;
    int introRequests = 0;
    bool CommandLineSwitch(const char* name, std::string& value) override {
        if (std::string(name) != "intro" || !hasIntroSwitch) return false;
        value = introSwitch;
        return true;
    }
    cIGZWin* CreateIntroVideoScreen(cIGZWinMgr* manager) override {
        ++introRequests;
        if (!provideIntro) return nullptr;
        auto* intro = new cGZWin(manager);
        intro->SetID(0x1a7f0000);
        return intro;
    }
};

struct Fixture {
    FakeMgr mgr;
    FakeHost host;
    gz::cRZWinKeyAccelerator* accelerator = new gz::cRZWinKeyAccelerator();
    cGZCheatCodeManager cheats;
    cGZWin* main = new cGZWin(&mgr);

    Fixture() {
        accelerator->AddRef();   // the test's own reference (the window takes another in Init)
        host.mgr = &mgr;
        host.accelerator = accelerator;
        host.cheats = &cheats;
        main->Init();
        main->SetArea(0, 0, 800, 600);
        main->AddRef();
        mgr.mainWin = main;
        // key down A -> command 0x1234
        accelerator->RegisterAccelerator(0x41, 0x1234);
    }
    ~Fixture() {
        main->Release();
        accelerator->Release();
    }
};

cGZMessage KeyMessage(std::uint32_t type, std::uint32_t key, std::uint32_t modifiers) {
    cGZMessage message;
    message.type = type;
    message.param1 = key;
    message.param2 = modifiers;
    return message;
}

}  // namespace


// ================================================================ cSC4WinText

namespace {

// The draw context the outline goes through (the compact fake used by the other window-paint tests).
struct Ctx final : gz::cIGZDrawContext {
    int fills = 0, pushes = 0, pops = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    void PushTransform() override {} void ClipReset() override {} void Clip(const cRZRect&) override {}
    void ClipAbsolute(const cRZRect&) override {} void Offset(const cRZPoint&) override {}
    void OffsetAbsolute(const cRZPoint&) override {} void PopTransform() override {}
    void PushState() override { ++pushes; } void PopState() override { ++pops; }
    cRZRect GetCanvasArea() override { return {}; } cRZRect GetClipRect() override { return {}; }
    cRZPoint GetOrigin() override { return {}; } int GetTransformStackDepth() override { return 0; }
    bool IsTargetAttached() override { return true; } void SetTarget(gz::IDrawTargetBuffer*) override {}
    gz::cIGZDrawEngine* GetEngine() override { return nullptr; } void ResetToDefaultState() override {}
    void SetColor(std::uint32_t) override {} void SetColor(std::uint32_t, std::uint32_t) override {}
    void SetAlphaBlend(int) override {} void SetBltModulate(bool) override {} void SetAntialiasing(bool) override {}
    void SetLineThickness(float) override {} void ClearGradient() override {}
    void SetGradientTransform(const float (*)[4], float, float) override {} void ClearBlt() override {}
    void SetBlt(cIGZUnknown*, float, float, float, float) override {} void ClearBltTiling() override {}
    void SetBltTiling(const cRZRect&) override {} void Clear() override {} void Point(int, int) override {}
    void Line(int, int, int, int) override {}
    void Fill(const cRZRect&) override { ++fills; }
    void Ellipse(int, int, int, int) override {} void FillEllipse(int, int, int, int) override {}
    void Blt(gz::IDrawTargetBuffer*, const cRZRect&, const cRZRect&) override {}
    void BltTiled(gz::IDrawTargetBuffer*, const cRZRect&, const cRZRect&, int, int) override {}
    void Flush() override {} bool IsSuspended() override { return false; } void Suspend() override {} void Resume() override {}
    void Draw(gz::eDrawingOp, const float*, int) override {} void DrawNoTransform(gz::eDrawingOp, const float*, int) override {}
    bool Lock(gz::cGZBufferSurfaceDesc&, bool) override { return true; } void Unlock() override {}
};

// The window's draw buffer: records the lock / fill calls the paint makes.
struct FakeBuffer final : IWinBuffer {
    struct FillCall { std::uint32_t color; cRZRect rect; };
    std::vector<FillCall> fills;
    std::vector<std::uint32_t> locks;
    gz::cIGZDrawContext* context = nullptr;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    const cRZRect* GetBounds() const override { static const cRZRect bounds{0, 0, 1024, 768}; return &bounds; }
    std::uint32_t MakeNativeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) override {
        return 0xff000000u | (std::uint32_t(r) << 16) | (std::uint32_t(g) << 8) | b;
    }
    void NativeToRGB(std::uint32_t n, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) override {
        r = (n >> 16) & 0xff; g = (n >> 8) & 0xff; b = n & 0xff;
    }
    void* GetDrawContext() override { return context; }
    bool SetFadeAlpha(std::uint8_t) override { return false; }
    bool Lock(std::uint32_t flags) override { locks.push_back(flags); return true; }
    void Unlock(std::uint32_t) override {}
    bool Fill(std::uint32_t color, const cRZRect& rect) override { fills.push_back({color, rect}); return true; }
};

// The font: answers the word wrap and character fit the paint asks for and records every DrawTextBox call.
struct FakeFont final : gz::IWinFont {
    struct Draw { cRZRect box; std::string text; std::uint32_t alignment, color, color2; };
    std::vector<Draw> draws;
    int formatCalls = 0, wordFits = 0, charFits = 0;
    std::string formatText, ellipsisSeen;
    std::uint32_t fitPerLine = 5, keepOnFit = 2;
    int lineHeight = 10, textWidthValue = 6;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    cIGZFont* AsFont() override { return nullptr; }
    std::uint32_t DefaultTextColor() override { return 0xffffffffu; }
    std::uint32_t DefaultShadowColor() override { return 0xff000000u; }
    std::int16_t LineHeight(int, int) override { return std::int16_t(lineHeight); }
    int CountLines(const char*, std::uint32_t, int) override { return 1; }
    int WrappedWidth(const char*, std::uint32_t, int) override { return 10; }
    void MeasureText(const char*, std::uint32_t, cRZRect& out) override { out = {0, 0, 10, lineHeight}; }
    int TextWidth(const char*, std::uint32_t) override { return textWidthValue; }
    void DrawText(IWinBuffer*, int, int, const char*, std::uint32_t, std::uint32_t, std::uint32_t,
                  const cRZRect*, int) override {}
    std::uint32_t FitLength(const char*, std::uint32_t, int, int) override { ++charFits; return keepOnFit; }
    std::uint32_t WordsToFitInWidth(const char*, std::uint32_t length, int, std::uint32_t*, int) override {
        ++wordFits;
        return std::min(length, fitPerLine);
    }
    bool FormatForEllipsis(cIGZString& text, const cIGZString& ellipsis, const cRZRect&, bool) override {
        ++formatCalls;
        formatText.assign(text.Data(), text.Strlen());
        ellipsisSeen.assign(ellipsis.Data(), ellipsis.Strlen());
        return true;
    }
    void DrawTextBox(IWinBuffer*, const cRZRect& box, const char* text, std::uint32_t length, std::uint32_t alignment,
                     std::uint32_t color, std::uint32_t color2, const cRZRect*, int) override {
        draws.push_back({box, std::string(text, length), alignment, color, color2});
    }
};

// The recovered paint reaches the cGZWin members the other window paints do; the test injects the buffer + context.
struct TestTextWin final : cSC4WinText {
    explicit TestTextWin(cIGZWinMgr* mgr) : cSC4WinText(mgr) {}
    void SetBuffers(IWinBuffer* buffer, gz::cIGZDrawContext* context) {
        drawBuffer_ = buffer;
        drawContext_ = context;
    }
};

bool operator==(const cRZRect& a, const cRZRect& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

void RunTextWindowTests() {
    // The paint takes the colour conversion from the installed graphics system.
    FakeWinGraphics2 graphics;
    SetWinGraphics(&graphics);
    gz::SetWinCtrlMgr(nullptr);   // no control manager: the paint falls back to the "..." ellipsis literal

    FakeMgr2 manager;
    FakeBuffer buffer;
    Ctx context;
    buffer.context = &context;
    FakeFont font;

    {
        // No draw buffer: the paint answers true and does nothing (0x1005c3353).
        auto* bare = new TestTextWin(&manager);
        bare->Init();
        CHECK(bare->GZPaint());
        bare->Release();
    }

    auto* window = new TestTextWin(&manager);
    window->Init();
    window->SetBuffers(&buffer, &context);
    window->SetFont(&font);
    window->SetArea(0, 0, 100, 20);
    window->SetWinTextFlag(gz::kTextWrap, false);
    window->SetCaption(cRZString("Hello"));
    window->SetAlignment(0);

    // ---- single line: ellipsize a copy of the caption and draw it once
    CHECK(window->GZPaint());
    CHECK(font.formatCalls == 1 && font.ellipsisSeen == "...");
    CHECK(font.formatText == "Hello");
    CHECK(font.draws.size() == 1);
    if (!font.draws.empty()) {
        CHECK(font.draws[0].text == "Hello");
        CHECK(font.draws[0].box == (cRZRect{2, 2, 98, 18}));   // drawArea + textRect (gutters 2)
        CHECK(font.draws[0].alignment == 0);
        // The caption's colours equal the font's own (SetFont made kTextDefaultColors true), so DrawTextBox gets the
        // "use the font's colours" sentinel.
        CHECK(font.draws[0].color == 0x8000000 && font.draws[0].color2 == 0x8000000);
    }
    // the opaque background: Lock(0x8001), Fill(GetFillColor(), drawArea_), Unlock(0x8001)
    CHECK(buffer.fills.size() == 1);
    if (!buffer.fills.empty()) {
        CHECK(buffer.fills[0].rect == (cRZRect{0, 0, 100, 20}));
        CHECK(buffer.fills[0].color == graphics.MakeNativeColor(0, 0, 0));
    }
    CHECK(buffer.locks.size() == 1 && buffer.locks[0] == 0x8001);

    // ---- the outline: Lock(0x8008) and the four Fill calls of RZDrawRectangleOutline
    window->SetOutlineUse(true, 0x0000ff);
    buffer.locks.clear();
    CHECK(window->GZPaint());
    CHECK(buffer.locks.size() == 2 && buffer.locks[0] == 0x8001 && buffer.locks[1] == 0x8008);
    CHECK(context.fills == 4 && context.pushes == 1 && context.pops == 1);

    // ---- wrapped: three lines from the font's word fit (5 bytes each), the vertical position from the alignment
    buffer.fills.clear();
    window->SetWinTextFlag(gz::kTextWrap, true);
    window->SetArea(0, 0, 100, 60);
    window->SetCaption(cRZString("abcdefghijkl"));
    font.draws.clear();
    font.wordFits = 0;
    const int formatsBeforeWrap = font.formatCalls;
    CHECK(window->GZPaint());
    CHECK(font.wordFits == 3);
    CHECK(font.draws.size() == 3);
    if (font.draws.size() == 3) {
        CHECK(font.draws[0].text == "abcde" && font.draws[0].box == (cRZRect{2, 2, 98, 12}));
        CHECK(font.draws[1].text == "fghij" && font.draws[1].box == (cRZRect{2, 12, 98, 22}));
        CHECK(font.draws[2].text == "kl" && font.draws[2].box == (cRZRect{2, 22, 98, 32}));
    }
    CHECK(font.formatCalls == formatsBeforeWrap);   // the wrapped path never calls FormatForEllipsis

    // ---- wrapped and cut: the last line keeps what fits and gets the ellipsis appended
    window->SetArea(0, 0, 100, 20);   // room for two lines
    window->SetCaption(cRZString("abcdefghijklmno"));
    font.draws.clear();
    font.charFits = 0;
    CHECK(window->GZPaint());
    CHECK(font.draws.size() == 2);
    CHECK(font.charFits == 1);
    if (font.draws.size() == 2) {
        CHECK(font.draws[0].text == "abcde");
        CHECK(font.draws[1].text == "fg...");               // FitLength's two bytes of the cut line + the ellipsis
        CHECK(font.draws[1].box == (cRZRect{2, 12, 98, 22}));
    }

    // ---- no room for the ellipsis: the cut line is not drawn at all (0x1005c3817)
    font.textWidthValue = 200;
    font.draws.clear();
    CHECK(window->GZPaint());
    CHECK(font.draws.size() == 1);
    CHECK(font.draws[0].text == "abcde");
    font.textWidthValue = 6;

    // ---- middle alignment: the slack is halved (0x1005c375a)
    window->SetAlignment(4);
    window->SetArea(0, 0, 100, 60);
    window->SetCaption(cRZString("abcdefghijkl"));
    font.draws.clear();
    CHECK(window->GZPaint());
    CHECK(font.draws.size() == 3);
    if (font.draws.size() == 3) {
        // (60 - 4 - 30) / 2 = 13
        CHECK(font.draws[0].box == (cRZRect{2, 15, 98, 25}));
    }

    window->Release();

    // ---- the GZCOM maker: 0x198 bytes and the cIGZWinText view
    // cSC4WinText adds no field to cGZWinText (the original allocates 0x198; the port's base is 0x190 because the
    // original's unnamed dword at +0x180 has no member here).
    static_assert(sizeof(cSC4WinText) == sizeof(gz::cGZWinText), "cSC4WinText adds no fields to cGZWinText");
    static_assert(sizeof(gz::cGZWinText) == 0x190, "cGZWinText layout changed");
    auto* made = CreateSC4WinText(&manager);
    CHECK(made != nullptr);
    CHECK(dynamic_cast<cSC4WinText*>(made) != nullptr);
    void* view = nullptr;
    CHECK(made->QueryInterface(0x212cdc1f /* kGZIID_cIGZWinText */, &view) && view == static_cast<void*>(made));
    static_cast<gz::cIGZWinText*>(view)->Release();   // the query's reference (the maker's object starts at 0)

    SetWinGraphics(nullptr);
}

}  // namespace

int main() {
    Fixture f;
    CHECK(f.main->GetChildCount() == 0);

    auto* window = new cWinSC4App(reinterpret_cast<cSC4App*>(&f.host), &f.mgr, &f.host);
    CHECK(window->App() == reinterpret_cast<cSC4App*>(&f.host));
    CHECK(window->Host() == &f.host);
    CHECK(window->GetFrameCount() == 0);
    CHECK(window->GetFrameRate() == 1.0f);
    CHECK(window->CurrentScreen() == nullptr);
    CHECK(window->View3D() == nullptr);

    // ---- Init happens when the application window is added to the desktop
    CHECK(f.main->ChildAdd(window));   // AddRef -> 1 and Init

    // ---- QueryInterface: the window view and the message-target view, nothing else (each answer AddRefs)
    // Two references are held after Init: the desktop's child list and the cheat manager's listener list.
    void* out = nullptr;
    CHECK(window->QueryInterface(kIIDMessageTarget2, &out) && out == static_cast<cIGZMessageTarget2*>(window));
    CHECK(static_cast<cIGZMessageTarget2*>(out)->Release() == 2);   // the reference count is shared
    for (std::uint32_t iid : {1u, kIIDWinAlt, kIIDWin}) {
        CHECK(window->QueryInterface(iid, &out) && out == window);
        CHECK(window->Release() == 2);
    }
    CHECK(!window->QueryInterface(0xdeadbeef, &out));
    CHECK(window->AddRef() == 3);
    CHECK(window->Release() == 2);
    CHECK(window->GetFlag(kWinInitialized));
    CHECK(window->GetCaption().Strlen() == 9);
    CHECK(std::string(window->GetCaption().ToChar()) == "WinSC4App");
    CHECK(window->GetID() == kWinSC4AppId);
    CHECK(!window->GetFlag(0x800));
    CHECK(window->GetW() == 800 && window->GetH() == 600);
    CHECK(window->KeyAccelerator() == f.accelerator);

    // the graphic system (+0x168) and its 0xc47b747c view (+0x170)
    CHECK(window->GraphicSystem() == &f.host.graphics);
    CHECK(f.host.graphics.refs == 1 && f.host.graphics.queries == 1);
    CHECK(window->GraphicSystemView() == &f.host.graphics.view && f.host.graphics.view.refs == 1);

    // the fps overlay: the RDD is told about the fps string
    CHECK(window->RealtimeDebugDisplay() == &f.host.rdd);
    CHECK(f.host.rdd.refs == 1 && f.host.addDebugText == 1);

    // the cheat manager: "ShowTime" registered (0xea5edb88) and a notification for every cheat
    std::uint32_t cheatId = 0;
    CHECK(f.cheats.DoesCheatCodeMatch(cRZString("ShowTime"), cheatId) != 0 && cheatId == 0xea5edb88);
    CHECK(f.cheats.DoesCheatCodeMatch(cRZString("showtime"), cheatId) != 0);   // case-insensitive

    // the message server: the cheat-entry command
    CHECK(f.host.server.notifications.size() == 1);
    CHECK(f.host.server.notifications[0].type == kCheatEntryCommand);
    CHECK(f.host.server.notifications[0].target == static_cast<cIGZMessageTarget2*>(window));
    CHECK(f.host.server.notifications[0].adds == 1);

    // the clock window preference was set
    CHECK(f.host.clockWindows == 1);

    // ---- the first screen: the splash screen
    cIGZWin* splash = window->CurrentScreen();
    CHECK(splash != nullptr);
    CHECK(dynamic_cast<cSC4WinSplashScreen*>(splash) != nullptr);
    CHECK(splash->IsVisible());
    CHECK(f.mgr.focus == splash);
    CHECK(window->GetChildCount() == 1);
    CHECK(splash->GetW() == 800 && splash->GetH() == 600);
    CHECK(!window->CreateSplashScreen());   // one screen at a time
    CHECK(!window->CreateRegionScreen());
    CHECK(!window->CreateCityScreen());

    // ---- GZPaint: frame counter, frame rate and the fps text
    CHECK(window->GZPaint());
    CHECK(window->GetFrameCount() == 1);
    CHECK(!std::isnan(window->GetFrameRate()));
    CHECK(window->FpsText().Strlen() != 0);
    CHECK(window->GZPaint() && window->GetFrameCount() == 2);
    f.host.showFps = false;
    CHECK(window->GZPaint() && window->GetFrameCount() == 3);
    std::string fpsKept(window->FpsText().ToChar());

    // ---- destroy the splash: it leaves the desktop, the focus and the registry
    CHECK(window->DestroyCurrentScreen());
    CHECK(window->CurrentScreen() == nullptr);
    CHECK(window->GetChildCount() == 0);
    CHECK(f.mgr.destroyed.size() == 1 && f.mgr.destroyed[0] == splash);
    CHECK(f.mgr.focus == nullptr);
    CHECK(!window->DestroyCurrentScreen());   // nothing to destroy

    // ---- the region screen; CreateRegionScreen sets the music flag
    CHECK(window->CreateRegionScreen());
    cIGZWin* region = window->CurrentScreen();
    CHECK(region != nullptr && dynamic_cast<cSC4WinRegionScreen*>(region) != nullptr);
    CHECK(region->IsVisible());
    CHECK(f.host.readyToHearMusic == 1);
    CHECK(f.mgr.focus == region);
    CHECK(window->View3D() == nullptr);
    CHECK(window->DestroyCurrentScreen());
    CHECK(window->GetChildCount() == 0);

    // ---- the city screen: the 3D view, its window and the SL::IWinSC4App view
    CHECK(window->CreateCityScreen());
    cSC4View3DWin* view = window->View3D();
    CHECK(view != nullptr);
    CHECK(window->CurrentScreen() == view->AsIGZWin());
    CHECK(window->CurrentScreen()->IsVisible());
    CHECK(f.mgr.focus == view->AsIGZWin());
    CHECK(window->GetChildCount() == 1);

    WinSC4AppApplication application(window);
    SL::IView3DSystems* systems = application.View3D();
    CHECK(systems != nullptr);
    CHECK(systems->View3DWin() == view->AsIGZWin());
    CHECK(systems->Renderer() == nullptr);          // no renderer on a view that was never initialised
    CHECK(systems->ModelMaker() == nullptr);
    CHECK(systems->ViewManager() == nullptr);
    CHECK(systems->RendererViewUtilities() == nullptr);
    CHECK(systems->RendererLightingManager() == nullptr);

    CHECK(window->DestroyCurrentScreen());
    CHECK(window->View3D() == nullptr);
    CHECK(window->GetChildCount() == 0);

    // ---- the key-unhandled message: the accelerator turns a matching key into a command
    cGZMessage keyDown = KeyMessage(kGZMsgKeyDown, 0x41, 0);
    cGZMessage unhandled;
    unhandled.type = gz::kWinMgrMsgKeyUnhandled;
    unhandled.param1 = reinterpret_cast<std::uint64_t>(window);
    unhandled.param2 = kGZMsgKeyDown;
    unhandled.param3 = reinterpret_cast<std::uint64_t>(&keyDown);
    CHECK(window->DoMessage(unhandled));
    CHECK(f.host.executed.size() == 1 && f.host.executed[0] == 0x1234);
    keyDown = KeyMessage(kGZMsgKeyDown, 0x42, 0);   // no accelerator
    CHECK(!window->DoMessage(unhandled));
    CHECK(f.host.executed.size() == 1);
    // a key-up also reaches the accelerator (Registered for key down only, so no command)
    keyDown = KeyMessage(kGZMsgKeyUp, 0x41, 0);
    unhandled.param2 = kGZMsgKeyUp;
    CHECK(!window->DoMessage(unhandled));
    // only key down / key up qualify; another type falls through to cGZWin::DoMessage
    unhandled.param2 = kGZMsgMouseDownL;
    CHECK(!window->DoMessage(unhandled));

    // ---- the cheat-typed message: only "ShowTime" opens the clock window
    cGZMessage cheat;
    cheat.type = kWinSC4AppMsgCheatTyped;
    cheat.param1 = kWinSC4AppCheatShowTime;
    CHECK(window->DoMessage(cheat));
    CHECK(f.host.clockWindows == 2);
    cheat.param1 = 0x12345678;
    CHECK(!window->DoMessage(cheat));
    CHECK(f.host.clockWindows == 2);

    // ---- the focus-change message: the app is told and the message is rebroadcast
    cGZMessage focus;
    focus.type = kWinSC4AppMsgFocusChange;
    focus.param1 = 1;
    window->DoMessage(focus);
    CHECK(f.host.appFocus.size() == 1 && f.host.appFocus[0]);
    CHECK(f.host.server.sent.size() == 1);
    CHECK(f.host.server.sent[0].type == kWinSC4AppMsgFocusChange);
    CHECK(f.host.server.sent[0].hasData1 && f.host.server.sent[0].data1 == 1);
    window->DoMessage(focus);   // still falls through to cGZWin::DoMessage (a second broadcast)

    // ---- the command message (cIGZMessage2*): the cheat-code entry dialog is run modally and destroyed
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(kCheatEntryCommand);
    CHECK(static_cast<cIGZMessageTarget2*>(window)->DoMessage(message));
    CHECK(f.host.cheatDialogs == 1);
    CHECK(f.mgr.popupCalls == 1);
    CHECK(f.mgr.destroyed.size() == 4);   // splash, region, city, dialog
    CHECK(f.mgr.destroyedIds.size() == 4 && f.mgr.destroyedIds[3] == 0xc4ea71d1);
    message->SetType(0x11111111);         // any other message is still answered true, without a dialog
    CHECK(static_cast<cIGZMessageTarget2*>(window)->DoMessage(message));
    CHECK(f.host.cheatDialogs == 1);
    message->Release();

    // ---- Shutdown: the services are released in the original's order
    CHECK(window->Shutdown());
    CHECK(!window->GetFlag(kWinInitialized));   // quirk: set to true before cGZWin::Shutdown, cleared there
    CHECK(window->KeyAccelerator() == nullptr);
    CHECK(window->GraphicSystem() == nullptr && f.host.graphics.refs == 0);
    CHECK(window->GraphicSystemView() == nullptr && f.host.graphics.view.refs == 0);
    CHECK(window->RealtimeDebugDisplay() == nullptr && f.host.rdd.refs == 0);
    CHECK(f.host.removeDebugText == 1);
    CHECK(f.host.server.notifications[0].removes == 1);
    CHECK(f.cheats.DoesCheatCodeMatch(cRZString("ShowTime"), cheatId) == 0);   // unregistered
    CHECK(window->Shutdown());   // idempotent: the flag is already clear
    CHECK(f.host.removeDebugText == 1);

    // ---- destruction through the desktop (ChildDeleteAll -> Shutdown -> Release -> delete)
    CHECK(f.main->ChildDeleteAll());
    CHECK(f.mgr.destroyed.size() == 5);   // + the application window

    // ---- the first screen with an intro: the host's intro screen (no switch), or the splash for -intro:OFF
    for (int variant = 0; variant < 3; ++variant) {
        Fixture g;
        g.host.provideIntro = variant != 2;
        g.host.hasIntroSwitch = variant == 1;
        g.host.introSwitch = "OFF";   // the original lower-cases the value before comparing
        auto* app = new cWinSC4App(reinterpret_cast<cSC4App*>(&g.host), &g.mgr, &g.host);
        CHECK(g.main->ChildAdd(app));
        cIGZWin* first = app->CurrentScreen();
        CHECK(first != nullptr);
        if (variant == 0) {
            CHECK(g.host.introRequests == 1);
            CHECK(first->GetID() == 0x1a7f0000);
            CHECK(first->IsVisible() && g.mgr.focus == first);
            CHECK(!app->CreateSplashScreen());   // one screen at a time
        } else {
            CHECK(g.host.introRequests == (variant == 1 ? 0 : 1));   // -intro:off never asks; no intro screen falls back
            CHECK(dynamic_cast<cSC4WinSplashScreen*>(first) != nullptr);
        }
        CHECK(app->DestroyCurrentScreen());
        CHECK(app->Shutdown());
        CHECK(g.main->ChildDeleteAll());
    }

    RunTextWindowTests();

    std::printf(failures ? "%d FAILURES\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
