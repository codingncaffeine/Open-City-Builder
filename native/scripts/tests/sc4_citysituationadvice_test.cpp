// cSC4CitySituationAdvice: Init / UpdateSituationFields against a tiny table-model Lua, the table getters with their
// fallbacks, the text builders and the group queries.
#include <cstdio>
#include <map>
#include <memory>

#include "sc4/CitySituationAdvice.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace sc4;

namespace {

struct Table;
struct Val {
    enum Kind { Nil, Num, Bool, Str, Tab } kind = Nil;
    double num = 0;
    bool b = false;
    std::string str;
    std::shared_ptr<Table> tab;
};
struct Table {
    std::map<std::string, Val> fields;
    std::map<double, Val> nums;
};
Val Number(double v) { Val x; x.kind = Val::Num; x.num = v; return x; }
Val Boolean(bool v) { Val x; x.kind = Val::Bool; x.b = v; return x; }
Val Text(const char* v) { Val x; x.kind = Val::Str; x.str = v; return x; }
Val NewTab() { Val x; x.kind = Val::Tab; x.tab = std::make_shared<Table>(); return x; }
Val ArrayOf(std::initializer_list<double> values) {
    Val t = NewTab();
    double i = 1;
    for (double v : values) t.tab->nums[i++] = Number(v);
    return t;
}

class FakeLua final : public ISituationLua {
public:
    std::vector<Val> stack;
    std::map<std::string, Val> globals;
    std::map<int, Val> registry;
    int nextRef = 10;
    std::vector<int> unrefs;

