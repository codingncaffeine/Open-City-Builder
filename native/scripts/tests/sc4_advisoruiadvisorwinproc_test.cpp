// cSC4AdvisorUIAdvisorWinProc: the advisor window. Init builds the script window and attaches a head to every advisor
// picture; the head buttons open one advisor's page (caption, its advices in reverse map order, the list markup by mood);
// the size and back buttons switch layouts; the advisor-system messages keep the list and the moods current; Shutdown
// destroys the windows.
//
// The windows are real cGZWin / cGZWinGen objects on FakeMgr; the script "creates" a hand-built tree with the original
// ids; the advisors, advices and the advice lists are fakes; the heads are the real cSC4AdvisorUIImageWinProc with a
// host that loads no models.
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "fake_winmgr.h"
#include "gz/WinGen.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/Advice.h"
#include "sc4/Advisor.h"
#include "sc4/AdvisorUIAdvisorWinProc.h"
#include "sc4/AdvisorUIImageWinProc.h"
#include "sc4/DrawContext.h"
#include "sc4/SC4UI.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECK_EQ(a, b) do { if (!((a) == (b))) { std::printf("FAIL line %d: %s == %s\n", __LINE__, #a, #b); ++failures; } } while (0)

// The head's paint path (the host gives no renderer, so these never run).
void SC4DrawContext::SetCamera(s3d::cS3DCamera*) {}
void SC4DrawContext::SetDefaultRenderStateUnilaterally() {}
void SC4DrawContext::SetDepthOffset(int) {}

namespace {

using namespace sc4;

constexpr int kAdviceLink = 0x20;   // cISC4Advice flag: the text has a link

cIGZWin* MakeWindow(FakeMgr& mgr, cIGZWin* parent, std::uint32_t id) {
    auto* window = new cGZWin(&mgr);
    mgr.valid.insert(window);
    window->SetID(id);
    window->SetArea(0, 0, 50, 50);
    if (parent) parent->ChildAdd(window);
    return window;
}

gz::cGZWinGen* MakeGenWindow(FakeMgr& mgr, cIGZWin* parent, std::uint32_t id) {
    auto* window = new gz::cGZWinGen(&mgr);
    mgr.valid.insert(window);
    window->SetID(id);
    window->SetArea(0, 0, 50, 50);
    window->SetWinGenFlag(0x1000, true);   // no title bar: no graphics services needed
    if (parent) parent->ChildAdd(window);
    return window;
}

struct FakeAdvice final : cISC4Advice {
    int refs = 0, mood = 3, advisorId = 0;
    std::uint32_t flags = kAdviceFlagCurrent;
    std::string title = "Title", message = "Message";
    mutable cRZString titleText, messageText;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cISC4Advice) return false;
        *out = static_cast<cISC4Advice*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return std::uint32_t(++refs); }
    std::uint32_t Release() override { return std::uint32_t(--refs); }   // the test owns it
    bool Init(int) override { return true; }
    bool Shutdown() override { return true; }
    bool Clone(cISC4Advice**) override { return false; }
    int GetMood() const override { return mood; }
    int GetPriority() const override { return 50; }
    bool GetTrigger() const override { return false; }
    cIGZString& GetTriggerString() override { return titleText; }
    int GetGUID() const override { return 0; }
    int GetAdvisorID() const override { return advisorId; }
    void SetAdvisorID(int id) override { advisorId = id; }
    int GetSessionID() const override { return 0; }
    bool SetFlags(int f, bool on) override {
        flags = on ? (flags | std::uint32_t(f)) : (flags & ~std::uint32_t(f));
        return true;
    }
    bool CheckFlags(int f) const override { return (flags & std::uint32_t(f)) != 0; }
    int GetFrequencyInDays() const override { return 0; }
    int GetTimeOutInDays() const override { return 0; }
    const cIGZString& GetTextTitle() const override { titleText = cRZString(title.c_str()); return titleText; }
    std::uint32_t GetTextTitleGUID() const override { return 0; }
    const cIGZString& GetTextMessage() const override { messageText = cRZString(message.c_str()); return messageText; }
    std::uint32_t GetTextMessageGUID() const override { return 0; }
    bool DoLink(const cIGZString&) override { return false; }
    void NotifyGlobalEffectTriggerChanged() override {}
    bool SetEventSubject(cISC4AdviceSubject*) override { return false; }
    cISC4AdviceSubject* GetEventSubject() override { return nullptr; }
    int GetEventID() override { return 0; }
    int GetCommandID() override { return 0; }
    bool UpdateText() override { return true; }
    bool IsCurrent() const override { return CheckFlags(kAdviceFlagCurrent); }
};

