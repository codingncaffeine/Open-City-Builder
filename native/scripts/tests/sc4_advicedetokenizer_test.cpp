// gAdviceDetokenizerFunction: the link prefix of the context advice (advisor id + SESSION id), the advisor's name, the
// trimmed case-insensitive token names, the "X@" format prefixes and the "_dasd_=(TOKEN)" Lua path, and the
// "%UNKNOWN TOKEN --name--%" answer when nothing resolves a token.
//
// The advisor system, advice, advisor and Lua are fakes. RZ::LM() answers null here (the language's number and date
// strings are not exercised), SC4MakeMoneyString writes "$N", and the situation manager is absent (its tokens resolve to
// the unknown text); its four out-of-line methods are defined here only to link.
#include <cstdio>
#include <string>
#include <vector>

#include "gz/RZServices.h"
#include "lua/ISCLua.h"
#include "rz/cRZString.h"
#include "sc4/Advice.h"
#include "sc4/AdviceDetokenizer.h"
#include "sc4/AdviceSubject.h"
#include "sc4/Advisor.h"
#include "sc4/AdvisorSystem.h"
#include "sc4/CitySituationManager.h"
#include "sc4/SC4Utilities.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECK_EQ(a, b) do { if (!((a) == (b))) { std::printf("FAIL line %d: %s == %s\n", __LINE__, #a, #b); ++failures; } } while (0)

// Link stand-ins.
cIGZLanguageManager* RZ::LM() { return nullptr; }
namespace sc4 {
void SC4MakeMoneyString(std::int64_t value, cIGZString& out) { out.Sprintf("$%lld", static_cast<long long>(value)); }
std::uint32_t cSC4CitySituationManager::GetNumTargetsRemaining() const { return 0; }
void cSC4CitySituationManager::GetCurrentTargetName(cRZString&) const {}
bool cSC4CitySituationManager::GetSituationAura(float&, float&) const { return false; }
bool cSC4CitySituationManager::GetSituationMoney(std::int64_t&) const { return false; }
}  // namespace sc4

