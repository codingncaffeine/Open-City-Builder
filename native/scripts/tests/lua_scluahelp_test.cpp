// Tests SCLuaHelp (table readers) and SCLuaTableRef against a small fake Lua stack that follows the cISCLua contract
// (pop-key/push-pair Next, relative indices, nil-for-missing GetTable).
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "lua/SCLuaHelp.h"
#include "lua/SCLuaTableRef.h"

#pragma clang diagnostic ignored "-Wunused-parameter"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

struct Table;
struct Value {
    enum Kind { Nil, Number, String, Bool, TableK } kind = Nil;
    double num = 0; std::string str; bool b = false; std::shared_ptr<Table> table;
    static Value N(double d) { Value v; v.kind = Number; v.num = d; return v; }
    static Value S(const char* s) { Value v; v.kind = String; v.str = s; return v; }
    static Value B(bool x) { Value v; v.kind = Bool; v.b = x; return v; }
    static Value T(std::shared_ptr<Table> t) { Value v; v.kind = TableK; v.table = std::move(t); return v; }
};
struct Table {
    std::vector<std::pair<Value, Value>> entries;   // insertion order = iteration order
    void Set(Value k, Value v) { entries.emplace_back(std::move(k), std::move(v)); }
    static bool SameKey(const Value& a, const Value& b) {
        if (a.kind != b.kind) return false;
        return a.kind == Value::Number ? a.num == b.num : a.kind == Value::String ? a.str == b.str : false;
    }
    Value Get(const Value& k) const { for (auto& e : entries) if (SameKey(e.first, k)) return e.second; return {}; }
};
static std::shared_ptr<Table> Arr(std::initializer_list<Value> values) {
    auto t = std::make_shared<Table>(); double i = 1;
    for (auto& v : values) t->Set(Value::N(i++), v);
    return t;
}

struct FakeLua : cISCLua {
    std::vector<Value> stack;
    std::map<std::string, Value> globals;
    int Abs(int i) const { return i < 0 ? static_cast<int>(stack.size()) + i : i - 1; }
    Value& At(int i) { return stack[static_cast<std::size_t>(Abs(i))]; }

