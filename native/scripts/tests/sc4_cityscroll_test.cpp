// cSC4ViewInputControlCityScroll::OnMouseMove (i386 0x0020687a): the cursor points the way the view scrolls.
#include <cstdio>
#include <map>

#include "sc4/ViewInputControlCityScroll.h"
#include "vic_fakes.h"

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

struct AnyCursors final : cIGZCursorManager {
    std::map<std::uint32_t, vic_fakes::Cursor> cursors;
    cIGZCursor* LoadCursor(std::uint32_t id) override { return &cursors[id]; }
    void SetCursor(cIGZCursor*) override {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
};
struct Mgr2 final : cIGZWinMgr {
    AnyCursors cursors;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    cIGZWin* GetMainWindow() override { return nullptr; }
    void AppendToPlotStrategy(cIGZWin*) override {}
    void Plot() override {}
    void PostMessageToTarget(cIGZWin*, const cGZMessage&) override {}
    void AddWindowToValidList(cIGZWin*) override {}
    void RemoveWindowFromValidList(cIGZWin*) override {}
    void CleanUpWindowReferences(cIGZWin*) override {}
    bool DestroyWindow(cIGZWin*) override { return true; }
    bool IsWindowValid(cIGZWin*) override { return true; }
    cIGZWin* GetWindowFromPoint(int, int) override { return nullptr; }
    void SetCurrentMouseWin(cIGZWin*) override {}
    cIGZWin* GZGetCapture() override { return nullptr; }
    void GZSetCapture(cIGZWin*) override {}
    void GZReleaseCapture(cIGZWin*) override {}
    cIGZCursorManager* GetCursorManager() override { return &cursors; }
    cIGZWin* GZGetFocus() override { return nullptr; }
    void GZSetFocus(cIGZWin*) override {}
    cIGZKeyboard* GetKeyboard() override { return nullptr; }
};
struct Host final : IViewInputHost, ICityScrollHost {
    Mgr2 mgr;
    IViewInputView* View3D() override { return nullptr; }
    cIGZWinMgr* WindowManager() override { return &mgr; }
    void PlaySound(int, std::uint32_t) override {}
    bool RightButtonDown() override { return true; }
    bool ReadTunables(std::uint32_t&, std::uint32_t&) override { return false; }   // dead zone 12, full speed 80
    void SetScrolling(bool, float, float) override {}
    void ScrollStop() override {}
};
}  // namespace

int main() {
    Host host;
    SetViewInputHost(&host);
    SetCityScrollHost(&host);
    auto* tool = new cSC4ViewInputControlCityScroll();
    tool->AddRef();
    tool->SetMouseScroll(100, 100);
    auto at = [&](int x, int y) { tool->OnMouseMove(x, y, 0); return tool->CursorId(); };
    CHECK(at(160, 100) == 0xc2a676a4u);   // right
    CHECK(at(40, 100) == 0x62a67694u);    // left
    CHECK(at(100, 40) == 0x62a67606u);    // up
    CHECK(at(160, 40) == 0x82a676a8u);    // up-right
    CHECK(at(40, 40) == 0x02a67691u);     // up-left
    CHECK(at(100, 160) == 0x62a6769bu);   // down
    CHECK(at(40, 160) == 0xa2a67698u);    // down-left
    CHECK(at(160, 160) == 0x22a676a0u);   // down-right
    CHECK(at(105, 110) == 0xc2a676acu);   // inside the dead zone (half offsets 2 / 5): the centre cursor (control)
    tool->Release();
    std::printf(failures ? "FAILED (%d)\n" : "sc4_cityscroll ok\n", failures);
    return failures ? 1 : 0;
}
