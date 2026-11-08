// nSC4UI + cSC4NewsTickerWinProc: the window-set / alignment / focus / cursor helpers, the palette ramp, the
// property token replacement, the catalog item builder, the three script dialogs and the news ticker's window
// procedure (message handling, ranked advice list, flipper ring and body panel markup).
//
// The windows are real cGZWin / cGZWinGen / cGZWinText objects on the shared FakeMgr; the engine services the
// recovered code reaches through globals are replaced by the nSC4UI host (SC4UI.h), the language formatter and
// the message server (SL::spMS2).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "fake_winmgr.h"
#include "gz/WinGen.h"
#include "gz/WinText.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/Advice.h"
#include "sc4/CatalogItem.h"
#include "sc4/Exemplar.h"
#include "sc4/NewsTickerWinProc.h"
#include "sc4/SC4UI.h"
#include "sc4/SC4Utilities.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/ServiceLocator.h"
#include "sc4/WinCatalogView.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECK_EQ(a, b) do { if (!((a) == (b))) { std::printf("FAIL line %d: %s == %s\n", __LINE__, #a, #b); ++failures; } } while (0)

// ---------------------------------------------------------------------------------------------------------
// test doubles

namespace {

// ---- a window tree on FakeMgr -------------------------------------------------------------------------

cIGZWin* MakeWindow(FakeMgr& mgr, cIGZWin* parent, std::uint32_t id, int l = 0, int t = 0, int r = 10, int b = 10) {
    auto* window = new cGZWin(&mgr);
    mgr.valid.insert(window);
    window->SetID(id);
    window->SetArea(l, t, r, b);
    if (parent) parent->ChildAdd(window);
    return window;
}

cIGZWin* MakeGenWindow(FakeMgr& mgr, cIGZWin* parent, std::uint32_t id, int l = 0, int t = 0, int r = 10, int b = 10) {
    auto* window = new gz::cGZWinGen(&mgr);
    mgr.valid.insert(window);
    window->SetID(id);
    window->SetArea(l, t, r, b);
    // A dialog gen has no title bar, so Init does not need the graphics / control-factory services.
    window->SetWinGenFlag(0x1000, true);
    if (parent) parent->ChildAdd(window);
    return window;
}

// ---- the nSC4UI host ----------------------------------------------------------------------------------

// A text field that remembers the captions the dialog code sets, so the test can read them after the dialog
// window itself is gone.
struct RecordingText final : gz::cGZWinText {
    RecordingText(cIGZWinMgr* mgr, std::vector<std::string>* captions) : cGZWinText(mgr), captions_(captions) {}
    void SetCaption(const cIGZString& caption) override {
        captions_->emplace_back(caption.ToChar(), caption.Strlen());
        cGZWinText::SetCaption(caption);
    }
    std::vector<std::string>* captions_;
};

struct TestHost final : nSC4UI::INSC4UIHost {
    FakeMgr* mgr = nullptr;
    int pauses = 0;
    std::vector<cIGZWin*> created;
    std::vector<std::string> captions;
    bool advanced = false;

