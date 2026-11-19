// cSC4TutorialSystem: the object layout and the five QueryInterface views, the constructor / destructor, Init / Shutdown
// (the three message subscriptions and the debug-gated "Tutorial" cheat), startTutorial / endTutorial with the
// two-reference window-procedure quirk, the message routes (tutorial finished / city loaded / city view started) and
// the "Tutorial reset|save|step|next|quit" cheat verbs, FilterMessage / the forwarding slots, the registry GUID
// conversion in Read / Write, the scripting-context cache, the process-wide instance quirk, and the file's free
// nsSC4TutorialSystem::FilterUICommandMessage.
//
// All engine services come in through ITutorialSystemHost; the tutorial window procedure, the task, the Lua view, the
// message server, the cheat manager and the registry are recording fakes. The streams are the port-only in-memory
// ones (rz/PortMemoryStreams.h) plus a recording cRZOStream so the written bytes can be checked.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gz/cIGZString.h"
#include "gz/cIGZMessage2.h"
#include "rz/PortMemoryStreams.h"
#include "rz/cRZOStream.h"
#include "rz/cRZString.h"
#include "sc4/TutorialSystem.h"
#include "sc4/TutorialTask.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace sc4;

namespace {

// ---------------------------------------------------------------------------------------------------------------
// A message with the cIGZMessage2Standard payload (only the parts the tutorial system reads are meaningful).
// ---------------------------------------------------------------------------------------------------------------
struct FakeMessage final : cIGZMessage2Standard {
    std::uint32_t type = 0;
    std::uint64_t data1 = 0;
    int refs = 0;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZMessage2Standard && iid != kGZIID_cIGZUnknown) return false;
        *out = static_cast<cIGZMessage2Standard*>(this);
        ++refs;
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Create(std::uint32_t, void**) const override { return false; }
    std::uint32_t GetType() const override { return type; }
    void SetType(std::uint32_t value) override { type = value; }
    bool IsEqual(const cIGZMessage2&) const override { return false; }
    bool IsLess(const cIGZMessage2&) const override { return false; }
    bool GetHasData1() const override { return true; }
    std::uint64_t GetData1() const override { return data1; }
    void* GetVoid1() const override { return nullptr; }
    void SetData1(std::uint64_t value) override { data1 = value; }
    void SetVoid1(void*) override {}
    bool GetHasData2() const override { return false; }
    std::uint64_t GetData2() const override { return 0; }
    void* GetVoid2() const override { return nullptr; }
    void SetData2(std::uint64_t) override {}
    void SetVoid2(void*) override {}
    bool GetHasData3() const override { return false; }
    std::uint64_t GetData3() const override { return 0; }
    void* GetVoid3() const override { return nullptr; }
    void SetData3(std::uint64_t) override {}
    void SetVoid3(void*) override {}
    bool GetHasData4() const override { return false; }
    std::uint64_t GetData4() const override { return 0; }
    void* GetVoid4() const override { return nullptr; }
    void SetData4(std::uint64_t) override {}
    void SetVoid4(void*) override {}
    bool GetHasString() const override { return false; }
    bool GetString(std::uint32_t, void**) override { return false; }
    void SetString(cIGZString*) override {}
    bool GetHasIGZUnknown() const override { return false; }
    cIGZUnknown* GetIGZUnknown() override { return nullptr; }
    void SetIGZUnknown(cIGZUnknown*) override {}
    bool GetHasExtra() const override { return false; }
    bool GetExtra(std::uint32_t, void**) override { return false; }
    void SetExtra(cIGZSerializable*) override {}
};

// ---------------------------------------------------------------------------------------------------------------
// The message server: records every notification add / remove with its target so the subscriptions can be checked.
// ---------------------------------------------------------------------------------------------------------------
class FakeMessages final : public cIGZMessageServer2 {
public:
    struct Notification {
        cIGZMessageTarget2* target;
        std::uint32_t type;
        bool add;
    };
    std::vector<Notification> notifications;
    int refs = 0;

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool MessageSend(cIGZMessage2*) override { return true; }
    bool MessagePost(cIGZMessage2*, bool) override { return true; }
    bool AddNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        notifications.push_back({target, type, true});
        return true;
    }
    bool RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        notifications.push_back({target, type, false});
        return true;
    }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return static_cast<std::uint32_t>(refs); }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

// ---------------------------------------------------------------------------------------------------------------
// The cheat-code manager, reduced to the four slots the tutorial system calls.
// ---------------------------------------------------------------------------------------------------------------
class FakeCheats final : public ITutorialCheatCodes {
public:
    struct Call {
        enum Kind { Register, Unregister, AddNotification, RemoveNotification } kind;
        std::uint32_t id;
        std::string text;
        cIGZMessageTarget* target;
    };
    std::vector<Call> calls;

    void RegisterCheatCode(std::uint32_t id, const char* text) override {
        calls.push_back({Call::Register, id, text ? text : "", nullptr});
    }
    void UnregisterCheatCode(std::uint32_t id) override { calls.push_back({Call::Unregister, id, "", nullptr}); }
    void AddNotification(cIGZMessageTarget* target, std::uint32_t id) override {
        calls.push_back({Call::AddNotification, id, "", target});
    }
    void RemoveNotification(cIGZMessageTarget* target, std::uint32_t id) override {
        calls.push_back({Call::RemoveNotification, id, "", target});
    }
};

