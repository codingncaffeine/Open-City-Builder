// cSC4AdvisorSystem: the object layout, QueryInterface views, constructor / destructor, Init / Shutdown, the advisor
// registry (AddAdvisor / RemoveAdvisor / GetAdvisor / load_advisor / load_advisors / update_advisors), the event
// subscription and push path, the context advice stack, the Lua value writers, DoLink, register_api / reload_data, the
// advice popup priority thresholds, the DoMessage routes (city started, periodic tick, cheat switches), simulate /
// tick / kill_advisors, the message notifications, the advisor window ownership and Write / Read.
//
// All engine services come in through IAdvisorSystemHost; the advisor / advice / message server / cheat manager /
// detokenizer / streams / Lua are the shared fakes. The nested advisor objects of Write / Read are the recovered
// SC4WriteGZSerializable / SC4ReadGZSerializable helpers; the test supplies recording stand-ins for the two plain
// stream overloads so no DB segment file is needed.
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "SDL.h"
#include "gz/CheatCodeManager.h"
#include "gz/cGZDate.h"
#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/AdviceSubject.h"
#include "sc4/ScriptLuaAdapter.h"
#include "sc4/TablePropertyReader.h"
#include "sc4/AdvisorSystem.h"
#include "sc4/DBSegment.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace sc4;

namespace {

// ---------------------------------------------------------------------------------------------------------------
// A small Lua 4 / cISCLua fake: a value stack, global tables with number / string keys, tags with their methods and
// user data. Enough for the advisor tables, the tuning constants and the "_on_reload" hook.
// ---------------------------------------------------------------------------------------------------------------
struct Table;
struct LValue {
    enum Kind { Nil, Bool, Num, Str, TableK, Func, UserDataK } kind = Nil;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::shared_ptr<Table> table;
    cISCLua::CFunction function = nullptr;
    void* userData = nullptr;
    int tag = 0;
};
struct Table {
    std::map<std::string, LValue> named;
    std::map<int, LValue> array;
    int length = 0;
};

LValue MakeNil() { return LValue(); }
LValue MakeBool(bool b) { LValue v; v.kind = LValue::Bool; v.boolean = b; return v; }
LValue MakeNum(double n) { LValue v; v.kind = LValue::Num; v.number = n; return v; }
LValue MakeStr(const char* s) { LValue v; v.kind = LValue::Str; v.text = s ? s : ""; return v; }
LValue MakeTable(const std::shared_ptr<Table>& t) { LValue v; v.kind = LValue::TableK; v.table = t; return v; }

class FakeLua final : public cISCLua {
public:
    std::shared_ptr<Table> globals = std::make_shared<Table>();
    std::map<int, LValue> registry;
    int nextRef = 1;
    int openCalls = 0;
    int closeCalls = 0;
    int gcCount = 0;
    int gcThreshold = 0;
    std::vector<int> gcThresholdSets;
    std::vector<int> newTags;
    std::vector<std::pair<int, std::string>> tagMethods;
    std::vector<std::string> doBuffers;
    std::vector<std::string> doStrings;
    std::vector<std::string> protectedCalls;
    std::uint32_t refs = 0;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != 0xea10c4a2) return false;
        *out = static_cast<cISCLua*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (refs > 1) return --refs;
        delete this;
        return 0;
    }
    bool Open(int libraries) override { ++openCalls; lastOpen = libraries; return true; }
    bool Close() override { ++closeCalls; return true; }
    void* AsIGZLua5() override { return nullptr; }
    int GetTop() override { return static_cast<int>(stack.size()); }
    void SetTop(int top) override { stack.resize(static_cast<std::size_t>(top < 0 ? 0 : top)); }
    void PushValue(int index) override { stack.push_back(At(index)); }
    void Remove(int) override {}
    void Insert(int) override {}
    void Pop(int count) override { if (count > 0) stack.resize(stack.size() - static_cast<std::size_t>(count)); }
    int Type(int index) override { return TypeOf(At(index)); }
    const char* TypeName(int) override { return "?"; }
    bool IsNumber(int index) override { return At(index).kind == LValue::Num; }
    bool IsString(int index) override { return At(index).kind == LValue::Str; }
    bool IsCFunction(int) override { return false; }
    int Tag(int index) override { return At(index).tag; }
    bool IsBoolean(int index) override { return At(index).kind == LValue::Bool; }
    bool Equal(int, int) override { return false; }
    bool LessThan(int, int) override { return false; }
    std::uint32_t ToUint32(int index) override { return static_cast<std::uint32_t>(ToNumber(index)); }
    std::int32_t ToSint32(int index) override { return static_cast<std::int32_t>(ToNumber(index)); }
    double ToNumber(int index) override { return At(index).kind == LValue::Num ? At(index).number : 0.0; }
    const char* ToString(int index) override {
        const LValue& v = At(index);
        if (v.kind == LValue::Str) return v.text.c_str();
        if (v.kind == LValue::Num) {
            char buffer[32];
            std::snprintf(buffer, sizeof buffer, "%g", v.number);
            scratch = buffer;
            return scratch.c_str();
        }
        return nullptr;
    }
    int Strlen(int index) override { const char* s = ToString(index); return s ? static_cast<int>(std::strlen(s)) : 0; }
    CFunction ToCFunction(int) override { return nullptr; }
    void* ToUserData(int index) override { return At(index).userData; }
    void* ToUserTag(int index) override { return At(index).userData; }
    bool ToBoolean(int index) override { return At(index).kind == LValue::Bool ? At(index).boolean : ToNumber(index) != 0; }
    void PushNil() override { stack.push_back(MakeNil()); }
    void PushNumber(double value) override { stack.push_back(MakeNum(value)); }
    void PushLString(const char* s, int length) override {
        LValue v; v.kind = LValue::Str; v.text.assign(s, static_cast<std::size_t>(length)); stack.push_back(v);
    }
    void PushString(const char* s) override { stack.push_back(MakeStr(s)); }
    void PushCClosure(CFunction fn, int) override {
        LValue v; v.kind = LValue::Func; v.function = fn; stack.push_back(v);
    }
    void PushUserTag(void* p, int tag) override { PushUserData(p, tag); }
    void PushBoolean(bool value) override { stack.push_back(MakeBool(value)); }
    void GetGlobal(const char* name) override {
        auto it = globals->named.find(name);
        stack.push_back(it == globals->named.end() ? MakeNil() : it->second);
    }
    void GetTable(int index) override {
        const int abs = Absolute(index);
        const LValue key = PopValue();
        stack.push_back(Lookup(abs, key));
    }
    void RawGet(int index) override { GetTable(index); }
    void RawGetI(int index, int n) override {
        const LValue table = At(index);
        LValue result;
        if (table.kind == LValue::TableK) {
            auto it = table.table->array.find(n);
            if (it != table.table->array.end()) result = it->second;
        }
        stack.push_back(result);
    }
    void GetGlobals() override { stack.push_back(MakeTable(globals)); }
    void GetTagMethod(int, const char*) override {}
    bool GetRef(int ref) override {
        auto it = registry.find(ref);
        if (it == registry.end()) return false;
        stack.push_back(it->second);
        return true;
    }
    void NewTable() override { stack.push_back(MakeTable(std::make_shared<Table>())); }
    void SetGlobal(const char* name) override { globals->named[name] = PopValue(); }
    void SetTable(int index) override { SetAt(index); }
    void RawSet(int index) override { SetAt(index); }
    void RawSetI(int, int) override {}
    void SetTagMethod(int tag, const char* event) override { tagMethods.emplace_back(tag, event ? event : ""); }
    int Ref(int) override { const int ref = nextRef++; registry[ref] = PopValue(); return ref; }
    int CallProtected(int args, int results) override {
        for (int i = 0; i < args; ++i) PopValue();
        const LValue fn = PopValue();
        if (fn.kind != LValue::Func) return 1;
        protectedCalls.emplace_back("call");
        for (int i = 0; i < results; ++i) stack.push_back(MakeNil());
        return 0;
    }
    void RawCall(int, int) override {}
    int DoFile(const char*) override { return 0; }
    int DoString(const char* code) override { doStrings.emplace_back(code ? code : ""); return 0; }
    int DoBuffer(const char* buffer, int size, const char*) override {
        doBuffers.emplace_back(buffer ? std::string(buffer, static_cast<std::size_t>(size)) : std::string());
        return 0;
    }
    int GetGCCount() override { return gcCount; }
    int GetGCThreshold() override { return gcThreshold; }
    void SetGCThreshold(int value) override { gcThreshold = value; gcThresholdSets.push_back(value); }
    int NewTag() override { const int tag = 100 + static_cast<int>(newTags.size()); newTags.push_back(tag); return tag; }
    void CopyTagMethods(int, int) override {}
    void SetTag(int) override {}
    void Unref(int ref) override { registry.erase(ref); }
    int Next(int index) override {
        const LValue tableValue = At(index);
        std::vector<std::pair<LValue, LValue>> entries;
        if (tableValue.kind == LValue::TableK) {
            for (const auto& e : tableValue.table->named) entries.emplace_back(MakeStr(e.first.c_str()), e.second);
            for (const auto& e : tableValue.table->array) entries.emplace_back(MakeNum(e.first), e.second);
        }
        std::size_t start = 0;
        if (!stack.empty() && stack.back().kind != LValue::Nil && nextActive) {
            const LValue key = stack.back();
            for (std::size_t i = 0; i < entries.size(); ++i) {
                if (SameKey(entries[i].first, key)) { start = i + 1; break; }
            }
        }
        stack.pop_back();   // the key (or the initial nil)
        if (start >= entries.size()) { nextActive = false; return 0; }
        nextActive = true;
        stack.push_back(entries[start].first);
        stack.push_back(entries[start].second);
        return 1;
    }
    int GetN(int index) override {
        const LValue v = At(index);
        return v.kind == LValue::TableK ? v.table->length : 0;
    }
    void Error(const char*) override {}
    void Concat(int) override {}
    void* NewUserData(int) override { return nullptr; }
    void Register(CFunction fn, const char* name) override {
        LValue v; v.kind = LValue::Func; v.function = fn;
        globals->named[name] = v;
    }
    void PushUserData(void* p, int tag) override {
        LValue v; v.kind = LValue::UserDataK; v.userData = p; v.tag = tag; stack.push_back(v);
    }
    int CloneTag(int tag) override { return tag; }
    bool IsFunction(int index) override { return At(index).kind == LValue::Func; }
    bool IsTable(int index) override { return At(index).kind == LValue::TableK; }
    bool IsUserData(int index) override { return At(index).kind == LValue::UserDataK; }
    bool IsNil(int index) override { return At(index).kind == LValue::Nil; }
    std::uintptr_t ToDataRegistryEntryID(cISC4GameDataRegistry*, int) override { return 0; }
    cIGZUnknown* ToDataRegistryNode(cISC4GameDataRegistry*, int) override { return nullptr; }
    void* ToDataRegistryValue(cISC4GameDataRegistry*, int) override { return nullptr; }

    LValue* FindGlobal(const char* name) {
        auto it = globals->named.find(name);
        return it == globals->named.end() ? nullptr : &it->second;
    }