struct FakeAdvisor final : cISC4Advisor {
    int refs = 0, id = 0;
    cRZString name, personal;
    AdviceMap advices;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return std::uint32_t(++refs); }
    std::uint32_t Release() override { return std::uint32_t(refs > 0 ? --refs : 0); }
    int GetID() const override { return id; }
    cIGZString& GetPersonalName() override { return personal; }
    const AdviceMap& GetCurrentAdvices() const override { return advices; }
    AdviceMap& GetCurrentAdvices() override { return advices; }
    bool Init(int) override { return true; }
    bool Shutdown() override { return true; }
    const cIGZString& GetName() const override { return name; }
    void SetName(const cIGZString&) override {}
    void SetPersonalName(const cIGZString&) override {}
    cISC4Advice* GetAdviceByGUID(int) const override { return nullptr; }
    cISC4Advice* GetAdviceBySessionID(int) const override { return nullptr; }
    bool Simulate(int) override { return true; }
    int GetHeadAnimCount(bool) override { return 8; }
    bool SetHeadAnim(bool, int) override { return true; }
    int GetHeadAnimID(int index) override { return 0x1000 + index; }
    bool PushEvent(int, cISC4AdviceSubject*) override { return false; }
    bool CommandWouldTrigger(std::uint32_t) override { return false; }
    bool RetireAdvice(cISC4Advice*) override { return false; }
};

// A cSC4WinAdviceList stand-in recording what the window writes.
struct FakeAdviceList final : cGZWin, cIGZWinAdviceList {
    explicit FakeAdviceList(cIGZWinMgr* mgr) : cGZWin(mgr) {}
    int fakeRefs = 0, clears = 0, rebuilds = 0;
    bool lastRebuild = true;
    std::uint32_t background = 0;
    struct Entry {
        std::string a, b, body, title, x;
        bool link = false, noLink = true;
        cISC4Advice* advice = nullptr;
        std::uint32_t image = 0;
    };
    std::vector<Entry> entries;
    static std::string S(const cIGZString& s) { return std::string(s.ToChar(), s.Strlen()); }
    std::uint32_t AddRef() override { return std::uint32_t(++fakeRefs); }
    std::uint32_t Release() override { return std::uint32_t(--fakeRefs); }
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cIGZWinAdviceList) {
            *out = static_cast<cIGZWinAdviceList*>(this);
            AddRef();
            return true;
        }
        return cGZWin::QueryInterface(iid, out);
    }
    cIGZWin* AsIGZWin() override { return this; }
    void AddEntry(const cIGZString& a, const cIGZString& b, const cIGZString& c, const cIGZString& d, bool link,
                  cISC4Advice* advice, const cIGZString& x, bool noLink, std::uint32_t image) override {
        entries.push_back({S(a), S(b), S(c), S(d), S(x), link, noLink, advice, image});
    }
    void RebuildHTML(bool force) override { ++rebuilds; lastRebuild = force; }
    void SetbgImage(std::uint32_t image) override { background = image; }
    void ClearList() override { ++clears; entries.clear(); }
    void SetGutters(int, int, int, int) override {}
    void ScrollToAnchor(const cIGZString&) override {}
    void DisableVerticalScrollbar() override {}
};

// The heads load nothing and tick nowhere.
struct HeadHost final : IAdvisorUIImageHost {
    cISC4Advisor* Advisor(int) override { return nullptr; }
    bool LlamaCheatActive() override { return false; }
    s3d::cIS3DModelInstanceAnimated* CreateModel(const cGZPersistResourceKey&) override { return nullptr; }
    void AddTickable(cIGZMessageTarget2*, std::uint32_t) override {}
    void RemoveTickable(cIGZMessageTarget2*, std::uint32_t) override {}
    bool MainWindowSize(std::uint16_t&, std::uint16_t&) override { return false; }
    cISC43DRender* Renderer() override { return nullptr; }
    gd::cIGZGDriver* Driver() override { return nullptr; }
};