    int GetTop() override { return static_cast<int>(stack.size()); }
    void Pop(int n) override { stack.resize(stack.size() - static_cast<std::size_t>(n)); }
    void PushNil() override { stack.push_back({}); }
    void PushString(const char* s) override { stack.push_back(Value::S(s)); }
    void GetGlobal(const char* name) override { auto it = globals.find(name); stack.push_back(it == globals.end() ? Value{} : it->second); }
    void GetTable(int index) override {
        Value key = stack.back(); stack.pop_back();
        Value t = At(index);
        stack.push_back(t.kind == Value::TableK ? t.table->Get(key) : Value{});
    }
    void RawGetI(int index, int n) override { Value t = At(index); stack.push_back(t.table->Get(Value::N(n))); }
    int GetN(int index) override {
        Value& t = At(index); if (t.kind != Value::TableK) return 0;
        int n = 0; while (t.table->Get(Value::N(n + 1)).kind != Value::Nil) ++n;
        return n;
    }
    int Next(int index) override {
        Value key = stack.back(); stack.pop_back();
        auto& es = At(index).table->entries;
        std::size_t i = 0;
        if (key.kind != Value::Nil) { while (i < es.size() && !Table::SameKey(es[i].first, key)) ++i; ++i; }
        if (i >= es.size()) return 0;
        stack.push_back(es[i].first); stack.push_back(es[i].second);
        return 1;
    }
    bool IsTable(int i) override { return At(i).kind == Value::TableK; }
    bool IsNil(int i) override { return At(i).kind == Value::Nil; }
    bool IsBoolean(int i) override { return At(i).kind == Value::Bool; }
    double ToNumber(int i) override { Value& v = At(i); return v.kind == Value::Number ? v.num : 0; }
    std::uint32_t ToUint32(int i) override { return static_cast<std::uint32_t>(ToNumber(i)); }
    std::int32_t ToSint32(int i) override { return static_cast<std::int32_t>(ToNumber(i)); }
    bool ToBoolean(int i) override { return At(i).b; }
    const char* ToString(int i) override {
        Value& v = At(i);
        if (v.kind == Value::Number) { char buf[32]; std::snprintf(buf, sizeof buf, "%g", v.num); v.str = buf; return v.str.c_str(); }
        return v.kind == Value::String ? v.str.c_str() : nullptr;
    }
    bool QueryInterface(std::uint32_t iid, void** out) override { return {}; }
    std::uint32_t AddRef() override { return {}; }
    std::uint32_t Release() override { return {}; }
    bool Open(int libraries) override { return {}; }
    bool Close() override { return {}; }
    void* AsIGZLua5() override { return {}; }
    void SetTop(int index) override { }
    void PushValue(int index) override { }
    void Remove(int index) override { }
    void Insert(int index) override { }
    int Type(int index) override { return {}; }
    const char* TypeName(int type) override { return {}; }
    bool IsNumber(int index) override { return {}; }
    bool IsString(int index) override { return {}; }
    bool IsCFunction(int index) override { return {}; }
    int Tag(int index) override { return {}; }
    bool Equal(int a, int b) override { return {}; }
    bool LessThan(int a, int b) override { return {}; }
    int Strlen(int index) override { return {}; }
    CFunction ToCFunction(int index) override { return {}; }
    void* ToUserData(int index) override { return {}; }
    void* ToUserTag(int index) override { return {}; }
    void PushNumber(double value) override { }
    void PushLString(const char* s, int length) override { }
    void PushCClosure(CFunction fn, int upvalues) override { }
    void PushUserTag(void* p, int tag) override { }
    void PushBoolean(bool value) override { }
    void RawGet(int index) override { }
    void GetGlobals() override { }
    void GetTagMethod(int tag, const char* event) override { }
    bool GetRef(int ref) override { return {}; }
    void NewTable() override { }
    void SetGlobal(const char* name) override { }
    void SetTable(int index) override { }
    void RawSet(int index) override { }
    void RawSetI(int index, int n) override { }
    void SetTagMethod(int tag, const char* event) override { }
    int Ref(int lockIt) override { return {}; }
    int CallProtected(int args, int results) override { return {}; }
    void RawCall(int args, int results) override { }
    int DoFile(const char* path) override { return {}; }
    int DoString(const char* code) override { return {}; }
    int DoBuffer(const char* buffer, int size, const char* name) override { return {}; }
    int GetGCCount() override { return {}; }
    int GetGCThreshold() override { return {}; }
    void SetGCThreshold(int value) override { }
    int NewTag() override { return {}; }
    void CopyTagMethods(int from, int to) override { }
    void SetTag(int tag) override { }
    void Unref(int ref) override { }
    void Error(const char* message) override { }
    void Concat(int count) override { }
    void* NewUserData(int size) override { return {}; }
    void Register(CFunction fn, const char* name) override { }
    void PushUserData(void* p, int tag) override { }
    int CloneTag(int tag) override { return {}; }
    bool IsFunction(int index) override { return {}; }
    bool IsUserData(int index) override { return {}; }
    std::uintptr_t ToDataRegistryEntryID(cISC4GameDataRegistry* registry, int index) override { return 0; }
    cIGZUnknown* ToDataRegistryNode(cISC4GameDataRegistry* registry, int index) override { return nullptr; }
    void* ToDataRegistryValue(cISC4GameDataRegistry* registry, int index) override { return nullptr; }
};

