// cSC4AdvisorPopupTracker — the object layout, the reference counting / QueryInterface views, Init / Shutdown and the
// three message routes, the popup rules of the show tick (pause / safety / preference / priority / tutorial / vehicle
// manager), the dialog build (the HTML page, the size computation, the advice parameter, the advisor image window
// procedure) and the window procedure's close / link handling.
//
// Everything the original reaches through SL::sp* / RZ:: / GZCOM() comes in through IAdvisorPopupTrackerHost: the
// message server, the window manager, the game pause, the script window creation and the fonts are recording fakes and
// the dialog is the recovered cGZWin whose two script children are interface fakes. The window location saver is left
// absent (the host answers null, like the SC4UI test does), so the two saver calls are the only ones not exercised.
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gz/Win.h"
#include "gz/WinGen.h"
#include "gz/WinHTML.h"
#include "gz/WinInterfaces.h"
#include "gz/WinProc.h"
#include "gz/cGZPersistResourceKey.h"
#include "gz/cIGZMessage2.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZVariant.h"
#include "sc4/AdvisorPopupTracker.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace sc4;

namespace sc4 {
// The offsets the original object layout documents (see the header).
struct AdvisorPopupTrackerTestAccess {
    static std::ptrdiff_t RefCount(void* object) {
        return reinterpret_cast<char*>(&static_cast<cSC4AdvisorPopupTracker*>(object)->refCount_) -
               static_cast<char*>(object);
    }
    static std::ptrdiff_t Initialized(void* object) {
        return reinterpret_cast<char*>(&static_cast<cSC4AdvisorPopupTracker*>(object)->initialized_) -
               static_cast<char*>(object);
    }
    static std::ptrdiff_t List(void* object) {
        return reinterpret_cast<char*>(&static_cast<cSC4AdvisorPopupTracker*>(object)->pending_) -
               static_cast<char*>(object);
    }
    static std::ptrdiff_t ImageWinProc(void* object) {
        return reinterpret_cast<char*>(&static_cast<cSC4AdvisorPopupTracker*>(object)->imageWinProc_) -
               static_cast<char*>(object);
    }
};
}  // namespace sc4

namespace {

// ---------------------------------------------------------------------------------------------------------------
// The advice: flags, mood, priority, advisor id and the two text strings the page is built from.
// ---------------------------------------------------------------------------------------------------------------
struct Advice final : cISC4Advice {
    std::uint32_t refs = 0;
    std::uint32_t flags = 0;
    int mood = 3;
    int priority = 50;
    int advisorId = 7;
    cRZString title{"TITLE"};
    cRZString message{"MESSAGE"};
    cRZString triggerString;
    std::vector<std::pair<std::uint32_t, bool>> flagChanges;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != kGZIID_cISC4Advice) return false;
        *out = static_cast<cISC4Advice*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (refs) --refs;
        return refs;
    }
    bool Init(int) override { return true; }
    bool Shutdown() override { return true; }
    bool Clone(cISC4Advice** out) override {
        *out = nullptr;
        return false;
    }
    int GetMood() const override { return mood; }
    int GetPriority() const override { return priority; }
    bool GetTrigger() const override { return false; }
    cIGZString& GetTriggerString() override { return triggerString; }
    int GetGUID() const override { return 0; }
    int GetAdvisorID() const override { return advisorId; }
    void SetAdvisorID(int id) override { advisorId = id; }
    int GetSessionID() const override { return 0; }
    bool SetFlags(int bits, bool on) override {
        const std::uint32_t mask = static_cast<std::uint32_t>(bits);
        if (on) flags |= mask; else flags &= ~mask;
        flagChanges.emplace_back(mask, on);
        return true;
    }
    bool CheckFlags(int bits) const override {
        const std::uint32_t mask = static_cast<std::uint32_t>(bits);
        return (flags & mask) == mask;
    }
    int GetFrequencyInDays() const override { return 30; }
    int GetTimeOutInDays() const override { return 30; }
    const cIGZString& GetTextTitle() const override { return title; }
    std::uint32_t GetTextTitleGUID() const override { return 1; }
    const cIGZString& GetTextMessage() const override { return message; }
    std::uint32_t GetTextMessageGUID() const override { return 2; }
    bool DoLink(const cIGZString&) override { return false; }
    void NotifyGlobalEffectTriggerChanged() override {}
    bool SetEventSubject(cISC4AdviceSubject*) override { return false; }
    cISC4AdviceSubject* GetEventSubject() override { return nullptr; }
    int GetEventID() override { return 0; }
    int GetCommandID() override { return 0; }
    bool UpdateText() override { return true; }
    bool IsCurrent() const override { return CheckFlags(kAdviceFlagCurrent); }
};

// ---------------------------------------------------------------------------------------------------------------
// The message server: records the notification subscriptions and the posted messages (with the target they went to).
// ---------------------------------------------------------------------------------------------------------------
struct Server final : cIGZMessageServer2 {
    struct Post {
        std::uint32_t type = 0;
        cIGZMessageTarget2* target = nullptr;
    };
    std::vector<std::pair<cIGZMessageTarget2*, std::uint32_t>> added;
    std::vector<std::pair<cIGZMessageTarget2*, std::uint32_t>> removed;
    std::vector<Post> posts;
    std::uint32_t refs = 0;

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (refs) --refs;
        return refs;
    }
    bool MessageSend(cIGZMessage2*) override { return true; }
    bool MessagePost(cIGZMessage2*, bool) override { return true; }
    bool AddNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        added.emplace_back(target, type);
        return true;
    }
    bool RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        removed.emplace_back(target, type);
        return true;
    }
    bool GeneralMessagePostToTarget(cIGZMessage2* message, cIGZMessageTarget2* target) override {
        posts.push_back({message ? message->GetType() : 0u, target});
        return true;
    }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return refs; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

// ---------------------------------------------------------------------------------------------------------------
// cSC4AdvisorUIImageWinProc: the four methods the tracker uses, on the cIGZWinProc surface.
// ---------------------------------------------------------------------------------------------------------------
struct ImageProc final : IAdvisorUIImageWinProc {
    std::uint32_t refs = 0;
    int advisorId = 0;
    std::vector<bool> enables;
    std::vector<int> moods;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != gz::kGZIID_cIGZWinProc) return false;
        *out = static_cast<gz::cIGZWinProc*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (refs) --refs;
        return refs;
    }
    bool DoWinProcMessage(cIGZWin*, cGZMessage&) override { return false; }
    void Enable(bool enable) override { enables.push_back(enable); }
    void ForceMood(int mood) override { moods.push_back(mood); }
};