    bool PopupModalDialogsEnabled() override { return true; }
    bool LoadExemplarString(cIGZString&, sc4::cISCPropertyHolder*, std::uint32_t, sc4::cSCStringDetokenizer*) override {
        return false;   // no SC4LoadString: the builder falls back to the plain properties
    }
    void MakeNumberString(std::int64_t value, cIGZString& out) override {
        const std::string text = "num:" + std::to_string(value);
        out.AssignFromCString(text.c_str());
    }
    cIGZWinMgr* WindowManager() override { return mgr; }
    cIGZWin* CreateWindowFromScript(const cGZPersistResourceKey&, cIGZWin* parent, std::uint32_t id) override {
        auto* window = new gz::cGZWinGen(mgr);
        mgr->valid.insert(window);
        window->SetID(id);
        window->SetArea(0, 0, 120, 80);
        window->SetWinGenFlag(0x1000, true);   // dialog gen: no title bar, no graphics service
        if (parent) parent->ChildAdd(window);
        // the text fields the dialogs fill, and the two confirmation buttons' captions as text
        for (std::uint32_t textId : {0xca8cc4b6u, 0x4a8cc4a9u, 0x8a91e4fdu, 0x6a8cc495u}) {
            auto* text = new RecordingText(mgr, &captions);
            mgr->valid.insert(text);
            text->SetID(textId);
            text->SetArea(0, 0, 100, 14);
            window->ChildAdd(text);
        }
        created.push_back(window);
        window->AddRef();   // the script service hands over one reference (the dialog drops it at the end)
        return window;
    }
    bool CreateWindowFromScriptAs(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id,
                                  std::uint32_t iid, void** out) override {
        *out = CreateWindowFromScript(key, parent, id);
        (void)iid;
        return *out != nullptr;
    }
    cSLWinLocationSaver* WindowLocationSaver() override { return nullptr; }
    void BeginGamePause() override { ++pauses; }
    void EndGamePause() override { ++pauses; }
};

// ---- language formatting for the money tokens ---------------------------------------------------------

struct TestFormatter final : sc4::ILanguageFormatter {
    void FormatMoney(std::int64_t value, cIGZString& out, const cRZString&) override {
        out.AssignFromCString(("$" + std::to_string(value)).c_str());
    }
    void FormatMoney(double value, cIGZString& out, const cIGZString& format, const cRZString&) override {
        char buffer[64];
        std::snprintf(buffer, sizeof buffer, format.ToChar(), value);
        out.AssignFromCString(("$" + std::string(buffer)).c_str());
    }
    void FormatNumber(std::int64_t value, cRZString& out) override { out = cRZString(std::to_string(value).c_str()); }
};

// ---- the properties the token replacement reads -------------------------------------------------------

struct PropertyContext {
    sc4::cSCPropertyHolder holder;
};

// ---- a fake advice ------------------------------------------------------------------------------------

struct FakeAdvice final : sc4::cISC4Advice {
    int refs = 0;
    int mood = 3;
    int priority = 50;
    int advisorId = 0;
    std::uint32_t flags = sc4::kAdviceFlagCurrent;
    std::string title = "Title";
    std::string message = "Message";
    mutable cRZString titleText, messageText;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != sc4::kGZIID_cISC4Advice) return false;
        *out = static_cast<sc4::cISC4Advice*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }   // never deletes: the test owns it

    bool Init(int) override { return true; }
    bool Shutdown() override { return true; }
    bool Clone(sc4::cISC4Advice**) override { return false; }
    int GetMood() const override { return mood; }
    int GetPriority() const override { return priority; }
    bool GetTrigger() const override { return false; }
    cIGZString& GetTriggerString() override { return titleText; }
    int GetGUID() const override { return 0; }
    int GetAdvisorID() const override { return advisorId; }
    void SetAdvisorID(int id) override { advisorId = id; }
    int GetSessionID() const override { return 0; }
    bool SetFlags(int f, bool on) override {
        flags = on ? (flags | static_cast<std::uint32_t>(f)) : (flags & ~static_cast<std::uint32_t>(f));
        return true;
    }
    bool CheckFlags(int f) const override { return (flags & static_cast<std::uint32_t>(f)) != 0; }
    int GetFrequencyInDays() const override { return 0; }
    int GetTimeOutInDays() const override { return 0; }
    const cIGZString& GetTextTitle() const override {
        titleText = cRZString(title.c_str());
        return titleText;
    }
    std::uint32_t GetTextTitleGUID() const override { return 0; }
    const cIGZString& GetTextMessage() const override {
        messageText = cRZString(message.c_str());
        return messageText;
    }
    std::uint32_t GetTextMessageGUID() const override { return 0; }
    bool DoLink(const cIGZString&) override { return false; }
    void NotifyGlobalEffectTriggerChanged() override {}
    bool SetEventSubject(sc4::cISC4AdviceSubject*) override { return false; }
    sc4::cISC4AdviceSubject* GetEventSubject() override { return nullptr; }
    int GetEventID() override { return 0; }
    int GetCommandID() override { return 0; }
    bool UpdateText() override { return true; }
    bool IsCurrent() const override { return CheckFlags(2); }
};

// A cSC4WinAdviceList stand-in: a child window that also answers the advice-list interface, recording calls.
struct FakeAdviceList final : cGZWin, sc4::IC4WinAdviceList {
    explicit FakeAdviceList(cIGZWinMgr* mgr) : cGZWin(mgr) {}
    int fakeRefs = 0;
    int clears = 0, backgrounds = 0, rebuilds = 0, gutterSets = 0, scrollbarOffs = 0;
    bool lastRebuild = false;
    std::uint32_t lastBackground = 0;
    struct Entry {
        std::string first, second, third, fourth, text;
        sc4::cISC4Advice* advice = nullptr;
        bool flag5 = false, flag8 = false;
        std::uint32_t image = 0;
    };
    std::vector<Entry> entries;

    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++fakeRefs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--fakeRefs); }
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == sc4::kGZIID_cISC4WinAdviceList) {
            *out = static_cast<sc4::IC4WinAdviceList*>(this);
            AddRef();
            return true;
        }
        return cGZWin::QueryInterface(iid, out);
    }
    cIGZWin* AsIGZWin() override { return this; }
    void AddEntry(const cIGZString& first, const cIGZString& second, const cIGZString& third, const cIGZString& fourth,
                  bool flag5, sc4::cISC4Advice* advice, const cIGZString& text, bool flag8,
                  std::uint32_t imageId) override {
        entries.push_back({std::string(first.ToChar(), first.Strlen()), std::string(second.ToChar(), second.Strlen()),
                           std::string(third.ToChar(), third.Strlen()), std::string(fourth.ToChar(), fourth.Strlen()),
                           std::string(text.ToChar(), text.Strlen()), advice, flag5, flag8, imageId});
    }
    void RebuildHTML(bool rebuild) override { ++rebuilds; lastRebuild = rebuild; }
    void SetbgImage(std::uint32_t imageId) override { ++backgrounds; lastBackground = imageId; }
    void ClearList() override { ++clears; entries.clear(); }
    void SetGutters(int, int, int, int) override { ++gutterSets; }
    void ScrollToAnchor(const cIGZString&) override {}
    void DisableVerticalScrollbar() override { ++scrollbarOffs; }
};