namespace {

using namespace sc4;

struct FakeSubject final : cISC4AdviceSubject {
    std::uint32_t data1 = 0, data2 = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    void SetLocation(float, float) override {}
    float GetLocationX() override { return 0; }
    float GetLocationZ() override { return 0; }
    bool HasLocation() override { return false; }
    bool SetObject(cIGZUnknown*) override { return false; }
    cIGZUnknown* GetObjectA() override { return nullptr; }
    std::uint32_t GetData1() const override { return data1; }
    void SetData1(std::uint32_t v) override { data1 = v; }
    std::uint32_t GetData2() const override { return data2; }
    void SetData2(std::uint32_t v) override { data2 = v; }
    bool Clone(cISC4AdviceSubject**) override { return false; }
};

struct FakeAdvice final : cISC4Advice {
    int advisorId = 0, guid = 0, session = 0;
    FakeSubject* subject = nullptr;
    cRZString text;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool Init(int) override { return true; }
    bool Shutdown() override { return true; }
    bool Clone(cISC4Advice**) override { return false; }
    int GetMood() const override { return 3; }
    int GetPriority() const override { return 0; }
    bool GetTrigger() const override { return false; }
    cIGZString& GetTriggerString() override { return text; }
    int GetGUID() const override { return guid; }
    int GetAdvisorID() const override { return advisorId; }
    void SetAdvisorID(int id) override { advisorId = id; }
    int GetSessionID() const override { return session; }
    bool SetFlags(int, bool) override { return true; }
    bool CheckFlags(int) const override { return true; }
    int GetFrequencyInDays() const override { return 0; }
    int GetTimeOutInDays() const override { return 0; }
    const cIGZString& GetTextTitle() const override { return text; }
    std::uint32_t GetTextTitleGUID() const override { return 0; }
    const cIGZString& GetTextMessage() const override { return text; }
    std::uint32_t GetTextMessageGUID() const override { return 0; }
    bool DoLink(const cIGZString&) override { return false; }
    void NotifyGlobalEffectTriggerChanged() override {}
    bool SetEventSubject(cISC4AdviceSubject*) override { return false; }
    cISC4AdviceSubject* GetEventSubject() override { return subject; }
    int GetEventID() override { return 0; }
    int GetCommandID() override { return 0; }
    bool UpdateText() override { return true; }
    bool IsCurrent() const override { return true; }
};

struct FakeAdvisor final : cISC4Advisor {
    int id = 0;
    cRZString name{"Utilities Advisor"}, personal{"Ben Wattson"};
    AdviceMap advices;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
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
    int GetHeadAnimCount(bool) override { return 0; }
    bool SetHeadAnim(bool, int) override { return true; }
    int GetHeadAnimID(int) override { return 0; }
    bool PushEvent(int, cISC4AdviceSubject*) override { return false; }
    bool CommandWouldTrigger(std::uint32_t) override { return false; }
    bool RetireAdvice(cISC4Advice*) override { return false; }
};

// A Lua whose DoString records the code and answers the scripted status; "_dasd_" holds the scripted value.
struct FakeLua final : cISCLua {
    enum Kind { Nil, Number, Text, User };
    struct Value { Kind kind = Nil; double number = 0; std::string text; void* user = nullptr; int tag = 0; };
    std::vector<Value> stack;
    Value result;
    int status = 0;
    std::vector<std::string> code;
    int setTops = 0;
    Value& At(int i) { return stack[static_cast<std::size_t>(i < 0 ? static_cast<int>(stack.size()) + i : i - 1)]; }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool Open(int) override { return false; }
    bool Close() override { return false; }
    void* AsIGZLua5() override { return nullptr; }
    int GetTop() override { return static_cast<int>(stack.size()); }
    void SetTop(int i) override { ++setTops; stack.resize(static_cast<std::size_t>(i < 0 ? 0 : i)); }
    void PushValue(int) override {}
    void Remove(int) override {}
    void Insert(int) override {}
    void Pop(int n) override { stack.resize(stack.size() - static_cast<std::size_t>(n)); }
    int Type(int) override { return 0; }
    const char* TypeName(int) override { return ""; }
    bool IsNumber(int i) override { return At(i).kind == Number; }
    bool IsString(int i) override { return At(i).kind == Text || At(i).kind == Number; }   // Lua: numbers are strings too
    bool IsCFunction(int) override { return false; }
    int Tag(int i) override { return At(i).tag; }
    bool IsBoolean(int) override { return false; }
    bool Equal(int, int) override { return false; }
    bool LessThan(int, int) override { return false; }
    std::uint32_t ToUint32(int i) override { return static_cast<std::uint32_t>(At(i).number); }
    std::int32_t ToSint32(int i) override { return static_cast<std::int32_t>(At(i).number); }
    double ToNumber(int i) override { return At(i).number; }
    const char* ToString(int i) override { return At(i).text.c_str(); }
    int Strlen(int i) override { return static_cast<int>(At(i).text.size()); }
    CFunction ToCFunction(int) override { return nullptr; }
    void* ToUserData(int i) override { return At(i).user; }
    void* ToUserTag(int i) override { return At(i).user; }
    bool ToBoolean(int) override { return false; }
    void PushNil() override { stack.push_back({}); }
    void PushNumber(double) override {}
    void PushLString(const char*, int) override {}
    void PushString(const char*) override {}
    void PushCClosure(CFunction, int) override {}
    void PushUserTag(void*, int) override {}
    void PushBoolean(bool) override {}
    void GetGlobal(const char* name) override { stack.push_back(std::string(name) == "_dasd_" ? result : Value{}); }
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
    int DoString(const char* c) override { code.push_back(c); return status; }
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
    bool IsUserData(int i) override { return At(i).kind == User; }
    bool IsNil(int i) override { return At(i).kind == Nil; }
    bool ToDataRegistryEntryID(::cISC4GameDataRegistry*, int) override { return false; }
    bool ToDataRegistryNode(::cISC4GameDataRegistry*, int) override { return false; }
    bool ToDataRegistryValue(::cISC4GameDataRegistry*, int) override { return false; }
};

struct FakeSystem final : cISC4AdvisorSystem {
    FakeLua lua;
    FakeAdvice* context = nullptr;
    FakeAdvisor advisor;
    std::vector<int> tagsAsked;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool AddAdvisor(cISC4Advisor*) override { return false; }
    bool RemoveAdvisor(int) override { return false; }
    cISC4Advisor* GetAdvisor(int id) override { return id == advisor.id ? &advisor : nullptr; }
    AdvisorMap* GetAdvisors() override { return nullptr; }
    cRZRandom* GetRandom() override { return nullptr; }
    cISCLua* GetScriptingContext() override { return &lua; }
    bool DoLink(const cIGZString&) override { return false; }
    bool SetGlobalValue(const char*, double) override { return false; }
    bool SetGlobalValue(const char*, cIGZString*) override { return false; }
    bool SetLocalValue(const char*, double, cISC4AdviceSubject*) override { return false; }
    bool PushEvent(int, cISC4AdviceSubject*) override { return false; }
    bool SubscribeToEvent(int, cISC4Advisor*) override { return false; }
    bool UnsubscribeFromEvent(int, cISC4Advisor*) override { return false; }
    void PushContextAdvice(cISC4Advice*) override {}
    cISC4Advice* PopContextAdvice() override { return nullptr; }
    cISC4Advice* GetContextAdvice() override { return context; }
    int GetAdvicePopupPriorityThreshold() const override { return 0; }
    int GetDataTag(int index) override { tagsAsked.push_back(index); return 77; }
};

struct Host final : IAdviceDetokenizerHost {
    FakeSystem* system = nullptr;
    cSC4CitySituationManager* SituationManager() override { return nullptr; }
    cISC4AdvisorSystem* System(void* context) override { return static_cast<FakeSystem*>(context); }
};

std::string Run(FakeSystem& system, const char* token, bool* answered = nullptr) {
    cRZString out;
    const bool ok = AdviceDetokenizerFunction(cRZString(token), out, &system);
    if (answered) *answered = ok;
    return std::string(out.ToChar(), out.Strlen());
}

void TestTokens() {
    Host host;
    SetAdviceDetokenizerHost(&host);
    FakeSystem system;
    FakeAdvice advice;
    advice.advisorId = 0x15;
    advice.guid = 0x1234;
    advice.session = 0x9a2;
    bool ok = false;

    // link_id: advisor id + SESSION id; sit_link_id the same; the name is trimmed and compared without case.
    CHECK_EQ(Run(system, "link_id", &ok), std::string("sc4://advice/0000000000000000"));   // no context advice
    CHECK(ok);
    system.context = &advice;
    CHECK_EQ(Run(system, "link_id"), std::string("sc4://advice/00000015000009a2"));
    CHECK_EQ(Run(system, " LINK_ID "), std::string("sc4://advice/00000015000009a2"));
    CHECK_EQ(Run(system, "sit_link_id"), std::string("sc4://advice/00000015000009a2"));

    // advisor: the personal name of the context advice's advisor; unknown without that advisor.
    system.advisor.id = 0x15;
    CHECK_EQ(Run(system, "advisor", &ok), std::string("Ben Wattson"));
    CHECK(ok);
    system.advisor.id = 0x16;
    CHECK_EQ(Run(system, "advisor", &ok), std::string("%UNKNOWN TOKEN --advisor--%"));
    CHECK(!ok);

    // building_initial_cost / building_monthly_cost: the subject's data1 / data2 as money.
    FakeSubject subject;
    subject.data1 = 5000;
    subject.data2 = 120;
    advice.subject = &subject;
    CHECK_EQ(Run(system, "building_initial_cost"), std::string("$5000"));
    CHECK_EQ(Run(system, "building_monthly_cost"), std::string("$120"));

    // Situation tokens without a situation manager: unknown, and Lua is not asked.
    system.lua.code.clear();
    CHECK_EQ(Run(system, "num_targets", &ok), std::string("%UNKNOWN TOKEN --num_targets--%"));
    CHECK(!ok);
    CHECK(system.lua.code.empty());

    // The Lua path: "_dasd_=(TOKEN)", the prefix stripped; s@ = "%i", m@ = money, p@ = number + "%".
    system.lua.result = {FakeLua::Number, 42.9};
    CHECK_EQ(Run(system, "s@g_residents", &ok), std::string("42"));
    CHECK(ok);
    CHECK(!system.lua.code.empty() && system.lua.code.back() == "_dasd_=(g_residents)");
    CHECK_EQ(Run(system, "M@budget.total"), std::string("$42"));
    CHECK(system.lua.code.back() == "_dasd_=(budget.total)");
    CHECK_EQ(Run(system, "p@ratio"), std::string("%"));   // no language manager in the harness: only the sign
    CHECK_EQ(Run(system, "x@other"), std::string(""));     // any other letter: the number string, prefix stripped
    CHECK(system.lua.code.back() == "_dasd_=(other)");
    CHECK_EQ(Run(system, "plain"), std::string(""));
    CHECK(system.lua.code.back() == "_dasd_=(plain)");
    // The stack is put back after a successful run.
    CHECK(system.lua.stack.empty());

    // A string answer is copied (s@ formats numbers only).
    system.lua.result = {FakeLua::Text, 0, "Kensington"};
    CHECK_EQ(Run(system, "s@city_name", &ok), std::string("Kensington"));
    CHECK(ok);
    // A user value with the system's data tag 1 is a cIGZString; another tag answers unknown.
    cRZString held("held text");
    system.lua.result = {FakeLua::User, 0, "", &held, 77};
    CHECK_EQ(Run(system, "subject_name", &ok), std::string("held text"));
    CHECK(ok && !system.tagsAsked.empty() && system.tagsAsked.back() == 1);
    system.lua.result = {FakeLua::User, 0, "", &held, 5};
    CHECK_EQ(Run(system, "subject_name", &ok), std::string("%UNKNOWN TOKEN --subject_name--%"));
    CHECK(!ok);
    // nil, and a Lua error: unknown, with the stripped name.
    system.lua.result = {};
    CHECK_EQ(Run(system, "m@missing", &ok), std::string("%UNKNOWN TOKEN --missing--%"));
    CHECK(!ok);
    system.lua.status = 1;
    system.lua.result = {FakeLua::Number, 1};
    CHECK_EQ(Run(system, "broken(", &ok), std::string("%UNKNOWN TOKEN --broken(--%"));
    CHECK(!ok);

    SetAdviceDetokenizerHost(nullptr);
}

}  // namespace

int main() {
    TestTokens();
    if (failures) {
        std::printf("sc4_advicedetokenizer: %d FAILED\n", failures);
        return 1;
    }
    std::printf("sc4_advicedetokenizer: all passed\n");
    return 0;
}