// ---------------------------------------------------------------------------------------------------------------
// The tutorial window procedure: counts the references (the tutorial system takes two) and records every call.
// Release does not delete: the test owns the object, like the other recovered-class fakes.
// ---------------------------------------------------------------------------------------------------------------
class FakeWinProc final : public ITutorialUIWinProc {
public:
    cISC4TutorialTask* task = nullptr;
    int addRefs = 0;
    int releases = 0;
    int inits = 0;
    int shutdowns = 0;
    std::vector<std::uint32_t> buttons;
    int hints = 0;
    std::vector<cISC4WinCatalogView*> catalogViews;
    std::vector<std::pair<int, int>> resets;       // (guid, index)
    int nextCalls = 0;
    std::vector<std::uint32_t> events;
    std::vector<std::pair<int, int>> leftClicks;   // (flags, unused) — x / y are also recorded
    std::vector<std::pair<int, int>> leftClickPoints;
    bool leftClickAnswer = true;
    int nextTaskNumber = 100;

    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++addRefs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(++releases); }
    void Init() override { ++inits; }
    void Shutdown() override { ++shutdowns; }
    void handleButtons(std::uint32_t command) override { buttons.push_back(command); }
    void ShowUIHintsForUsers() override { ++hints; }
    void HandleCatalogView(cISC4WinCatalogView* view) override { catalogViews.push_back(view); }
    cISC4TutorialTask* GetCurrentTask() override { return task; }
    bool FilterLeftMouseClick(int& x, int& y, int flags) override {
        leftClicks.push_back({flags, 0});
        leftClickPoints.push_back({x, y});
        return leftClickAnswer;
    }
    int ResetTutorialNumAndIndex(int guid, int index) override {
        resets.push_back({guid, index});
        return nextTaskNumber + index;
    }
    void moveToNext() override { ++nextCalls; }
    bool OnTutorialEvent(std::uint32_t event) override { events.push_back(event); return true; }
};

// ---------------------------------------------------------------------------------------------------------------
// The tutorial task, reduced to the one predicate the system asks for (plus the edge-scrolling answer).
// ---------------------------------------------------------------------------------------------------------------
class FakeTask final : public cISC4TutorialTask {
public:
    bool edgeScrolling = true;
    int refs = 0;

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Init(cISCLua*, int) override { return true; }
    bool Shutdown() override { return true; }
    const SC4TutorialRect& GetRegion() const override { return region_; }
    std::uint32_t GetGUID() const override { return 0; }
    bool HasButtonGUID(std::uint32_t) override { return false; }
    const cIGZString& GetInstructionMsg() const override { return text_; }
    const cIGZString& GetTryAgainMsg() const override { return text_; }
    const cIGZString& GetCongratulationMsg() const override { return text_; }
    std::uint32_t GetTargetGUID() const override { return 0; }
    cIGZWin* GetTargetWindow() override { return nullptr; }
    bool DoLink(const cIGZString&) override { return true; }
    bool ExecuteTask() override { return true; }
    void ShowHintsForUsers() override {}
    bool EndTask() override { return true; }
    bool drawDecals(int, int, int, int) override { return true; }
    std::uint32_t GetExtraData() const override { return 0; }
    std::uint32_t GetCatalogButtonItemID() const override { return 0; }
    int GetWhichFunction() const override { return 0; }
    bool handleButtons(std::uint32_t) override { return true; }
    int GetZoomLevel() const override { return 0; }
    int GetWindowOffsetX() const override { return 0; }
    int GetWindowOffsetY() const override { return 0; }
    bool IsEdgeScrollingAllowed() const override { return edgeScrolling; }
    bool TriggerFire(const SC4TutorialRect&) override { return true; }

private:
    SC4TutorialRect region_;
    cRZString text_;
};