// ---- a fake exemplar for the catalog builder ----------------------------------------------------------

struct FakeExemplar final : sc4::cISCResExemplar {
    sc4::cSCPropertyHolder holder;
    cGZPersistResourceKey key{0x111, 0x222, 0x333};
    int refs = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool Write(cIGZOStream&) const override { return false; }
    bool Read(cIGZIStream&) override { return false; }
    std::uint32_t GetGZCLSID() const override { return 0; }
    sc4::cISCPropertyHolder* AsISCPropertyHolder() override { return &holder; }
    bool IsPropertyLocal(std::uint32_t) override { return false; }
    sc4::cISCResExemplarCohort* GetCohort() override { return nullptr; }
    void SetCohort(sc4::cISCResExemplarCohort*, bool) override {}
    bool GetKey(cGZPersistResourceKey& out) override { out = key; return true; }
    bool SetKey(const cGZPersistResourceKey& value) override { key = value; return true; }
    void CompactProperties() override {}
    void GenerateCachedMap() override {}
};

}  // namespace

// ---------------------------------------------------------------------------------------------------------

static void TestPalette() {
    // stops: position 0 -> black, 4 -> 0xffffffff, 8 -> 0xff0000ff
    const std::uint32_t ramp[] = {0, 0x00000000, 4, 0xffffffff, 8, 0xff0000ff};
    std::uint32_t out[10] = {0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef,
                             0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef};
    nSC4UI::InterpolateRampToPalette(out, 10, ramp, 6);
    // Exact values (the interpolation blends the ARGB channels pairwise with a 0x800080 rounding bias; the
    // factor is (position - start) * 256 / span).
    CHECK_EQ(out[0], 0x00000000u);
    CHECK_EQ(out[1], 0x40404040u);
    CHECK_EQ(out[2], 0x80808080u);
    CHECK_EQ(out[3], 0xbfbfbfbfu);
    CHECK_EQ(out[4], 0xffffffffu);   // on a stop the ramp takes that stop's colour
    CHECK_EQ(out[5], 0xffbfbfffu);
    CHECK_EQ(out[6], 0xff8080ffu);
    CHECK_EQ(out[7], 0xff4040ffu);
    CHECK_EQ(out[8], 0xff0000ffu);
    CHECK_EQ(out[9], 0xff0000ffu);   // past the last stop

    // A ramp that starts later fills the leading entries with its first colour.
    const std::uint32_t late[] = {3, 0x11223344, 6, 0x55667788};
    std::uint32_t lateOut[5] = {0, 0, 0, 0, 0};
    nSC4UI::InterpolateRampToPalette(lateOut, 5, late, 4);
    CHECK_EQ(lateOut[0], 0x11223344u);
    CHECK_EQ(lateOut[1], 0x11223344u);
    CHECK_EQ(lateOut[2], 0x11223344u);
    CHECK_EQ(lateOut[3], 0x11223344u);
    CHECK_EQ(lateOut[4], 0x28394a5bu);   // one third of the way to the second stop

    // count 0 touches nothing; a single-stop ramp only fills.
    std::uint32_t untouched = 0xabcdef01;
    nSC4UI::InterpolateRampToPalette(&untouched, 0, ramp, 6);
    CHECK_EQ(untouched, 0xabcdef01u);
    const std::uint32_t single[] = {0, 0x00ff00ff};
    std::uint32_t singleOut[2] = {0, 0};
    nSC4UI::InterpolateRampToPalette(singleOut, 2, single, 2);
    CHECK_EQ(singleOut[0], 0x00ff00ffu);
    CHECK_EQ(singleOut[1], 0x00ff00ffu);
}