private:
    static int TypeOf(const LValue& v) {
        switch (v.kind) {
            case LValue::Nil: return 0;
            case LValue::Bool: return 1;
            case LValue::Num: return 3;
            case LValue::Str: return 4;
            case LValue::TableK: return 5;
            case LValue::Func: return 6;
            case LValue::UserDataK: return 7;
        }
        return 0;
    }
    static bool SameKey(const LValue& a, const LValue& b) {
        if (a.kind != b.kind) return false;
        if (a.kind == LValue::Str) return a.text == b.text;
        if (a.kind == LValue::Num) return a.number == b.number;
        return false;
    }
    int Absolute(int index) const { return index < 0 ? static_cast<int>(stack.size()) + index : index - 1; }
    LValue& At(int index) {
        const int abs = Absolute(index);
        if (abs < 0 || abs >= static_cast<int>(stack.size())) { outside = LValue(); return outside; }
        return stack[static_cast<std::size_t>(abs)];
    }
    LValue PopValue() {
        if (stack.empty()) return LValue();
        LValue v = stack.back();
        stack.pop_back();
        return v;
    }
    LValue Lookup(int absIndex, const LValue& key) {
        if (absIndex < 0 || absIndex >= static_cast<int>(stack.size())) return LValue();
        const LValue& tableValue = stack[static_cast<std::size_t>(absIndex)];
        if (tableValue.kind != LValue::TableK) return LValue();
        if (key.kind == LValue::Str) {
            auto it = tableValue.table->named.find(key.text);
            if (it != tableValue.table->named.end()) return it->second;
        } else if (key.kind == LValue::Num) {
            auto it = tableValue.table->array.find(static_cast<int>(key.number));
            if (it != tableValue.table->array.end()) return it->second;
        }
        return LValue();
    }
    void SetAt(int index) {
        const int abs = Absolute(index);
        LValue value = PopValue();
        LValue key = PopValue();
        if (abs < 0 || abs >= static_cast<int>(stack.size())) return;
        LValue& tableValue = stack[static_cast<std::size_t>(abs)];
        if (tableValue.kind != LValue::TableK) return;
        if (key.kind == LValue::Str) {
            tableValue.table->named[key.text] = value;
        } else if (key.kind == LValue::Num) {
            const int n = static_cast<int>(key.number);
            tableValue.table->array[n] = value;
            if (n > tableValue.table->length) tableValue.table->length = n;
        }
    }

    std::vector<LValue> stack;
    LValue outside;
    bool nextActive = false;
    std::string scratch;
    int lastOpen = 0;
};

// ---------------------------------------------------------------------------------------------------------------
// Fake message server (records sent / posted messages and notification add / remove calls)
// ---------------------------------------------------------------------------------------------------------------
struct PostedMessage {
    std::uint32_t type = 0;
    bool hasData1 = false;
    std::uint64_t data1 = 0;
    void* void1 = nullptr;
    void* void2 = nullptr;
    cIGZUnknown* unknown = nullptr;
    bool post = false;
};
struct FakeMessages final : cIGZMessageServer2 {
    std::vector<PostedMessage> messages;
    std::vector<std::pair<cIGZMessageTarget2*, std::uint32_t>> added;
    std::vector<std::pair<cIGZMessageTarget2*, std::uint32_t>> removed;
    std::uint32_t refs = 0;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != kGZIID_cIGZMessageServer2) return false;
        *out = static_cast<cIGZMessageServer2*>(this);
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return refs ? --refs : 0; }
    bool MessageSend(cIGZMessage2* message) override { Record(message, false); return true; }
    bool MessagePost(cIGZMessage2* message, bool) override { Record(message, true); return true; }
    bool AddNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        added.emplace_back(target, type);
        return true;
    }
    bool RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        removed.emplace_back(target, type);
        return true;
    }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return false; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return false; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return refs; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }

