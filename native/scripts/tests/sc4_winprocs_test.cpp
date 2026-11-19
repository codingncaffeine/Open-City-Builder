#include <cstdio>
#include <vector>

#include "fake_winmgr.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/WinProcs.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

struct Director final : IWinProcDirector {
    int adds = 0, releases = 0;
    void AddRef() override { ++adds; }
    void OnProcReleased() override { ++releases; }
};
struct Legend final : ILegendView {
    FakeMgr* mgr;
    cIGZWin *panel = nullptr, *minimized = nullptr;
    int updates = 0;
    bool minimizedState = false;
    std::vector<std::pair<std::uint32_t, bool>> toggles;
    cIGZWin* LegendPanel() override { return panel; }
    cIGZWin* LegendMinimizedPanel() override { return minimized; }
    void UpdatePaths() override { ++updates; }
    void SetLegendMinimized(bool m) override { minimizedState = m; }
    void SetButtonToggleState(cIGZWin*, std::uint32_t id, bool on) override { toggles.push_back({id, on}); }
};
struct Compass final : ICompassHost {
    int rotation = 2;
    std::vector<std::uint32_t> added, removed;
    int CameraRotation() override { return rotation; }
    void AddNotification(cIGZMessageTarget2*, std::uint32_t m) override { added.push_back(m); }
    void RemoveNotification(cIGZMessageTarget2*, std::uint32_t m) override { removed.push_back(m); }
};

static cGZWin* NewWin(cIGZWinMgr* mgr) {
    auto* w = new cGZWin(mgr);
    w->AddRef();
    return w;
}

int main() {
    FakeMgr mgr;
    FakeWinGraphics graphics;
    SetWinGraphics(&graphics);

    // dummy
    Director director;
    SetWinProcDirector(&director);
    auto* dummy = new cGZDummyWinProc();
    dummy->AddRef();
    cGZMessage none{};
    CHECK(!dummy->DoWinProcMessage(nullptr, none));
    CHECK(dummy->DoWinMsg(nullptr, 0xa2bf8ad0, 0, 0, 0) && dummy->DoWinMsg(nullptr, 0xa2bf8ad1, 0, 0, 0) &&
          !dummy->DoWinMsg(nullptr, 0xa2bf8ad2, 0, 0, 0) && !dummy->DoWinMsg(nullptr, 0x1234, 0, 0, 0));
    void* out = nullptr;
    CHECK(dummy->QueryInterface(0x22e85d8e, &out) && !dummy->QueryInterface(3, &out));
    dummy->Release();
    dummy->Release();
    CHECK(director.adds == 2 && director.releases == 2);
    SetWinProcDirector(nullptr);

    // generic dialog
    cGZWin& dialog = *NewWin(&mgr);
    cSC4GenericDialogWinProc generic;
    generic.AddRef();
    cGZMessage command{kGZMsgCommand, gz::kWinCommandSender, 77, 0};
    CHECK(generic.DoWinProcMessage(&dialog, command) && mgr.dialogs.size() == 1 && mgr.dialogs[0].result == 77 && mgr.dialogs[0].code == 0xf);
    cGZMessage wrong{kGZMsgCommand, 5, 77, 0};
    CHECK(!generic.DoWinProcMessage(&dialog, wrong) && mgr.dialogs.size() == 1);

    // legend
    Legend view;
    view.mgr = &mgr;
    cGZWin &panel = *NewWin(&mgr), &small = *NewWin(&mgr);
    view.panel = &panel;
    view.minimized = &small;
    cSC4LegendDialogWinProc legend(&view);
    legend.AddRef();
    cGZWin& legendWindow = *NewWin(&mgr);
    legendWindow.SetID(0xbb0f5e7);
    cGZMessage init{gz::kWinProcInit, 0, 0, 0};
    CHECK(!legend.DoWinProcMessage(&legendWindow, init) && view.toggles.size() == 2 && view.toggles[0].second && !view.toggles[1].second);
    view.toggles.clear();
    auto press = [&](std::uint64_t id) {
        cGZMessage m{kGZMsgCommand, gz::kWinCommandSender, id, 0};
        legend.DoWinProcMessage(&legendWindow, m);
    };
    press(0x4bb0f628);
    CHECK(view.updates == 1 && view.toggles.empty());
    press(0xebb0f666);
    CHECK(view.updates == 2 && view.toggles.size() == 2 && !view.toggles[0].second && view.toggles[1].second);
    press(0x1234);
    CHECK(view.updates == 2);
    press(0xebb91356);
    CHECK(view.minimizedState && view.updates == 2);
    small.ShowWindow();
    panel.HideWindow();
    press(0x4bb92c1f);
    CHECK(panel.IsVisible() && !small.IsVisible() && view.updates == 2);

    // compass: faces are children id+1.. ; rotation 2 -> only the third face is visible
    Compass host;
    cGZWin& compassWindow = *NewWin(&mgr);
    compassWindow.SetID(100);
    cGZWin &face1 = *NewWin(&mgr), &face2 = *NewWin(&mgr), &face3 = *NewWin(&mgr);
    face1.SetID(101); face2.SetID(102); face3.SetID(103);
    compassWindow.ChildAdd(&face1); compassWindow.ChildAdd(&face2); compassWindow.ChildAdd(&face3);
    auto* proc = new cSC4CompassWinProc(&host);
    proc->AddRef();
    cGZMessage start{gz::kWinProcInit, 0, 0, 0};
    CHECK(proc->DoWinProcMessage(&compassWindow, start) && host.added.size() == 2);
    proc->UpdateRotation();
    CHECK(!face1.IsVisible() && !face2.IsVisible() && face3.IsVisible());
    host.rotation = 0;
    int before[2] = {0, 5}, after[2] = {0, 6};
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(kMessageCameraViewChanged);
    message->SetVoid1(before);
    message->SetVoid2(after);
    CHECK(proc->DoMessage(message) && face1.IsVisible() && !face3.IsVisible());
    host.rotation = 1;
    after[1] = 5;                                   // unchanged value: no refresh
    CHECK(proc->DoMessage(message) && face1.IsVisible());
    message->SetType(kMessageCameraRotated);
    CHECK(proc->DoMessage(message) && face2.IsVisible() && !face1.IsVisible());
    message->SetType(7);
    CHECK(!proc->DoMessage(message));
    cGZMessage stop{gz::kWinProcShutdown, 0, 0, 0};
    CHECK(proc->DoWinProcMessage(&compassWindow, stop) && host.removed.size() == 2);
    CHECK(proc->QueryInterface(0x452294aa, &out) && proc->QueryInterface(0x22e85d8e, &out) && !proc->QueryInterface(9, &out));
    message->Release();
    proc->Release(); proc->Release(); proc->Release();   // the count of 1 plus the two successful queries
    for (cGZWin* w : {&dialog, &panel, &small, &legendWindow, &face1, &face2, &face3}) w->Release();
    compassWindow.Release();
    std::printf(failures ? "FAILED\n" : "OK\n");
    return failures != 0;
}
