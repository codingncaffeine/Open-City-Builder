// cSC4TipOfTheDayWinProc (tip-of-the-day.md): the presence scan runs once per run; the dialog is made once (a second
// call while it is up makes none); it starts at the tip after the last one shown (the second tip when none matches);
// Next / Previous wrap; closing stores the check box and the shown tip's instance in the preferences.
#include <cstdio>
#include <cstring>
#include <vector>

#include "fake_drawcontext.h"
#include "fake_winmgr.h"
#include "gz/WinBtn.h"
#include "gz/WinGen.h"
#include "sc4/TipOfTheDayWinProc.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
struct Host final : sc4::ITipOfTheDayHost {
    FakeMgr* mgr = nullptr;
    cIGZWin* parent = nullptr;
    std::vector<std::pair<cGZPersistResourceKey, std::uint32_t>> exemplars;   // key, exemplar type
    int walks = 0, typeReads = 0;
    std::uint8_t prefs[0x600] = {};
    gz::cGZWinBtn* box = nullptr;
    cIGZWin* DialogParent() override { return parent; }
    bool CreateWindowFromScript(const cGZPersistResourceKey&, cIGZWin* in, std::uint32_t id, std::uint32_t, void** out) override {
        auto* gen = new gz::cGZWinGen(mgr);
        gen->AddRef();
        gen->SetID(id);
        in->ChildAdd(gen);
        box = new gz::cGZWinBtn(mgr);
        box->SetID(sc4::kTipCheckBox);
        gen->ChildAdd(box);
        *out = static_cast<gz::cIGZWinGen*>(gen);
        return true;
    }
    cIGZWinHTML* CreateHtmlControl() override { return nullptr; }
    void ForEachExemplarKey(const std::function<void(const cGZPersistResourceKey&)>& fn) override {
        ++walks;
        for (auto& e : exemplars) fn(e.first);
    }
    bool ExemplarType(const cGZPersistResourceKey& key, std::uint32_t& type) override {
        ++typeReads;
        for (auto& e : exemplars)
            if (e.first.instance == key.instance) { type = e.second; return true; }
        return false;
    }
    bool TipText(const cGZPersistResourceKey&, cRZString& out) override { out = cRZString("tip"); return true; }
    std::uint8_t* Preferences() override { return prefs; }
};

cGZMessage Command(std::uint32_t id) {
    cGZMessage m;
    m.type = kGZMsgCommand;
    m.param1 = 0x287259f6;
    m.param2 = id;
    return m;
}
}  // namespace

int main() {
    FakeWinGraphics graphics;
    SetWinGraphics(&graphics);
    FakeMgr mgr;
    cGZWin parent(&mgr);
    parent.SetArea(0, 0, 800, 600);
    Host host;
    host.mgr = &mgr;
    host.parent = &parent;
    for (std::uint32_t i = 1; i <= 5; ++i) host.exemplars.push_back({cGZPersistResourceKey{0x6534284a, 0x1000, i}, i == 3 ? 2u : 0x29u});
    sc4::SetTipOfTheDayHost(&host);
    using sc4::cSC4TipOfTheDayWinProc;

    // the presence scan: true, once per run (a second ask walks nothing), and it reads no more types after the first tip
    CHECK(!cSC4TipOfTheDayWinProc::HasBeenShownAlready());
    CHECK(cSC4TipOfTheDayWinProc::EntriesPresent());
    CHECK(host.walks == 1 && host.typeReads == 1);
    CHECK(cSC4TipOfTheDayWinProc::EntriesPresent() && host.walks == 1);

    // the dialog: made once; Init (0xa2bf8ad5) finds the four tips; no last tip matches: it starts at the second tip
    std::uint32_t last = 0x99;
    std::memcpy(host.prefs + sc4::kTipPrefLastTip, &last, 4);
    cSC4TipOfTheDayWinProc* proc = nullptr;
    CHECK(cSC4TipOfTheDayWinProc::DoModelessWin(&proc) && proc);
    CHECK(!cSC4TipOfTheDayWinProc::DoModelessWin(nullptr));   // already up
    if (proc) {
        // attaching the procedure makes the window send it the init message (0xa2bf8ad5): the dialog is set up
        CHECK(cSC4TipOfTheDayWinProc::HasBeenShownAlready());
        CHECK(proc->TipCount() == 4 && proc->TipIndex() == 1);
        // Previous wraps below the first, Next wraps past the last
        cGZMessage previous = Command(sc4::kTipPrevious);
        CHECK(proc->DoWinProcMessage(proc->Window(), previous) && proc->TipIndex() == 0);
        CHECK(proc->DoWinProcMessage(proc->Window(), previous) && proc->TipIndex() == 3);
        cGZMessage next = Command(sc4::kTipNext);
        CHECK(proc->DoWinProcMessage(proc->Window(), next) && proc->TipIndex() == 0);
        // another window's messages are not the dialog's
        CHECK(!proc->DoWinProcMessage(&parent, next) && proc->TipIndex() == 0);
        // close: the check box state and the shown tip's instance (1) go to the preferences; the window leaves its parent
        host.box->SetChecked(true);
        host.prefs[sc4::kTipPrefShowTips] = 0;
        cGZMessage close = Command(0xaa3be170);
        CHECK(proc->DoWinProcMessage(proc->Window(), close));
        std::memcpy(&last, host.prefs + sc4::kTipPrefLastTip, 4);
        CHECK(last == 1 && host.prefs[sc4::kTipPrefShowTips] == 1);
        CHECK(parent.GetChildWindowFromIDRecursive(sc4::kTipWindowId) == nullptr);
        proc->Release();
    }

    // a later dialog starts after the stored tip (instance 4 is index 2 -> index 3)
    last = 4;
    std::memcpy(host.prefs + sc4::kTipPrefLastTip, &last, 4);
    proc = nullptr;
    CHECK(cSC4TipOfTheDayWinProc::DoModelessWin(&proc) && proc);
    if (proc) {
        CHECK(proc->TipIndex() == 3);
        proc->EndDialog();
        proc->Release();
    }

    // no tips in the data: the scan answers false
    cSC4TipOfTheDayWinProc::ResetForTest();
    for (auto& e : host.exemplars) e.second = 2;
    CHECK(!cSC4TipOfTheDayWinProc::EntriesPresent());

    sc4::SetTipOfTheDayHost(nullptr);
    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("sc4_tipoftheday: all checks passed\n");
    return failures ? 1 : 0;
}