private:
    void Record(cIGZMessage2* message, bool post) {
        auto* standard = static_cast<cIGZMessage2Standard*>(message);
        PostedMessage m;
        m.type = standard->GetType();
        m.hasData1 = standard->GetHasData1();
        m.data1 = standard->GetData1();
        m.void1 = standard->GetVoid1();
        m.void2 = standard->GetVoid2();
        m.unknown = standard->GetIGZUnknown();
        m.post = post;
        messages.push_back(m);
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Fake city (only used as an opaque pointer by City() / GetVoid1)
// ---------------------------------------------------------------------------------------------------------------
struct FakeCity final : cIGZUnknown {
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1) return false;
        *out = this;
        return true;
    }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
};

// ---------------------------------------------------------------------------------------------------------------
// Advisor / advice fakes
// ---------------------------------------------------------------------------------------------------------------
struct AdviceState {
    int guid = 0;
    std::vector<std::string> links;
    std::uint32_t refs = 0;
};
struct FakeAdvice final : cISC4Advice {
    explicit FakeAdvice(std::shared_ptr<AdviceState> s) : state(std::move(s)) {}
    std::shared_ptr<AdviceState> state;
    cRZString title{"title"};
    cRZString message{"message"};
    cRZString trigger{"return 0"};
    cISC4AdviceSubject* subject = nullptr;
    bool current = false;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != kGZIID_cISC4Advice) return false;
        *out = static_cast<cISC4Advice*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++state->refs; }
    std::uint32_t Release() override {
        if (state->refs > 1) return --state->refs;
        state->refs = 0;
        delete this;
        return 0;
    }
    bool Init(int guid) override { state->guid = guid; return true; }
    bool Shutdown() override { return true; }
    bool Clone(cISC4Advice** out) override { *out = nullptr; return false; }
    int GetMood() const override { return 3; }
    int GetPriority() const override { return 50; }
    bool GetTrigger() const override { return false; }
    cIGZString& GetTriggerString() override { return trigger; }
    int GetGUID() const override { return state->guid; }
    int GetAdvisorID() const override { return 0; }
    void SetAdvisorID(int) override {}
    int GetSessionID() const override { return 0; }
    bool SetFlags(int, bool) override { return true; }
    bool CheckFlags(int) const override { return current; }
    int GetFrequencyInDays() const override { return 30; }
    int GetTimeOutInDays() const override { return 30; }
    const cIGZString& GetTextTitle() const override { return title; }
    std::uint32_t GetTextTitleGUID() const override { return 0; }
    const cIGZString& GetTextMessage() const override { return message; }
    std::uint32_t GetTextMessageGUID() const override { return 0; }
    bool DoLink(const cIGZString& link) override { state->links.emplace_back(link.ToChar()); return true; }
    void NotifyGlobalEffectTriggerChanged() override {}
    bool SetEventSubject(cISC4AdviceSubject* s) override { subject = s; return true; }
    cISC4AdviceSubject* GetEventSubject() override { return subject; }
    int GetEventID() override { return 0; }
    int GetCommandID() override { return 0; }
    bool UpdateText() override { return true; }
    bool IsCurrent() const override { return current; }
};

struct AdvisorState {
    int id = 0;
    int initResult = 1;
    std::vector<int> initRows;
    std::vector<int> simulatePeriods;
    std::vector<int> pushedEvents;
    int shutdownCalls = 0;
    int adviceGuid = 0, adviceSession = 0;
    std::shared_ptr<AdviceState> advice = std::make_shared<AdviceState>();
    std::uint32_t refs = 0;
};
struct FakeAdvisor final : cISC4Advisor {
    explicit FakeAdvisor(std::shared_ptr<AdvisorState> s) : state(std::move(s)) { advice = new FakeAdvice(state->advice); }
    ~FakeAdvisor() { advice->Release(); }
    std::shared_ptr<AdvisorState> state;
    FakeAdvice* advice = nullptr;
    cRZString name{"advisor"};
    cRZString personal{"name"};
    AdviceMap advices;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != kGZIID_cISC4Advisor) return false;
        *out = static_cast<cISC4Advisor*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++state->refs; }
    std::uint32_t Release() override {
        if (state->refs > 1) return --state->refs;
        state->refs = 0;
        delete this;
        return 0;
    }
    bool Init(int row) override { state->initRows.push_back(row); return state->initResult != 0; }
    bool Shutdown() override { ++state->shutdownCalls; return true; }
    int GetID() const override { return state->id; }
    const cIGZString& GetName() const override { return name; }
    void SetName(const cIGZString& n) override { name.Copy(n); }
    cIGZString& GetPersonalName() override { return personal; }
    void SetPersonalName(const cIGZString& n) override { personal.Copy(n); }
    const AdviceMap& GetCurrentAdvices() const override { return advices; }
    AdviceMap& GetCurrentAdvices() override { return advices; }
    cISC4Advice* GetAdviceByGUID(int guid) const override {
        return guid == state->adviceGuid ? advice : nullptr;
    }
    // an advice link names the advice by its session id (the original's DoLink calls vslot 0x34, i386 0x001e54xx)
    cISC4Advice* GetAdviceBySessionID(int id) const override { return id == state->adviceSession ? advice : nullptr; }
    bool Simulate(int period) override { state->simulatePeriods.push_back(period); return true; }
    int GetHeadAnimCount(bool) override { return 0; }
    bool SetHeadAnim(bool, int) override { return false; }
    int GetHeadAnimID(int) override { return 0; }
    bool PushEvent(int eventId, cISC4AdviceSubject*) override { state->pushedEvents.push_back(eventId); return true; }
    bool CommandWouldTrigger(std::uint32_t) override { return false; }
    bool RetireAdvice(cISC4Advice*) override { return false; }
};

// ---------------------------------------------------------------------------------------------------------------
// Advisor window proc fake
// ---------------------------------------------------------------------------------------------------------------
struct WinProcState {
    int created = 0;
    int initCalls = 0;
    int shutdownCalls = 0;
    int showCalls = 0;
    int hideCalls = 0;
    bool visible = false;
    std::uint32_t refs = 0;
};
struct FakeWinProc final : IAdvisorWinProc {
    explicit FakeWinProc(std::shared_ptr<WinProcState> s) : state(std::move(s)) {}
    std::shared_ptr<WinProcState> state;
    std::uint32_t own = 0;   // per instance: the shared state spans every window the host creates
    void AddRef() override { ++own; ++state->refs; }
    void Release() override {
        if (state->refs) --state->refs;
        if (own > 1) { --own; return; }
        delete this;
    }
    void Init() override { ++state->initCalls; }
    void Shutdown() override { ++state->shutdownCalls; }
    void ShowWindow() override { ++state->showCalls; state->visible = true; }
    void HideWindow() override { ++state->hideCalls; state->visible = false; }
    bool IsVisible() override { return state->visible; }
};

