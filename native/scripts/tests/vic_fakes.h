// Shared test doubles for the cSC4ViewInputControl* tests: cursor manager, window manager, a window.
#pragma once
#include <cstdint>

#include "gz/Win.h"
#include "gz/WinInterfaces.h"
#include "sc4/ViewInputControl.h"

namespace vic_fakes {
struct Cursor final : cIGZCursor {
    int refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
};
struct CursorMgr final : cIGZCursorManager {
    Cursor cursors[4];
    cIGZCursor* LoadCursor(std::uint32_t id) override { return id < 4 ? &cursors[id] : nullptr; }
    void SetCursor(cIGZCursor*) override {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
};
struct Mgr final : cIGZWinMgr {
    CursorMgr cursors; cIGZWin *capture = nullptr, *setCapture = nullptr, *released = nullptr; int refs = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    cIGZWin* GetMainWindow() override { return nullptr; }
    void AppendToPlotStrategy(cIGZWin*) override {}
    void Plot() override {}
    void PostMessageToTarget(cIGZWin*, const cGZMessage&) override {}
    void AddWindowToValidList(cIGZWin*) override {}
    void RemoveWindowFromValidList(cIGZWin*) override {}
    void CleanUpWindowReferences(cIGZWin*) override {}
    bool DestroyWindow(cIGZWin* w) override {   // a window with a parent leaves it (else ChildDeleteAll never empties its list)
        if (w && w->GetParentWin()) return w->GetParentWin()->ChildRemove(w);
        return true;
    }
    bool IsWindowValid(cIGZWin*) override { return true; }
    cIGZWin* GetWindowFromPoint(int, int) override { return nullptr; }
    void SetCurrentMouseWin(cIGZWin*) override {}
    cIGZWin* GZGetCapture() override { return capture; }
    void GZSetCapture(cIGZWin* w) override { setCapture = capture = w; }
    void GZReleaseCapture(cIGZWin* w) override { released = w; capture = nullptr; }
    cIGZCursorManager* GetCursorManager() override { return &cursors; }
    cIGZWin* GZGetFocus() override { return nullptr; }
    void GZSetFocus(cIGZWin*) override {}
    cIGZKeyboard* GetKeyboard() override { return nullptr; }
};

using namespace sc4;
struct CursorWin final : cGZWin {
    using cGZWin::cGZWin;
    cIGZCursor* seen = nullptr; int calls = 0;
    void SetCursor(cIGZCursor* c, bool) override { seen = c; ++calls; cGZWin::SetCursor(c, false); }
};
}
