// cSC4WinAdviceList: the advisor panel's advice list. Checked against the x86_64 disassembly of
// 0x1005c7918..0x1005c8b6c (otool -arch x86_64 -tV), not against the Ghidra decompiler.
//
// Covered: QueryInterface 0x1005c79e2 (iid 1 and 0xca1492a2 answer the advice-list view, 0x22ba0121 the primary,
// everything else false), Init 0x1005c7a54 (the html child created through GZCOM, given the window's area, one flag
// cleared, added as a child), Shutdown 0x1005c7b7a (clears the list then cGZWin::Shutdown), SetArea 0x1005c7b9a and
// SetGutters 0x1005c7c0c forwarding, AddEntry 0x1005c806e (all nine fields and the background image id, with the
// advice reference the copy constructor takes), the exact document RebuildHTML 0x1005c82a0 generates (both headers,
// the three cells per row, the disabled title, the extra line only when enabled, the no-link entry, the column width
// = window width - 0x3d) plus its Get/SetScrollPosition pair for force, ClearList 0x1005c8828, SetbgImage
// 0x1005c828a, ScrollToAnchor 0x1005c8848 and DisableVerticalScrollbar 0x1005c8870 forwarding, and DoMessage
// 0x1005c7c48 (the click sound, the toggle / close / foreign-URL branches, the messages they post, the advice flag
// bits, and the quirk that both handled links report false).
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "sc4/WinAdviceList.h"

#include "fake_winmgr.h"
#include "gz/cIGZMessage2.h"
#include "sc4/Advice.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {

// ---- fakes ---------------------------------------------------------------------------------------------------

struct FakeAdvice final : sc4::cISC4Advice {
    std::uint32_t refCount = 0;   // the test's own AddRef makes it 1
    int advisorId = 7;
    int mood = 3;
    std::vector<std::pair<int, bool>> flagCalls;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refCount; }
    std::uint32_t Release() override {
        if (refCount > 0) --refCount;
        return refCount;
    }
    bool Init(int) override { return true; }
    bool Shutdown() override { return true; }
    bool Clone(sc4::cISC4Advice**) override { return false; }
    int GetMood() const override { return mood; }
    int GetPriority() const override { return 50; }
    bool GetTrigger() const override { return false; }
    cIGZString& GetTriggerString() override { return triggerString; }
    int GetGUID() const override { return 0; }
    int GetAdvisorID() const override { return advisorId; }
    void SetAdvisorID(int id) override { advisorId = id; }
    int GetSessionID() const override { return 0; }
    bool SetFlags(int flags, bool on) override {
        flagCalls.emplace_back(flags, on);
        return true;
    }
    bool CheckFlags(int) const override { return false; }
    int GetFrequencyInDays() const override { return 30; }
    int GetTimeOutInDays() const override { return 30; }
    const cIGZString& GetTextTitle() const override { return triggerString; }
    std::uint32_t GetTextTitleGUID() const override { return 0; }
    const cIGZString& GetTextMessage() const override { return triggerString; }
    std::uint32_t GetTextMessageGUID() const override { return 0; }
    bool DoLink(const cIGZString&) override { return false; }
    void NotifyGlobalEffectTriggerChanged() override {}
    bool SetEventSubject(sc4::cISC4AdviceSubject*) override { return false; }
    sc4::cISC4AdviceSubject* GetEventSubject() override { return nullptr; }
    int GetEventID() override { return 0; }
    int GetCommandID() override { return 0; }
    bool UpdateText() override { return true; }
    bool IsCurrent() const override { return false; }
    cRZString triggerString;
};