    int Abs(int index) const { return index > 0 ? index : static_cast<int>(stack.size()) + index + 1; }
    Val& At(int index) { return stack[Abs(index) - 1]; }
    int GetTop() override { return static_cast<int>(stack.size()); }
    void SetTop(int top) override { stack.resize(top); }
    void PushValue(int index) override { stack.push_back(At(index)); }
    void Pop(int count) override { stack.resize(stack.size() - count); }
    bool IsNumber(int index) override { return At(index).kind == Val::Num; }
    double ToNumber(int index) override { return At(index).num; }
    void PushNumber(double value) override { stack.push_back(Number(value)); }
    void GetGlobal(const char* name) override { stack.push_back(globals[name]); }
    void RawGetTable(int index) override {
        Val key = stack.back();
        Val table = At(index);   // the index counts the key still on the stack
        stack.pop_back();
        Val out;
        if (table.kind == Val::Tab && table.tab->nums.count(key.num)) out = table.tab->nums[key.num];
        stack.push_back(out);
    }
    void RawSetTable(int) override {}
    void GetRef(int ref) override { stack.push_back(registry[ref]); }
    void NewTable() override { stack.push_back(NewTab()); }
    int Ref(bool) override {
        registry[nextRef] = stack.back();
        stack.pop_back();
        return nextRef++;
    }
    void CallProtected(int, int) override {}
    void Unref(int ref) override { unrefs.push_back(ref); registry.erase(ref); }
    bool IsFunction(int) override { return false; }
    bool IsTable(int index) override { return At(index).kind == Val::Tab; }
    bool GetGlobalTable(const char* name) override { stack.push_back(globals[name]); return stack.back().kind != Val::Nil; }
    bool GetField(const char* name, int tableIndex) override {
        Val t = At(tableIndex);
        auto it = t.tab->fields.find(name);
        if (it == t.tab->fields.end()) return false;
        stack.push_back(it->second);
        return true;
    }
    const Val* Field(const char* name, int tableIndex) {
        Val& t = At(tableIndex);
        auto it = t.tab->fields.find(name);
        return it == t.tab->fields.end() ? nullptr : &it->second;
    }
    void ReadFieldU32(const char* name, std::uint32_t& value, int t) override {
        if (const Val* v = Field(name, t)) if (v->kind == Val::Num) value = static_cast<std::uint32_t>(v->num);
    }
    void ReadFieldInt(const char* name, std::int32_t& value, int t) override {
        if (const Val* v = Field(name, t)) if (v->kind == Val::Num) value = static_cast<std::int32_t>(v->num);
    }
    void ReadFieldFloat(const char* name, float& value, int t) override {
        if (const Val* v = Field(name, t)) if (v->kind == Val::Num) value = static_cast<float>(v->num);
    }
    void ReadFieldBool(const char* name, bool& value, int t) override {
        if (const Val* v = Field(name, t)) if (v->kind == Val::Bool) value = v->b;
    }
    void ReadFieldString(const char* name, cRZString& value, int t) override {
        if (const Val* v = Field(name, t)) if (v->kind == Val::Str) value.AssignFromCString(v->str.c_str());
    }
    void ReadTableAsArrayU32(std::vector<std::uint32_t>& out, int index) override {
        out.clear();
        for (auto& kv : At(index).tab->nums) out.push_back(static_cast<std::uint32_t>(kv.second.num));
    }
    void ReadTableAsArrayString(std::vector<cRZString>& out, int index) override {
        out.clear();
        for (auto& kv : At(index).tab->nums) out.emplace_back(kv.second.str.c_str());
    }
    void ForEachEntry(void (*visit)(void*), void* context) override {
        Val t = stack.back();
        for (auto& kv : t.tab->nums) {
            stack.push_back(kv.second);
            visit(context);
            stack.pop_back();
        }
    }
};

struct FakeBase final : ISituationAdviceBase {
    int id = 0, mood = 77, initCalls = 0, shutdownCalls = 0, flagSetCalls = 0;
    bool initResult = true;
    bool Init(int adviceId) override { id = adviceId; ++initCalls; return initResult; }
    void Shutdown() override { ++shutdownCalls; }
    int AdviceId() override { return id; }
    int AdvisorId() override { return 5; }
    void SetFlags(int flags, bool on) override { CHECK(flags == 0x2000 && on); ++flagSetCalls; }
    int Mood() override { return mood; }
    std::uint32_t Trigger() override { return 0xabc; }
    bool QueryBaseInterface(std::uint32_t, void**) override { return false; }
};

struct FakeHost final : ICitySituationAdviceHost {
    FakeLua* lua = nullptr;
    bool twinAvailable = false;
    int released = 0, registered = 0;
    ISituationLua* Lua() override { return lua; }
    int CityCellsX() override { return 64; }
    float CityCellSizeXMeters() override { return 16.0f; }
    bool IsLinkedAdviceAvailable(int advisorId, std::uint32_t guid) override { return twinAvailable && advisorId == 5 && guid != 0; }
    void AdvisorSystemReleaseHook() override { ++released; }
    void AdvisorSystemRegister(void*) override { ++registered; }
};

struct FakeOccupant final : ISituationOccupant {
    std::uint32_t group = 0;
    ISituationPropertyHolder* PropertyHolder() override { return nullptr; }
    std::uint32_t OccupantType() override { return 0; }
    void Position(float*) override {}
    void SetPosition(const float*) override {}
    bool IsInGroup(std::uint32_t g) override { return g == group; }
    ISituationAutomaton* AsAutomaton() override { return nullptr; }
    bool IsNetworkOccupant() override { return false; }
    bool NetworkFlagsMatch(std::uint32_t) override { return false; }
    void NetworkCell(int&, int&) override {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
};
struct FakeAutomaton final : ISituationAutomaton {
    FakeOccupant* occupant = nullptr;
    ISituationOccupant* Occupant() override { return occupant; }
    void Position(float*) override {}
    void Velocity(float*) override {}
    void SetPlayerControl(bool) override {}
    void SetFlag(std::uint32_t, bool) override {}
    bool CheckFlag(std::uint32_t) override { return false; }
    void AddListener(void*) override {}
    void RemoveListener(void*) override {}
    ISituationAutomaton* PursuitTarget() override { return nullptr; }
    int AutomatonType() override { return 0; }
    void SendCommand(std::uint32_t) override {}
    int FirstIndicator() override { return 0; }
    void OnSituationAttached() override {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
};

}  // namespace

int main() {
    FakeLua lua;
    FakeHost host;
    host.lua = &lua;
    SetCitySituationAdviceHost(&host);

    // the script row advices[7]
    Val row = NewTab();
    auto& f = row.tab->fields;
    f["automata_list"] = ArrayOf({100, 101});
    f["target_list"] = Number(55);                 // single number
    f["target_sequence"] = ArrayOf({0, 9, 0, 11});
    f["create_target"] = Boolean(true);
    f["always_create_target"] = Boolean(true);
    f["training_mission"] = Boolean(true);
    f["condition"] = Number(3);
    f["time_limit"] = Number(90);
    f["success_distance"] = Number(8);
    f["min_target_distance"] = Number(5);
    f["max_target_distance"] = Number(0);
    f["success_timeout"] = Number(2);
    f["delay_after_disabled"] = Number(3);
    f["active_radius"] = Number(40);
    f["service_mission"] = Boolean(true);
    f["mysim_mission"] = Boolean(true);
    f["evade_list"] = ArrayOf({200, 201});
    f["evade_distance"] = Number(30);
    f["evade_timeout"] = Number(12);
    f["coverage_type"] = Number(4);
    f["coverage_rect"] = ArrayOf({1, 2, 3});       // short: padded with zero
    f["coverage_cells_min"] = Number(4);
    f["coverage_cells_max"] = Number(9);
    f["coverage_success_count"] = Number(3);
    f["coverage_success_pct"] = Number(50);
    f["evil_twin"] = Number(0x1234);
    f["reward_guid"] = Number(0x5678);
    f["success_aura_radius"] = Number(6);
    f["success_aura_magnitude"] = Number(1.5);
    f["success_effect"] = Text("sparkle");
    f["success_money"] = Number(250);
    f["failure_money"] = Number(-100);
    f["title"] = Text("The Title");
    f["message"] = Text("Hello");
    f["success_text"] = Text("won");
    f["failure_text"] = Text("lost");
    f["image"] = Number(0x111);
    f["failure_mood"] = Number(9);
    f["progress_text"] = Val();
    f["progress_text"] = NewTab();
    f["progress_text"].tab->nums[1] = Text("first");
    f["progress_text"].tab->nums[2] = Text("second");
    Val advices = NewTab();
    advices.tab->nums[7] = row;
    lua.globals["advices"] = advices;

    FakeBase base;
    auto* s = CreateCitySituationAdvice(&base);
    s->AddRef();
    CHECK(CitySituationAdviceClassId() == 0x8be3753b && s->GetGZCLSID() == 0x8be3753b);
    // constructor defaults
    CHECK(s->GetSuccessCondition() == 1 && s->GetSuccessDistance() == 16.0f && s->GetScriptTableRef() == -2);
    CHECK(s->GetSituationFlags() == 0 && !s->HasEvadeList() && !s->HasTargetSequence());
    CHECK(s->GetTrigger() == 0xabc && s->AsAdvice() == &base);
    CHECK(s->GetMaxTargetDistance() == 64.0f * 16.0f);   // no max configured: whole city
    CHECK(s->GetSituationImageId() == 0 && s->GetTextTitle().Strlen() == 0);

    CHECK(s->Init(7));
    CHECK(base.initCalls == 1 && base.flagSetCalls == 1 && host.registered == 1 && host.released == 0);
    CHECK(s->GetScriptTableRef() == 10);
    CHECK(s->GetAutomataGroups() == (std::vector<std::uint32_t>{100, 101}));
    CHECK(s->GetTargetGroups() == (std::vector<std::uint32_t>{55}));
    CHECK(s->GetTargetSequence() == (std::vector<std::uint32_t>{9, 11}));   // zero entries dropped
    CHECK(lua.GetTop() == 0);
    CHECK(s->CheckSituationFlag(kSituationFlagCreateTarget) && s->CheckSituationFlag(kSituationFlagAlwaysCreateTarget));
    CHECK(s->CheckSituationFlag(kSituationFlagTraining) && s->HasTargetSequence());
    CHECK(s->CheckSituationFlag(kSituationFlagServiceMission) && s->CheckSituationFlag(kSituationFlagMySimMission));
    CHECK(s->CheckSituationFlag(kSituationFlagEvade) && !s->CheckSituationFlag(kSituationFlagPursuit));
    CHECK(!s->CheckSituationFlag(kSituationFlagUseLotBoundary));
    CHECK(s->GetSuccessCondition() == 3 && s->GetTimeLimit() == 90.0f && s->GetSuccessDistance() == 8.0f);
    CHECK(s->GetMinTargetDistance() == 5.0f && s->GetSuccessTimeout() == 2.0f && s->GetDelayAfterDisabled() == 3.0f);
    CHECK(s->GetActiveRadius() == 40.0f && s->GetMaxTargetDistance() == 1024.0f);
    CHECK(s->HasEvadeList() && s->GetEvadeList().size() == 2 && s->GetEvadeDistance() == 30.0f && s->GetEvadeTimeout() == 12.0f);
    CHECK(s->GetCoverageType() == kCoverageRect);
    const std::uint16_t* rect = s->GetCoverageRect();
    CHECK(rect[0] == 1 && rect[1] == 2 && rect[2] == 3 && rect[3] == 0);
    CHECK(s->GetCoverageCellsMin() == 4 && s->GetCoverageCellsMax() == 9 && s->GetCoverageSuccessCount() == 3);
    CHECK(s->GetCoverageSuccessPct() == 50.0f * 0.01f);
    CHECK(s->GetEvilTwinGuid() == 0x1234 && s->GetRewardGuid() == 0x5678);

    float radius = 0, magnitude = 0;
    CHECK(s->GetSuccessAura(radius, magnitude) && radius == 6.0f && magnitude == 1.5f);
    CHECK(!s->GetFailureAura(radius, magnitude) && radius == 0.0f);
    cRZString effect;
    CHECK(s->GetSuccessEffectName(effect) && std::string(effect.ToChar()) == "sparkle");
    CHECK(!s->GetFailureEffectName(effect) && effect.Strlen() == 0);
    long long money = 0;
    CHECK(s->GetSuccessMoney(money) && money == 250);
    CHECK(s->GetFailureMoney(money) && money == -100);

    // table getters
    CHECK(std::string(s->GetTextTitle().ToChar()) == "The Title");
    CHECK(std::string(s->GetSuccessText().ToChar()) == "won" && std::string(s->GetFailureText().ToChar()) == "lost");
    CHECK(s->GetRewardProgressText().Strlen() == 0);
    CHECK(s->GetSituationImageId() == 0x111);
    CHECK(s->GetSuccessImageId() == 0x111 && s->GetFailureImageId() == 0x111 && s->GetRewardUnlockedImageId() == 0x111);
    CHECK(s->GetSuccessMood() == 77);   // falls back to the base advice mood
    CHECK(s->GetFailureMood() == 9);
    CHECK(std::string(s->GetProgressText(1).ToChar()) == "first");
    CHECK(std::string(s->GetProgressText(2).ToChar()) == "second");
    CHECK(std::string(s->GetProgressText(9).ToChar()) == "first");   // out of range -> first entry
    CHECK(s->GetProgressText(0).Strlen() == 0);
    CHECK(lua.GetTop() == 0);

    // message text and the evil twin suffix
    CHECK(std::string(s->GetTextMessage().ToChar()) == "Hello");
    host.twinAvailable = true;
    CHECK(std::string(s->GetTextMessage().ToChar()) == "Hello&nbsp;<BR>&nbsp;");

    // success condition text: conditions 1, 2, 3 and 5 only (resource lookup fails without a manager, text is empty)
    cRZString text;
    CHECK(s->GetSuccessConditionText(text));          // condition 3
    f["condition"] = Number(4);
    lua.registry[10].tab->fields["condition"] = Number(4);
    s->UpdateSituationFields();
    CHECK(s->GetSuccessCondition() == 4 && !s->GetSuccessConditionText(text));

    // group relevance
    CHECK(s->IsGroupRelevant({1, 101}) && !s->IsGroupRelevant({1, 2}) && !s->IsGroupRelevant({}));
    FakeOccupant occ;
    FakeAutomaton automaton;
    automaton.occupant = &occ;
    occ.group = 100;
    CHECK(s->IsAutomatonRelevant(&automaton));
    occ.group = 3;
    CHECK(!s->IsAutomatonRelevant(&automaton) && !s->IsAutomatonRelevant(nullptr));

    // coverage cells (type 7) and zone (type 1)
    Val live = lua.registry[10];
    live.tab->fields["coverage_type"] = Number(7);
    Val cells = NewTab();
    cells.tab->nums[1] = ArrayOf({3, 4});
    cells.tab->nums[2] = Number(5);                  // not a table: skipped
    cells.tab->nums[3] = ArrayOf({7});               // too short: skipped
    cells.tab->nums[4] = ArrayOf({8, 9, 10});
    live.tab->fields["coverage_cells"] = cells;
    lua.registry[10] = live;
    s->UpdateSituationFields();
    CHECK(s->GetCoverageType() == kCoverageCells && s->GetCellCoverageList().size() == 2);
    CHECK(s->GetCellCoverageList().front().x == 3 && s->GetCellCoverageList().back().z == 9);
    CHECK(lua.GetTop() == 0);
    live.tab->fields["coverage_type"] = Number(1);
    live.tab->fields["coverage_zone"] = Number(6);
    s->UpdateSituationFields();
    CHECK(s->GetCoverageZone() == 6);

    // Shutdown releases the script reference and answers true
    CHECK(s->Shutdown() && base.shutdownCalls == 1 && lua.unrefs.size() == 1 && lua.unrefs[0] == 10);
    CHECK(s->GetScriptTableRef() == -2 && s->Shutdown() && lua.unrefs.size() == 1);

    // QueryInterface
    void* out = nullptr;
    CHECK(s->QueryInterface(kGZIID_cISC4CitySituationAdvice, &out) && out == static_cast<cISC4CitySituationAdvice*>(s));
    CHECK(!s->QueryInterface(0x1234, &out));
    s->Release();
    s->Release();   // last reference: deletes

    // a missing script row: the advisor system hook is called and Init answers false
    FakeBase base2;
    auto* t = CreateCitySituationAdvice(&base2);
    t->AddRef();
    CHECK(!t->Init(99) && host.released == 1 && t->GetScriptTableRef() == -2 && lua.GetTop() == 0);
    // no script host at all
    host.lua = nullptr;
    CHECK(!t->Init(7) && host.released == 1);
    t->Release();

    // A situation that is a view of an outer advice object shares its reference count (the original is one object).
    struct CountingOuter final : cIGZUnknown {
        int refs = 1;
        bool QueryInterface(std::uint32_t, void**) override { return false; }
        std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
        std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    } outer;
    struct OuterBase final : ISituationAdviceBase {
        cIGZUnknown* owner = nullptr;
        bool Init(int) override { return true; }
        void Shutdown() override {}
        int AdviceId() override { return 0; }
        int AdvisorId() override { return 0; }
        void SetFlags(int, bool) override {}
        int Mood() override { return 0; }
        std::uint32_t Trigger() override { return 0; }
        bool QueryBaseInterface(std::uint32_t, void**) override { return false; }
        cIGZUnknown* Outer() override { return owner; }
    } outerBase;
    outerBase.owner = &outer;
    {
        cSC4CitySituationAdvice viewOfOuter(&outerBase);
        CHECK(viewOfOuter.AddRef() == 2 && outer.refs == 2);
        void* view = nullptr;
        CHECK(viewOfOuter.QueryInterface(kGZIID_cISC4CitySituationAdvice, &view) && outer.refs == 3);
        CHECK(viewOfOuter.Release() == 2 && viewOfOuter.Release() == 1 && outer.refs == 1);   // never deletes the member view
    }

    SetCitySituationAdviceHost(nullptr);
    std::printf(failures ? "citysituationadvice: %d FAILURES\n" : "citysituationadvice: ok\n", failures);
    return failures ? 1 : 0;
}
