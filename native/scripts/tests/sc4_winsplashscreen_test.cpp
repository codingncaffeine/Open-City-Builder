// cSC4WinSplashScreen test: fake host (script window factory, graphic system, message server, app screens) and a fake
// window manager. Covers the two-view QueryInterface, Init / SetupScriptWindow centring, the double paint and its
// one-shot timer, the transition message (timer, click, key), DoMessage and Shutdown.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "fake_winmgr2.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/WinSplashScreen.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {

struct FakeGraphics final : cIGZUnknown {
    int refs = 1;
    int queries = 0;
    cIGZUnknown* view = nullptr;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        ++queries;
        if (iid != kSplashIIDGraphicSystem2 || !view) return false;
        *out = view; view->AddRef(); return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
};

struct FakeServer final : cIGZMessageServer2 {
    struct Post { std::uint32_t type; cIGZMessageTarget2* target; };
    std::vector<Post> posts;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool MessageSend(cIGZMessage2*) override { return true; }
    bool MessagePost(cIGZMessage2*, bool) override { return true; }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t) override { return true; }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t) override { return true; }
    bool GeneralMessagePostToTarget(cIGZMessage2* m, cIGZMessageTarget2* t) override { posts.push_back({m->GetType(), t}); return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 1; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

struct FakeHost final : ISplashScreenHost {
    FakeMgr2* mgr = nullptr;
    FakeGraphics graphics;
    FakeServer server;
    bool serverPresent = true;
    int scriptW = 400, scriptH = 300;
    bool scriptFails = false;
    std::vector<cGZPersistResourceKey> keys;
    std::vector<std::uint32_t> ids;
    cIGZWin* script = nullptr;
    int clears = 0;
    std::vector<int> order;   // 1 = destroy, 2 = create region
    cIGZWin* CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id) override {
        keys.push_back(key); ids.push_back(id);
        if (scriptFails) return nullptr;
        auto* w = new cGZWin(mgr);
        w->Init();
        w->SetArea(0, 0, scriptW, scriptH);
        w->SetID(id);
        parent->ChildAdd(w);
        script = w;
        w->AddRef();   // the reference the caller releases
        return w;
    }
    cIGZUnknown* GraphicSystemObject() override { return &graphics; }
    cIGZMessageServer2* MessageServer2() override { return serverPresent ? &server : nullptr; }
    cIGZMessage2Standard* CreateStandardMessage() override { auto* m = new cRZMessage2Standard(); m->AddRef(); return m; }
    void ClearScreenToBlack() override { ++clears; }
    void DestroyCurrentScreen() override { order.push_back(1); }
    void CreateRegionScreen() override { order.push_back(2); }
};

struct KeyCatcher final : cGZWin {
    std::vector<cGZMessage> got;
    explicit KeyCatcher(cIGZWinMgr* m) : cGZWin(m) {}
    bool DoMessage(cGZMessage& m) override { got.push_back(m); return true; }
};

}  // namespace