struct FakeServer final : cIGZMessageServer2 {
    struct Sent {
        bool posted;   // true = MessagePost, false = MessageSend
        std::uint32_t type;
        std::uint64_t data1;
        std::uint64_t data2;
        cIGZUnknown* unknown;
    };
    std::vector<Sent> sent;
    std::uint32_t refCount = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refCount; }
    std::uint32_t Release() override { return refCount > 0 ? --refCount : 0; }
    bool MessageSend(cIGZMessage2* message) override {
        auto* standard = static_cast<cIGZMessage2Standard*>(message);
        sent.push_back({false, standard->GetType(), standard->GetData1(), standard->GetData2(),
                        standard->GetHasIGZUnknown() ? standard->GetIGZUnknown() : nullptr});
        return true;
    }
    bool MessagePost(cIGZMessage2* message, bool) override {
        auto* standard = static_cast<cIGZMessage2Standard*>(message);
        sent.push_back({true, standard->GetType(), standard->GetData1(), standard->GetData2(),
                        standard->GetHasIGZUnknown() ? standard->GetIGZUnknown() : nullptr});
        return true;
    }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t) override { return true; }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t) override { return true; }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 1; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

struct FakeHtml final : sc4::IAdviceListHtml {
    FakeMgr* mgr = nullptr;
    cGZWin* child = nullptr;
    int releases = 0;
    int width = 200;
    int areas = 0;
    int lastArea[4] = {0, 0, 0, 0};
    int gutters = 0;
    int lastGutters[4] = {0, 0, 0, 0};
    int clears = 0;
    int lastClear = 0;
    int scrollGets = 0;
    int scrollSets = 0;
    int scrollX = 12, scrollY = 34;
    int lastScroll[2] = {0, 0};
    std::string document;
    std::string baseUrl;
    std::string anchors;
    void Release() override { ++releases; }
    cIGZWin* AsIGZWin() override { return child; }
    int WindowWidth() override { return width; }
    void SetArea(int l, int t, int r, int b) override {
        ++areas;
        lastArea[0] = l; lastArea[1] = t; lastArea[2] = r; lastArea[3] = b;
    }
    void GetScrollPosition(int& x, int& y) override {
        ++scrollGets;
        x = scrollX;
        y = scrollY;
    }
    void SetScrollPosition(int x, int y) override {
        ++scrollSets;
        lastScroll[0] = x;
        lastScroll[1] = y;
    }
    bool ScrollToAnchor(const cIGZString& anchor) override {
        anchors = anchor.ToChar();
        return true;
    }
    bool SetDocument(const std::string& html, const cIGZString& base) override {
        document = html;
        baseUrl = base.ToChar();
        return true;
    }
    void ClearFlag(int flag) override {
        ++clears;
        lastClear = flag;
    }
    void SetGutters(int l, int t, int r, int b) override {
        ++gutters;
        lastGutters[0] = l; lastGutters[1] = t; lastGutters[2] = r; lastGutters[3] = b;
    }
};

struct FakeHost final : sc4::IWinAdviceListHost {
    FakeHtml html;
    FakeServer server;
    FakeMgr mgr;
    cGZWin* child = nullptr;   // the real window the fake html answers AsIGZWin with
    int created = 0;
    std::vector<std::uint32_t> sounds;
    std::string lastUrl;
    bool handleResult = false;
    int handled = 0;
    sc4::IAdviceListHtml* CreateHtmlWindow() override {
        ++created;
        child = new cGZWin(&mgr);
        html.child = child;
        html.mgr = &mgr;
        return &html;
    }
    cIGZMessageServer2* MessageServer() override { return &server; }
    void PlaySound(std::uint32_t id) override { sounds.push_back(id); }
    bool HandleUrl(const cIGZString& link) override {
        ++handled;
        lastUrl = link.ToChar();
        return handleResult;
    }
};

cGZMessage LinkMessage(const cRZString& url) {
    cGZMessage message;
    message.type = sc4::kAdviceListMsgLinkJump;
    message.param2 = reinterpret_cast<std::uint64_t>(&url);
    return message;
}