// ---------------------------------------------------------------------------------------------------------------
// The cISCLua view (the system only caches / references it, so every scripting slot is a stub).
// ---------------------------------------------------------------------------------------------------------------
class FakeLua final : public cISCLua {
public:
    int refs = 0;

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(refs > 0 ? --refs : 0); }
    bool Open(int) override { return true; }
    bool Close() override { return true; }
    void* AsIGZLua5() override { return nullptr; }
    int GetTop() override { return 0; }
    void SetTop(int) override {}
    void PushValue(int) override {}
    void Remove(int) override {}
    void Insert(int) override {}
    void Pop(int) override {}
    int Type(int) override { return 0; }
    const char* TypeName(int) override { return "nil"; }
    bool IsNumber(int) override { return false; }
    bool IsString(int) override { return false; }
    bool IsCFunction(int) override { return false; }
    int Tag(int) override { return 0; }
    bool IsBoolean(int) override { return false; }
    bool Equal(int, int) override { return false; }
    bool LessThan(int, int) override { return false; }
    std::uint32_t ToUint32(int) override { return 0; }
    std::int32_t ToSint32(int) override { return 0; }
    double ToNumber(int) override { return 0; }
    const char* ToString(int) override { return ""; }
    int Strlen(int) override { return 0; }
    CFunction ToCFunction(int) override { return nullptr; }
    void* ToUserData(int) override { return nullptr; }
    void* ToUserTag(int) override { return nullptr; }
    bool ToBoolean(int) override { return false; }
    void PushNil() override {}
    void PushNumber(double) override {}
    void PushLString(const char*, int) override {}
    void PushString(const char*) override {}
    void PushCClosure(CFunction, int) override {}
    void PushUserTag(void*, int) override {}
    void PushBoolean(bool) override {}
    void GetGlobal(const char*) override {}
    void GetTable(int) override {}
    void RawGet(int) override {}
    void RawGetI(int, int) override {}
    void GetGlobals() override {}
    void GetTagMethod(int, const char*) override {}
    bool GetRef(int) override { return false; }
    void NewTable() override {}
    void SetGlobal(const char*) override {}
    void SetTable(int) override {}
    void RawSet(int) override {}
    void RawSetI(int, int) override {}
    void SetTagMethod(int, const char*) override {}
    int Ref(int) override { return 0; }
    int CallProtected(int, int) override { return 0; }
    void RawCall(int, int) override {}
    int DoFile(const char*) override { return 0; }
    int DoString(const char*) override { return 0; }
    int DoBuffer(const char*, int, const char*) override { return 0; }
    int GetGCCount() override { return 0; }
    int GetGCThreshold() override { return 0; }
    void SetGCThreshold(int) override {}
    int NewTag() override { return 0; }
    void CopyTagMethods(int, int) override {}
    void SetTag(int) override {}
    void Unref(int) override {}
    int Next(int) override { return 0; }
    int GetN(int) override { return 0; }
    void Error(const char*) override {}
    void Concat(int) override {}
    void* NewUserData(int) override { return nullptr; }
    void Register(CFunction, const char*) override {}
    void PushUserData(void*, int) override {}
    int CloneTag(int) override { return 0; }
    bool IsFunction(int) override { return false; }
    bool IsTable(int) override { return false; }
    bool IsUserData(int) override { return false; }
    bool IsNil(int) override { return true; }
    bool ToDataRegistryEntryID(cISC4GameDataRegistry*, int) override { return false; }
    bool ToDataRegistryNode(cISC4GameDataRegistry*, int) override { return false; }
    bool ToDataRegistryValue(cISC4GameDataRegistry*, int) override { return false; }
};

// ---------------------------------------------------------------------------------------------------------------
// The host: one recording method per original global / call chain.
// ---------------------------------------------------------------------------------------------------------------
class FakeHost final : public ITutorialSystemHost {
public:
    FakeMessages* messages = nullptr;
    FakeCheats* cheats = nullptr;
    bool debugEnabled = true;
    FakeLua* lua = nullptr;

    std::vector<std::pair<int, bool>> setActive;    // (guid, active)
    std::map<int, bool> activeItems;                // TutorialItemActive answers
    std::vector<int> whichToGUID;                   // inputs
    std::vector<int> resetCity;
    std::vector<int> cityGUID;
    int saveRequests = 0;
    std::vector<int> menuCloses;                    // 1 = tertiary, 2 = secondary
    int createWinProcCalls = 0;
    std::vector<std::unique_ptr<FakeWinProc>> winProcs;

    cIGZMessageServer2* Messages() override { return messages; }
    ITutorialCheatCodes* CheatCodes() override { return cheats; }
    bool DebugFunctionalityEnabled() override { return debugEnabled; }
    bool TutorialRegistrySetActive(int guid, bool active) override {
        setActive.push_back({guid, active});
        return true;
    }
    bool TutorialItemActive(int guid) override {
        auto it = activeItems.find(guid);
        return it != activeItems.end() && it->second;
    }
    int TutorialGUIDFromStoredValue(int value) override {
        whichToGUID.push_back(value);
        if (value == 0) return kTutorialGUIDDefault;
        if (value == 1) return kTutorialGUIDSecond;
        return value;
    }
    cISCLua* ScriptingContext() override { return lua; }
    void CloseTertiaryMenu() override { menuCloses.push_back(1); }
    void CloseSecondaryMenu() override { menuCloses.push_back(2); }
    void ResetTutorialCity(int guid) override { resetCity.push_back(guid); }
    void SetRegionalCityTutorialGUID(int guid) override { cityGUID.push_back(guid); }
    void RequestSaveCity() override { ++saveRequests; }
    ITutorialUIWinProc* CreateTutorialUIWinProc() override {
        ++createWinProcCalls;
        winProcs.push_back(std::make_unique<FakeWinProc>());
        return winProcs.back().get();
    }
};

// ---------------------------------------------------------------------------------------------------------------
// A recording cRZOStream (the recovered base writes the typed values, the test keeps the bytes and the call order).
// ---------------------------------------------------------------------------------------------------------------
class RecordingOStream final : public cRZOStream {
public:
    RecordingOStream() : cRZOStream(true) {}
    std::vector<unsigned char> bytes;
    std::vector<std::string> calls;
    bool fail = false;

    void Flush() override {}
    bool SetVoid(const void* source, std::uint32_t count) override {
        if (fail) return false;
        const auto* p = static_cast<const unsigned char*>(source);
        bytes.insert(bytes.end(), p, p + count);
        return true;
    }
    bool GetError() const override { return fail; }
    bool SetUint16(std::uint16_t value) override {
        calls.push_back("u16:" + std::to_string(value));
        return cRZOStream::SetUint16(value);
    }
    bool SetUint8(std::uint8_t value) override {
        calls.push_back("u8:" + std::to_string(value));
        return cRZOStream::SetUint8(value);
    }
    bool SetSint32(std::int32_t value) override {
        calls.push_back("i32:" + std::to_string(value));
        return cRZOStream::SetSint32(value);
    }
};