// ---------------------------------------------------------------------------------------------------------------
// Detokenizer fake (records the advice detokenizer registration)
// ---------------------------------------------------------------------------------------------------------------
struct FakeDetokenizer final : cSCStringDetokenizer {
    struct Registration {
        DetokenizeMethod method;
        void* context;
        bool add;
    };
    std::vector<Registration> registrations;
    void AddUnknownTokenReplacementMethod(DetokenizeMethod method, void* context, bool add) override {
        registrations.push_back({method, context, add});
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------------------------------------------
struct HostState {
    std::map<std::uint32_t, int> classToAdvisorId;
    std::vector<std::shared_ptr<AdvisorState>> advisors;
};

struct FakeHost final : IAdvisorSystemHost {
    FakeLua* lua = nullptr;
    FakeMessages* messages = nullptr;
    cGZCheatCodeManager* cheats = nullptr;
    FakeDetokenizer* detokenizer = nullptr;
    std::shared_ptr<WinProcState> winProc = std::make_shared<WinProcState>();
    FakeCity city;
    bool cityPresent = true;
    bool cityEstablished = true;
    int difficulty = 0;
    bool debugEnabled = false;
    std::string dataDirectory = "/data/";
    std::string pluginDirectory = "/plugins/";
    std::vector<std::string> updatedDirectories;
    int packageScriptCalls = 0;
    std::vector<cISCLua*> tutorialLua;
    std::vector<IAdvisorSystemHost::LuaExtension> extensions;
    cGZDate simDate;
    int simStartYear = 2000;
    bool anyPaused = false;
    int llamaToggles = 0;
    HostState state;

    cISCLua* Lua() override { return lua; }
    cIGZMessageServer2* Messages() override { return messages; }
    cGZCheatCodeManager* CheatCodeManager() override { return cheats; }
    cSCStringDetokenizer* Detokenizer() override { return detokenizer; }
    IAdvisorWinProc* CreateAdvisorWinProc() override {
        if (!winProc) return nullptr;
        ++winProc->created;
        return new FakeWinProc(winProc);
    }
    cIGZUnknown* City() override { return cityPresent ? &city : nullptr; }
    bool CityEstablished(cIGZUnknown* /*city*/) override { return cityEstablished; }
    int CityDifficultyLevel() override { return difficulty; }
    bool DebugFunctionalityEnabled() override { return debugEnabled; }
    void GetDataDirectory(cIGZString& out, int /*language*/) override { out.AssignFromCString(dataDirectory.c_str()); }
    void GetPluginDirectory(cIGZString& out) override { out.AssignFromCString(pluginDirectory.c_str()); }
    void SetTutorialRegistryLua(cISCLua* l) override { tutorialLua.push_back(l); }
    bool CreateAdvisor(std::uint32_t classId, std::uint32_t iid, cISC4Advisor** out) override {
        *out = nullptr;
        if (iid != kGZIID_cISC4Advisor) return false;
        auto it = state.classToAdvisorId.find(classId);
        if (it == state.classToAdvisorId.end()) return false;
        auto advisorState = std::make_shared<AdvisorState>();
        advisorState->id = it->second;
        auto* advisor = new FakeAdvisor(advisorState);
        advisor->AddRef();   // the creation reference
        state.advisors.push_back(advisorState);
        *out = advisor;
        return true;
    }
    DetokenizeMethod AdviceDetokenizerFunction() override { return DummyDetokenize; }
    const LuaExtension* LuaExtensions(std::size_t& count) override {
        count = extensions.size();
        return extensions.empty() ? nullptr : extensions.data();
    }
    void UpdateResourcesFromDirectory(std::uint32_t /*package*/, const char* directory) override {
        updatedDirectories.emplace_back(directory ? directory : "");
    }
    void LoadPackageScripts(cISCLua*, std::uint32_t, int) override { ++packageScriptCalls; }
    cIGZDate* SimDate() override { return &simDate; }
    void SimStartDate(cIGZDate& out) override { out.Set(1u, 1u, static_cast<std::uint32_t>(simStartYear)); }
    bool SimulatorAnyPaused() override { return anyPaused; }
    void ToggleLlamaCheat() override { ++llamaToggles; }

    static bool DummyDetokenize(const cIGZString&, cIGZString&, void*) { return false; }
};

// ---------------------------------------------------------------------------------------------------------------
// Recording stand-ins for the nested-object DB helpers (the real ones live in sc4/DBSegment.cpp).
// ---------------------------------------------------------------------------------------------------------------
int SomeExtensionFunction(lua_State*) { return 0; }

std::vector<const cIGZUnknown*> g_dbWritten;
std::vector<int> g_dbReadIds;

}  // namespace

namespace sc4 {

bool SC4WriteGZSerializable(cIGZOStream& /*stream*/, const cIGZUnknown* object) {
    g_dbWritten.push_back(object);
    return true;
}

bool SC4ReadGZSerializable(cIGZIStream& /*stream*/, std::uint32_t iid, void** out, bool /*loadNow*/) {
    *out = nullptr;
    if (iid != kGZIID_cISC4Advisor || g_dbReadIds.empty()) return false;
    auto state = std::make_shared<AdvisorState>();
    state->id = g_dbReadIds.front();
    g_dbReadIds.erase(g_dbReadIds.begin());
    auto* advisor = new FakeAdvisor(state);
    advisor->AddRef();
    *out = static_cast<cISC4Advisor*>(advisor);
    return true;
}

}  // namespace sc4

// ---------------------------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------------------------
namespace {

LValue TableWithRows(int count) {
    auto table = std::make_shared<Table>();
    table->length = count;
    for (int i = 1; i <= count; ++i) {
        auto row = std::make_shared<Table>();
        row->named["class_id"] = MakeNum(0x1000 + i);
        row->named["id"] = MakeNum(10 * i);
        table->array[i] = MakeTable(row);
    }
    return MakeTable(table);
}

void SetAdviceTable(FakeLua& lua, int count) {
    lua.globals->named["advisors"] = TableWithRows(count);
}

void SetTuningConstants(FakeLua& lua, int low, int medium, int high) {
    auto tuning = std::make_shared<Table>();
    auto priorities = std::make_shared<Table>();
    priorities->named["low"] = MakeNum(low);
    priorities->named["medium"] = MakeNum(medium);
    priorities->named["high"] = MakeNum(high);
    tuning->named["ADVICE_POPUP_PRIORITIES_BY_SKILL_LEVEL"] = MakeTable(priorities);
    lua.globals->named["tuning_constants"] = MakeTable(tuning);
    lua.globals->named["game"] = MakeTable(std::make_shared<Table>());
}

std::uint32_t ReadU32(const std::vector<unsigned char>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // ---- fixture ----------------------------------------------------------------------------------------------
    auto* lua = new FakeLua();
    lua->AddRef();
    SetAdviceTable(*lua, 2);
    SetTuningConstants(*lua, 70, 80, 90);

    FakeMessages messages;
    auto* cheats = new cGZCheatCodeManager();
    cheats->AddRef();
    auto* detokenizer = new FakeDetokenizer();
    detokenizer->AddRef();

    FakeHost host;
    host.lua = lua;
    host.messages = &messages;
    host.cheats = cheats;
    host.detokenizer = detokenizer;
    host.state.classToAdvisorId = {{0x1001, 10}, {0x1002, 20}};
    host.simDate.Set(7u, 4u, 2024u);   // July 4th 2024

    // process-wide extension record: one global (Init) and one game-table (reload) entry
    host.extensions.push_back({"print", &SomeExtensionFunction, true});
    host.extensions.push_back({"advice_info", &SomeExtensionFunction, false});

    SetAdvisorSystemHost(&host);

    // ---- construction / layout --------------------------------------------------------------------------------
    auto* system = new cSC4AdvisorSystem();
    CHECK(system != nullptr);
    CHECK(cSC4AdvisorSystem::Instance() == system);
    CHECK(sizeof(cSC4AdvisorSystem) == 0xf8);
    CHECK(reinterpret_cast<char*>(&system->Advisors()) - reinterpret_cast<char*>(system) == 0x60);
    CHECK(reinterpret_cast<char*>(&system->EventSubscribers()) - reinterpret_cast<char*>(system) == 0x98);
    CHECK(reinterpret_cast<char*>(&system->ContextAdvices()) - reinterpret_cast<char*>(system) == 0xb0);
    CHECK(reinterpret_cast<const char*>(system->Thresholds()) - reinterpret_cast<const char*>(system) == 0x2c);
    CHECK(system->Thresholds()[0] == 100 && system->Thresholds()[1] == 100 && system->Thresholds()[2] == 100);
    CHECK(system->GetRandom() != nullptr);
    CHECK(!system->Initialized() && !system->LoadedFromSave() && !system->DataReloaded());
    CHECK(system->LastDateNumber() == 0);
    CHECK(system->GetAdvisor(1) == nullptr);

    system->AddRef();

    // ---- QueryInterface views ---------------------------------------------------------------------------------
    void* view = nullptr;
    CHECK(system->QueryInterface(kGZIID_cISC4AdvisorSystem, &view) && view == static_cast<cISC4AdvisorSystem*>(system));
    CHECK(system->QueryInterface(kGZIID_cIGZMessageTarget2, &view) &&
          view == static_cast<cIGZMessageTarget2*>(system));
    CHECK(system->QueryInterface(kGZIID_cGZMessageTargetLegacy, &view) &&
          view == static_cast<cIGZMessageTarget*>(system));
    CHECK(system->QueryInterface(kGZIID_cIGZSerializable, &view) &&
          view == static_cast<cIGZSerializable*>(system));
    CHECK(system->QueryInterface(kGZIID_cIGZUnknown, &view));
    CHECK(!system->QueryInterface(0x11111111, &view));
    system->Release();   // the QueryInterface references
    system->Release();
    system->Release();
    system->Release();
    system->Release();

    // ---- Init -------------------------------------------------------------------------------------------------
    CHECK(system->Init());
    CHECK(lua->openCalls == 1);
    CHECK(system->DataTags().size() == 2);
    CHECK(system->DataTags()[0] == 100 && system->DataTags()[1] == 101);
    CHECK(lua->tagMethods.size() == 2 && lua->tagMethods[0].second == "gc" && lua->tagMethods[1].second == "gc");
    CHECK(system->Initialized());
    CHECK(!system->LoadedFromSave());
    CHECK(system->Advisors().size() == 2);
    CHECK(system->GetAdvisor(10) != nullptr && system->GetAdvisor(20) != nullptr);
    CHECK(system->GetAdvisor(10)->GetID() == 10);
    CHECK(host.state.advisors.size() == 2);
    CHECK(host.state.advisors[0]->initRows.size() == 2);   // load_advisor + update_advisors
    // the detokenizer registration used `this` and add = true
    CHECK(detokenizer->registrations.size() == 1);
    CHECK(detokenizer->registrations[0].add);
    CHECK(detokenizer->registrations[0].context == system);
    CHECK(detokenizer->registrations[0].method == &FakeHost::DummyDetokenize);
    // reload_data: both "advisors/" directories were scanned, the package scripts loaded, the tutorial got the Lua
    CHECK(host.updatedDirectories.size() == 2);
    CHECK(host.updatedDirectories[0] == "/data/advisors/");
    CHECK(host.updatedDirectories[1] == "/plugins/advisors/");
    CHECK(host.packageScriptCalls == 1);
    CHECK(host.tutorialLua.size() == 1 && host.tutorialLua[0] == lua);
    // the thresholds came from the tuning constants
    CHECK(system->Thresholds()[0] == 70 && system->Thresholds()[1] == 80 && system->Thresholds()[2] == 90);
    // three notifications added, the window created and initialised, the cheats registered
    CHECK(messages.added.size() == 3);
    CHECK(messages.added[0].second == kAdvisorMessageCityStarted);
    CHECK(messages.added[1].second == kAdvisorMessageCityStarted2);
    CHECK(messages.added[2].second == kAdvisorMessageTick);
    CHECK(host.winProc->created == 1 && host.winProc->initCalls == 1);
    CHECK(system->WinProc() != nullptr);
    std::uint32_t cheatCount = 0;
    CHECK(cheats->GetListOfAllCheatStrings(nullptr, cheatCount) == 1);
    // register_api: the global extension went to the globals, the other one to "game"
    CHECK(lua->FindGlobal("print") != nullptr);
    LValue* game = lua->FindGlobal("game");
    CHECK(game && game->kind == LValue::TableK && game->table->named.count("advice_info") == 1);
    // the detokenizer function pointer was resolved through the host
    // (the registered method is FakeHost::DummyDetokenize as checked above)

    // a second Init does not reopen the scripts but recreates the window
    CHECK(system->Init());
    CHECK(lua->openCalls == 1);
    CHECK(host.winProc->created == 2 && host.winProc->initCalls == 2 && host.winProc->shutdownCalls == 0);
    CHECK(messages.added.size() == 3);

    // ---- advisor registry -------------------------------------------------------------------------------------
    auto advisorState = std::make_shared<AdvisorState>();
    advisorState->id = 30;
    auto* advisor = new FakeAdvisor(advisorState);
    advisor->AddRef();
    const std::size_t postedBefore = messages.messages.size();
    CHECK(system->AddAdvisor(advisor));
    CHECK(system->GetAdvisor(30) == advisor);
    CHECK(advisorState->refs == 2);   // creation reference + the map
    CHECK(messages.messages.size() == postedBefore + 1);
    CHECK(messages.messages.back().type == kAdvisorNotifyAdvisorAdded);
    CHECK(messages.messages.back().unknown == advisor);
    CHECK(messages.messages.back().post);
    // re-adding keeps a single map reference
    CHECK(system->AddAdvisor(advisor));
    CHECK(advisorState->refs == 2);
    CHECK(!system->RemoveAdvisor(999));
    // event subscriptions
    CHECK(system->SubscribeToEvent(0x1234, advisor));
    CHECK(system->SubscribeToEvent(0x1234, system->GetAdvisor(10)));
    CHECK(advisorState->pushedEvents.empty());
    CHECK(system->PushEvent(0x1234, nullptr));
    CHECK(advisorState->pushedEvents.size() == 1 && advisorState->pushedEvents[0] == 0x1234);
    CHECK(system->PushEvent(0x9999, nullptr));   // unknown event: no-op, still true
    CHECK(!system->UnsubscribeFromEvent(0x9999, advisor));
    CHECK(!system->UnsubscribeFromEvent(0x1234, system->GetAdvisor(20)));
    CHECK(system->RemoveAdvisor(30));
    CHECK(system->GetAdvisor(30) == nullptr);
    CHECK(advisorState->refs == 1);   // only the creation reference remains
    CHECK(messages.messages.back().type == kAdvisorNotifyAdvisorRemoved);
    CHECK(advisor->Shutdown() == true);   // direct call, not through the system
    advisor->Release();

    // ---- context advice stack ---------------------------------------------------------------------------------
    auto adviceState = std::make_shared<AdviceState>();
    auto* advice = new FakeAdvice(adviceState);
    advice->AddRef();
    CHECK(system->GetContextAdvice() == nullptr);
    system->PushContextAdvice(advice);
    CHECK(adviceState->refs == 2);
    CHECK(system->GetContextAdvice() == advice);
    CHECK(system->PopContextAdvice() == advice);
    CHECK(adviceState->refs == 1);
    CHECK(system->GetContextAdvice() == nullptr);
    CHECK(system->PopContextAdvice() == nullptr);
    advice->Release();

    // ---- values -----------------------------------------------------------------------------------------------
    CHECK(system->SetGlobalValue("answer", 42.0));
    LValue* gameTable = lua->FindGlobal("game");
    CHECK(gameTable && gameTable->kind == LValue::TableK);
    CHECK(gameTable->table->named.count("answer") == 1 && gameTable->table->named["answer"].number == 42.0);
    cRZString text("hello");
    CHECK(system->SetGlobalValue("label", &text));
    const LValue& stored = gameTable->table->named["label"];
    CHECK(stored.kind == LValue::UserDataK && stored.userData == &text && stored.tag == system->GetDataTag(1));
    CHECK(text.AddRef() == 2);   // AddRef'd for the userdata
    text.Release();
    auto* subject = new cSC4AdviceSubject();
    subject->AddRef();
    CHECK(system->SetLocalValue("population", 1234.0, subject));
    CHECK(gameTable->table->named.count("population") == 1);
    CHECK(gameTable->table->named["population"].number == 1234.0);
    const LValue& subjectValue = gameTable->table->named["population_subject"];
    CHECK(subjectValue.kind == LValue::UserDataK && subjectValue.userData == static_cast<cISC4AdviceSubject*>(subject) &&
          subjectValue.tag == system->GetDataTag(0));
    CHECK(system->SetLocalValue("nothing", 1.0, nullptr));
    const LValue& nullSubject = gameTable->table->named["nothing_subject"];
    CHECK(nullSubject.kind == LValue::UserDataK && nullSubject.userData == nullptr && nullSubject.tag == 0);
    subject->Release();   // the creation reference
    subject->Release();   // the reference the Lua userdata holds (no GC in the fake)

    // ---- DoLink -----------------------------------------------------------------------------------------------
    lua->doBuffers.clear();
    CHECK(system->DoLink(cRZString("000000000my chunk")));
    CHECK(lua->doBuffers.size() == 1 && lua->doBuffers.back() == "0my chunk");
    lua->doBuffers.clear();
    CHECK(system->DoLink(cRZString("0000001d00000000return 2")));
    CHECK(lua->doBuffers.size() == 1 && lua->doBuffers.back() == "return 2");
    CHECK(!system->DoLink(cRZString("not a link")));
    CHECK(!system->DoLink(cRZString("00000001")));   // sscanf reads only one value
    // an advice link: type 30 (the advisor) + guid + body, the body after the 16 hex characters
    auto* linkAdvisor = system->GetAdvisor(10);
    static_cast<FakeAdvisor*>(linkAdvisor)->state->adviceSession = 0x1234;
    lua->doBuffers.clear();
    CHECK(system->DoLink(cRZString("0000000a00001234some body")));
    CHECK(static_cast<FakeAdvisor*>(linkAdvisor)->state->advice->links.size() == 1);
    CHECK(!static_cast<FakeAdvisor*>(linkAdvisor)->state->advice->links.empty() &&
          static_cast<FakeAdvisor*>(linkAdvisor)->state->advice->links.back() == "some body");
    CHECK(lua->doBuffers.empty());
    // an unknown type falls back to the Lua chunk after the first eight characters
    CHECK(system->DoLink(cRZString("0000009900000000chunk")));
    CHECK(lua->doBuffers.size() == 1 && lua->doBuffers.back() == "00000000chunk");

    // ---- priority thresholds by difficulty --------------------------------------------------------------------
    host.difficulty = 0;
    CHECK(system->GetAdvicePopupPriorityThreshold() == 70);
    host.difficulty = 1;
    CHECK(system->GetAdvicePopupPriorityThreshold() == 80);
    host.difficulty = 2;
    CHECK(system->GetAdvicePopupPriorityThreshold() == 90);
    host.difficulty = 7;
    CHECK(system->GetAdvicePopupPriorityThreshold() == 70);

    // ---- simulation -------------------------------------------------------------------------------------------
    system->simulate_all_advisors(1);
    CHECK(host.state.advisors[0]->simulatePeriods.size() == 1 &&
          host.state.advisors[0]->simulatePeriods[0] == 1);
    CHECK(host.state.advisors[1]->simulatePeriods.size() == 1);

    // ---- gc tag method ------------------------------------------------------------------------------------------
    {
        struct Holder {
            alignas(8) cIGZUnknown* threadSlot = nullptr;
            char state[64] = {};
        } holder;
        struct FakeThread final : cIGZUnknown {
            FakeLua* lua = nullptr;
            bool QueryInterface(std::uint32_t iid, void** out) override {
                if (iid != 0xea10c4a2) return false;
                *out = static_cast<cISCLua*>(lua);
                lua->AddRef();
                return true;
            }
            std::uint32_t AddRef() override { return 1; }
            std::uint32_t Release() override { return 1; }
        } thread;
        thread.lua = lua;
        holder.threadSlot = &thread;
        auto tagState = std::make_shared<AdviceState>();
        auto* tagObject = new FakeAdvice(tagState);
        tagObject->AddRef();
        lua->PushUserData(tagObject, system->GetDataTag(0));
        CHECK(cSC4AdvisorSystem::gc_tag_method(reinterpret_cast<lua_State*>(holder.state)) == 0);
        CHECK(tagState->refs == 0);   // released (and deleted) by the tag method
        // a second userdata with the other tag is released as well
        auto tagState2 = std::make_shared<AdviceState>();
        auto* tagObject2 = new FakeAdvice(tagState2);
        tagObject2->AddRef();
        lua->PushUserData(tagObject2, system->GetDataTag(1));
        CHECK(cSC4AdvisorSystem::gc_tag_method(reinterpret_cast<lua_State*>(holder.state)) == 0);
        CHECK(tagState2->refs == 0);
        // a tag that is neither of the two is left alone
        auto* tagObject3 = new FakeAdvice(std::make_shared<AdviceState>());
        tagObject3->AddRef();
        lua->PushUserData(tagObject3, 0x7777);
        CHECK(cSC4AdvisorSystem::gc_tag_method(reinterpret_cast<lua_State*>(holder.state)) == 0);
        CHECK(tagObject3->state->refs == 1);
        tagObject3->Release();
        // clear the stack
        lua->SetTop(0);
    }

    // ---- DoMessage: city started --------------------------------------------------------------------------------
    {
        cRZMessage2Standard message;
        message.SetType(kAdvisorMessageCityStarted);
        message.SetVoid1(&host.city);
        messages.messages.clear();
        CHECK(system->DoMessage(&message));
        CHECK(!system->LoadedFromSave());
        std::size_t tickPosts = 0;
        for (const PostedMessage& m : messages.messages) {
            if (m.type == kAdvisorMessageTick) ++tickPosts;
        }
        CHECK(tickPosts == 1);
        // lastDateNumber_ was still 0, so a month was simulated for both advisors
        CHECK(host.state.advisors[0]->simulatePeriods.size() == 2);
    }
    {
        // not established: 0x26d31ec1 only clears the loaded-from-save flag (no tick, no simulation)
        host.cityEstablished = false;
        cRZMessage2Standard message;
        message.SetType(kAdvisorMessageCityStarted);
        message.SetVoid1(&host.city);
        messages.messages.clear();
        const std::size_t periodsBefore = host.state.advisors[0]->simulatePeriods.size();
        CHECK(system->DoMessage(&message));
        CHECK(messages.messages.empty());
        CHECK(host.state.advisors[0]->simulatePeriods.size() == periodsBefore);
        host.cityEstablished = true;
    }
    {
        // 0x26d31ec4: pushes the event, ticks and simulates a month while no date was ticked
        cRZMessage2Standard message;
        message.SetType(kAdvisorMessageCityStarted2);
        messages.messages.clear();
        CHECK(system->DoMessage(&message));
        std::size_t tickPosts = 0;
        for (const PostedMessage& m : messages.messages) {
            if (m.type == kAdvisorMessageTick) ++tickPosts;
        }
        CHECK(tickPosts == 1);
    }

    // ---- DoMessage: periodic tick ---------------------------------------------------------------------------
    {
        // a new day (July 5th): lastDateNumber_ moves, the g_* globals are written, no monthly pass
        cGZDate nextDay(7u, 5u, 2024u);
        host.simDate = nextDay;
        cRZMessage2Standard message;
        message.SetType(kAdvisorMessageTick);
        const std::size_t simulationsBefore = host.state.advisors[0]->simulatePeriods.size();
        CHECK(system->DoMessage(&message));
        CHECK(system->LastDateNumber() == static_cast<int>(nextDay.DayNumber()));
        gameTable = lua->FindGlobal("game");
        CHECK(gameTable->table->named.count("g_year") == 1 && gameTable->table->named["g_year"].number == 2024.0);
        CHECK(gameTable->table->named.count("g_month") == 1 && gameTable->table->named["g_month"].number == 7.0);
        CHECK(gameTable->table->named.count("g_day") == 1 && gameTable->table->named["g_day"].number == 5.0);
        CHECK(gameTable->table->named.count("g_date") == 1);
        CHECK(gameTable->table->named.count("g_year_count") == 1 &&
              gameTable->table->named["g_year_count"].number == 24.0);
        CHECK(host.state.advisors[0]->simulatePeriods.size() == simulationsBefore + 1);   // simulate_all_advisors(0)
    }
    {
        // the 28th of the month: one simulation with period 1 and the Lua GC threshold reset
        cGZDate monthly(7u, 28u, 2024u);
        host.simDate = monthly;
        lua->gcThreshold = 500;
        lua->gcThresholdSets.clear();
        cRZMessage2Standard message;
        message.SetType(kAdvisorMessageTick);
        const std::size_t periods = host.state.advisors[0]->simulatePeriods.size();
        CHECK(system->DoMessage(&message));
        CHECK(lua->gcThresholdSets.size() == 1 && lua->gcThresholdSets[0] == 0);
        CHECK(host.state.advisors[0]->simulatePeriods.size() == periods + 2);   // monthly + the regular pass
        CHECK(host.state.advisors[0]->simulatePeriods[periods] == 1);
        CHECK(host.state.advisors[0]->simulatePeriods[periods + 1] == 0);
    }
    {
        // the same day: not paused and the timer has not run 501 ms -> only a tick, no simulation
        cRZMessage2Standard message;
        message.SetType(kAdvisorMessageTick);
        messages.messages.clear();
        const std::size_t periods = host.state.advisors[0]->simulatePeriods.size();
        host.anyPaused = false;
        CHECK(system->DoMessage(&message));
        CHECK(host.state.advisors[0]->simulatePeriods.size() == periods);
        std::size_t tickPosts = 0;
        for (const PostedMessage& m : messages.messages) {
            if (m.type == kAdvisorMessageTick) ++tickPosts;
        }
        CHECK(tickPosts == 1);
    }
    {
        // a new day with the tick clock already running: the full pass restarts the timer at g_ticks = 1000
        g_ticks = 1000;
        cGZDate next(7u, 29u, 2024u);
        host.simDate = next;
        cRZMessage2Standard message;
        message.SetType(kAdvisorMessageTick);
        CHECK(system->DoMessage(&message));
        CHECK(system->LastDateNumber() == static_cast<int>(next.DayNumber()));
    }
    {
        // the same day while paused but within the 501 ms interval: tick only
        cRZMessage2Standard message;
        message.SetType(kAdvisorMessageTick);
        messages.messages.clear();
        const std::size_t periods = host.state.advisors[0]->simulatePeriods.size();
        host.anyPaused = true;
        g_ticks = 1500;   // 500 ms since the restart
        CHECK(system->DoMessage(&message));
        CHECK(host.state.advisors[0]->simulatePeriods.size() == periods);
    }
    {
        // the same day, paused, past the interval: a full pass (simulate 0 + timer restart)
        cRZMessage2Standard message;
        message.SetType(kAdvisorMessageTick);
        messages.messages.clear();
        const std::size_t periods = host.state.advisors[0]->simulatePeriods.size();
        host.anyPaused = true;
        g_ticks = 1600;   // 600 ms since the restart
        CHECK(system->DoMessage(&message));
        CHECK(host.state.advisors[0]->simulatePeriods.size() == periods + 1);
        CHECK(host.state.advisors[0]->simulatePeriods.back() == 0);
        host.anyPaused = false;
    }
    {
        // the data reload flag makes the tick reload the scripts first
        system->SetDataReloaded(true);
        CHECK(system->DataReloaded());
        host.updatedDirectories.clear();
        cGZDate later(7u, 30u, 2024u);
        host.simDate = later;
        cRZMessage2Standard message;
        message.SetType(kAdvisorMessageTick);
        CHECK(system->DoMessage(&message));
        CHECK(!system->DataReloaded());
        CHECK(host.updatedDirectories.size() == 2);
    }

    // ---- DoMessage: cheats ------------------------------------------------------------------------------------
    {
        cRZMessage2Standard message;
        message.SetType(kAdvisorMessageCheat);
        message.SetData1(kAdvisorCheatLlamaHead);
        CHECK(system->DoMessage(&message));
        CHECK(host.llamaToggles == 1);
    }
    {
        cRZString command("/guids:on");
        cRZMessage2Standard message;
        message.SetType(kAdvisorMessageCheat);
        message.SetData1(kAdvisorCheatCheats);
        message.SetVoid2(&command);
        messages.messages.clear();
        CHECK(system->DoMessage(&message));
        CHECK(messages.messages.size() == 1);
        CHECK(messages.messages[0].type == kAdvisorMessageTriggered && messages.messages[0].data1 == 1);
    }
    {
        cRZString command("/guids:off");
        cRZMessage2Standard message;
        message.SetType(kAdvisorMessageCheat);
        message.SetData1(kAdvisorCheatCheats);
        message.SetVoid2(&command);
        messages.messages.clear();
        CHECK(system->DoMessage(&message));
        CHECK(messages.messages.size() == 1 && messages.messages[0].data1 == 0);
    }
    {
        cRZString command("/trigger:0000abcd");
        cRZMessage2Standard message;
        message.SetType(kAdvisorMessageCheat);
        message.SetData1(kAdvisorCheatCheats);
        message.SetVoid2(&command);
        messages.messages.clear();
        CHECK(system->DoMessage(&message));
        CHECK(messages.messages.size() == 1);
        CHECK(messages.messages[0].type == kAdvisorMessageAdviceChanged);
        CHECK(messages.messages[0].data1 == 0xabcd);
    }
    {
        cRZString command("/print:00000010");
        cRZMessage2Standard message;
        message.SetType(kAdvisorMessageCheat);
        message.SetData1(kAdvisorCheatCheats);
        message.SetVoid2(&command);
        messages.messages.clear();
        CHECK(system->DoMessage(&message));
        CHECK(messages.messages.size() == 1);
        CHECK(messages.messages[0].type == kAdvisorMessageAdviceDump && messages.messages[0].data1 == 0x10);
    }
    {
        cRZString command("/refresh");
        cRZMessage2Standard message;
        message.SetType(kAdvisorMessageCheat);
        message.SetData1(kAdvisorCheatCheats);
        message.SetVoid2(&command);
        messages.messages.clear();
        CHECK(system->DoMessage(&message));
        CHECK(messages.messages.size() == 1 && messages.messages[0].type == kAdvisorMessageAdviceReset);
    }
    {
        cRZString command("/reload");
        cRZMessage2Standard message;
        message.SetType(kAdvisorMessageCheat);
        message.SetData1(kAdvisorCheatCheats);
        message.SetVoid2(&command);
        host.updatedDirectories.clear();
        CHECK(system->DoMessage(&message));
        CHECK(host.updatedDirectories.size() == 2);
    }
    {
        cRZString command("/guids:maybe");
        cRZMessage2Standard message;
        message.SetType(kAdvisorMessageCheat);
        message.SetData1(kAdvisorCheatCheats);
        message.SetVoid2(&command);
        messages.messages.clear();
        CHECK(system->DoMessage(&message));
        CHECK(messages.messages.empty());
    }
    {
        // an unrelated message type is ignored
        cRZMessage2Standard message;
        message.SetType(0xdeadbeef);
        CHECK(system->DoMessage(&message));
        CHECK(system->DoMessage(static_cast<cIGZMessage2*>(nullptr)));
    }

    // ---- window -------------------------------------------------------------------------------------------------
    CHECK(system->WinProc() != nullptr);
    system->ShowAdvisorWindow();
    CHECK(host.winProc->showCalls == 1 && system->IsAdvisorWindowVisible());
    system->HideAdvisorWindow();
    CHECK(host.winProc->hideCalls == 1 && !system->IsAdvisorWindowVisible());
    system->RemoveWinProcs();
    CHECK(host.winProc->shutdownCalls == 1 && system->GetAdvisorWinProc() == nullptr && !system->IsAdvisorWindowVisible());
    system->RemoveWinProcs();   // no window: no-op
    system->startAdvisorWindow();
    CHECK(host.winProc->created == 3 && system->WinProc() != nullptr);

    // ---- Write --------------------------------------------------------------------------------------------------
    {
        g_dbWritten.clear();
        PortMemoryOStream stream;
        CHECK(system->Write(stream));
        const std::vector<unsigned char>& bytes = stream.bytes();
        CHECK(bytes.size() == 2 + 4 + 4 * system->Advisors().size());
        CHECK((bytes[0] | (bytes[1] << 8)) == kAdvisorStreamVersion);
        CHECK(ReadU32(bytes, 2) == system->Advisors().size());
        CHECK(g_dbWritten.size() == system->Advisors().size());
        CHECK(ReadU32(bytes, 6) == 10);    // first advisor id
        CHECK(ReadU32(bytes, 10) == 20);   // second advisor id
        CHECK(g_dbWritten[0] != nullptr && g_dbWritten[1] != nullptr);
    }
    // ---- Read ---------------------------------------------------------------------------------------------------
    {
        auto* readSystem = new cSC4AdvisorSystem();
        readSystem->AddRef();
        std::vector<unsigned char> bytes;
        bytes.push_back(1); bytes.push_back(0);            // version 1
        bytes.push_back(2); bytes.push_back(0); bytes.push_back(0); bytes.push_back(0);   // count 2
        for (std::uint32_t id : {50u, 60u}) {
            bytes.push_back(static_cast<unsigned char>(id));
            bytes.push_back(0); bytes.push_back(0); bytes.push_back(0);
        }
        g_dbReadIds = {50, 60};
        PortMemoryIStream stream(bytes.data(), bytes.size());
        CHECK(readSystem->Read(stream));
        CHECK(readSystem->LoadedFromSave());
        CHECK(readSystem->Advisors().size() == 2);
        CHECK(readSystem->GetAdvisor(50) != nullptr && readSystem->GetAdvisor(50)->GetID() == 50);
        CHECK(readSystem->GetAdvisor(60) != nullptr && readSystem->GetAdvisor(60)->GetID() == 60);
        CHECK(g_dbReadIds.empty());
        // a wrong version is refused and does not mark the system loaded
        std::vector<unsigned char> bad{9, 0, 0, 0, 0, 0};
        PortMemoryIStream badStream(bad.data(), bad.size());
        CHECK(!readSystem->Read(badStream));
        readSystem->Release();
    }

    // ---- kill_advisors (through Shutdown) ----------------------------------------------------------------------
    const std::size_t advisorsBeforeShutdown = system->Advisors().size();
    CHECK(advisorsBeforeShutdown == 2);
    messages.messages.clear();
    detokenizer->registrations.clear();
    CHECK(system->Shutdown());
    CHECK(!system->Initialized());
    CHECK(system->Advisors().empty());
    CHECK(system->EventSubscribers().empty());
    CHECK(system->ContextAdvices().empty());
    for (const auto& advisorState2 : host.state.advisors) {
        CHECK(advisorState2->shutdownCalls == 1);
    }
    // the kill notification, then the two removal notifications
    bool killed = false;
    for (const PostedMessage& m : messages.messages) {
        if (m.type == kAdvisorNotifyAdvisorsKilled) killed = true;
    }
    CHECK(killed);
    CHECK(messages.removed.size() == 3);
    // detokenizer: unregistered with the Lua context and add = false
    CHECK(detokenizer->registrations.size() == 1);
    CHECK(!detokenizer->registrations[0].add);
    CHECK(detokenizer->registrations[0].context == lua);
    // the cheat registrations are gone
    cheatCount = 0;
    CHECK(cheats->GetListOfAllCheatStrings(nullptr, cheatCount) == 0);
    // the Lua state was closed and the tutorial registry cleared
    CHECK(lua->closeCalls == 1);
    CHECK(lua->gcThresholdSets.size() >= 1 && lua->gcThresholdSets.back() == 0);
    CHECK(host.tutorialLua.size() >= 2 && host.tutorialLua.back() == nullptr);
    CHECK(system->WinProc() == nullptr);
    CHECK(system->Shutdown());   // already shut down, still true

    // ---- destructor ---------------------------------------------------------------------------------------------
    system->Release();
    CHECK(cSC4AdvisorSystem::Instance() == nullptr);   // the second (Read) instance already cleared it
    CHECK(host.winProc->shutdownCalls == 2);
    host.winProc.reset();   // the fake win proc state must not outlive the assertions

    SetAdvisorSystemHost(nullptr);
    lua->Release();
    detokenizer->Release();
    cheats->Release();

    if (failures == 0) {
        std::printf("sc4_advisorsystem_test: all checks passed\n");
        return 0;
    }
    std::printf("sc4_advisorsystem_test: %d failure(s)\n", failures);
    return 1;
}