// The window script's tree, under the city view: the all-advisors page is the script window itself.
struct Host final : IAdvisorUIAdvisorHost {
    FakeMgr mgr;
    cIGZWin* main = nullptr;
    cIGZWin* view = nullptr;
    gz::cGZWinGen* all = nullptr;
    cIGZWin* small = nullptr;
    cIGZWin* large = nullptr;
    cIGZWin* caption1 = nullptr;
    cIGZWin* caption2 = nullptr;
    cIGZWin* buttonA = nullptr;
    cIGZWin* buttonB = nullptr;
    gz::cGZWinGen* images[kAdvisorWindowAdvisorCount] = {};
    gz::cGZWinGen* smallImage = nullptr;
    gz::cGZWinGen* largeImage = nullptr;
    FakeAdviceList* smallList = nullptr;
    FakeAdviceList* largeList = nullptr;
    FakeAdvisor advisors[kAdvisorWindowAdvisorCount];
    std::vector<cGZPersistResourceKey> keys;
    std::vector<std::uint32_t> ids, iids;
    bool city = true;

    Host() {
        main = MakeWindow(mgr, nullptr, 0x9000);
        mgr.mainWin = main;
        view = MakeWindow(mgr, main, 0x100);
        for (int i = 0; i < kAdvisorWindowAdvisorCount; ++i) {
            advisors[i].id = kAdvisorWindowAdvisorIds[i];
            advisors[i].name = cRZString(("Advisor" + std::to_string(i)).c_str());
            advisors[i].personal = cRZString(("Person" + std::to_string(i)).c_str());
        }
    }
    cIGZWin* View3DWindow() override { return view; }
    bool CityPresent() override { return city; }
    cISC4Advisor* Advisor(int advisorId) override {
        for (auto& advisor : advisors) {
            if (advisor.id == advisorId) return &advisor;
        }
        return nullptr;
    }
    bool CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id, std::uint32_t iid,
                                void** out) override {
        keys.push_back(key);
        ids.push_back(id);
        iids.push_back(iid);
        all = MakeGenWindow(mgr, parent, kAdvisorWindowAll);
        for (int i = 0; i < kAdvisorWindowAdvisorCount; ++i) {
            images[i] = MakeGenWindow(mgr, all, kAdvisorWindowImageIds[i]);
            MakeWindow(mgr, all, kAdvisorWindowHeadButtons[i]);
        }
        small = MakeWindow(mgr, parent, kAdvisorWindowSmall);
        smallImage = MakeGenWindow(mgr, small, kAdvisorWindowSmallImage);
        smallList = new FakeAdviceList(&mgr);
        mgr.valid.insert(smallList);
        smallList->SetID(kAdvisorWindowSmallList);
        small->ChildAdd(smallList);
        caption1 = MakeWindow(mgr, small, kAdvisorWindowCaption1);
        large = MakeWindow(mgr, parent, kAdvisorWindowLarge);
        largeImage = MakeGenWindow(mgr, large, kAdvisorWindowLargeImage);
        largeList = new FakeAdviceList(&mgr);
        mgr.valid.insert(largeList);
        largeList->SetID(kAdvisorWindowLargeList);
        large->ChildAdd(largeList);
        caption2 = MakeWindow(mgr, large, kAdvisorWindowCaption2);
        buttonA = MakeWindow(mgr, small, kAdvisorWindowButtonA);
        buttonB = MakeWindow(mgr, small, kAdvisorWindowButtonB);
        for (std::uint32_t id : {kAdvisorWindowClose[0], kAdvisorWindowToLarge[0], kAdvisorWindowToSmall[0], kAdvisorWindowToAll[0]})
            MakeWindow(mgr, small, id);
        return all->QueryInterface(iid, out);
    }
    cIGZMessageServer2* Messages() override { return nullptr; }
    void FilterUICommandMessage(cGZMessage&) override {}
};

// The nSC4UI services AlignWindowSetToWindow reaches: none (it aligns through the windows alone).

std::string S(const cIGZString& s) { return std::string(s.ToChar(), s.Strlen()); }

bool Click(cSC4AdvisorUIAdvisorWinProc* proc, cIGZWin* window, std::uint32_t id, std::uint64_t sender = gz::kWinCommandSender,
           std::uint64_t param3 = 0) {
    cGZMessage message{kGZMsgCommand, sender, id, param3};
    return proc->DoWinProcMessage(window, message);
}

void Post(cSC4AdvisorUIAdvisorWinProc* proc, std::uint32_t type, cIGZUnknown* payload, std::uint32_t data1 = 0,
          std::uint32_t data2 = 0) {
    cRZMessage2Standard message;
    message.SetType(type);
    message.SetData1(data1);
    message.SetData2(data2);
    message.SetIGZUnknown(payload);
    proc->DoMessage(&message);
}