// ---------------------------------------------------------------------------------------------------------------
// The HTML page (child 2 of the script window).
// ---------------------------------------------------------------------------------------------------------------
struct Html final : cIGZWinHTML {
    std::uint32_t refs = 0;
    cIGZWin* window = nullptr;
    std::string document;
    std::string baseUrl;
    std::vector<int> clearedFlags;
    std::int32_t documentWidth = 300, documentHeight = 100;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != kGZIID_cIGZWinHTML) return false;
        *out = static_cast<cIGZWinHTML*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (refs) --refs;
        return refs;
    }
    cIGZWin* AsIGZWin() override { return window; }
    bool SetWWWSystem(html::IHtmlWebSystem*) override { return false; }
    std::int32_t GetLoadStatus() override { return 2; }
    void GetScrollPosition(int& x, int& y) override { x = y = 0; }
    void SetScrollPosition(int, int) override {}
    bool GetAnchorPosition(const cIGZString&, cRZRect&) override { return false; }
    bool ScrollToAnchor(const cIGZString&) override { return false; }
    void GetDocumentSize(gz::cRZSize& size) override { size.width = documentWidth; size.height = documentHeight; }
    bool SetScrollBarImageVertical(cIGZBuffer*, const cRZRect&) override { return false; }
    bool SetScrollBarImageHorizontal(cIGZBuffer*, const cRZRect&) override { return false; }
    bool GetScrollBarImageVertical(cIGZBuffer*& image, cRZRect&) const override { image = nullptr; return false; }
    bool GetScrollBarImageHorizontal(cIGZBuffer*& image, cRZRect&) const override { image = nullptr; return false; }
    bool GetURL(cRZString&) override { return false; }
    bool GetNewURL(cRZString&) override { return false; }
    bool SetURL(const cRZString&, bool) override { return false; }
    bool SetBaseURL(const cRZString&) override { return false; }
    bool SetRelativeURL(const cRZString&) override { return false; }
    bool SetURL(cRZURL&, bool) override { return false; }
    bool GetDocument(std::string& out) override { out = document; return true; }
    bool SetDocument(const std::string& text, const cRZString& base) override {
        document = text;
        baseUrl = base.ToChar();
        return true;
    }
    void SetMissingBitmapPath(const cIGZString&) override {}
    bool CancelCurrentLoadRequest() override { return false; }
    bool DoLinkJump(const cIGZString&) override { return false; }
    void SetFlag(int) override {}
    void SetFlags(int) override {}
    void ClearFlag(int flag) override { clearedFlags.push_back(flag); }
    bool IsFlagSet(int) override { return false; }
    std::int32_t GetFlags() override { return 0; }
    void GetGutters(int&, int&, int&, int&) override {}
    void SetGutters(int, int, int, int) override {}
    void SetTextSizeTable(const std::uint32_t*) override {}
};

// ---------------------------------------------------------------------------------------------------------------
// The generic window (child 4 of the script window, and the window the tick creates) the window procedure is
// attached to.
// ---------------------------------------------------------------------------------------------------------------
struct Gen final : gz::cIGZWinGen {
    std::uint32_t refs = 0;
    cIGZWin* window = nullptr;
    gz::cIGZWinProc* proc = nullptr;
    std::vector<gz::cIGZWinProc*> procs;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != gz::kGZIID_cIGZWinGen) return false;
        *out = static_cast<gz::cIGZWinGen*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (refs) --refs;
        return refs;
    }
    cIGZWin* AsIGZWin() override { return window; }
    std::uint32_t GetCLSID() const override { return gz::kGZCLSID_cGZWinGen; }
    bool GetWinGenFlag(int) const override { return false; }
    bool SetWinGenFlag(int, bool) override { return false; }
    bool SetWinProc(gz::cIGZWinProc* p) override {
        proc = p;
        procs.push_back(p);
        return true;
    }
    gz::cIGZWinProc* GetWinProc() const override { return proc; }
    bool GetWinProcAs(std::uint32_t, void** out) const override { (void)out; return false; }
    bool GetUserArea(cRZRect& out) const override { out = cRZRect{}; return true; }
    bool SetUserArea(const cRZRect&) override { return false; }
    bool SetBackgroundBitmap(gz::BGBltType, cIGZBuffer*) override { return false; }
    bool GetBackgroundBitmap(gz::BGBltType&, cIGZBuffer*&) const override { return false; }
    bool SetBackgroundBitmap(cIGZBuffer*) override { return false; }
    bool GetBackgroundBitmap(cIGZBuffer** out) const override { *out = nullptr; return false; }
    bool SetBackgroundBlitType(gz::BGBltType) override { return false; }
    bool GetBackgroundBlitType(gz::BGBltType&) const override { return false; }
    bool SetUserData(std::uint32_t, std::uint32_t) override { return false; }
    bool GetUserData(std::uint32_t, std::uint32_t&) const override { return false; }
    bool SetUserData(std::uint32_t) override { return false; }
    std::uint32_t GetUserData() const override { return 0; }
    void SetUserData(std::uint32_t, cIGZUnknown*) override {}
    bool GetUserData(std::uint32_t, std::uint32_t, void**) override { return false; }
    void RemoveUserData(std::uint32_t) override {}
    bool SetGutters(std::uint8_t, std::uint8_t) override { return false; }
    bool GetGutters(int&, int&) override { return false; }
    void SetGutters(std::int8_t, std::int8_t, std::int8_t, std::int8_t) override {}
    bool GetGutters(int&, int&, int&, int&) const override { return false; }
    bool AddColorBar(int, int, int, const cRZRect&) override { return false; }
    void AddDividerH(int, int) override {}
    bool RemoveColorBar(int) override { return false; }
};

