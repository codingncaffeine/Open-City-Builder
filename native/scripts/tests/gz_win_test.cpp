#include <cstdio>
#include <set>
#include <vector>

#include "gz/Win.h"
#include "rz/cRZVariant.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {

struct FakeMgr final : cIGZWinMgr {
    std::set<cIGZWin*> valid;
    cIGZWin *focus = nullptr, *capture = nullptr, *mouseWin = nullptr;
    std::vector<cGZMessage> posted;
    int cleanups = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    cIGZWin* GetMainWindow() override { return nullptr; }
    void AppendToPlotStrategy(cIGZWin*) override {}
    void Plot() override {}
    void PostMessageToTarget(cIGZWin*, const cGZMessage& m) override { posted.push_back(m); }
    void AddWindowToValidList(cIGZWin* w) override { valid.insert(w); }
    void RemoveWindowFromValidList(cIGZWin* w) override { valid.erase(w); }
    void CleanUpWindowReferences(cIGZWin* w) override { ++cleanups; if (focus == w) focus = nullptr; if (capture == w) capture = nullptr; }
    bool DestroyWindow(cIGZWin* w) override { if (w->GetParentWin()) return w->GetParentWin()->ChildRemove(w); return true; }
    bool IsWindowValid(cIGZWin* w) override { return valid.count(w) != 0; }
    cIGZWin* GetWindowFromPoint(int, int) override { return nullptr; }
    void SetCurrentMouseWin(cIGZWin* w) override { mouseWin = w; }
    cIGZWin* GZGetCapture() override { return capture; }
    void GZSetCapture(cIGZWin* w) override { capture = w; }
    void GZReleaseCapture(cIGZWin* w) override { if (capture == w) capture = nullptr; }
    cIGZCursorManager* GetCursorManager() override { return nullptr; }
    cIGZWin* GZGetFocus() override { return focus; }
    void GZSetFocus(cIGZWin* w) override { focus = w; }
    cIGZKeyboard* GetKeyboard() override { return nullptr; }
};

struct TestWin : cGZWin {
    using cGZWin::cGZWin;
    std::vector<std::string> log;
    bool GZOnMouseDownL(int x, int y, std::uint32_t f) override { log.push_back("down " + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(f)); return true; }
    bool GZOnKeyDown(std::uint32_t k, std::uint32_t m) override { log.push_back("key " + std::to_string(k) + "," + std::to_string(m)); return k == 1; }
    bool GZOnCommand(std::uint32_t c, std::uint64_t p) override { log.push_back("cmd " + std::to_string(c) + "," + std::to_string(p)); return true; }
    bool GZOnMouseWheel(int x, int y, std::uint32_t f, int d) override { log.push_back("wheel " + std::to_string(d) + "," + std::to_string(f)); return true; }
};

struct Accel final : cIGZWinKeyAccelerator {
    int refs = 1, calls = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool RegisterAccelerator(std::uint32_t, std::uint32_t) override { return false; }
    bool RegisterAccelerator(std::uint32_t, const cGZMessage&) override { return false; }
    bool UnregisterAccelerator(std::uint32_t) override { return false; }
    bool IsAcceleratorPresent(std::uint32_t, bool, bool) override { return false; }
    bool TestForAccelerator(const cGZMessage&, std::uint32_t&) override { return false; }
    bool TestForAccelerator(const cGZMessage&, cGZMessage&) override { return false; }
    bool TestForAccelerator(std::uint32_t, std::uint32_t, bool, std::uint32_t&) override { return false; }
    bool TestForAccelerator(std::uint32_t, std::uint32_t, bool, cGZMessage&) override { return false; }
    int GetKeysForAccelerator(std::uint32_t, std::uint32_t*, std::uint32_t&) override { return 0; }
    bool ProcessAccelerator(const cGZMessage&) override { ++calls; return true; }
};

struct Filter final : cIGZWinMessageFilter {
    int refs = 1; bool consume = false; int seen = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool DoMessage(cIGZWin*, cGZMessage&) override { ++seen; return consume; }
};

std::vector<std::uint32_t> Order(cGZWin& w) {
    std::vector<std::uint32_t> ids;
    w.EnumChildren(kGZIID_cIGZWin, [](cIGZWin*, std::uint32_t id, void*, void* ctx) { static_cast<std::vector<std::uint32_t>*>(ctx)->push_back(id); return true; }, &ids);
    return ids;
}

TestWin* Make(FakeMgr& mgr, std::uint32_t id, int l, int t, int r, int b) {
    auto* w = new TestWin(&mgr);
    w->SetID(id);
    w->SetArea(l, t, r, b);
    return w;
}

}  // namespace