std::vector<unsigned char> MakeTutorialStream(std::uint16_t version, std::uint8_t active, std::int32_t task,
                                              std::int32_t guid) {
    std::vector<unsigned char> out;
    auto put16 = [&](std::uint16_t v) { out.push_back(v & 0xff); out.push_back((v >> 8) & 0xff); };
    auto put32 = [&](std::int32_t v) {
        const auto u = static_cast<std::uint32_t>(v);
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<unsigned char>((u >> (8 * i)) & 0xff));
    };
    put16(version);
    out.push_back(active);
    put32(task);
    put32(guid);
    return out;
}

FakeWinProc* CurrentProc(FakeHost& host) {
    return host.winProcs.empty() ? nullptr : host.winProcs.back().get();
}

cGZMessage CheatMessage(const cRZString& text, std::uint64_t cheatId = kTutorialCheatId) {
    cGZMessage message;
    message.type = kTutorialMessageCheatCode;
    message.param1 = cheatId;
    message.param2 = reinterpret_cast<std::uint64_t>(static_cast<const cIGZString*>(&text));
    return message;
}

}  // namespace

int main() {
    // ---- construction, layout defaults, QueryInterface views ---------------------------------------------------
    FakeMessages messages;
    FakeCheats cheats;
    FakeHost host;
    host.messages = &messages;
    host.cheats = &cheats;
    SetTutorialSystemHost(&host);

    auto* system = new cSC4TutorialSystem();
    CHECK(cSC4TutorialSystem::Instance() == system);
    CHECK(!system->IsTutorial());
    CHECK(system->GetTutorialTaskNumber() == 0);
    CHECK(system->GetTutorialGUID() == kTutorialGUIDNone);
    CHECK(system->Unknown38() == 3);
    CHECK(system->QuitPending() == false);
    CHECK(system->CityViewHandled() == false);
    CHECK(system->ScriptingContextCache() == nullptr);
    CHECK(system->WinProc() == nullptr);
    CHECK(system->GetCurrentTask() == nullptr);
    CHECK(system->IsEdgeScrollingAllowed());
    CHECK(system->GetGZCLSID() == kGZCLSID_cSC4TutorialSystem);

    {
        // QueryInterface on a throw-away object (each successful call takes a reference on the +0x20 subobject)
        auto* views = new cSC4TutorialSystem();
        void* out = nullptr;
        CHECK(views->QueryInterface(kGZIID_cISC4TutorialSystem, &out));
        CHECK(out == static_cast<cISC4TutorialSystem*>(views));
        CHECK(views->QueryInterface(kGZIID_cIGZMessageTarget2, &out));
        CHECK(out == static_cast<cIGZMessageTarget2*>(views));
        CHECK(views->QueryInterface(kGZIID_cGZMessageTargetLegacy, &out));
        CHECK(out == static_cast<cIGZMessageTarget*>(views));
        CHECK(views->QueryInterface(kGZIID_cIGZSerializable, &out));
        CHECK(out == static_cast<cIGZSerializable*>(views));
        CHECK(views->QueryInterface(kGZIID_cIGZUnknown, &out));
        CHECK(out == static_cast<cRZUnknown*>(views));
        CHECK(!views->QueryInterface(0x12345678, &out));
        CHECK(!views->QueryInterface(kGZIID_cISC4TutorialSystem, nullptr));
        // the original's subobject offsets: message target 2 at +8, legacy target at +0x10, serializable at
        // +0x18, cRZUnknown at +0x20
        const auto* base = reinterpret_cast<const char*>(views);
        CHECK(reinterpret_cast<const char*>(static_cast<cIGZMessageTarget2*>(views)) - base == 8);
        CHECK(reinterpret_cast<const char*>(static_cast<cIGZMessageTarget*>(views)) - base == 0x10);
        CHECK(reinterpret_cast<const char*>(static_cast<cIGZSerializable*>(views)) - base == 0x18);
        CHECK(reinterpret_cast<const char*>(static_cast<cRZUnknown*>(views)) - base == 0x20);
        CHECK(views->Release() == 4);   // five QueryInterface references
        CHECK(views->Release() == 3);
        CHECK(views->Release() == 2);
        CHECK(views->Release() == 1);
        CHECK(views->Release() == 0);   // deleted
    }

    // ---- the script-number accessors -------------------------------------------------------------------------
    system->ResetTutorialIndex(4);
    CHECK(system->GetTutorialTaskNumber() == 4);
    system->IncreaseTutorialNumber();
    CHECK(system->GetTutorialTaskNumber() == 5);

    // ---- Init: the three message subscriptions and the debug-gated cheat -------------------------------------
    CHECK(system->Init());
    CHECK(messages.notifications.size() == 3);
    CHECK(messages.notifications[0].add && messages.notifications[0].type == kTutorialMessageFinished);
    CHECK(messages.notifications[1].add && messages.notifications[1].type == kTutorialMessageCityViewStarted);
    CHECK(messages.notifications[2].add && messages.notifications[2].type == kTutorialMessageCityLoaded);
    CHECK(messages.notifications[0].target == static_cast<cIGZMessageTarget2*>(system));
    CHECK(cheats.calls.size() == 2);
    CHECK(cheats.calls[0].kind == FakeCheats::Call::AddNotification);
    CHECK(cheats.calls[0].target == static_cast<cIGZMessageTarget*>(system));
    CHECK(cheats.calls[0].id == 0);
    CHECK(cheats.calls[1].kind == FakeCheats::Call::Register);
    CHECK(cheats.calls[1].id == kTutorialCheatId);
    CHECK(cheats.calls[1].text == "Tutorial");
    // debug functionality off: the message subscriptions still happen, the cheat is not registered
    messages.notifications.clear();
    cheats.calls.clear();
    host.debugEnabled = false;
    auto* noDebug = new cSC4TutorialSystem();
    CHECK(noDebug->Init());
    CHECK(messages.notifications.size() == 3);
    CHECK(cheats.calls.empty());
    noDebug->Release();
    host.debugEnabled = true;

    // ---- startTutorial: the window procedure, two references, registry activation ----------------------------
    system->startTutorial();
    CHECK(host.createWinProcCalls == 1);
    CHECK(system->WinProc() != nullptr);
    FakeWinProc* proc = host.winProcs.back().get();
    CHECK(proc->inits == 1);
    CHECK(proc->addRefs == 2);          // quirk: the created object is referenced twice
    CHECK(system->IsTutorial());
    CHECK(!system->QuitPending());
    CHECK(host.setActive.size() == 1 && host.setActive[0].first == kTutorialGUIDNone &&
          host.setActive[0].second == true);
    // a second start reuses the existing window procedure and only re-activates the registry entry
    system->startTutorial();
    CHECK(host.createWinProcCalls == 1);
    CHECK(proc->addRefs == 2);
    CHECK(host.setActive.size() == 2 && host.setActive[1].second == true);

    // ---- the forwarding slots ---------------------------------------------------------------------------------
    system->ShowUIHintsForUsers();
    CHECK(proc->hints == 1);
    CHECK(system->HandleCatalogMenu(reinterpret_cast<cISC4WinCatalogView*>(std::uintptr_t{0x1000})));
    CHECK(proc->catalogViews.size() == 1 &&
          proc->catalogViews[0] == reinterpret_cast<cISC4WinCatalogView*>(std::uintptr_t{0x1000}));
    int clickX = 11, clickY = 22;
    proc->leftClickAnswer = false;
    CHECK(!system->FilterLeftMouseClick(clickX, clickY, 7));
    CHECK(proc->leftClicks.size() == 1 && proc->leftClicks[0].first == 7);
    CHECK(proc->leftClickPoints.size() == 1 && proc->leftClickPoints[0].first == 11 &&
          proc->leftClickPoints[0].second == 22);
    CHECK(system->OnTutorialEvent(cISC4TutorialSystem::kTutorialEventOpenTaskLink));
    CHECK(proc->events.size() == 1 && proc->events[0] == 1);

    // ---- ResetTutorialNumAndIndex (the GUID and the registry's task number) -----------------------------------
    FakeWinProc* before = proc;
    CHECK(system->ResetTutorialNumAndIndex(0x77, 3));
    CHECK(before->resets.size() == 1 && before->resets[0].first == 0x77 && before->resets[0].second == 3);
    CHECK(system->GetTutorialGUID() == 0x77);
    CHECK(system->GetTutorialTaskNumber() == 100 + 3);

    // ---- GetCurrentTask / IsEdgeScrollingAllowed --------------------------------------------------------------
    FakeTask task;
    CHECK(system->IsEdgeScrollingAllowed());        // no current task -> true
    before->task = &task;
    CHECK(system->GetCurrentTask() == &task);
    CHECK(system->IsEdgeScrollingAllowed());
    task.edgeScrolling = false;
    CHECK(!system->IsEdgeScrollingAllowed());

    // ---- FilterMessage: type 3, three command ids --------------------------------------------------------------
    cGZMessage message;
    message.type = kGZMsgCommand;
    before->buttons.clear();
    message.param1 = kTutorialCommandCatalogItem;
    message.param2 = 0x1111111122222222ull;
    message.param3 = 0x3333333344444444ull;
    CHECK(system->FilterMessage(message));
    CHECK(before->buttons.size() == 1 && before->buttons[0] == 0x44444444u);   // low half of the third word
    message.param1 = kTutorialCommandButtonChanged;
    CHECK(system->FilterMessage(message));
    CHECK(before->buttons.size() == 2 && before->buttons[1] == 0x22222222u);   // low half of the second word
    message.param1 = kTutorialCommandButtonActivated;
    CHECK(system->FilterMessage(message));
    CHECK(before->buttons.size() == 3 && before->buttons[2] == 0x22222222u);
    message.param1 = 0x12345678;
    CHECK(system->FilterMessage(message));
    CHECK(before->buttons.size() == 3);
    message.param1 = kTutorialCommandCatalogItem;
    message.type = kGZMsgDeleteChild;
    CHECK(system->FilterMessage(message));
    CHECK(before->buttons.size() == 3);

    // ---- DoMessage(cIGZMessage2*): tutorial finished -----------------------------------------------------------
    FakeMessage finished;
    finished.type = kTutorialMessageFinished;
    finished.data1 = 0;                       // data1 == 0 -> endTutorial(save = true)
    CHECK(system->DoMessage(static_cast<cIGZMessage2*>(&finished)));
    CHECK(before->shutdowns == 1);
    CHECK(before->releases == 2);             // quirk: both references released
    CHECK(system->WinProc() == nullptr);
    CHECK(!system->IsTutorial());
    CHECK(host.setActive.size() == 3 && host.setActive[2].first == 0x77 && host.setActive[2].second == false);
    CHECK(host.menuCloses.size() == 2 && host.menuCloses[0] == 1 && host.menuCloses[1] == 2);
    // no window procedure: the finished message does nothing
    finished.data1 = 1;
    CHECK(system->DoMessage(static_cast<cIGZMessage2*>(&finished)));
    CHECK(host.setActive.size() == 3);

    // ---- DoMessage(cIGZMessage2*): city view started (the one-shot latch) --------------------------------------
    system->startTutorial();
    FakeWinProc* second = host.winProcs.back().get();
    CHECK(host.createWinProcCalls == 2);
    host.activeItems[kTutorialGUIDNone] = false;    // the registry item is not active -> no restart
    FakeMessage cityView;
    cityView.type = kTutorialMessageCityViewStarted;
    const std::size_t createsBefore = static_cast<std::size_t>(host.createWinProcCalls);
    CHECK(system->DoMessage(static_cast<cIGZMessage2*>(&cityView)));
    CHECK(system->CityViewHandled());
    CHECK(system->WinProc() == second);             // the running window procedure is untouched
    CHECK(static_cast<std::size_t>(host.createWinProcCalls) == createsBefore);
    // the latch is one-shot even after the tutorial ends and starts again
    system->endTutorial(false);
    CHECK(system->QuitPending());                   // ending without saving arms the city-loaded path
    CHECK(!system->IsTutorial());
    system->startTutorial();
    host.activeItems[kTutorialGUIDNone] = true;
    CHECK(system->DoMessage(static_cast<cIGZMessage2*>(&cityView)));
    CHECK(host.createWinProcCalls == 3);            // still no restart (latch)

    // ---- DoMessage(cIGZMessage2*): a finished message with data1 != 0 ends without saving -----------------------
    CHECK(system->WinProc() != nullptr);
    finished.data1 = 1;                              // endTutorial(save = false)
    const std::size_t setActiveFinished = host.setActive.size();
    CHECK(system->DoMessage(static_cast<cIGZMessage2*>(&finished)));
    CHECK(system->WinProc() == nullptr);
    CHECK(host.setActive.size() == setActiveFinished);    // the registry is not touched
    CHECK(!system->IsTutorial());
    CHECK(system->QuitPending());

    // ---- DoMessage(cIGZMessage2*): city loaded ends a quit-without-save tutorial -------------------------------
    system->endTutorial(false);     // arms the flag again (the start above cleared it)
    CHECK(system->QuitPending());
    FakeMessage cityLoaded;
    cityLoaded.type = kTutorialMessageCityLoaded;
    const std::size_t setActiveBefore = host.setActive.size();
    CHECK(system->DoMessage(static_cast<cIGZMessage2*>(&cityLoaded)));
    CHECK(host.setActive.size() == setActiveBefore + 1);
    CHECK(host.setActive.back().second == false);   // endTutorial(true)
    CHECK(system->WinProc() == nullptr);
    CHECK(!system->QuitPending());                  // endTutorial(true) clears it

    // ---- DoMessage(cGZMessage&): the cheat verbs -----------------------------------------------------------------
    // a different message type / cheat id is ignored
    {
        cRZString text("Tutorial save 9");
        cGZMessage other = CheatMessage(text, 0x11111111);
        host.setActive.clear();
        const int guidBefore = system->GetTutorialGUID();
        const int taskBefore = system->GetTutorialTaskNumber();
        CHECK(system->DoMessage(other));
        CHECK(system->GetTutorialGUID() == guidBefore);
        CHECK(system->GetTutorialTaskNumber() == taskBefore);
        other.type = 0x12345678;
        CHECK(system->DoMessage(other));
        CHECK(system->GetTutorialGUID() == guidBefore);
    }
    // "save <guid>": sets the state and asks the city to save
    {
        cRZString text("tutorial SAVE 7");
        cGZMessage cheat = CheatMessage(text);
        host.cityGUID.clear();
        host.saveRequests = 0;
        CHECK(system->DoMessage(cheat));
        CHECK(system->IsTutorial());
        CHECK(system->GetTutorialGUID() == 7);
        CHECK(system->GetTutorialTaskNumber() == 0);
        CHECK(!system->QuitPending());
        CHECK(host.cityGUID.size() == 1 && host.cityGUID[0] == 7);
        CHECK(host.saveRequests == 1);
    }
    // "save 0" and "save 0xffffffff" are refused
    {
        cRZString zero("Tutorial save 0");
        cGZMessage cheat = CheatMessage(zero);
        system->ResetTutorialIndex(9);
        host.saveRequests = 0;
        CHECK(system->DoMessage(cheat));
        CHECK(system->GetTutorialGUID() == 7);
        CHECK(system->GetTutorialTaskNumber() == 9);
        CHECK(host.saveRequests == 0);
        cRZString maxGuid("Tutorial save 4294967295");
        cGZMessage cheatMax = CheatMessage(maxGuid);
        CHECK(system->DoMessage(cheatMax));
        CHECK(system->GetTutorialGUID() == 7);
        CHECK(host.saveRequests == 0);
    }
    // "reset <guid>": resets the region's tutorial city, starts the tutorial and takes the registry's task number.
    // The tutorial is started with the *previous* GUID (startTutorial runs before ResetTutorialNumAndIndex stores the
    // new one), so the registry is told to activate the old entry — a quirk of the original's argument order.
    {
        cRZString text("Tutorial reset 5");
        cGZMessage cheat = CheatMessage(text);
        host.resetCity.clear();
        host.setActive.clear();
        system->endTutorial(false);
        host.createWinProcCalls = 0;
        system->startTutorial();                      // creates a fresh window procedure
        host.createWinProcCalls = 0;
        CHECK(system->DoMessage(cheat));
        CHECK(host.resetCity.size() == 1 && host.resetCity[0] == 5);
        CHECK(host.createWinProcCalls == 0);          // the existing window procedure is reused
        CHECK(system->GetTutorialGUID() == 5);
        CHECK(system->GetTutorialTaskNumber() == 100);
        CHECK(host.setActive.size() == 2);
        CHECK(host.setActive[0].first == 7 && host.setActive[0].second == true);
        CHECK(host.setActive[1].first == 7 && host.setActive[1].second == true);
    }
    // "step <index>": forwards the current GUID and the index
    {
        cRZString text("Tutorial step 3");
        cGZMessage cheat = CheatMessage(text);
        CHECK(system->DoMessage(cheat));
        CHECK(system->GetTutorialGUID() == 5);
        CHECK(system->GetTutorialTaskNumber() == 100 + 3);
        CHECK(CurrentProc(host)->resets.back() == std::make_pair(5, 3));
    }
    // "next": the window procedure moves on
    {
        cRZString text("Tutorial next");
        cGZMessage cheat = CheatMessage(text);
        CHECK(system->DoMessage(cheat));
        CHECK(CurrentProc(host)->nextCalls == 1);
    }
    // "quit": ends the tutorial without saving
    {
        cRZString text("Tutorial quit");
        cGZMessage cheat = CheatMessage(text);
        CHECK(system->DoMessage(cheat));
        CHECK(!system->IsTutorial());
        CHECK(system->QuitPending());
        CHECK(system->WinProc() == nullptr);
    }
    // two arguments: "save" (no GUID) writes the current GUID
    {
        cRZString save("Tutorial save 11");
        cGZMessage setGuid = CheatMessage(save);
        CHECK(system->DoMessage(setGuid));
        cRZString text("tutorial save");
        cGZMessage cheat = CheatMessage(text);
        host.cityGUID.clear();
        host.saveRequests = 0;
        CHECK(system->DoMessage(cheat));
        CHECK(host.cityGUID.size() == 1 && host.cityGUID[0] == 11);
        CHECK(host.saveRequests == 1);
    }
    // two arguments: "reset" resets the current GUID
    {
        cRZString text("Tutorial reset");
        cGZMessage cheat = CheatMessage(text);
        host.resetCity.clear();
        CHECK(system->DoMessage(cheat));
        CHECK(host.resetCity.size() == 1 && host.resetCity[0] == 11);
        CHECK(system->GetTutorialGUID() == 11);
        CHECK(system->GetTutorialTaskNumber() == 100);
    }
    // an unknown verb or a wrong argument count is ignored
    {
        cRZString unknown("Tutorial frobnicate 1");
        cGZMessage cheat = CheatMessage(unknown);
        CHECK(system->DoMessage(cheat));
        CHECK(system->GetTutorialGUID() == 11);
        cRZString tooMany("Tutorial reset 1 2");
        cGZMessage cheat2 = CheatMessage(tooMany);
        host.resetCity.clear();
        CHECK(system->DoMessage(cheat2));
        CHECK(host.resetCity.empty());
        cRZString none("Tutorial");
        cGZMessage cheat3 = CheatMessage(none);
        CHECK(system->DoMessage(cheat3));
        CHECK(host.resetCity.empty());
    }

    // ---- serialisation -------------------------------------------------------------------------------------
    system->ResetTutorialIndex(0x2a);
    CHECK(system->ResetTutorialNumAndIndex(0x1234, 0));   // stores the GUID, keeps the returned task number
    system->ResetTutorialIndex(0x2a);
    {
        RecordingOStream stream;
        CHECK(system->Write(stream));
        const std::vector<unsigned char> expected = MakeTutorialStream(2, 1, 0x2a, 0x1234);
        CHECK(stream.bytes == expected);
        CHECK(stream.bytes.size() == 11);
        CHECK(stream.calls.size() == 4);
        CHECK(stream.calls[0] == "u16:2");
        CHECK(stream.calls[1] == "u8:1");
        CHECK(stream.calls[2] == "i32:42");
        CHECK(stream.calls[3] == "i32:4660");
        stream.fail = true;
        CHECK(!system->Write(stream));                     // an already failed stream writes nothing and answers false
    }
    {
        // a fresh object: the inactive flag is 0 and the GUID -1
        auto* fresh = new cSC4TutorialSystem();
        RecordingOStream stream;
        CHECK(fresh->Write(stream));
        const std::vector<unsigned char> expected = MakeTutorialStream(2, 0, 0, -1);
        CHECK(stream.bytes == expected);
        CHECK(stream.calls[1] == "u8:0");
        CHECK(stream.calls[3] == "i32:-1");
        fresh->Release();
    }
    {
        const std::vector<unsigned char> bytes = MakeTutorialStream(2, 0, 7, 1);
        PortMemoryIStream stream(bytes.data(), bytes.size());
        host.whichToGUID.clear();
        CHECK(system->Read(stream));
        CHECK(host.whichToGUID.size() == 1 && host.whichToGUID[0] == 1);
        CHECK(!system->IsTutorial());
        CHECK(system->GetTutorialTaskNumber() == 7);
        CHECK(system->GetTutorialGUID() == kTutorialGUIDSecond);   // 1 -> the second city GUID
    }
    {
        // a wrong version is refused and changes nothing
        const std::vector<unsigned char> bytes = MakeTutorialStream(3, 1, 99, 99);
        PortMemoryIStream stream(bytes.data(), bytes.size());
        const int guid = system->GetTutorialGUID();
        CHECK(!system->Read(stream));
        CHECK(system->GetTutorialGUID() == guid);
    }
    {
        // a truncated payload: the version matches, the reads fail, the answer is false
        const std::vector<unsigned char> bytes = MakeTutorialStream(2, 1, 0, 0);
        PortMemoryIStream stream(bytes.data(), 2);
        host.whichToGUID.clear();
        CHECK(!system->Read(stream));
        CHECK(!host.whichToGUID.empty());   // the original converts after checking the error, so it still runs
    }

    // ---- the scripting-context cache -------------------------------------------------------------------------
    FakeLua lua;
    host.lua = &lua;
    CHECK(system->ScriptingContextCache() == nullptr);
    CHECK(system->GetScriptingContext() == &lua);
    CHECK(lua.refs == 1);
    CHECK(system->GetScriptingContext() == &lua);
    CHECK(lua.refs == 1);                   // cached, not referenced twice
    CHECK(system->ScriptingContextCache() == &lua);

    // ---- the destructor releases the context and clears the instance ----------------------------------------
    // The object is still at reference count 0 (nothing has taken a reference on it), and cRZUnknown::Release
    // deletes a zero- or one-count object, so this is also the class's normal deletion path.
    FakeWinProc* procAtDestruction = CurrentProc(host);
    const int procReleases = procAtDestruction ? procAtDestruction->releases : 0;
    auto* first = system;
    auto* secondSystem = new cSC4TutorialSystem();
    CHECK(cSC4TutorialSystem::Instance() == secondSystem);   // the constructor steals the instance
    first->Release();                                       // deletes: count 0 -> destructor
    CHECK(procAtDestruction && procAtDestruction->releases == procReleases + 1);   // one release, no Shutdown
    CHECK(procAtDestruction && procAtDestruction->shutdowns == 0);
    CHECK(cSC4TutorialSystem::Instance() == nullptr);       // quirk: the destructor clears it unconditionally
    CHECK(secondSystem->Instance() == nullptr);
    CHECK(lua.refs == 0);                                   // the cached scripting context was released
    secondSystem->Release();

    // ---- nsSC4TutorialSystem::FilterUICommandMessage ---------------------------------------------------------
    {
        SetTutorialFilterSystem(nullptr);
        cGZMessage command;
        command.type = kGZMsgCommand;
        command.param1 = kTutorialCommandCatalogItem;
        command.param3 = 5;
        CHECK(FilterUICommandMessage(command));

        auto* filterSystem = new cSC4TutorialSystem();
        SetTutorialFilterSystem(filterSystem);
        CHECK(!filterSystem->IsTutorial());
        CHECK(FilterUICommandMessage(command));            // no tutorial running: the command passes
        filterSystem->startTutorial();
        CHECK(filterSystem->IsTutorial());
        CHECK(FilterUICommandMessage(command));            // forwarded to the window procedure, which answers true
        CHECK(filterSystem->WinProc() != nullptr);
        CurrentProc(host)->buttons.clear();
        command.param1 = kTutorialCommandButtonChanged;
        command.param2 = 0x99;
        CHECK(FilterUICommandMessage(command));
        CHECK(CurrentProc(host)->buttons.size() == 1 && CurrentProc(host)->buttons[0] == 0x99);
        SetTutorialFilterSystem(nullptr);
        CHECK(FilterUICommandMessage(command));
        filterSystem->endTutorial(false);
        filterSystem->Release();
    }

    // ---- Shutdown -------------------------------------------------------------------------------------------
    messages.notifications.clear();
    cheats.calls.clear();
    auto* shutdownSystem = new cSC4TutorialSystem();
    shutdownSystem->startTutorial();
    FakeWinProc* lastProc = host.winProcs.back().get();
    CHECK(lastProc->addRefs == 2);
    CHECK(shutdownSystem->Shutdown());
    CHECK(shutdownSystem->WinProc() == nullptr);
    CHECK(lastProc->shutdowns == 1);
    CHECK(lastProc->releases == 2);
    CHECK(cheats.calls.size() == 2);
    CHECK(cheats.calls[0].kind == FakeCheats::Call::RemoveNotification && cheats.calls[0].id == 0 &&
          cheats.calls[0].target == static_cast<cIGZMessageTarget*>(shutdownSystem));
    CHECK(cheats.calls[1].kind == FakeCheats::Call::Unregister && cheats.calls[1].id == kTutorialCheatId);
    CHECK(messages.notifications.size() == 3);
    for (const auto& notification : messages.notifications) CHECK(!notification.add);
    shutdownSystem->Release();
    CHECK(cSC4TutorialSystem::Instance() == nullptr);

    // ---- the reference count is shared by every view --------------------------------------------------------
    {
        auto* counted = new cSC4TutorialSystem();
        CHECK(counted->AddRef() == 1);
        auto* secondary = static_cast<cIGZSerializable*>(counted);
        CHECK(secondary->AddRef() == 2);
        CHECK(secondary->Release() == 1);
        CHECK(counted->Release() == 0);   // deletes through the saved reference count
    }

    SetTutorialSystemHost(nullptr);
    host.winProcs.clear();

    if (failures == 0) {
        std::printf("sc4_tutorialsystem_test: all checks passed\n");
        return 0;
    }
    std::printf("sc4_tutorialsystem_test: %d failure(s)\n", failures);
    return 1;
}