// ---------------------------------------------------------------------------------------------------------------
// The window manager: records the modal dialog run, the focus and the two EndDialog pieces.
// ---------------------------------------------------------------------------------------------------------------
struct Manager final : cIGZWinMgr {
    struct Dialog {
        std::uint32_t code = 0;
        cIGZWin* window = nullptr;
        std::int64_t result = 0;
    };
    std::vector<cIGZWin*> popups;
    std::vector<Dialog> dialogs;
    std::vector<cIGZWin*> destroyed;
    cIGZWin* focus = nullptr;
    cIGZWin* mainWindow = nullptr;

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    cIGZWin* GetMainWindow() override { return mainWindow; }
    void EndDialog(std::uint32_t code, cIGZWin* window, std::int64_t result) override {
        dialogs.push_back({code, window, result});
    }
    void AppendToPlotStrategy(cIGZWin*) override {}
    void Plot() override {}
    void PostMessageToTarget(cIGZWin*, const cGZMessage&) override {}
    void AddWindowToValidList(cIGZWin*) override {}
    void RemoveWindowFromValidList(cIGZWin*) override {}
    void CleanUpWindowReferences(cIGZWin*) override {}
    bool DestroyWindow(cIGZWin* window) override {
        destroyed.push_back(window);
        return true;
    }
    bool IsWindowValid(cIGZWin* window) override { return window != nullptr; }
    cIGZWin* GetWindowFromPoint(int, int) override { return nullptr; }
    void GetCursorPos(cIGZWin*, int& x, int& y) override { x = y = 0; }
    void SetCurrentMouseWin(cIGZWin*) override {}
    cIGZWin* GZGetCapture() override { return nullptr; }
    void GZSetCapture(cIGZWin*) override {}
    void GZReleaseCapture(cIGZWin*) override {}
    cIGZCursorManager* GetCursorManager() override { return nullptr; }
    cIGZWin* GZGetFocus() override { return focus; }
    void GZSetFocus(cIGZWin* window) override { focus = window; }
    cIGZKeyboard* GetKeyboard() override { return nullptr; }
    int RunPopup(cIGZWin* popup) override {
        popups.push_back(popup);
        return 0;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// The dialog: the recovered cGZWin with the two script children answered by the fakes (the real GetChildAsRecursive
// would need whole window trees) and the moves recorded.
// ---------------------------------------------------------------------------------------------------------------
struct Dialog final : cGZWin {
    explicit Dialog(cIGZWinMgr* mgr) : cGZWin(mgr) {}
    Html* html = nullptr;
    Gen* image = nullptr;
    cIGZWin* layout = nullptr;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> childLookups;
    std::vector<std::pair<int, int>> moves;

    bool GetChildAsRecursive(std::uint32_t id, std::uint32_t iid, void** out) override {
        childLookups.emplace_back(id, iid);
        if (id == kAdvisorPopupHtmlId && iid == kGZIID_cIGZWinHTML && html) {
            html->AddRef();
            *out = static_cast<cIGZWinHTML*>(html);
            return true;
        }
        if (id == kAdvisorPopupImageId && iid == gz::kGZIID_cIGZWinGen && image) {
            image->AddRef();
            *out = static_cast<gz::cIGZWinGen*>(image);
            return true;
        }
        return false;
    }
    cIGZWin* GetChildWindowFromIDRecursive(std::uint32_t id) override {
        return id == kAdvisorPopupLayoutId ? layout : nullptr;
    }
    void GZWinMoveTo(int x, int y) override {
        moves.emplace_back(x, y);
        cGZWin::GZWinMoveTo(x, y);
    }
};

// ---------------------------------------------------------------------------------------------------------------
// A plain window whose moves are recorded (the layout child of the dialog).
// ---------------------------------------------------------------------------------------------------------------
struct MoveRecorder final : cGZWin {
    explicit MoveRecorder(cIGZWinMgr* mgr) : cGZWin(mgr) {}
    std::vector<std::pair<int, int>> moves;
    void GZWinMoveTo(int x, int y) override {
        moves.emplace_back(x, y);
        cGZWin::GZWinMoveTo(x, y);
    }
};

// ---------------------------------------------------------------------------------------------------------------
// The host: every engine call of the tracker, recorded.
// ---------------------------------------------------------------------------------------------------------------
struct Host final : IAdvisorPopupTrackerHost {
    Server server;
    bool messagesPresent = true;
    bool safe = true, dialogsEnabled = true, preference = true;
    bool advisorSystemPresent = true;
    int threshold = 50;
    bool tutorial = false, vehiclePaused = false, simulatorPaused = false;
    bool fontsPresent = true;
    AdvisorPopupFont titleFont, messageFont;
    bool createWindow = true;
    Gen* window = nullptr;
    Manager manager;
    int pauses = 0;
    int focusCalls = 0;
    std::vector<cGZPersistResourceKey> keys;
    std::vector<std::uint32_t> windowIds;
    std::vector<std::uint32_t> windowIids;
    std::vector<std::unique_ptr<ImageProc>> images;
    cIGZString* lastLink = nullptr;
    bool doUrlAnswer = false;

    cIGZMessageServer2* Messages() override { return messagesPresent ? &server : nullptr; }
    bool PopupModalDialogsSafe() override { return safe; }
    bool PopupModalDialogsEnabled() override { return dialogsEnabled; }
    bool AdvisorPopupsPreferenceEnabled() override { return preference; }
    bool AdvicePopupPriorityThreshold(int& out) override {
        if (!advisorSystemPresent) return false;
        out = threshold;
        return true;
    }
    bool TutorialRunning() override { return tutorial; }
    bool VehicleManagerPaused() override { return vehiclePaused; }
    bool SimulatorPaused() override { return simulatorPaused; }
    bool CreateWindowFromScript(const cGZPersistResourceKey& key, std::uint32_t windowId, std::uint32_t iid,
                               void** out) override {
        keys.push_back(key);
        windowIds.push_back(windowId);
        windowIids.push_back(iid);
        if (!createWindow || !window) return false;
        window->AddRef();
        *out = static_cast<gz::cIGZWinGen*>(window);
        return true;
    }
    void BeginGamePause() override { ++pauses; }
    void EndGamePause() override { ++pauses; }
    cSLWinLocationSaver* WindowLocationSaver() override { return nullptr; }
    cIGZWinMgr* WindowManager() override { return &manager; }
    void FocusView3D() override { ++focusCalls; }
    IAdvisorUIImageWinProc* CreateImageWinProc(int advisorId) override {
        auto proc = std::make_unique<ImageProc>();
        proc->advisorId = advisorId;
        images.push_back(std::move(proc));
        return images.back().get();
    }
    bool SystemFont(std::uint32_t fontId, AdvisorPopupFont& out) override {
        if (!fontsPresent) return false;
        if (fontId == kAdvisorPopupTitleFontId) {
            out = titleFont;
            return true;
        }
        if (fontId == kAdvisorPopupMessageFontId) {
            out = messageFont;
            return true;
        }
        return false;
    }
    bool DoURL(cIGZString* link) override {
        lastLink = link;
        return doUrlAnswer;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// The layout: the two vtable views at +0 and +8 and the documented members.
// ---------------------------------------------------------------------------------------------------------------
void TestLayout() {
    cSC4AdvisorPopupTracker tracker;
    CHECK(sizeof(cSC4AdvisorPopupTracker) == 0x38);
    CHECK(reinterpret_cast<char*>(static_cast<cIGZMessageTarget2*>(&tracker)) == reinterpret_cast<char*>(&tracker));
    CHECK(reinterpret_cast<char*>(static_cast<gz::cIGZWinProc*>(&tracker)) == reinterpret_cast<char*>(&tracker) + 8);
    CHECK(AdvisorPopupTrackerTestAccess::RefCount(&tracker) == 0x10);
    CHECK(AdvisorPopupTrackerTestAccess::Initialized(&tracker) == 0x14);
    CHECK(AdvisorPopupTrackerTestAccess::List(&tracker) == 0x18);
    CHECK(AdvisorPopupTrackerTestAccess::ImageWinProc(&tracker) == 0x30);
    // The constructor's state.
    CHECK(tracker.RefCount() == 0);
    CHECK(!tracker.Initialized());
    CHECK(tracker.Pending().empty());
    CHECK(tracker.ImageWinProc() == nullptr);
}

// ---------------------------------------------------------------------------------------------------------------
// QueryInterface / AddRef / Release.
// ---------------------------------------------------------------------------------------------------------------
void TestReferenceCounting() {
    auto* tracker = new cSC4AdvisorPopupTracker();
    void* out = nullptr;
    CHECK(tracker->QueryInterface(1, &out) && out == static_cast<cIGZMessageTarget2*>(tracker));
    CHECK(tracker->RefCount() == 1);
    out = nullptr;
    CHECK(tracker->QueryInterface(0x452294aa, &out) && out == static_cast<cIGZMessageTarget2*>(tracker));
    CHECK(tracker->RefCount() == 2);
    // The window-procedure iid of this class is 0x2a5a754d, and the answer is the primary pointer (not the +8 view).
    out = nullptr;
    CHECK(tracker->QueryInterface(0x2a5a754d, &out) && out == static_cast<cIGZMessageTarget2*>(tracker));
    CHECK(tracker->RefCount() == 3);
    // Every other id - including the cIGZWinProc iid every other window procedure answers - is refused.
    out = reinterpret_cast<void*>(0x1);
    CHECK(!tracker->QueryInterface(0x22e85d8e, &out));
    CHECK(!tracker->QueryInterface(kGZIID_cISC4Advice, &out));
    CHECK(!tracker->QueryInterface(0, &out));
    CHECK(out == reinterpret_cast<void*>(0x1));
    CHECK(!tracker->QueryInterface(1, nullptr));   // the port's null guard
    // AddRef / Release arithmetic, and the object destroys itself at the last reference.
    CHECK(tracker->AddRef() == 4);
    CHECK(tracker->Release() == 3);
    CHECK(tracker->Release() == 2);
    CHECK(tracker->Release() == 1);
    CHECK(tracker->Release() == 0);   // deletes the object (ASan verifies the destruction is sound)
}

// ---------------------------------------------------------------------------------------------------------------
// Init / Shutdown: the three notifications, the posted tick, and the idempotence.
// ---------------------------------------------------------------------------------------------------------------
void TestInitShutdown() {
    Host host;
    SetAdvisorPopupTrackerHost(&host);
    auto* tracker = new cSC4AdvisorPopupTracker();
    CHECK(tracker->RefCount() == 0);
    CHECK(tracker->Init());
    CHECK(tracker->Initialized());
    CHECK(host.server.added.size() == 3);
    CHECK(host.server.added[0].second == kAdvisorPopupMessageAdviceActivated);
    CHECK(host.server.added[1].second == kAdvisorPopupMessageShow);
    CHECK(host.server.added[2].second == kAdvisorPopupMessageClear);
    for (const auto& entry : host.server.added) CHECK(entry.first == static_cast<cIGZMessageTarget2*>(tracker));
    CHECK(host.server.posts.size() == 1);   // the empty tick Init posts to itself
    CHECK(host.server.posts[0].type == kAdvisorPopupMessageShow);
    CHECK(host.server.posts[0].target == static_cast<cIGZMessageTarget2*>(tracker));
    // A second Init does nothing.
    CHECK(tracker->Init());
    CHECK(host.server.added.size() == 3);
    CHECK(host.server.posts.size() == 1);
    // Shutdown unregisters the three, in order, and is idempotent as well.
    CHECK(tracker->Shutdown());
    CHECK(!tracker->Initialized());
    CHECK(host.server.removed.size() == 3);
    CHECK(host.server.removed[0].second == kAdvisorPopupMessageAdviceActivated);
    CHECK(host.server.removed[1].second == kAdvisorPopupMessageShow);
    CHECK(host.server.removed[2].second == kAdvisorPopupMessageClear);
    CHECK(tracker->Shutdown());
    CHECK(host.server.removed.size() == 3);
    tracker->Release();
    // Without a message server nothing happens and nothing crashes.
    host.messagesPresent = false;
    auto* lonely = new cSC4AdvisorPopupTracker();
    CHECK(lonely->Init());
    CHECK(lonely->Initialized());
    CHECK(lonely->Shutdown());
    CHECK(lonely->Release() == 0);
    // Without any host at all the engine calls are simply absent.
    SetAdvisorPopupTrackerHost(nullptr);
    auto* bare = new cSC4AdvisorPopupTracker();
    CHECK(bare->Init());
    CHECK(bare->Shutdown());
    CHECK(bare->Release() == 0);
}

// ---------------------------------------------------------------------------------------------------------------
// The clear and activation routes.
// ---------------------------------------------------------------------------------------------------------------
void TestActivationRoute() {
    Host host;
    SetAdvisorPopupTrackerHost(&host);
    cSC4AdvisorPopupTracker tracker;
    Advice advice;

    // 0x26d31ec2 drops the queue; the advice keeps its 0x4000 flag (the original does not clear it).
    advice.SetFlags(kAdvisorPopupAdviceFlagQueued, true);
    cRZMessage2Standard clear;
    clear.SetType(kAdvisorPopupMessageClear);
    advice.refs = 0;
    tracker.Pending().emplace_back(&advice);
    CHECK(advice.refs == 1);
    CHECK(tracker.DoMessage(&clear));
    CHECK(tracker.Pending().empty());
    CHECK(advice.refs == 0);
    CHECK(advice.CheckFlags(kAdvisorPopupAdviceFlagQueued));

    // No unknown on the message: nothing queued.
    cRZMessage2Standard activated;
    activated.SetType(kAdvisorPopupMessageAdviceActivated);
    CHECK(tracker.DoMessage(&activated));
    CHECK(tracker.Pending().empty());

    // An object that is not an advice: nothing queued.
    ImageProc other;
    activated.SetIGZUnknown(&other);
    CHECK(tracker.DoMessage(&activated));
    CHECK(tracker.Pending().empty());

    // An advice that is already on screen and queued (both 0x6000 bits) is ignored, no tick is posted.
    advice.flags = kAdvisorPopupAdviceFlagPopupMask;
    activated.SetIGZUnknown(&advice);
    const std::size_t posts = host.server.posts.size();
    CHECK(tracker.DoMessage(&activated));
    CHECK(tracker.Pending().empty());
    CHECK(host.server.posts.size() == posts);

    // The 0x4000 ("queued") bit alone does not stop a second activation: the original tests both 0x6000 bits, so an
    // advice the tracker already holds is queued twice. Kept and asserted.
    advice.flags = kAdvisorPopupAdviceFlagQueued;
    CHECK(tracker.DoMessage(&activated));
    CHECK(tracker.Pending().size() == 1);
    tracker.Pending().clear();
    CHECK(advice.refs == 1);   // the message payload still holds its reference
    CHECK(host.server.posts.size() == posts + 1);
    CHECK(host.server.posts.back().type == kAdvisorPopupMessageShow);

    // A new advice is queued, flagged 0x4000 and the show tick is posted to the tracker.
    advice.flags = 0;
    advice.flagChanges.clear();
    CHECK(tracker.DoMessage(&activated));
    CHECK(tracker.Pending().size() == 1);
    CHECK(tracker.Pending().front().get() == &advice);
    CHECK(advice.refs == 2);   // the queue and the message payload
    CHECK(advice.flags == kAdvisorPopupAdviceFlagQueued);
    CHECK(advice.flagChanges.size() == 1);
    CHECK(advice.flagChanges[0].first == kAdvisorPopupAdviceFlagQueued && advice.flagChanges[0].second);
    CHECK(host.server.posts.size() == posts + 2);
    CHECK(host.server.posts.back().type == kAdvisorPopupMessageShow);
    CHECK(host.server.posts.back().target == static_cast<cIGZMessageTarget2*>(&tracker));
    // An unrelated message type is ignored (and answered true).
    cRZMessage2Standard otherMessage;
    otherMessage.SetType(0x12345678);
    CHECK(tracker.DoMessage(&otherMessage));
    tracker.Pending().clear();
    CHECK(advice.refs == 1);   // only the message payload is left
}

// ---------------------------------------------------------------------------------------------------------------
// The show tick's suppression rules: the advice stays queued and the tick re-posts itself, or it is popped and
// dropped without a window.
// ---------------------------------------------------------------------------------------------------------------
void TestShowTickSuppression() {
    Host host;
    SetAdvisorPopupTrackerHost(&host);
    cSC4AdvisorPopupTracker tracker;
    Advice advice;
    advice.refs = 0;
    cRZMessage2Standard tick;
    tick.SetType(kAdvisorPopupMessageShow);

    // An empty queue does nothing.
    CHECK(tracker.DoMessage(&tick));
    CHECK(host.server.posts.empty());
    CHECK(host.keys.empty());

    // No advisor system in the city: the advice stays queued and nothing is posted.
    tracker.Pending().emplace_back(&advice);
    host.advisorSystemPresent = false;
    std::size_t posts = host.server.posts.size();
    CHECK(tracker.DoMessage(&tick));
    CHECK(tracker.Pending().size() == 1);
    CHECK(host.server.posts.size() == posts);
    CHECK(host.keys.empty());
    host.advisorSystemPresent = true;

    // A paused simulator: the advice stays queued and the tick is re-posted.
    host.simulatorPaused = true;
    CHECK(tracker.DoMessage(&tick));
    CHECK(tracker.Pending().size() == 1);
    CHECK(host.server.posts.size() == posts + 1);
    CHECK(host.server.posts.back().type == kAdvisorPopupMessageShow);
    CHECK(host.keys.empty());
    host.simulatorPaused = false;

    // An unsafe moment behaves the same.
    posts = host.server.posts.size();
    host.safe = false;
    CHECK(tracker.DoMessage(&tick));
    CHECK(tracker.Pending().size() == 1);
    CHECK(host.server.posts.size() == posts + 1);
    CHECK(host.keys.empty());
    host.safe = true;

    // From here on the advice is popped; the window is never created while a rule suppresses it.
    struct Case {
        bool Host::*flag;
        bool suppressing;
    } cases[] = {{&Host::dialogsEnabled, false},
                 {&Host::preference, false},
                 {&Host::tutorial, true},
                 {&Host::vehiclePaused, true}};
    for (const Case& c : cases) {
        advice.flags = kAdvisorPopupAdviceFlagQueued;
        advice.refs = 0;
        tracker.Pending().clear();
        tracker.Pending().emplace_back(&advice);
        host.*(c.flag) = c.suppressing;
        const std::size_t before = host.keys.size();
        CHECK(tracker.DoMessage(&tick));
        CHECK(tracker.Pending().empty());
        CHECK(host.keys.size() == before);   // no window
        CHECK(!advice.CheckFlags(kAdvisorPopupAdviceFlagQueued));
        host.*(c.flag) = !c.suppressing;
    }
    // Below the priority threshold: popped and dropped.
    advice.flags = kAdvisorPopupAdviceFlagQueued;
    advice.refs = 0;
    tracker.Pending().clear();
    tracker.Pending().emplace_back(&advice);
    host.threshold = advice.priority + 1;
    const std::size_t before = host.keys.size();
    CHECK(tracker.DoMessage(&tick));
    CHECK(tracker.Pending().empty());
    CHECK(host.keys.size() == before);
    CHECK(!advice.CheckFlags(kAdvisorPopupAdviceFlagQueued));
    // Equal to the threshold passes, but the window cannot be created (the host has none): the advice is still popped
    // and no tick is posted for the now-empty queue.
    host.threshold = advice.priority;
    advice.flags = kAdvisorPopupAdviceFlagQueued;
    advice.refs = 0;
    tracker.Pending().clear();
    tracker.Pending().emplace_back(&advice);
    posts = host.server.posts.size();
    CHECK(tracker.DoMessage(&tick));
    CHECK(tracker.Pending().empty());
    CHECK(host.keys.size() == before + 1);
    CHECK(advice.refs == 0);
    CHECK(host.server.posts.size() == posts);
}

// ---------------------------------------------------------------------------------------------------------------
// The script window, its parent and children, and the recovered HTML window with a known fill colour and area.
// ---------------------------------------------------------------------------------------------------------------
struct Rig {
    Manager manager;
    cGZWin parent{&manager};
    Dialog dialog{&manager};
    cGZWin htmlWindow{&manager};
    Html html;
    Gen imageWindow;
    Gen scriptWindow;
    MoveRecorder layout{&manager};

    Rig() {
        parent.SetArea(0, 0, 800, 600);
        dialog.SetArea(0, 0, 400, 300);
        dialog.SetParentWin(&parent);
        htmlWindow.SetArea(10, 0, 110, 100);   // 100 x 100 at x = 10
        htmlWindow.SetFillColor(static_cast<std::uint8_t>(0x12), static_cast<std::uint8_t>(0x34),
                                static_cast<std::uint8_t>(0x56));
        layout.SetArea(0, 50, 100, 60);
        html.window = &htmlWindow;
        dialog.html = &html;
        dialog.image = &imageWindow;
        dialog.layout = &layout;
        imageWindow.window = &dialog;
        scriptWindow.window = &dialog;
    }
    // The window keeps the advice variant it was given as a parameter; a window that was never initialized does not
    // release it on shutdown.
    ~Rig() { dialog.SetParam(kAdvisorPopupParamAdvice, nullptr); }
};

// ---------------------------------------------------------------------------------------------------------------
// The tick that builds the dialog: the script key, the game pause, the window procedure, SetupDialog, the modal run,
// the focus and the 0x1000 -> 0x8 flag rule.
// ---------------------------------------------------------------------------------------------------------------
void TestShowDialog() {
    Host host;
    SetAdvisorPopupTrackerHost(&host);
    Rig rig;
    host.manager = rig.manager;
    host.window = &rig.scriptWindow;
    host.titleFont.name = cRZString("TitleFace");
    host.titleFont.bold = true;
    host.titleFont.size = 22;   // (22*4 + 8) / 18 = 5
    host.messageFont.name = cRZString("MsgFace");
    host.messageFont.bold = false;
    host.messageFont.size = 13; // (13*4 + 8) / 18 = 3
    cSC4AdvisorPopupTracker tracker;
    Advice advice;
    advice.refs = 0;
    advice.flags = kAdvisorPopupAdviceFlagQueued;
    advice.mood = 3;
    tracker.Pending().emplace_back(&advice);
    cRZMessage2Standard tick;
    tick.SetType(kAdvisorPopupMessageShow);
    const std::size_t posts = host.server.posts.size();
    CHECK(tracker.DoMessage(&tick));

    // The script key: group 0x96a006b0, instance 0x4a5a89d5 (mood >= 3), window id and cIGZWinGen iid.
    CHECK(host.keys.size() == 1);
    CHECK(host.keys[0].type == 0);
    CHECK(host.keys[0].group == kAdvisorPopupScriptGroup);
    CHECK(host.keys[0].instance == kAdvisorPopupScriptInstance);
    CHECK(host.windowIds.size() == 1 && host.windowIds[0] == kAdvisorPopupWindowId);
    CHECK(host.windowIids.size() == 1 && host.windowIids[0] == gz::kGZIID_cIGZWinGen);
    // The game is paused around the dialog, and the window procedure is the tracker's +8 view.
    CHECK(host.pauses == 2);
    CHECK(rig.scriptWindow.procs.size() == 1);
    CHECK(rig.scriptWindow.procs[0] == static_cast<gz::cIGZWinProc*>(&tracker));
    // SetupDialog built the page: the title in bold, the message plain, the mood palette and the fill colour.
    CHECK(rig.html.baseUrl == "sc4://");
    CHECK(rig.html.document ==
          "<HTML><HEAD></HEAD><BODY TEXT=\"#394472\" BGCOLOR=\"#123456\" LINK=\"#0000ff\">"
          "<P><FONT FACE=\"TitleFace\" SIZE=5><B>TITLE</B></FONT></P>"
          "<P><FONT FACE=\"MsgFace\" SIZE=3>MESSAGE</FONT></P></BODY></HTML>");
    CHECK(rig.html.clearedFlags.size() == 1 && rig.html.clearedFlags[0] == kHtmlOutline);
    CHECK(rig.html.refs == 0);
    // The dialog grew by the document height (+0x14) minus the HTML window's "height", then was centred in its parent.
    CHECK(rig.dialog.GetW() == 400);
    CHECK(rig.dialog.GetH() == 310);   // 300 + (100 + 0x14 - (110 - 0))
    CHECK(rig.htmlWindow.GetW() == 90);   // bottom - left
    CHECK(rig.htmlWindow.GetH() == 120);  // (right - top) + extra
    CHECK(rig.layout.moves.size() == 1);
    CHECK(rig.layout.moves[0].first == 0 && rig.layout.moves[0].second == 60);   // top 50 + 10
    CHECK(rig.dialog.moves.size() == 1);
    CHECK(rig.dialog.moves[0].first == (800 - 400) / 2 && rig.dialog.moves[0].second == (600 - 310) / 2);
    // The advice is in the window parameter and the image window procedure is on child 4.
    CHECK(rig.imageWindow.procs.size() == 1);
    cIGZVariant* variant = nullptr;
    CHECK(rig.dialog.GetParam(kAdvisorPopupParamAdvice, &variant));
    CHECK(variant != nullptr);
    cISC4Advice* stored = nullptr;
    CHECK(variant && variant->GetValUnknown(kGZIID_cISC4Advice, reinterpret_cast<void**>(&stored)));
    CHECK(stored == &advice);
    if (stored) stored->Release();
    if (variant) variant->Release();
    CHECK(host.images.size() == 1);
    CHECK(host.images[0]->advisorId == advice.advisorId);
    CHECK(host.images[0]->refs == 1);   // the tracker's reference
    CHECK(rig.imageWindow.procs[0] == static_cast<gz::cIGZWinProc*>(host.images[0].get()));
    CHECK(host.images[0]->enables.size() == 1 && host.images[0]->enables[0]);
    CHECK(host.images[0]->moods.size() == 1 && host.images[0]->moods[0] == advice.mood);
    CHECK(tracker.ImageWinProc() == host.images[0].get());
    // The modal run, the focus and the flag rules.
    CHECK(host.manager.popups.size() == 1);
    CHECK(host.manager.popups[0] == static_cast<cIGZWin*>(&rig.dialog));
    CHECK(host.focusCalls == 1);
    CHECK(!advice.CheckFlags(kAdvisorPopupAdviceFlagQueued));   // 0x4000 cleared after the dialog
    CHECK(rig.scriptWindow.refs == 0);                          // the created window was released
    CHECK(tracker.Pending().empty());
    CHECK(host.server.posts.size() == posts);                   // nothing queued: no re-post
    // The 0x1000 rule: an advice carrying it gets the no-link flag (0x8) once its dialog closed.
    Advice quiet;
    quiet.refs = 0;
    quiet.flags = kAdvisorPopupAdviceFlagQueued | kAdvisorPopupAdviceFlagNoLinksWhenDone;
    tracker.Pending().emplace_back(&quiet);
    CHECK(tracker.DoMessage(&tick));
    CHECK(quiet.CheckFlags(kAdviceFlagNoLinks));
    CHECK(!quiet.CheckFlags(kAdvisorPopupAdviceFlagQueued));
    // A bad mood uses the previous script instance and the red palette.
    rig.html.document.clear();
    rig.html.clearedFlags.clear();
    Advice angry;
    angry.refs = 0;
    angry.flags = kAdvisorPopupAdviceFlagQueued;
    angry.mood = 1;
    tracker.Pending().emplace_back(&angry);
    CHECK(tracker.DoMessage(&tick));
    CHECK(host.keys.size() == 3);
    CHECK(host.keys[2].instance == kAdvisorPopupScriptInstance - 1);
    CHECK(rig.html.document ==
          "<HTML><HEAD></HEAD><BODY TEXT=\"#723939\" BGCOLOR=\"#123456\" LINK=\"#ff0000\">"
          "<P><FONT FACE=\"TitleFace\" SIZE=5><B>TITLE</B></FONT></P>"
          "<P><FONT FACE=\"MsgFace\" SIZE=3>MESSAGE</FONT></P></BODY></HTML>");
    // The image window procedure of the new dialog replaced the previous one (and the tracker released that one).
    CHECK(host.images.size() == 3);
    CHECK(host.images[0]->refs == 0);
    CHECK(host.images[1]->refs == 0);
    CHECK(tracker.ImageWinProc() == host.images[2].get());
    rig.dialog.SetParam(kAdvisorPopupParamAdvice, nullptr);   // while the advice objects are still alive
}

// ---------------------------------------------------------------------------------------------------------------
// SetupDialog on its own: the font fallbacks, the image window procedure replacement, the missing children and the
// height clamp.
// ---------------------------------------------------------------------------------------------------------------
void TestSetupDialog() {
    Host host;
    SetAdvisorPopupTrackerHost(&host);
    cSC4AdvisorPopupTracker tracker;
    Rig rig;
    Advice advice;
    advice.refs = 0;

    // A missing font: the "Arta" default, sizes 4 / 3 and no bold tags.
    host.fontsPresent = false;
    tracker.SetupDialog(&rig.dialog, &advice);
    CHECK(rig.html.document ==
          "<HTML><HEAD></HEAD><BODY TEXT=\"#394472\" BGCOLOR=\"#123456\" LINK=\"#0000ff\">"
          "<P><FONT FACE=\"Arta\" SIZE=4>TITLE</FONT></P>"
          "<P><FONT FACE=\"Arta\" SIZE=3>MESSAGE</FONT></P></BODY></HTML>");
    // No parent and no HTML child: the advice parameter and the image window procedure are still created.
    Dialog orphan(&host.manager);
    orphan.image = &rig.imageWindow;
    tracker.SetupDialog(&orphan, &advice);
    cIGZVariant* variant = nullptr;
    CHECK(orphan.GetParam(kAdvisorPopupParamAdvice, &variant));
    if (variant) variant->Release();
    CHECK(tracker.ImageWinProc() != nullptr);
    // Setting up another dialog creates a fresh procedure and releases the previous one.
    ImageProc* first = static_cast<ImageProc*>(tracker.ImageWinProc());
    CHECK(first->refs == 1);
    tracker.SetupDialog(&orphan, &advice);
    CHECK(host.images.size() == 3);
    CHECK(first->refs == 0);
    CHECK(tracker.ImageWinProc() == host.images[2].get());
    // The height clamp: a document of 500 + 0x14 needs 520 - 110 = 410 extra, but the dialog may not exceed 0x1c2.
    rig.html.documentHeight = 500;
    rig.dialog.SetArea(0, 0, 400, 300);
    rig.htmlWindow.SetArea(10, 0, 110, 100);
    tracker.SetupDialog(&rig.dialog, &advice);
    CHECK(rig.dialog.GetH() == kAdvisorPopupMaxDialogHeight);
    CHECK(rig.htmlWindow.GetH() == 110 + (kAdvisorPopupMaxDialogHeight - 300));
    // A document that fits does not resize at all (extra <= 0).
    rig.html.documentHeight = 10;
    rig.dialog.SetArea(0, 0, 400, 300);
    rig.htmlWindow.SetArea(10, 0, 110, 100);
    tracker.SetupDialog(&rig.dialog, &advice);
    CHECK(rig.dialog.GetH() == 300);
    // Null arguments are ignored.
    tracker.SetupDialog(nullptr, &advice);
    tracker.SetupDialog(&rig.dialog, nullptr);
    orphan.SetParam(kAdvisorPopupParamAdvice, nullptr);
    rig.dialog.SetParam(kAdvisorPopupParamAdvice, nullptr);
}

// ---------------------------------------------------------------------------------------------------------------
// DoWinProcMessage: the advice flag rules, the link jump, the install / detach messages and the close command.
// ---------------------------------------------------------------------------------------------------------------
void TestDoWinProcMessage() {
    Host host;
    SetAdvisorPopupTrackerHost(&host);
    cSC4AdvisorPopupTracker tracker;
    Rig rig;
    Advice advice;
    advice.refs = 0;
    cGZMessage message;

    // No parameter at all: the message routing still runs.
    message.type = 0x12345678;
    CHECK(!tracker.DoWinProcMessage(&rig.dialog, message));
    message.type = kAdvisorPopupWinProcAttach;
    CHECK(tracker.DoWinProcMessage(&rig.dialog, message));
    message.type = kAdvisorPopupWinProcDetach;
    CHECK(tracker.DoWinProcMessage(&rig.dialog, message));
    message.type = kGZMsgCommand;
    message.param1 = gz::kWinCommandSender;
    CHECK(tracker.DoWinProcMessage(&rig.dialog, message));
    CHECK(host.manager.dialogs.size() == 1);
    CHECK(host.manager.dialogs[0].code == 0xf);
    CHECK(host.manager.dialogs[0].window == static_cast<cIGZWin*>(&rig.dialog));
    CHECK(host.manager.dialogs[0].result == 0);
    CHECK(host.manager.destroyed.size() == 1);
    CHECK(host.manager.destroyed[0] == static_cast<cIGZWin*>(&rig.dialog));
    // A command from another sender does not close it.
    message.param1 = 0x12345678;
    CHECK(!tracker.DoWinProcMessage(&rig.dialog, message));
    CHECK(host.manager.dialogs.size() == 1);

    // With the advice in the window parameter, current and link-handling: the dialog stays open.
    rig.dialog.SetParam(kAdvisorPopupParamAdvice, new cRZVariant(&advice));
    advice.flags = kAdviceFlagCurrent;
    message = cGZMessage{};
    message.type = 0x12345678;
    CHECK(!tracker.DoWinProcMessage(&rig.dialog, message));
    CHECK(host.manager.dialogs.size() == 1);
    // A link jump: the handler is asked and the dialog closes; the answer is the negation of the handler's.
    cRZString link("action/advice/1");
    message.type = kWinHtmlMsgLinkJump;
    message.param2 = reinterpret_cast<std::uint64_t>(&link);
    host.doUrlAnswer = false;
    CHECK(tracker.DoWinProcMessage(&rig.dialog, message));
    CHECK(host.lastLink == &link);
    CHECK(host.manager.dialogs.size() == 2);
    rig.dialog.SetParam(kAdvisorPopupParamAdvice, new cRZVariant(&advice));
    host.doUrlAnswer = true;
    CHECK(!tracker.DoWinProcMessage(&rig.dialog, message));
    CHECK(host.manager.dialogs.size() == 3);
    // The no-link flag (0x8) closes the dialog on the first message.
    rig.dialog.SetParam(kAdvisorPopupParamAdvice, new cRZVariant(&advice));
    advice.flags = kAdviceFlagCurrent | kAdviceFlagNoLinks;
    message = cGZMessage{};
    message.type = 0x12345678;
    CHECK(tracker.DoWinProcMessage(&rig.dialog, message));
    CHECK(host.manager.dialogs.size() == 4);
    // An advice that is no longer current closes it too.
    rig.dialog.SetParam(kAdvisorPopupParamAdvice, new cRZVariant(&advice));
    advice.flags = 0;
    CHECK(tracker.DoWinProcMessage(&rig.dialog, message));
    CHECK(host.manager.dialogs.size() == 5);
    // The parameter and the image window procedure are gone after the close.
    cIGZVariant* variant = nullptr;
    CHECK(!rig.dialog.GetParam(kAdvisorPopupParamAdvice, &variant));
    CHECK(tracker.ImageWinProc() == nullptr);
    CHECK(host.manager.destroyed.size() == 5);
}

// ---------------------------------------------------------------------------------------------------------------
// EndDialog: the image window procedure is disabled and released, then the window is ended and destroyed.
// ---------------------------------------------------------------------------------------------------------------
void TestEndDialog() {
    Host host;
    SetAdvisorPopupTrackerHost(&host);
    cSC4AdvisorPopupTracker tracker;
    Rig rig;
    Advice advice;
    advice.refs = 0;
    // Build a dialog so the tracker owns an image window procedure.
    tracker.SetupDialog(&rig.dialog, &advice);
    CHECK(tracker.ImageWinProc() != nullptr);
    ImageProc* proc = static_cast<ImageProc*>(tracker.ImageWinProc());
    CHECK(proc->refs == 1);
    tracker.EndDialog(&rig.dialog);
    CHECK(proc->enables.size() == 2);   // SetupDialog enabled it, EndDialog disabled it
    CHECK(proc->enables[0] && !proc->enables[1]);
    CHECK(proc->refs == 0);
    CHECK(tracker.ImageWinProc() == nullptr);
    cIGZVariant* variant = nullptr;
    CHECK(!rig.dialog.GetParam(kAdvisorPopupParamAdvice, &variant));
    CHECK(host.manager.dialogs.size() == 1 && host.manager.dialogs[0].code == 0xf);
    CHECK(host.manager.destroyed.size() == 1 && host.manager.destroyed[0] == static_cast<cIGZWin*>(&rig.dialog));
    // A second EndDialog without an image window procedure does not crash (the port's null guard) and still ends the
    // dialog.
    tracker.EndDialog(&rig.dialog);
    CHECK(host.manager.dialogs.size() == 2);
    // No host: the window manager calls are simply absent.
    SetAdvisorPopupTrackerHost(nullptr);
    tracker.EndDialog(&rig.dialog);
    CHECK(host.manager.dialogs.size() == 2);
}

}  // namespace

int main() {
    TestLayout();
    TestReferenceCounting();
    TestInitShutdown();
    TestActivationRoute();
    TestShowTickSuppression();
    TestShowDialog();
    TestSetupDialog();
    TestDoWinProcMessage();
    TestEndDialog();
    SetAdvisorPopupTrackerHost(nullptr);
    if (failures == 0) {
        std::printf("sc4_advisorpopuptracker: all checks passed\n");
        return 0;
    }
    std::printf("sc4_advisorpopuptracker: %d failure(s)\n", failures);
    return 1;
}