int main() {
    FakeMgr mgr;
    auto* root = Make(mgr, 100, 0, 0, 640, 480);
    root->AddRef();
    root->SetFlag(kWinDesktop, true);
    CHECK(root->GetFlag(kWinVisible) && root->GetFlag(kWinEnabled) && root->GetW() == 640 && root->GetH() == 480);

    auto* a = Make(mgr, 1, 10, 20, 110, 70);
    auto* b = Make(mgr, 2, 50, 50, 150, 150);
    auto* c = Make(mgr, 3, 0, 0, 20, 20);
    CHECK(root->ChildAdd(a) && root->ChildAdd(b) && root->ChildAdd(c));
    CHECK(!root->ChildAdd(a));                                  // already a child
    CHECK(Order(*root) == (std::vector<std::uint32_t>{3, 2, 1}));   // newest in front
    CHECK(root->GetChildCount() == 3 && a->GetParentWin() == root && a->GetNotificationTarget() == root && a->GetFlag(kWinInitialized));
    CHECK(root->ChildExists(a) && root->ChildExists(2u) && !root->ChildExists(77u));

    // z-order
    CHECK(root->ChildToFront(a));
    CHECK(Order(*root) == (std::vector<std::uint32_t>{1, 3, 2}));
    CHECK(root->ChildToBack(a));
    CHECK(Order(*root) == (std::vector<std::uint32_t>{3, 2, 1}));
    CHECK(root->ChildStepFront(a));
    CHECK(Order(*root) == (std::vector<std::uint32_t>{3, 1, 2}));
    CHECK(root->ChildStepBack(a));
    CHECK(Order(*root) == (std::vector<std::uint32_t>{3, 2, 1}));
    CHECK(root->ChildMoveRelative(c, a, true));                 // c just before a
    CHECK(Order(*root) == (std::vector<std::uint32_t>{2, 3, 1}));
    CHECK(root->ChildMoveRelative(b, a, false));                // b just after a
    CHECK(Order(*root) == (std::vector<std::uint32_t>{3, 1, 2}));
    a->PullToFront();
    CHECK(Order(*root) == (std::vector<std::uint32_t>{1, 3, 2}));
    a->SetFlag(kWinFlag800, false);
    CHECK(!root->ChildToBack(a));                               // fixed windows don't move
    a->SetFlag(kWinFlag800, true);
    CHECK(root->SortChildren(nullptr, nullptr, true));
    CHECK(Order(*root) == (std::vector<std::uint32_t>{1, 2, 3}));

    // lookup
    auto* inner = Make(mgr, 9, 5, 5, 25, 25);
    CHECK(a->ChildAdd(inner));
    CHECK(root->GetChildWindowFromID(2) == b && root->GetChildWindowFromID(9) == nullptr && root->GetChildWindowFromIDRecursive(9) == inner);
    void* as = nullptr;
    CHECK(root->GetChildAsRecursive(9, kGZIID_cIGZWin, &as) && as == inner);
    inner->Release();

    // geometry
    CHECK(inner->GetAreaAbsolute().left == 15 && inner->GetAreaAbsolute().top == 25 && inner->GetAreaAbsolute().right == 35);
    a->GZWinMoveTo(100, 100);
    CHECK(inner->GetAreaAbsolute().left == 105 && inner->GetAreaAbsolute().bottom == 125);
    a->GZWinOffset(-90, -80);
    CHECK(a->GetL() == 10 && a->GetT() == 20 && a->GetR() == 110 && a->GetB() == 70);
    int x = 3, y = 4;
    inner->WindowToScreenCoordinates(x, y);
    CHECK(x == 18 && y == 29);
    inner->ScreenToWindowCoordinates(x, y);
    CHECK(x == 3 && y == 4);
    int px = 3, py = 4;
    CHECK(inner->WindowToWindowCoordinates(root, px, py) && px == 18 && py == 29);
    CHECK(inner->IsPointInWindowWindowCoordinates(19, 19) && !inner->IsPointInWindowWindowCoordinates(20, 5) && !inner->IsPointInWindowWindowCoordinates(-1, 5));
    CHECK(inner->IsPointInWindowParentCoordinates(5, 5) && !inner->IsPointInWindowParentCoordinates(25, 5));
    CHECK(inner->IsPointInWindowScreenCoordinates(15, 25) && !inner->IsPointInWindowScreenCoordinates(35, 25));
    CHECK(root->GetChildWindowFromPoint(20, 30) == inner && root->GetChildWindowFromPoint(60, 30) == a && root->GetChildWindowFromPoint(600, 400) == root);
    CHECK(root->GetChildWindowFromPoint(700, 700) == nullptr);
    inner->SetFlag(kWinIgnoreCursor, true);
    CHECK(root->GetChildWindowFromCursorPoint(20, 30) == a);
    cRZRect r{-5, -5, 700, 500};
    CHECK(!root->FitRectToWindow(r, 2) && r.left == -60 && r.top == -20 && r.right == 638 && r.bottom == 478);
    cRZRect fits{-1, 5, 100, 100};
    CHECK(root->FitRectToWindow(fits, 2) && fits.left == 2 && fits.top == 5);
    cRZRect centered{0, 0, 100, 100};
    b->CenterWindowInRect(centered);
    CHECK(b->GetL() == 0 && b->GetT() == 0 && b->GetW() == 100);

    // flags and focus
    mgr.focus = a;
    a->SetFlag(kWinVisible, false);
    CHECK(!a->IsVisible() && mgr.focus == root);              // focus falls back to the notification target
    a->ShowWindow();
    CHECK(a->IsVisible());
    mgr.capture = b;
    b->SetFlag(kWinEnabled, false);
    CHECK(mgr.capture == nullptr && !b->IsEnabled());
    b->SetFlag(kWinEnabled, true);

    // parameters
    auto* v = new cRZVariant();
    v->AddRef();
    v->SetValUint32(77);
    CHECK(a->SetParam(5, v));
    cIGZVariant* got = nullptr;
    CHECK(a->GetParam(5, &got) && got == v && !a->GetParam(6, &got));
    int seen = 0;
    a->EnumParams([](cIGZWin*, std::uint32_t k, cIGZVariant*, void* c) { *static_cast<int*>(c) += k; return true; }, &seen);
    CHECK(seen == 5);
    a->SetParam(5, nullptr);
    v->Release();
    got->Release();  // GetParam hands out a reference

    // messages: routing, filters, accelerator
    Filter filter;
    a->AddMessageFilter(&filter);
    cGZMessage down; down.type = kGZMsgMouseDownL; down.param1 = 12; down.param2 = 34; down.param3 = std::uint64_t(3) << 16;
    CHECK(a->DoMessage(down) && a->log.back() == "down 12,34,3" && filter.seen == 1);
    filter.consume = true;
    a->log.clear();
    CHECK(a->DoMessage(down) && a->log.empty());
    CHECK(a->RemoveMessageFilter(&filter) && !a->RemoveMessageFilter(&filter) && filter.refs == 1);
    cGZMessage cmd; cmd.type = kGZMsgCommand; cmd.param1 = 5; cmd.param2 = 99;
    CHECK(a->DoMessage(cmd) && a->log.back() == "cmd 5,99");
    cGZMessage wheel; wheel.type = kGZMsgMouseWheel; wheel.param1 = 1; wheel.param2 = 2; wheel.param3 = (std::uint64_t(7) << 16) | 0xfffe;
    CHECK(a->DoMessage(wheel) && a->log.back() == "wheel -2,7");
    Accel accel;
    a->SetKeyboardAccelerator(&accel);
    cGZMessage key; key.type = kGZMsgKeyDown; key.param1 = 2; key.param2 = 0;
    CHECK(a->DoMessage(key) && accel.calls == 1);              // not handled by the window -> accelerator
    key.param1 = 1;
    CHECK(a->DoMessage(key) && accel.calls == 1);
    cGZMessage unknown; unknown.type = 2;
    CHECK(!a->DoMessage(unknown));
    a->PostMsg(b, 9, 1, 2, 3);
    CHECK(mgr.posted.size() == 1 && mgr.posted[0].type == 9 && mgr.posted[0].param3 == 3);
    a->SetKeyboardAccelerator(nullptr);
    CHECK(accel.refs == 1);

    // key equivalents and misc
    CHECK(a->MakeKeyEquivalent(0x41, 2) == 0x20041);
    a->SetKeyEquivalent(0x20041);
    CHECK(a->CheckKeyEquivalent(0x41, 2) && root->IsChildKeyEquivalent(0x41, 2) == a && !root->IsChildKeyEquivalent(0x42, 2));
    a->SetFadeEffectPeriod(10, 20);
    int fi, fo;
    a->GetFadeEffectPeriod(fi, fo);
    CHECK(fi == 10 && fo == 20);
    a->SetFillColor(std::uint8_t(1), std::uint8_t(2), std::uint8_t(3));
    CHECK(a->GetFillColorRGB() == 0x010203);

    // read locks: removing while enumerating works on a snapshot
    struct Ctx { cGZWin* parent; int visited = 0; } ctx{root};
    root->EnumChildren(kGZIID_cIGZWin, [](cIGZWin*, std::uint32_t id, void* child, void* c) {
        auto* cx = static_cast<Ctx*>(c);
        ++cx->visited;
        if (id == 1) cx->parent->ChildRemove(static_cast<cIGZWin*>(child));
        return true;
    }, &ctx);
    CHECK(ctx.visited == 3 && root->GetChildCount() == 2 && !root->ChildExists(1u));

    // teardown: deleting all children
    CHECK(root->ChildDeleteAll() && root->GetChildCount() == 0);
    root->Release();
    std::printf(failures ? "FAILED\n" : "gz_win ok\n");
    return failures ? 1 : 0;
}