void TestWindow() {
    HeadHost heads;
    SetAdvisorUIImageHost(&heads);
    Host host;
    SetAdvisorUIAdvisorHost(&host);
    auto* proc = new cSC4AdvisorUIAdvisorWinProc();
    proc->AddRef();

    // QueryInterface: the window procedure (0x22e85d8e) and the message target (0x452294aa and 1).
    void* out = nullptr;
    CHECK(proc->QueryInterface(0x22e85d8e, &out) && out == static_cast<gz::cIGZWinProc*>(proc));
    proc->Release();
    CHECK(proc->QueryInterface(1, &out) && out == static_cast<cIGZMessageTarget2*>(proc));
    proc->Release();
    CHECK(!proc->QueryInterface(0x12345678, &out));

    // get_advisor_mood: none -> 3 (neutral), else the lowest mood from 7, stopping at 0.
    FakeAdvisor probe;
    CHECK_EQ(proc->get_advisor_mood(nullptr), 3);
    CHECK_EQ(proc->get_advisor_mood(&probe), 3);
    FakeAdvice m5, m3, m0, m8;
    m5.mood = 5; m3.mood = 3; m0.mood = 0; m8.mood = 8;
    probe.advices[AdviceRef(&m5)] = 0;
    probe.advices[AdviceRef(&m3)] = 1;
    CHECK_EQ(proc->get_advisor_mood(&probe), 3);
    probe.advices[AdviceRef(&m0)] = 2;
    CHECK_EQ(proc->get_advisor_mood(&probe), 0);
    probe.advices.clear();
    probe.advices[AdviceRef(&m8)] = 0;
    CHECK_EQ(proc->get_advisor_mood(&probe), 7);
    probe.advices.clear();

    // Init: the script {0, 0x96a006b0, 0xcbc905cd}, window 0x6a15c767 as cIGZWinGen; every page hidden; a head on each
    // picture, flagged 0x8000000, in kAdvisorIndexToIDTable order.
    proc->Init();
    CHECK_EQ(host.keys.size(), 1u);
    CHECK(host.keys[0].type == 0 && host.keys[0].group == 0x96a006b0u && host.keys[0].instance == 0xcbc905cdu);
    CHECK(host.ids[0] == kAdvisorWindowAll && host.iids[0] == gz::kGZIID_cIGZWinGen);
    CHECK(proc->Window() == static_cast<gz::cIGZWinGen*>(host.all));
    CHECK(!host.all->GetFlag(kWinVisible) && !host.small->GetFlag(kWinVisible) && !host.large->GetFlag(kWinVisible));
    CHECK(host.all->GetWinProc() == static_cast<gz::cIGZWinProc*>(proc));
    for (int i = 0; i < kAdvisorWindowAdvisorCount; ++i) {
        CHECK(proc->Head(i) != nullptr);
        CHECK(host.images[i]->GetWinProc() == static_cast<gz::cIGZWinProc*>(proc->Head(i)));
        CHECK(host.images[i]->GetFlag(kAdvisorWindowImageFlag));
        if (proc->Head(i)) CHECK(!proc->Head(i)->Enabled());
    }
    CHECK(!proc->IsVisible());

    // ShowWindow: the all-advisors page and every head.
    proc->ShowWindow();
    CHECK(proc->IsVisible());
    CHECK(host.all->GetFlag(kWinVisible));
    for (int i = 0; i < kAdvisorWindowAdvisorCount; ++i) {
        if (proc->Head(i)) CHECK(proc->Head(i)->Enabled());
    }

    // A pressed head button shifts its head by the press state (command 0x287259f8, param3).
    Click(proc, host.all, kAdvisorWindowHeadButtons[2], kAdvisorWindowButtonPressed, 1);
    CHECK(proc->Head(2) && proc->Head(2)->Offset() == 1);

    // The first head button opens advisor 0x19's small page; its advices are listed in REVERSE map order.
    FakeAdvisor& first = host.advisors[0];
    FakeAdvice alarm, good, plain, expired;
    alarm.mood = 0; alarm.title = "Fire"; alarm.message = "Burning";
    good.mood = 5; good.title = "Clean"; good.message = "Air";
    plain.mood = 7; plain.title = "Note"; plain.message = "Body";
    expired.mood = 1; expired.title = "Old"; expired.message = "Gone"; expired.flags = kAdviceLink;   // not current
    for (FakeAdvice* a : {&alarm, &good, &plain, &expired}) {
        a->advisorId = 0x19;
        first.advices[AdviceRef(a)] = 0;
    }
    std::vector<cISC4Advice*> expected;
    for (auto it = first.advices.rbegin(); it != first.advices.rend(); ++it) expected.push_back(it->first.get());
    host.buttonA->SetFlag(kWinEnabled, true);
    Click(proc, host.all, kAdvisorWindowHeadButtons[0]);
    CHECK_EQ(proc->AdvisorId(), 0x19);
    CHECK_EQ(proc->AdvisorIndex(), 0);
    CHECK_EQ(proc->Layout(), 1);
    CHECK(!host.all->GetFlag(kWinVisible) && host.small->GetFlag(kWinVisible));
    CHECK(proc->AdviceList() == static_cast<cIGZWinAdviceList*>(host.smallList));
    CHECK_EQ(S(host.small->GetCaption()), std::string("Advisor0 - Person0"));
    CHECK_EQ(S(host.caption1->GetCaption()), std::string("Advisor0 - Person0"));
    CHECK_EQ(S(host.caption2->GetCaption()), std::string("Advisor0 - Person0"));
    // Only the shown advisor's head is attached, on the small page's picture.
    CHECK(proc->Head(0) && host.smallImage->GetWinProc() == static_cast<gz::cIGZWinProc*>(proc->Head(0)));
    for (int i = 1; i < kAdvisorWindowAdvisorCount; ++i) CHECK(proc->Head(i) == nullptr);
    CHECK_EQ(proc->Messages().size(), 4u);
    std::vector<cISC4Advice*> listed;
    for (const auto& entry : proc->Messages()) listed.push_back(entry.get());
    CHECK(listed == expected);
    // The expired advice keeps its link flag: GetMessageList clears 0x20 only on advices it already listed.
    CHECK(expired.CheckFlags(kAdviceLink));

    // The markup: one entry per advice in list order, background and entry image 0x14416264, RebuildHTML(false), and
    // the two buttons disabled afterwards.
    FakeAdviceList& list = *host.smallList;
    CHECK_EQ(list.entries.size(), 4u);
    CHECK_EQ(list.background, kAdvisorWindowListImage);
    CHECK(list.rebuilds >= 1 && !list.lastRebuild);
    CHECK(!host.buttonA->GetFlag(kWinEnabled) && !host.buttonB->GetFlag(kWinEnabled));
    for (const auto& entry : list.entries) {
        CHECK_EQ(entry.image, kAdvisorWindowListImage);
        CHECK(!entry.noLink);
        if (entry.advice == &alarm) {
            CHECK_EQ(entry.title, std::string("<BLINK><FONT COLOR=\"#f70a0b\" FACE=\"StocletITC TT\" SIZE=3><B>Fire</B></FONT></BLINK>"));
            CHECK_EQ(entry.body, std::string("<FONT COLOR=\"#3f4967\" FACE=\"Arta\" SIZE=3><I>Burning</I></FONT>"));
            CHECK(entry.a.find("14416252") != std::string::npos && entry.b.find("14416251") != std::string::npos);
            CHECK(entry.x.find("14416253") != std::string::npos);
        } else if (entry.advice == &good) {
            CHECK(entry.title.rfind("<FONT COLOR=\"#095a22\"", 0) == 0);
            CHECK(entry.a.find("1441625E") != std::string::npos && entry.x.find("1441625F") != std::string::npos);
        } else if (entry.advice == &plain) {
            // Mood 7: the neutral dot twice, no link.
            CHECK(entry.a.find("14416258") != std::string::npos && entry.b == entry.a);
            CHECK(!entry.link);
            CHECK_EQ(entry.body, std::string("<FONT COLOR=\"#3f4967\" FACE=\"Arta\" SIZE=3><I>Body</I></FONT>"));
        } else if (entry.advice == &expired) {
            CHECK_EQ(entry.title, std::string("<FONT COLOR=\"#8e9499\" FACE=\"StocletITC TT\" SIZE=3><B>Old</B></FONT>"));
            CHECK(entry.body.rfind("<FONT COLOR=\"#8e9499\" FACE=\"Arta\"", 0) == 0);
            CHECK(entry.a.find("1441625A") != std::string::npos);
            CHECK(entry.link);   // the link flag is read as it is
        } else {
            CHECK(!"an entry for an unknown advice");
        }
    }

    // The large page: list 0x100101, layout 2, the head moves to 0xaa1d97df; GetMessageList clears 0x20 on the
    // listed advice that is not current.
    Click(proc, host.all, kAdvisorWindowToLarge[0]);
    CHECK_EQ(proc->Layout(), 2);
    CHECK(proc->Large());
    CHECK(host.large->GetFlag(kWinVisible) && !host.small->GetFlag(kWinVisible));
    CHECK(proc->AdviceList() == static_cast<cIGZWinAdviceList*>(host.largeList));
    CHECK(host.smallList->clears >= 1 && host.largeList->entries.size() == 4u);
    CHECK(proc->Head(0) && host.largeImage->GetWinProc() == static_cast<gz::cIGZWinProc*>(proc->Head(0)));
    CHECK(!host.smallImage->GetWinProc());
    CHECK(!expired.CheckFlags(kAdviceLink));

    // Messages: an advice added for the advisor shown is listed, one for another advisor is not; a removal drops it.
    FakeAdvice extra, other;
    extra.advisorId = 0x19;
    other.advisorId = 0x15;
    Post(proc, kAdvisorWindowAdviceAdded, &extra);
    Post(proc, kAdvisorWindowAdviceAdded, &other);
    CHECK_EQ(proc->Messages().size(), 5u);
    CHECK(proc->Messages().back().get() == &extra);
    CHECK_EQ(host.largeList->entries.size(), 5u);
    Post(proc, kAdvisorWindowAdviceRemoved, &extra);
    CHECK_EQ(proc->Messages().size(), 4u);
    CHECK_EQ(extra.refs, 0);
    CHECK_EQ(other.refs, 0);
    // A mood toggle reaches the head of that advisor (data1 id, data2 mood; ForceMood maps 5 -> 1).
    Post(proc, kAdvisorWindowMoodToggled, nullptr, 0x19, 5);
    CHECK(proc->Head(0) && proc->Head(0)->Mood() == 1);

    // Back to all advisors: every head again, the advisor dropped.
    Click(proc, host.all, kAdvisorWindowToAll[0]);
    CHECK_EQ(proc->Layout(), 0);
    CHECK_EQ(proc->AdvisorId(), -1);
    CHECK(host.all->GetFlag(kWinVisible) && !host.large->GetFlag(kWinVisible));
    for (int i = 0; i < kAdvisorWindowAdvisorCount; ++i) CHECK(proc->Head(i) != nullptr);
    CHECK(host.images[0]->GetWinProc() == static_cast<gz::cIGZWinProc*>(proc->Head(0)));
    // The head of 0x19 now shows its advisor's lowest mood (alarm 0 -> ForceMood 3).
    CHECK(proc->Head(0) && proc->Head(0)->Mood() == 3);

    // Another advisor while the large layout is chosen opens straight on the large page.
    Click(proc, host.all, kAdvisorWindowHeadButtons[3]);
    CHECK_EQ(proc->AdvisorId(), 0x17);
    CHECK_EQ(proc->Layout(), 2);
    CHECK_EQ(S(host.large->GetCaption()), std::string("Advisor3 - Person3"));

    // Close: hidden, the shown head disabled.
    Click(proc, host.all, kAdvisorWindowClose[0]);
    CHECK(!proc->IsVisible());
    CHECK(!host.large->GetFlag(kWinVisible));
    CHECK(proc->Head(3) && !proc->Head(3)->Enabled());

    // Shutdown destroys the three pages (the script window is the first of them) and drops everything.
    proc->Shutdown();
    for (std::uint32_t id : {kAdvisorWindowAll, kAdvisorWindowSmall, kAdvisorWindowLarge})
        CHECK(host.view->GetChildWindowFromIDRecursive(id) == nullptr);
    CHECK(proc->Window() == nullptr && proc->AdviceList() == nullptr);
    for (int i = 0; i < kAdvisorWindowAdvisorCount; ++i) CHECK(proc->Head(i) == nullptr);
    proc->Release();

    for (FakeAdvice* a : {&alarm, &good, &plain}) CHECK_EQ(a->refs, 1);   // only the advisor's map holds them now
    for (auto& advisor : host.advisors) advisor.advices.clear();   // the advices are locals that die before the host
    SetAdvisorUIAdvisorHost(nullptr);
    SetAdvisorUIImageHost(nullptr);
}

}  // namespace

int main() {
    TestWindow();
    if (failures) {
        std::printf("sc4_advisoruiadvisorwinproc: %d FAILED\n", failures);
        return 1;
    }
    std::printf("sc4_advisoruiadvisorwinproc: all passed\n");
    return 0;
}