std::string Entry0() {
    return "<TR><TD WIDTH=\"18\"><A NAME=\"item0\" HREF=\"sc4://action/toggle?item=0\">TitleA</A>"
           "</TD><TD WIDTH=\"139\"><A HREF=\"sc4://action/toggle?item=0\"><SPAN STYLE=\"text-decoration: none\">"
           "ToggleA</SPAN></A><BR>DetailA</TD><TD WIDTH=\"18\"><A NAME=\"item0\" "
           "HREF=\"sc4://action/close?item=0\">CloseA</A></TD></TR>";
}
std::string Entry1() {
    return "<TR><TD WIDTH=\"18\"><A NAME=\"item1\" HREF=\"sc4://action/toggle?item=1\">TitleOffB</A>"
           "</TD><TD WIDTH=\"139\"><A HREF=\"sc4://action/toggle?item=1\"><SPAN STYLE=\"text-decoration: none\">"
           "ToggleB</SPAN></A></TD><TD WIDTH=\"18\"><A NAME=\"item1\" "
           "HREF=\"sc4://action/close?item=1\">CloseB</A></TD></TR>";
}

}  // namespace

int main() {
    // ---- QueryInterface: the list view for iid 1 / 0xca1492a2, the primary for 0x22ba0121 --------------------
    {
        FakeHost host;
        sc4::SetWinAdviceListHost(&host);
        auto* list = new sc4::cSC4WinAdviceList(&host.mgr);
        list->AddRef();
        void* out = nullptr;
        CHECK(list->QueryInterface(1, &out));
        CHECK(out == static_cast<sc4::cIGZWinAdviceList*>(list));
        out = nullptr;
        CHECK(list->QueryInterface(sc4::kGZIID_cIGZWinAdviceList, &out));
        CHECK(out == static_cast<sc4::cIGZWinAdviceList*>(list));
        out = nullptr;
        CHECK(list->QueryInterface(sc4::kGZIID_cSC4WinAdviceList, &out));
        CHECK(out == static_cast<cIGZWin*>(list));
        CHECK(!list->QueryInterface(0x12345678u, &out));
        // the advice-list view and AsIGZWin agree
        CHECK(static_cast<sc4::cIGZWinAdviceList*>(list)->AsIGZWin() == static_cast<cIGZWin*>(list));
        list->Release(); list->Release(); list->Release();   // the three successful queries
        list->Release();
        sc4::SetWinAdviceListHost(nullptr);
    }

    // ---- Init: the html child gets the window's area, one flag cleared, and is added as a child ---------------
    {
        FakeHost host;
        sc4::SetWinAdviceListHost(&host);
        auto* list = new sc4::cSC4WinAdviceList(&host.mgr);
        list->AddRef();
        list->SetArea(10, 20, 210, 140);   // 200 x 120
        CHECK(list->Init());
        CHECK(host.created == 1);
        CHECK(list->HtmlWindow() == &host.html);
        CHECK(host.html.areas == 1);
        CHECK(host.html.lastArea[0] == 0 && host.html.lastArea[1] == 0 && host.html.lastArea[2] == 200 &&
              host.html.lastArea[3] == 120);
        CHECK(host.html.clears == 1 && host.html.lastClear == sc4::kAdviceListHtmlFlag);
        CHECK(host.mgr.valid.count(host.child) == 1);   // ChildAdd put it in the window tree
        CHECK(host.child->GetParentWin() == list);
        // a second Init is a no-op
        CHECK(list->Init());
        CHECK(host.created == 1);
        CHECK(host.html.areas == 1);
        list->Release();
        sc4::SetWinAdviceListHost(nullptr);
    }

    // ---- SetArea / SetGutters / ScrollToAnchor / DisableVerticalScrollbar forwarding --------------------------
    {
        FakeHost host;
        sc4::SetWinAdviceListHost(&host);
        auto* list = new sc4::cSC4WinAdviceList(&host.mgr);
        list->AddRef();
        list->SetArea(0, 0, 50, 60);
        list->Init();
        host.html.areas = 0;
        list->SetArea(5, 6, 105, 86);   // 100 x 80
        CHECK(host.html.areas == 1 && host.html.lastArea[2] == 100 && host.html.lastArea[3] == 80);
        list->SetGutters(1, 2, 3, 4);
        CHECK(host.html.gutters == 1 && host.html.lastGutters[0] == 1 && host.html.lastGutters[3] == 4);
        cRZString anchor("item3");
        list->ScrollToAnchor(anchor);
        CHECK(host.html.anchors == "item3");
        list->DisableVerticalScrollbar();
        CHECK(host.html.lastClear == sc4::kAdviceListHtmlVScrollFlag);
        list->Release();
        sc4::SetWinAdviceListHost(nullptr);
    }

    // ---- AddEntry / ClearList / SetbgImage -------------------------------------------------------------------
    {
        FakeHost host;
        sc4::SetWinAdviceListHost(&host);
        auto* list = new sc4::cSC4WinAdviceList(&host.mgr);
        list->AddRef();
        FakeAdvice advice;
        advice.AddRef();   // owned by the test
        const cRZString a("A"), b("B"), c("C"), d("D"), e("E");
        list->AddEntry(a, b, c, d, true, &advice, e, false, 0x77);
        CHECK(list->Entries().size() == 1);
        CHECK(list->BackgroundImage() == 0x77);
        const sc4::cAdviceEntry& entry = list->Entries().front();
        CHECK(entry.a.ToChar() == std::string("A"));
        CHECK(entry.b.ToChar() == std::string("B"));
        CHECK(entry.c.ToChar() == std::string("C"));
        CHECK(entry.d.ToChar() == std::string("D"));
        CHECK(entry.e.ToChar() == std::string("E"));
        CHECK(entry.enabled && !entry.noLink);
        CHECK(entry.advice == &advice);
        CHECK(advice.refCount == 2);   // the entry took its own reference
        // the copy constructor bumps the count too
        {
            sc4::cAdviceEntry copy(list->Entries().front());
            CHECK(advice.refCount == 3);
            CHECK(copy.a.ToChar() == std::string("A") && copy.advice == &advice);
        }
        CHECK(advice.refCount == 2);
        list->SetbgImage(0x1234);
        CHECK(list->BackgroundImage() == 0x1234);
        list->ClearList();
        CHECK(list->Entries().empty());
        CHECK(advice.refCount == 1);   // the entry dropped its reference
        advice.Release();
        list->Release();
        sc4::SetWinAdviceListHost(nullptr);
    }

    // ---- RebuildHTML: the exact document, the fixed header, force's scroll position pair --------------------
    {
        FakeHost host;
        sc4::SetWinAdviceListHost(&host);
        auto* list = new sc4::cSC4WinAdviceList(&host.mgr);
        list->AddRef();
        list->SetArea(0, 0, 200, 120);
        list->Init();
        FakeAdvice advice;
        advice.AddRef();
        const cRZString a("TitleA"), b("TitleOffA"), c("DetailA"), d("ToggleA"), e("CloseA");
        const cRZString a2("TitleB"), b2("TitleOffB"), c2("DetailB"), d2("ToggleB"), e2("CloseB");
        list->AddEntry(a, b, c, d, true, &advice, e, false, 0);
        list->AddEntry(a2, b2, c2, d2, false, nullptr, e2, false, 0);

        list->RebuildHTML(false);
        CHECK(host.html.scrollGets == 0);   // no scroll read without force
        CHECK(host.html.scrollSets == 0);
        const std::string expected =
            "<HTML><HEAD></HEAD><BODY BGCOLOR=\"#dee8e3\"><FONT FACE=\"Arta\" SIZE=3><TABLE BORDER=0>" + Entry0() +
            Entry1() + "</TABLE></FONT></BODY></HTML>";
        if (host.html.document != expected) {
            std::printf("--- got:\n%s\n--- want:\n%s\n", host.html.document.c_str(), expected.c_str());
            CHECK(host.html.document == expected);
        }
        CHECK(host.html.baseUrl == "sc4://");

        // force reads the scroll position first and writes it back last
        list->RebuildHTML(true);
        CHECK(host.html.scrollGets == 1);
        CHECK(host.html.scrollSets == 1);
        CHECK(host.html.lastScroll[0] == 12 && host.html.lastScroll[1] == 34);
        CHECK(host.html.document == expected);

        // an image id switches the header and prints it as %x
        list->SetbgImage(0x2a);
        list->RebuildHTML(false);
        const std::string expectedImage =
            "<HTML><HEAD></HEAD><BODY BACKGROUND=\"sc4://HTML/46a006b0/2a\"><FONT FACE=\"Arta\" SIZE=3>"
            "<TABLE BORDER=0>" + Entry0() + Entry1() + "</TABLE></FONT></BODY></HTML>";
        CHECK(host.html.document == expectedImage);

        // a no-link entry has no anchors at all
        FakeAdvice advice2;
        advice2.AddRef();
        const cRZString plain("PlainTitle");
        list->ClearList();
        list->SetbgImage(0);
        list->AddEntry(plain, b, c, d, true, &advice2, e, true, 0);
        list->RebuildHTML(false);
        // noLink drops the title anchors and the toggle cell, but the close cell is still written
        const std::string expectedPlain =
            "<HTML><HEAD></HEAD><BODY BGCOLOR=\"#dee8e3\"><FONT FACE=\"Arta\" SIZE=3><TABLE BORDER=0>"
            "<TR><TD WIDTH=\"18\">PlainTitle</TD><TD WIDTH=\"139\"><BR>DetailA"
            "</TD><TD WIDTH=\"18\"><A NAME=\"item0\" HREF=\"sc4://action/close?item=0\">CloseA</A></TD></TR>"
            "</TABLE></FONT></BODY></HTML>";
        if (host.html.document != expectedPlain) {
            std::printf("--- got:\n%s\n--- want:\n%s\n", host.html.document.c_str(), expectedPlain.c_str());
            CHECK(host.html.document == expectedPlain);
        }
        advice.Release();
        advice2.Release();
        list->Release();
        sc4::SetWinAdviceListHost(nullptr);
    }

    // ---- DoMessage: the toggle branch ------------------------------------------------------------------------
    {
        FakeHost host;
        sc4::SetWinAdviceListHost(&host);
        auto* list = new sc4::cSC4WinAdviceList(&host.mgr);
        list->AddRef();
        list->SetArea(0, 0, 200, 120);
        list->Init();
        FakeAdvice advice;
        advice.AddRef();
        advice.advisorId = 41;
        advice.mood = 2;
        const cRZString a("TitleA"), b("TitleOffA"), c("DetailA"), d("ToggleA"), e("CloseA");
        list->AddEntry(a, b, c, d, false, &advice, e, false, 0);
        host.html.document.clear();

        cRZString url("sc4://action/toggle?item=0");
        cGZMessage message = LinkMessage(url);
        CHECK(!list->DoMessage(message));   // (sic) handled, but reports false
        CHECK(host.sounds.size() == 1 && host.sounds[0] == sc4::kAdviceListClickSound);
        CHECK(list->Entries().front().enabled);
        CHECK(advice.flagCalls.size() == 1 && advice.flagCalls[0].first == sc4::kAdviceListToggleAdviceFlag &&
              advice.flagCalls[0].second);
        CHECK(host.server.sent.size() == 1);
        if (host.server.sent.size() == 1) {
            const FakeServer::Sent& sent = host.server.sent[0];
            CHECK(sent.posted);   // MessagePost
            CHECK(sent.type == sc4::kAdviceListMsgAdviceToggled);
            CHECK(sent.data1 == 41);
            CHECK(sent.data2 == 2);
        }
        CHECK(!host.html.document.empty());   // RebuildHTML(true) ran
        // now it is enabled: the title switches to entry.a
        CHECK(host.html.document.find(">TitleA</A>") != std::string::npos);

        // a second toggle disables it again
        CHECK(!list->DoMessage(message));
        CHECK(!list->Entries().front().enabled);
        CHECK(advice.flagCalls.size() == 2 && !advice.flagCalls[1].second);

        // an index outside the list is refused (port guard; the original would walk off the list)
        cRZString bad("sc4://action/toggle?item=5");
        cGZMessage badMessage = LinkMessage(bad);
        CHECK(!list->DoMessage(badMessage));
        CHECK(advice.flagCalls.size() == 2);
        CHECK(host.server.sent.size() == 2);
        // a link without the sscanf-able tail is refused
        cRZString junk("sc4://action/toggle?nope");
        cGZMessage junkMessage = LinkMessage(junk);
        CHECK(!list->DoMessage(junkMessage));
        CHECK(advice.flagCalls.size() == 2);
        advice.Release();
        list->Release();
        sc4::SetWinAdviceListHost(nullptr);
    }

    // ---- DoMessage: the close branch -------------------------------------------------------------------------
    {
        FakeHost host;
        sc4::SetWinAdviceListHost(&host);
        auto* list = new sc4::cSC4WinAdviceList(&host.mgr);
        list->AddRef();
        list->SetArea(0, 0, 200, 120);
        list->Init();
        FakeAdvice advice;
        advice.AddRef();
        const cRZString a("TitleA"), b("TitleOffA"), c("DetailA"), d("ToggleA"), e("CloseA");
        list->AddEntry(a, b, c, d, true, &advice, e, false, 0);
        cRZString url("sc4://action/close?item=0");
        cGZMessage message = LinkMessage(url);
        CHECK(!list->DoMessage(message));
        CHECK(advice.flagCalls.size() == 1 && advice.flagCalls[0].first == sc4::kAdviceListNoLinksAdviceFlag &&
              advice.flagCalls[0].second);
        CHECK(host.server.sent.size() == 1);
        if (host.server.sent.size() == 1) {
            const FakeServer::Sent& sent = host.server.sent[0];
            CHECK(!sent.posted);   // MessageSend
            CHECK(sent.type == sc4::kAdviceListMsgAdviceRemoved);
            CHECK(sent.data1 == sc4::kAdviceListMsgAdviceRemoved);
            CHECK(sent.unknown == static_cast<cIGZUnknown*>(&advice));
        }
        advice.Release();
        list->Release();
        sc4::SetWinAdviceListHost(nullptr);
    }

    // ---- DoMessage: a foreign URL goes to cSC4HTMLLinkHandler::DoURL and the answer is inverted ---------------
    {
        FakeHost host;
        sc4::SetWinAdviceListHost(&host);
        auto* list = new sc4::cSC4WinAdviceList(&host.mgr);
        list->AddRef();
        list->SetArea(0, 0, 200, 120);
        list->Init();
        cRZString url("sc4://other/thing");
        cGZMessage message = LinkMessage(url);
        host.handleResult = true;
        CHECK(!list->DoMessage(message));   // DoURL true -> false
        CHECK(host.handled == 1 && host.lastUrl == "sc4://other/thing");
        CHECK(host.sounds.size() == 1);   // the sound plays for every link notification
        host.handleResult = false;
        CHECK(list->DoMessage(message));    // DoURL false -> true
        CHECK(host.handled == 2);
        CHECK(host.sounds.size() == 2);
        // a different message type goes to cGZWin::DoMessage
        cGZMessage other;
        other.type = 0x12345678u;
        CHECK(!list->DoMessage(other));
        CHECK(host.handled == 2);
        CHECK(host.sounds.size() == 2);
        list->Release();
        sc4::SetWinAdviceListHost(nullptr);
    }

    // ---- Shutdown clears the list and stops the window -------------------------------------------------------
    {
        FakeHost host;
        sc4::SetWinAdviceListHost(&host);
        auto* list = new sc4::cSC4WinAdviceList(&host.mgr);
        list->AddRef();
        list->SetArea(0, 0, 200, 120);
        list->Init();
        FakeAdvice advice;
        advice.AddRef();
        const cRZString a("A"), b("B"), c("C"), d("D"), e("E");
        list->AddEntry(a, b, c, d, true, &advice, e, false, 0);
        CHECK(list->Shutdown());
        CHECK(list->Entries().empty());
        CHECK(!list->get_init_flag());
        advice.Release();
        list->Release();
        sc4::SetWinAdviceListHost(nullptr);
    }

    if (failures == 0) std::printf("sc4_winadvicelist: ok\n");
    else std::printf("sc4_winadvicelist: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