static void TestWindowSets() {
    FakeMgr mgr;
    auto* parent = new cGZWin(&mgr);
    mgr.valid.insert(parent);
    parent->SetArea(0, 0, 200, 100);
    auto* first = new cGZWin(&mgr);
    mgr.valid.insert(first);
    first->SetArea(10, 10, 40, 40);
    parent->ChildAdd(first);
    auto* second = new cGZWin(&mgr);
    mgr.valid.insert(second);
    second->SetArea(50, 10, 90, 40);
    parent->ChildAdd(second);

    std::set<cIGZWin*> children;
    CHECK_EQ(nSC4UI::GetWindowChildrenSet(parent, children), 2u);
    CHECK(children.count(first) && children.count(second));
    CHECK_EQ(nSC4UI::GetWindowChildrenSet(nullptr, children), 0u);   // the port's null guard

    std::set<cIGZWin*> excluded{first};
    std::set<cIGZWin*> difference;
    CHECK_EQ(nSC4UI::GetWindowChildrenNotInSet(parent, excluded, difference), 1u);
    CHECK(difference.count(second) && !difference.count(first));

    // ClearAcceptFocusFlagRecursive clears 0x8000 on the window and every descendant.
    first->SetFlag(0x8000, true);
    second->SetFlag(0x8000, true);
    parent->SetFlag(0x8000, true);
    auto* grandchild = new cGZWin(&mgr);
    mgr.valid.insert(grandchild);
    grandchild->SetFlag(0x8000, true);
    first->ChildAdd(grandchild);
    nSC4UI::ClearAcceptFocusFlagRecursive(parent);
    CHECK(!parent->GetFlag(0x8000) && !first->GetFlag(0x8000) && !second->GetFlag(0x8000) &&
          !grandchild->GetFlag(0x8000));
}

static void TestCursorHitTest() {
    FakeMgr mgr;
    auto* parent = new cGZWin(&mgr);
    mgr.valid.insert(parent);
    parent->SetArea(0, 0, 200, 100);
    auto* visible = new cGZWin(&mgr);
    mgr.valid.insert(visible);
    visible->SetID(1);
    visible->SetArea(10, 10, 60, 60);
    parent->ChildAdd(visible);
    auto* ignored = new cGZWin(&mgr);
    mgr.valid.insert(ignored);
    ignored->SetID(2);
    ignored->SetArea(20, 20, 80, 80);
    ignored->SetFlag(kWinIgnoreCursor, true);
    parent->ChildAdd(ignored);
    auto* hidden = new cGZWin(&mgr);
    mgr.valid.insert(hidden);
    hidden->SetID(3);
    hidden->SetArea(30, 30, 90, 90);
    hidden->SetFlag(kWinVisible, false);
    parent->ChildAdd(hidden);
    auto* nested = new cGZWin(&mgr);
    mgr.valid.insert(nested);
    nested->SetID(4);
    nested->SetArea(15, 15, 45, 45);
    visible->ChildAdd(nested);

    // The child list is front-to-back: the last added window is hit first. The hit test skips hidden windows
    // and the ones flagged 0x200000 and descends into the one it found.
    CHECK_EQ(nSC4UI::GetChildWindowForCursorPos(parent, 20, 20), visible);
    CHECK_EQ(nSC4UI::GetChildWindowForCursorPosRecursive(parent, 20, 20), visible);
    // The plain hit test answers the first-level child (the recursion inside the enum proc stops once a window
    // is found); the Recursive flavour repeats the search on it and reaches the nested one.
    CHECK_EQ(nSC4UI::GetChildWindowForCursorPos(parent, 30, 30), visible);
    CHECK_EQ(nSC4UI::GetChildWindowForCursorPosRecursive(parent, 30, 30), nested);
    CHECK_EQ(nSC4UI::GetChildWindowForCursorPos(parent, 150, 150), nullptr);
    CHECK_EQ(nSC4UI::GetChildWindowForCursorPosRecursive(parent, 150, 150), nullptr);
}