int main() {
    FakeLua L;
    auto cfg = std::make_shared<Table>();
    cfg->Set(Value::S("name"), Value::S("Hospital"));
    cfg->Set(Value::S("cost"), Value::N(1234.6));
    cfg->Set(Value::S("neg"), Value::N(-2.5));
    cfg->Set(Value::S("flag"), Value::B(true));
    cfg->Set(Value::S("numAsFlag"), Value::N(1));
    cfg->Set(Value::S("single"), Value::N(7));
    cfg->Set(Value::S("pair"), Value::T(Arr({Value::N(3), Value::N(9), Value::N(11)})));
    cfg->Set(Value::S("one"), Value::T(Arr({Value::N(5)})));
    cfg->Set(Value::S("names"), Value::T(Arr({Value::S("a"), Value::S("bb")})));
    cfg->Set(Value::S("holey"), Value::T(Arr({Value::N(1), Value::N(2)})));
    L.globals["Cfg"] = Value::T(cfg);
    L.globals["Number"] = Value::N(3);
    L.stack.push_back(Value::T(cfg));   // table at index 1

    // GetGlobal / GetField leave the value on the stack only when non-nil.
    CHECK(SCLuaHelp::GetGlobal(&L, "Cfg") && L.GetTop() == 2); L.Pop(1);
    CHECK(!SCLuaHelp::GetGlobal(&L, "Missing") && L.GetTop() == 1);
    CHECK(SCLuaHelp::GetField(&L, "cost", 1) && L.GetTop() == 2); L.Pop(1);
    CHECK(!SCLuaHelp::GetField(&L, "nope", 1) && L.GetTop() == 1);

    // ReadTableField overloads.
    cRZString s("keep");
    CHECK(SCLuaHelp::ReadTableField(&L, "name", s, 1) && std::string(s.ToChar()) == "Hospital");
    CHECK(!SCLuaHelp::ReadTableField(&L, "nope", s, 1) && s.Strlen() == 0);
    float f = 9; CHECK(SCLuaHelp::ReadTableField(&L, "cost", f, 1) && f == 1234.6f);
    CHECK(!SCLuaHelp::ReadTableField(&L, "nope", f, 1) && f == 0.0f);
    unsigned u = 9; CHECK(SCLuaHelp::ReadTableField(&L, "cost", u, 1) && u == 1234u);
    int i = 9; CHECK(SCLuaHelp::ReadTableField(&L, "neg", i, 1) && i == -2);
    CHECK(!SCLuaHelp::ReadTableField(&L, "nope", i, 1) && i == 0);
    bool b = false; CHECK(SCLuaHelp::ReadTableField(&L, "flag", b, 1) && b);
    b = true; CHECK(!SCLuaHelp::ReadTableField(&L, "numAsFlag", b, 1) && !b);   // numbers are not booleans
    CHECK(L.GetTop() == 1);

    // Arrays.
    std::vector<int> vi{9, 9, 9, 9};
    L.stack.clear(); L.stack.push_back(cfg->Get(Value::S("pair")));
    CHECK(SCLuaHelp::ReadTableAsArray(&L, vi, 1) && vi == (std::vector<int>{3, 9, 11}));
    vi.clear(); L.stack[0] = cfg->Get(Value::S("single"));
    CHECK(SCLuaHelp::ReadTableAsArray(&L, vi, 1) && vi == (std::vector<int>{7}));   // scalar -> one element
    std::vector<float> vf; L.stack[0] = cfg->Get(Value::S("pair"));
    CHECK(SCLuaHelp::ReadTableAsArray(&L, vf, 1) && vf == (std::vector<float>{3, 9, 11}));
    std::vector<unsigned> vu; CHECK(SCLuaHelp::ReadTableAsArray(&L, vu, -1) && vu == (std::vector<unsigned>{3, 9, 11}));
    std::vector<cRZString> vs; L.stack[0] = cfg->Get(Value::S("names"));
    CHECK(SCLuaHelp::ReadTableAsArray(&L, vs, 1) && vs.size() == 2 && std::string(vs[1].ToChar()) == "bb");
    L.stack[0] = Value::T(std::make_shared<Table>());
    vi = {1};
    CHECK(!SCLuaHelp::ReadTableAsArray(&L, vi, 1) && vi == std::vector<int>{1});   // empty table: false, output untouched
    CHECK(L.GetTop() == 1);

    // Lists and maps walk every entry (negative index made absolute).
    L.stack[0] = cfg->Get(Value::S("names"));
    std::list<cRZString> ls; CHECK(SCLuaHelp::ReadTableAsList(&L, ls, -1) && ls.size() == 2 && std::string(ls.front().ToChar()) == "a");
    L.stack[0] = cfg->Get(Value::S("pair"));
    std::list<unsigned> lu; CHECK(SCLuaHelp::ReadTableAsList(&L, lu, 1) && lu == (std::list<unsigned>{3, 9, 11}));
    auto mapTable = std::make_shared<Table>();
    mapTable->Set(Value::S("x"), Value::N(2.4)); mapTable->Set(Value::S("y"), Value::N(2.5));
    L.stack[0] = Value::T(mapTable);
    std::unordered_map<std::string, int> m;
    CHECK(SCLuaHelp::ReadTableAsMap(&L, m, 1) && m.size() == 2 && m["x"] == 2 && m["y"] == 2);   // 2.5 rounds to even
    L.stack[0] = Value::T(std::make_shared<Table>());
    CHECK(!SCLuaHelp::ReadTableAsMap(&L, m, 1) && m.empty());
    CHECK(L.GetTop() == 1);

    // Ranges.
    L.stack[0] = Value::T(cfg);
    float rf[2] = {0, 0}; CHECK(SCLuaHelp::ReadTableRange(&L, "pair", rf, 1) && rf[0] == 3 && rf[1] == 9);
    CHECK(SCLuaHelp::ReadTableRange(&L, "one", rf, 1) && rf[0] == 5 && rf[1] == 5);
    CHECK(SCLuaHelp::ReadTableRange(&L, "single", rf, 1) && rf[0] == 7 && rf[1] == 7);
    int ri[2] = {0, 0}; CHECK(SCLuaHelp::ReadTableRange(&L, "pair", ri, 1) && ri[0] == 3 && ri[1] == 9);
    CHECK(SCLuaHelp::ReadTableRange(&L, "cost", ri, 1) && ri[0] == 1235 && ri[1] == 1235);
    CHECK(!SCLuaHelp::ReadTableRange(&L, "nope", ri, 1));
    CHECK(L.GetTop() == 1);

    // SCLuaTableRef: by index.
    L.stack.clear(); L.stack.push_back(Value::N(0)); L.stack.push_back(cfg->Get(Value::S("pair")));
    {
        SCLuaTableRef ref(&L, -1);
        CHECK(ref.TableIndex() == 2 && !ref.Finished());
        double sum = 0; int count = 0;
        for (bool ok = ref.begin(); ok; ok = ref.next()) { sum += L.ToNumber(-1); ++count; }
        CHECK(count == 3 && sum == 23 && ref.Finished());
        CHECK(L.GetTop() == 2);              // the last Next popped the key and pushed nothing
        CHECK(!ref.next());                  // finished cursors stay finished
        CHECK(ref.begin() && count == 3);    // begin restarts
        L.Pop(2);
    }
    // by global name: table, non-table, missing, empty name.
    L.stack.clear();
    L.globals["Pair"] = cfg->Get(Value::S("pair"));
    {
        SCLuaTableRef ref(&L, "Pair");
        CHECK(ref.TableIndex() == 1 && !ref.Finished() && L.GetTop() == 1 && std::string(ref.Name().ToChar()) == "Pair");
        int count = 0; for (bool ok = ref.begin(); ok; ok = ref.next()) ++count;
        CHECK(count == 3);
    }
    L.stack.clear();
    { SCLuaTableRef ref(&L, "Number"); CHECK(ref.Finished() && ref.TableIndex() == -1 && L.GetTop() == 0 && !ref.begin()); }
    { SCLuaTableRef ref(&L, "Missing"); CHECK(ref.Finished() && L.GetTop() == 0 && !ref.begin()); }
    { SCLuaTableRef ref(&L, ""); CHECK(ref.Finished() && L.GetTop() == 0); }

    std::printf(g_fail ? "FAILED %d\n" : "lua_scluahelp OK\n", g_fail);
    return g_fail ? 1 : 0;
}