int main() {
    FakeMgr2 mgr;
    FakeWinGraphics2 gfx;
    SetWinGraphics(&gfx);
    FakeHost host;
    host.mgr = &mgr;
    SetSplashScreenHost(&host);

    // desktop: 800 x 600
    auto* desktop = new cGZWin(&mgr);
    desktop->AddRef();
    desktop->Init();
    desktop->SetArea(0, 0, 800, 600);
    mgr.mainWin = desktop;

    cIGZUnknown* view = &host.graphics;   // the iid 0xc47b747c view answers with another reference
    host.graphics.view = view;
    const int graphicsRefsBefore = host.graphics.refs;
    auto* splash = new cSC4WinSplashScreen(&mgr);
    splash->AddRef();
    desktop->ChildAdd(splash);   // adding a child initialises it, so Init runs here

    // constructor: the private-buffer flag is cleared
    CHECK(!splash->GetFlag(kWinPrivateBuffer));

    // QueryInterface: 1 and 0x452294aa give the message-target view, 0xaa38326e the window
    void* out = nullptr;
    CHECK(splash->QueryInterface(kSplashIIDMessageTarget2, &out) && out == static_cast<cIGZMessageTarget2*>(splash));
    splash->Release();
    CHECK(static_cast<void*>(static_cast<cIGZMessageTarget2*>(splash)) != static_cast<void*>(static_cast<cIGZWin*>(splash)));
    out = nullptr;
    CHECK(splash->QueryInterface(1, &out) && out == static_cast<cIGZMessageTarget2*>(splash));
    splash->Release();
    out = nullptr;
    CHECK(splash->QueryInterface(kSplashIIDWindow, &out) && out == static_cast<cIGZWin*>(splash));
    splash->Release();
    out = nullptr;
    CHECK(!splash->QueryInterface(0x12345678, &out));

    // Init: area follows the main window, the script window is built and centred, graphics are held
    CHECK(splash->Init());   // (already ran inside ChildAdd; the call is a no-op)
    CHECK(splash->GetW() == 800 && splash->GetH() == 600);
    CHECK(host.keys.size() == 1 && host.keys[0].type == 0 && host.keys[0].group == 0x96a006b0 && host.keys[0].instance == 0x8aa9aa14);
    CHECK(host.ids[0] == 0xaaa9c9d9);
    CHECK(host.script->GetL() == 200 && host.script->GetT() == 150);
    CHECK(!host.script->GetFlag(kWinFlag800));
    CHECK(splash->GraphicSystem() == &host.graphics && splash->GraphicSystem2() == view);
    CHECK(host.graphics.refs == graphicsRefsBefore + 2);
    CHECK(splash->Init());                       // already initialised: nothing more happens
    CHECK(host.keys.size() == 1);

    // SetupScriptWindow: a script window wider than the screen sits at 0, one pixel too wide centres to 0 as well
    host.scriptW = 900; host.scriptH = 601;
    CHECK(splash->SetupScriptWindow());
    CHECK(host.script->GetL() == 0 && host.script->GetT() == 0);
    host.scriptW = 799; host.scriptH = 500;
    CHECK(splash->SetupScriptWindow());
    CHECK(host.script->GetL() == 0 && host.script->GetT() == 50);
    host.scriptFails = true;
    CHECK(!splash->SetupScriptWindow());
    host.scriptFails = false;

    // GZPaint: first paint arms the 3 s timer and paints twice in a row (counter 0 -> 1, then 1 -> 0)
    CHECK(!splash->FirstPaintDone() && !splash->TimerRunning() && splash->PaintCounter() == 0);
    CHECK(splash->GZPaint());
    CHECK(splash->FirstPaintDone() && splash->TimerRunning() && splash->PaintCounter() == 1 && host.clears == 1);
    CHECK(splash->GZPaint());
    CHECK(splash->PaintCounter() == 0 && host.clears == 2);
    CHECK(splash->GZPaint());
    CHECK(splash->PaintCounter() == 1 && host.clears == 3);   // the cycle repeats
    std::uint32_t period = 0;
    splash->Timer().GetPeriod(period);
    CHECK(period == 3000);

    // transition message: the timer function posts 0x09dc338f to the message-target view
    cRZCallbackTimer::CallbackFunction fn = &cSC4WinSplashScreen::TransitionTimerCallback;
    fn(splash);
    CHECK(host.server.posts.size() == 1 && host.server.posts[0].type == kSplashMsgTransition);
    CHECK(host.server.posts[0].target == static_cast<cIGZMessageTarget2*>(splash));
    host.serverPresent = false;
    cSC4WinSplashScreen::TransitionTimerCallback(splash);   // no message server: nothing is posted
    CHECK(host.server.posts.size() == 1);
    host.serverPresent = true;

    // click: stops the timer, posts the transition, answers false
    CHECK(splash->TimerRunning());
    CHECK(!splash->GZOnMouseDownL(10, 10, 0));
    CHECK(!splash->TimerRunning());
    CHECK(host.server.posts.size() == 2 && host.server.posts[1].type == kSplashMsgTransition);

    // DoMessage: the transition destroys the splash then creates the region screen
    cRZMessage2Standard other;
    other.AddRef();
    other.SetType(0x1234);
    CHECK(!splash->DoMessage(static_cast<cIGZMessage2*>(&other)) && host.order.empty());
    other.SetType(kSplashMsgTransition);
    CHECK(splash->DoMessage(static_cast<cIGZMessage2*>(&other)));
    CHECK(host.order.size() == 2 && host.order[0] == 1 && host.order[1] == 2);

    // GZOnKeyDown: forwarded to the parent as a key-down message; no parent -> false
    auto* lone = new cSC4WinSplashScreen(&mgr);
    lone->AddRef();
    CHECK(!lone->GZOnKeyDown(65, 2));
    auto* catcher = new KeyCatcher(&mgr);
    catcher->AddRef();
    catcher->Init();
    catcher->ChildAdd(lone);
    mgr.AddWindowToValidList(catcher);
    CHECK(lone->GZOnKeyDown(65, 2));
    CHECK(catcher->got.size() == 1 && catcher->got[0].type == kGZMsgKeyDown && catcher->got[0].param1 == 65 && catcher->got[0].param2 == 2);

    // Shutdown: stops a running timer, deletes the children, drops the graphics references
    host.graphics.refs = 1;
    host.graphics.view = nullptr;
    splash->Timer().StartOneShot(3000, nullptr);
    CHECK(splash->TimerRunning());
    CHECK(splash->Shutdown());
    CHECK(!splash->TimerRunning() && splash->GetChildCount() == 0);
    CHECK(!splash->GetFlag(kWinInitialized));
    CHECK(splash->GraphicSystem() == nullptr && splash->GraphicSystem2() == nullptr);
    CHECK(splash->Shutdown());                   // second shutdown does nothing

    // a splash whose host has no graphic system initialises without one
    lone->Init();
    CHECK(lone->GraphicSystem() != nullptr);

    catcher->Release();   // deletes its child list reference to lone first
    lone->Release();
    desktop->Release();   // releases the splash it holds
    splash->Release();
    SetSplashScreenHost(nullptr);
    std::printf(failures ? "sc4_winsplashscreen: %d FAILED\n" : "sc4_winsplashscreen: all passed\n", failures);
    return failures ? 1 : 0;
}