static void TestAlignment() {
    FakeMgr mgr;
    auto* main = MakeGenWindow(mgr, nullptr, 0x9000, 0, 0, 200, 100);
    mgr.mainWin = main;
    TestHost host;
    host.mgr = &mgr;
    nSC4UI::SetHost(&host);
    auto* window = MakeWindow(mgr, main, 0x10, 10, 20, 110, 120);
    auto* anchor = MakeWindow(mgr, window, 0xaaaa, 5, 5, 35, 15);
    anchor->SetCaption(cRZString("-tc"));
    main->CalcAbsoluteArea();

    // "-tc": the anchor child ends up centred horizontally at the top of the main window.
    CHECK(nSC4UI::AlignWindowToWindow(0x10, 0xaaaa, 0, main));
    cRZRect anchorArea;
    anchor->GetAreaAbsolute(anchorArea);
    CHECK_EQ(anchorArea.left, (200 - 30) / 2);
    CHECK_EQ(anchorArea.top, 0);

    // A caption starting with '-' whose letters are not tmb/lcr answers true and moves nothing (quirk kept).
    cRZRect before;
    anchor->GetAreaAbsolute(before);
    anchor->SetCaption(cRZString("-xz"));
    CHECK(nSC4UI::AlignWindowToWindow(0x10, 0xaaaa, 0, main));
    cRZRect after;
    anchor->GetAreaAbsolute(after);
    CHECK_EQ(before.left, after.left);
    CHECK_EQ(before.top, after.top);

    // "-br": bottom-right corner.
    anchor->SetCaption(cRZString("-br"));
    CHECK(nSC4UI::AlignWindowToWindow(0x10, 0xaaaa, 0, main));
    anchor->GetAreaAbsolute(after);
    CHECK_EQ(after.left, 200 - 30);
    CHECK_EQ(after.top, 100 - 10);

    // A hex id caption aligns the anchor onto that window's origin.
    auto* target = MakeWindow(mgr, main, 0x42, 120, 70, 160, 90);
    main->CalcAbsoluteArea();
    anchor->SetCaption(cRZString("00000042"));
    cRZRect anchorBefore, targetArea;
    anchor->GetAreaAbsolute(anchorBefore);
    target->GetAreaAbsolute(targetArea);
    CHECK(nSC4UI::AlignWindowToWindow(0x10, 0xaaaa, 0, main));
    anchor->GetAreaAbsolute(after);
    // The anchor lands on the reference window's origin (the offset is the origin difference).
    CHECK_EQ(after.left, targetArea.left);
    CHECK_EQ(after.top, targetArea.top);
    CHECK(after.left != anchorBefore.left || after.top != anchorBefore.top);

    // The explicit three-window overload offsets by the difference of the origins.
    auto* moved = MakeWindow(mgr, main, 0x77, 10, 10, 20, 20);
    auto* alignTo = MakeWindow(mgr, main, 0x78, 30, 30, 40, 40);
    auto* reference = MakeWindow(mgr, main, 0x79, 80, 60, 90, 70);
    main->CalcAbsoluteArea();
    cRZRect movedBefore;
    moved->GetAreaAbsolute(movedBefore);
    CHECK(nSC4UI::AlignWindowToWindow(moved, alignTo, reference));
    cRZRect movedAfter;
    moved->GetAreaAbsolute(movedAfter);
    CHECK_EQ(movedAfter.left, movedBefore.left + (80 - 30));
    CHECK_EQ(movedAfter.top, movedBefore.top + (60 - 30));

    // A missing window id answers false.
    CHECK(!nSC4UI::AlignWindowToWindow(0xdead, 0xaaaa, 0, main));

    // AlignWindowSetToWindow drives every window through its 0xaaaa anchor (all "-tc" here).
    auto* second = MakeWindow(mgr, main, 0x11, 0, 0, 50, 50);
    auto* secondAnchor = MakeWindow(mgr, second, 0xaaaa, 0, 0, 20, 5);
    secondAnchor->SetCaption(cRZString("-tc"));
    main->CalcAbsoluteArea();
    std::set<cIGZWin*> windows{window, second};
    CHECK(nSC4UI::AlignWindowSetToWindow(windows, main));
    cRZRect secondArea;
    secondAnchor->GetAreaAbsolute(secondArea);
    CHECK_EQ(secondArea.left, (200 - 20) / 2);
    CHECK_EQ(secondArea.top, 0);

    // Passing a null parent uses the host's main window.
    CHECK(nSC4UI::AlignWindowToWindow(0x11, 0xaaaa, 0, nullptr));
    nSC4UI::SetHost(nullptr);
}

static void TestPropertyTokens() {
    TestHost host;
    nSC4UI::SetHost(&host);
    TestFormatter formatter;
    sc4::SetLanguageFormatter(&formatter);

    PropertyContext context;
    sc4::cSCPropertyHelp::SetPropertyValue(&context.holder, 0x2a, 1.5f);
    sc4::cSCPropertyHelp::SetPropertyValue(&context.holder, 0x2b, static_cast<std::int32_t>(-42));
    sc4::cSCPropertyHelp::SetPropertyValue(&context.holder, 0x2c, 12.25f);

    cRZString out;
    CHECK(nSC4UI::PropertyTokenReplacementFunction(cRZString("D:0000002a"), out, &context.holder));
    CHECK_EQ(std::string(out.ToChar()), "1.50");
    CHECK(nSC4UI::PropertyTokenReplacementFunction(cRZString("d:0000002b"), out, &context.holder));
    CHECK_EQ(std::string(out.ToChar()), "num:-42");
    CHECK(nSC4UI::PropertyTokenReplacementFunction(cRZString("M:0000002b"), out, &context.holder));
    CHECK_EQ(std::string(out.ToChar()), "$-42");
    CHECK(nSC4UI::PropertyTokenReplacementFunction(cRZString("m:0000002c"), out, &context.holder));
    CHECK_EQ(std::string(out.ToChar()), "$12.25");

    // Wrong length, missing colon, unknown kind and a missing property are all rejected.
    CHECK(!nSC4UI::PropertyTokenReplacementFunction(cRZString("D:0000002"), out, &context.holder));
    CHECK(!nSC4UI::PropertyTokenReplacementFunction(cRZString("Dx0000002a"), out, &context.holder));
    CHECK(!nSC4UI::PropertyTokenReplacementFunction(cRZString("X:0000002a"), out, &context.holder));
    CHECK(!nSC4UI::PropertyTokenReplacementFunction(cRZString("D:00000099"), out, &context.holder));
    nSC4UI::SetHost(nullptr);
    sc4::SetLanguageFormatter(nullptr);
}

static void TestCatalog() {
    FakeExemplar exemplar;
    sc4::cSCPropertyHelp::SetPropertyValue(&exemplar.holder, 0x899afbad, cRZString("Road"));
    sc4::cSCPropertyHelp::SetPropertyValue(&exemplar.holder, 0x8a2602a9, cRZString("A road"));
    sc4::cSCPropertyHelp::SetPropertyValue(&exemplar.holder, 0x8a2602b8, 0x144161ecu);   // icon instance
    sc4::cSCPropertyHelp::SetPropertyValue(&exemplar.holder, 0x8a2602bb, 0x1234u);       // item id
    auto* detokenizer = new sc4::cSCStringDetokenizer();
    detokenizer->AddRef();
    sc4::cSC4CatalogItem* item = nSC4UI::CreateCatalogItemFromExemplar(&exemplar, nullptr, 7, detokenizer);
    CHECK(item != nullptr);
    if (item) {
        CHECK_EQ(std::string(item->GetName().ToChar()), "Road");
        CHECK_EQ(std::string(item->GetDescription().ToChar()), "A road");
        CHECK_EQ(item->GetItemType(), 7u);
        CHECK_EQ(item->GetItemID(), 0x1234u);
        CHECK_EQ(item->GetCost(), 0u);          // quirk: the builder always stores cost 0
        CHECK(item->GetEnabled());
        CHECK(item->GetIcon() == nullptr);      // no bitmap host installed
        CHECK_EQ(item->Release(), 0u);          // count 1 -> destroyed
    }
    // A missing name (both name properties absent) answers null.
    FakeExemplar empty;
    CHECK(nSC4UI::CreateCatalogItemFromExemplar(&empty, nullptr, 7, detokenizer) == nullptr);
    CHECK_EQ(detokenizer->Release(), 0u);

    // Without a resource manager the catalog view comes back empty.
    SL::spRM = nullptr;
    FakeMgr mgr;
    TestHost host;
    host.mgr = &mgr;
    nSC4UI::SetHost(&host);
    sc4::cSC4WinCatalogView* view = nSC4UI::CreateCatalogView(7, 9, 0, 0);
    CHECK(view != nullptr);
    if (view) {
        CHECK_EQ(view->GetItemCount(), 0);
        CHECK_EQ(view->Release(), 0u);
    }
    nSC4UI::SetHost(nullptr);
}

static void TestDialogs() {
    FakeMgr mgr;
    mgr.mainWin = MakeWindow(mgr, nullptr, 0x9001, 0, 0, 400, 300);
    TestHost host;
    host.mgr = &mgr;
    nSC4UI::SetHost(&host);

    mgr.popupAnswer = -1;
    nSC4UI::GenericNotificationDialog(cRZString("Hello"), cRZString("Body text"));
    CHECK_EQ(mgr.popups, 1);
    CHECK_EQ(host.pauses, 2);   // begin + end
    bool sawTitle = false, sawBody = false;
    for (const std::string& caption : host.captions) {
        if (caption == "Hello") sawTitle = true;
        if (caption == "Body text") sawBody = true;
    }
    CHECK(sawTitle && sawBody);

    mgr.popupAnswer = 0x8a8dfd4e;   // OK
    CHECK(nSC4UI::GenericConfirmationDialog(cRZString("Sure?"), cRZString("Really?"), nullptr, nullptr));
    mgr.popupAnswer = 0x8a8dfd4a;   // Cancel
    CHECK(!nSC4UI::GenericConfirmationDialog(cRZString("Sure?"), cRZString("Really?"), nullptr, nullptr));

    mgr.popupAnswer = 0x1234;
    CHECK_EQ(nSC4UI::DoScriptedModalDialog(0x2a5cfb2c, 0x98765432), 0x1234u);
    CHECK_EQ(mgr.popups, 4);
    nSC4UI::SetHost(nullptr);
}

// ---- the news ticker ----------------------------------------------------------------------------------

struct TickerTree {
    FakeMgr mgr;
    cIGZWin* main = nullptr;
    cIGZWin* parent = nullptr;
    gz::cGZWinGen* gen = nullptr;
    FakeAdviceList* body = nullptr;
    FakeAdviceList* flipper = nullptr;
    cIGZWin* host = nullptr;
    cIGZWin* label = nullptr;
    cIGZWin* tickerWindow = nullptr;

    TickerTree() {
        main = MakeWindow(mgr, nullptr, 0x9000, 0, 0, 400, 300);
        mgr.mainWin = main;
        parent = MakeWindow(mgr, main, 0x200, 0, 0, 400, 300);
        gen = static_cast<gz::cGZWinGen*>(MakeGenWindow(mgr, parent, sc4::kNewsTickerWindowId, 0, 0, 400, 40));
        tickerWindow = gen;
        for (std::uint32_t id : {sc4::kNewsTickerButtonToggle, sc4::kNewsTickerButtonToggleAlt,
                                 sc4::kNewsTickerButtonStop}) {
            MakeWindow(mgr, parent, id, 0, 0, 20, 20);
        }
        host = MakeWindow(mgr, parent, sc4::kNewsTickerControlHost, 0, 40, 400, 120);
        label = MakeWindow(mgr, parent, sc4::kNewsTickerResizeLabel, 0, 100, 200, 140);
        body = new FakeAdviceList(&mgr);
        mgr.valid.insert(body);
        body->SetID(sc4::kNewsTickerAdviceListBody);
        body->SetArea(0, 40, 400, 100);
        host->ChildAdd(body);
        flipper = new FakeAdviceList(&mgr);
        mgr.valid.insert(flipper);
        flipper->SetID(sc4::kNewsTickerAdviceListFlipper);
        flipper->SetArea(0, 0, 400, 20);
        gen->ChildAdd(flipper);   // the flipper line lives in the ticker's own window (Mac i386 Init), not in the host
    }
};

static void TestTicker() {
    TickerTree tree;
    auto* proc = new sc4::cSC4NewsTickerWinProc();
    proc->AddRef();

    // QueryInterface accepts only the message target and the window procedure; each answer holds a reference.
    void* out = nullptr;
    CHECK(proc->QueryInterface(sc4::kNewsTickerIIDMessageTarget2, &out));
    CHECK(out == static_cast<cIGZMessageTarget2*>(proc));
    static_cast<cIGZMessageTarget2*>(out)->Release();
    CHECK(proc->QueryInterface(gz::kGZIID_cIGZWinProc, &out));
    CHECK(out == static_cast<gz::cIGZWinProc*>(proc));
    static_cast<gz::cIGZWinProc*>(out)->Release();
    CHECK(!proc->QueryInterface(1, &out));

    CHECK(proc->Init(tree.gen));
    CHECK(proc->Initialised());
    CHECK(proc->AdviceList() == tree.body && proc->FlipperList() == tree.flipper);
    CHECK_EQ(tree.flipper->scrollbarOffs, 1);
    CHECK_EQ(tree.flipper->gutterSets, 1);
    CHECK_EQ(proc->LineHeight(), 0);   // no window colour manager in the harness

    // HideTicker is called by Init; the ticker child is hidden (flag 1, Mac i386 Show/HideTicker).
    CHECK(!tree.tickerWindow->GetFlag(kWinVisible));
    proc->ShowTicker();
    CHECK(tree.tickerWindow->GetFlag(kWinVisible));
    // The control only turns visible once the ticker is enabled (the proc's +0x81 flag, still false here).
    CHECK(!tree.host->GetFlag(kWinVisible));
    proc->HideTicker();
    CHECK(!tree.tickerWindow->GetFlag(kWinVisible));
    CHECK(!tree.host->GetFlag(kWinVisible));

    // The ranked list: high priority first, duplicates ignored.
    FakeAdvice low, high, middle;
    low.priority = 30;
    high.priority = 70;
    middle.priority = 50;
    proc->AddMessage(&middle);
    proc->AddMessage(&low);
    proc->AddMessage(&high);
    proc->AddMessage(&high);
    CHECK_EQ(proc->Messages().size(), 3u);
    auto it = proc->Messages().begin();
    CHECK_EQ(it->get(), &high);
    ++it;
    CHECK_EQ(it->get(), &middle);
    ++it;
    CHECK_EQ(it->get(), &low);

    // RemoveMessage drops just that advice; RemoveInactive drops the non-current ones.
    proc->RemoveMessage(&middle);
    CHECK_EQ(proc->Messages().size(), 2u);
    low.flags = 0;   // not current
    proc->RemoveInactive();
    CHECK_EQ(proc->Messages().size(), 1u);
    CHECK_EQ(proc->Messages().front().get(), &high);

    // updateBodyTextCtrl fills the advice panel from the header / font / footer fragments.
    high.mood = 3;
    high.title = "Busy";
    high.message = "The city is busy";
    tree.body->entries.clear();
    proc->updateBodyTextCtrl();
    CHECK_EQ(tree.body->entries.size(), 1u);
    if (!tree.body->entries.empty()) {
        const auto& entry = tree.body->entries.front();
        CHECK_EQ(entry.third, std::string("<FONT COLOR=\"#3f4967\" FACE=\"Arta\" SIZE=3><I>") +
                                  "The city is busy" + "</I></FONT>");
        CHECK_EQ(entry.fourth, std::string("<FONT COLOR=\"#3f4967\" FACE=\"StocletITC TT\" SIZE=3><B>") +
                                   "Busy" + "</B></FONT>");
        CHECK_EQ(entry.advice, &high);
        CHECK_EQ(entry.image, sc4::kNewsTickerImageBody);
        CHECK(!entry.flag5);
        CHECK(!entry.flag8);
    }
    CHECK_EQ(tree.body->rebuilds, 1);
    CHECK_EQ(tree.body->lastBackground, sc4::kNewsTickerImageBody);

    // The expired colours are used for an advice that is no longer current.
    high.flags = 0;
    proc->updateBodyTextCtrl();
    if (!tree.body->entries.empty()) {
        CHECK_EQ(tree.body->entries.front().fourth,
                 std::string("<FONT COLOR=\"#8e9499\" FACE=\"StocletITC TT\" SIZE=3><B>") + "Busy" + "</B></FONT>");
    }
    high.flags = sc4::kAdviceFlagCurrent;

    // The flipper: one line per ring entry, plus the two empty ones.
    tree.flipper->entries.clear();
    proc->DoFlipperMessage();
    CHECK_EQ(tree.flipper->entries.size(), 3u);
    if (!tree.flipper->entries.empty()) {
        CHECK(tree.flipper->entries[0].third.find("Busy") != std::string::npos);
        CHECK_EQ(tree.flipper->entries[0].advice, &high);
        CHECK_EQ(tree.flipper->entries[0].image, sc4::kNewsTickerImageFlipper);
        // The empty lines use the low-priority header and a single space instead of the title.
        CHECK(tree.flipper->entries[1].third.find("<FONT COLOR=\"#3f4967\" FACE=\"StocletITC TT\" SIZE=3><B> ") !=
              std::string::npos);
        CHECK(tree.flipper->entries[1].advice == nullptr);
    }
    CHECK_EQ(proc->RingCount(), 1u);
    CHECK_EQ(proc->FlipperScroll(), 0u);   // the first call only appends
    CHECK_EQ(tree.flipper->rebuilds, 0);   // rebuildPending was 0 before the call

    // A flipper tick first moves the list back to the full area and rebuilds it.
    tree.flipper->entries.clear();
    cRZMessage2Standard tick;
    tick.SetType(sc4::kNewsTickerFlipperTick);
    proc->DoMessage(&tick);
    CHECK_EQ(tree.flipper->entries.size(), 3u);
    CHECK_EQ(proc->FlipperScroll(), 0u);   // the line height is 0 in the harness, so the scroll stays at 0
    CHECK_EQ(tree.flipper->rebuilds, 1);

    // Advice-added: the new advice is ranked and the panel rebuilt.
    FakeAdvice added;
    added.mood = 5;
    added.priority = 90;
    added.title = "New";
    cRZMessage2Standard message;
    message.SetType(sc4::kNewsTickerAdviceAdded);
    message.SetIGZUnknown(&added);
    tree.body->entries.clear();
    proc->DoMessage(&message);
    CHECK_EQ(proc->Messages().size(), 2u);
    CHECK_EQ(proc->Messages().front().get(), &added);
    CHECK_EQ(tree.body->entries.size(), 2u);

    // Shutdown releases the lists and detaches from the window.
    proc->Shutdown();
    CHECK(!proc->Initialised());
    CHECK(proc->AdviceList() == nullptr && proc->FlipperList() == nullptr);
    CHECK(proc->Window() == nullptr);
    CHECK(proc->Release() == 0u);   // count 1 -> destroyed
}

int main() {
    FakeWinGraphics graphics;
    SetWinGraphics(&graphics);
    TestPalette();
    TestWindowSets();
    TestCursorHitTest();
    TestAlignment();
    TestPropertyTokens();
    TestCatalog();
    TestDialogs();
    TestTicker();
    SetWinGraphics(nullptr);
    if (failures == 0) std::printf("sc4_ui_helpers: ok\n");
    else std::printf("sc4_ui_helpers: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
