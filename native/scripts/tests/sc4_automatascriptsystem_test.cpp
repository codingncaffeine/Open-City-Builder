// cSC4AutomataScriptSystem (src/sc4/AutomataScriptSystem.{h,cpp}): the lifecycle, the game-data registry commands and the script
// importers. The Lua state is a fake cISCLua (a real stack with tables, globals and registry references), the registry / parameter set /
// controller manager / automata manager are fakes behind cISC4GameDataRegistry, cIGZCommandParameterSet and IAutomataScriptHost.
//
// The fixture is a small "_config.lua": two controller templates (an attractor and a generator), an occupant group and automata groups
// (one with a parent, one without a group id, one per vehicle kind), drive parameters, key effects, the player-drive key map and the
// track building groups. The checks cover the recovered fields and the quirks the header documents.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "lua/ISCLua.h"
#include "rz/PortMemoryStreams.h"
#include "rz/cRZVariant.h"
#include "sc4/AutomataScriptSystem.h"

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)
#define CHECK_EQ(a, b) do { const auto _a = (a); const auto _b = (b); if (!(_a == _b)) { \
    std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, \
                static_cast<long long>(_a), static_cast<long long>(_b)); ++g_failures; } } while (0)
#define CHECK_NEAR(a, b) do { const double _a = (a); const double _b = (b); if (std::fabs(_a - _b) > 1e-5) { \
    std::printf("FAIL %s:%d: %s ~= %s (%f vs %f)\n", __FILE__, __LINE__, #a, #b, _a, _b); ++g_failures; } } while (0)

using namespace sc4;

// =====================================================================================================================
// A fake cISCLua: a value stack, global tables with string / number keys, registry references and user data.
// =====================================================================================================================
struct LTable;
struct LVal {
    enum Kind { Nil, Num, Str, Bool, TableK, UserData } kind = Nil;
    double num = 0.0;
    std::string text;
    bool b = false;
    std::shared_ptr<LTable> table;
    void* user = nullptr;
    int tag = 0;
};

static bool SameKey(const LVal& a, const LVal& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case LVal::Num: return a.num == b.num;
        case LVal::Str: return a.text == b.text;
        case LVal::Bool: return a.b == b.b;
        default: return false;
    }
}

struct LTable {
    std::vector<std::pair<LVal, LVal>> entries;   // insertion order = Lua's next() order
    LVal Get(const LVal& key) const {
        for (const auto& entry : entries) {
            if (SameKey(entry.first, key)) return entry.second;
        }
        return LVal{};
    }
    void Set(const LVal& key, const LVal& value) {
        for (auto& entry : entries) {
            if (SameKey(entry.first, key)) {
                entry.second = value;
                return;
            }
        }
        entries.emplace_back(key, value);
    }
    int Length() const {
        int count = 0;
        for (;;) {
            LVal key;
            key.kind = LVal::Num;
            key.num = count + 1;
            if (Get(key).kind == LVal::Nil) break;
            ++count;
        }
        return count;
    }
};

struct FakeLua final : cISCLua {
    std::vector<LVal> stack;
    std::map<std::string, LVal> globals;
    std::vector<LVal> refs;   // 1-based registry references
    std::vector<std::pair<CFunction, std::string>> registered;
    std::vector<std::string> globalNames;
    int openCalls = 0, closeCalls = 0, openFlags = -1;

    int Abs(int index) const { return index < 0 ? static_cast<int>(stack.size()) + index : index - 1; }
    LVal& At(int index) { return stack[static_cast<std::size_t>(Abs(index))]; }
    const LVal& At(int index) const { return stack[static_cast<std::size_t>(Abs(index))]; }
    int NonNilRefCount() const {
        int count = 0;
        for (const LVal& value : refs) count += value.kind == LVal::Nil ? 0 : 1;
        return count;
    }

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool Open(int libraries) override { ++openCalls; openFlags = libraries; return true; }
    bool Close() override { ++closeCalls; stack.clear(); return true; }
    void* AsIGZLua5() override { return nullptr; }
    int GetTop() override { return static_cast<int>(stack.size()); }
    void SetTop(int index) override { stack.resize(index < 0 ? 0 : static_cast<std::size_t>(index)); }
    void PushValue(int index) override { stack.push_back(At(index)); }
    void Remove(int index) override { stack.erase(stack.begin() + Abs(index)); }
    void Insert(int index) override { stack.insert(stack.begin() + Abs(index) - 1, stack.back()); stack.pop_back(); }
    void Pop(int count) override {
        assert(count >= 0 && static_cast<std::size_t>(count) <= stack.size());
        stack.resize(stack.size() - static_cast<std::size_t>(count));
    }
    int Type(int index) override {
        switch (At(index).kind) {
            case LVal::Nil: return 0;
            case LVal::Bool: return 1;
            case LVal::Num: return 3;
            case LVal::Str: return 4;
            case LVal::TableK: return 5;
            default: return 7;
        }
    }
    const char* TypeName(int) override { return ""; }
    bool IsNumber(int index) override { return At(index).kind == LVal::Num; }
    bool IsString(int index) override { return At(index).kind == LVal::Str; }
    bool IsCFunction(int) override { return false; }
    int Tag(int index) override { return At(index).tag; }
    bool IsBoolean(int index) override { return At(index).kind == LVal::Bool; }
    bool Equal(int a, int b) override { return SameKey(At(a), At(b)); }
    bool LessThan(int, int) override { return false; }
    std::uint32_t ToUint32(int index) override { return static_cast<std::uint32_t>(static_cast<std::int64_t>(ToNumber(index))); }
    std::int32_t ToSint32(int index) override { return static_cast<std::int32_t>(std::nearbyint(ToNumber(index))); }
    double ToNumber(int index) override { return At(index).kind == LVal::Num ? At(index).num : 0.0; }
    const char* ToString(int index) override {
        LVal& value = At(index);
        if (value.kind == LVal::Num) {
            char buffer[32];
            std::snprintf(buffer, sizeof buffer, "%g", value.num);
            value.text = buffer;
        }
        return value.kind == LVal::Str || value.kind == LVal::Num ? value.text.c_str() : nullptr;
    }
    int Strlen(int index) override { return static_cast<int>(At(index).text.size()); }
    CFunction ToCFunction(int) override { return nullptr; }
    void* ToUserData(int index) override { return At(index).kind == LVal::UserData ? At(index).user : nullptr; }
    void* ToUserTag(int index) override { return ToUserData(index); }
    bool ToBoolean(int index) override { return At(index).kind == LVal::Bool && At(index).b; }
    void PushNil() override { stack.push_back(LVal{}); }
    void PushNumber(double value) override { LVal v; v.kind = LVal::Num; v.num = value; stack.push_back(v); }
    void PushLString(const char* text, int length) override {
        LVal v; v.kind = LVal::Str; v.text.assign(text, static_cast<std::size_t>(length)); stack.push_back(v);
    }
    void PushString(const char* text) override {
        LVal v; v.kind = LVal::Str; v.text = text ? text : ""; stack.push_back(v);
    }
    void PushCClosure(CFunction, int) override { PushNil(); }
    void PushUserTag(void* pointer, int tag) override {
        LVal v; v.kind = LVal::UserData; v.user = pointer; v.tag = tag; stack.push_back(v);
    }
    void PushBoolean(bool value) override { LVal v; v.kind = LVal::Bool; v.b = value; stack.push_back(v); }
    void GetGlobal(const char* name) override {
        auto it = globals.find(name ? name : "");
        stack.push_back(it == globals.end() ? LVal{} : it->second);
    }
    void GetTable(int index) override {
        const int abs = Abs(index);
        const LVal key = stack.back();
        stack.pop_back();
        const LVal table = stack[static_cast<std::size_t>(abs)];
        stack.push_back(table.kind == LVal::TableK ? table.table->Get(key) : LVal{});
    }
    void RawGet(int index) override { GetTable(index); }
    void RawGetI(int index, int n) override {
        const LVal table = At(index);
        LVal key;
        key.kind = LVal::Num;
        key.num = n;
        stack.push_back(table.kind == LVal::TableK ? table.table->Get(key) : LVal{});
    }
    void GetGlobals() override { PushNil(); }
    void GetTagMethod(int, const char*) override { PushNil(); }
    bool GetRef(int ref) override {
        if (ref < 1 || static_cast<std::size_t>(ref) > refs.size()) {
            stack.push_back(LVal{});
            return false;
        }
        stack.push_back(refs[static_cast<std::size_t>(ref - 1)]);
        return true;
    }
    void NewTable() override { LVal v; v.kind = LVal::TableK; v.table = std::make_shared<LTable>(); stack.push_back(v); }
    void SetGlobal(const char* name) override {
        globalNames.emplace_back(name ? name : "");
        globals[name ? name : ""] = stack.back();
        stack.pop_back();
    }
    void SetTable(int index) override {
        const int abs = Abs(index);
        const LVal value = stack.back();
        stack.pop_back();
        const LVal key = stack.back();
        stack.pop_back();
        if (stack[static_cast<std::size_t>(abs)].kind == LVal::TableK) {
            stack[static_cast<std::size_t>(abs)].table->Set(key, value);
        }
    }
    void RawSet(int index) override { SetTable(index); }
    void RawSetI(int index, int n) override {
        const LVal value = stack.back();
        stack.pop_back();
        LVal key;
        key.kind = LVal::Num;
        key.num = n;
        if (At(index).kind == LVal::TableK) At(index).table->Set(key, value);
    }
    void SetTagMethod(int, const char*) override {}
    int Ref(int) override { refs.push_back(stack.back()); stack.pop_back(); return static_cast<int>(refs.size()); }
    int CallProtected(int, int results) override {
        for (int i = 0; i < results; ++i) PushNil();
        return 0;
    }
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
    void Unref(int ref) override {
        if (ref >= 1 && static_cast<std::size_t>(ref) <= refs.size()) refs[static_cast<std::size_t>(ref - 1)] = LVal{};
    }
    int Next(int index) override {
        const int abs = Abs(index);
        const LVal key = stack.back();
        stack.pop_back();
        const LVal table = stack[static_cast<std::size_t>(abs)];
        if (table.kind != LVal::TableK) return 0;
        std::size_t next = 0;
        if (key.kind != LVal::Nil) {
            std::size_t at = 0;
            for (; at < table.table->entries.size(); ++at) {
                if (SameKey(table.table->entries[at].first, key)) break;
            }
            next = at + 1;
        }
        if (next >= table.table->entries.size()) return 0;
        stack.push_back(table.table->entries[next].first);
        stack.push_back(table.table->entries[next].second);
        return 1;
    }
    int GetN(int index) override {
        const LVal table = At(index);
        return table.kind == LVal::TableK ? table.table->Length() : 0;
    }
    void Error(const char*) override {}
    void Concat(int) override {}
    void* NewUserData(int) override { return nullptr; }
    void Register(CFunction fn, const char* name) override { registered.emplace_back(fn, name ? name : ""); }
    void PushUserData(void* pointer, int tag) override { PushUserTag(pointer, tag); }
    int CloneTag(int) override { return 0; }
    bool IsFunction(int) override { return false; }
    bool IsTable(int index) override { return At(index).kind == LVal::TableK; }
    bool IsUserData(int index) override { return At(index).kind == LVal::UserData; }
    bool IsNil(int index) override { return At(index).kind == LVal::Nil; }
    std::uintptr_t ToDataRegistryEntryID(::cISC4GameDataRegistry*, int) override { return 0; }
    cIGZUnknown* ToDataRegistryNode(::cISC4GameDataRegistry*, int) override { return nullptr; }
    void* ToDataRegistryValue(::cISC4GameDataRegistry*, int) override { return nullptr; }
};

// ---- small table builders -------------------------------------------------------------------------------------------
using TablePtr = std::shared_ptr<LTable>;
static LVal Num(double value) { LVal v; v.kind = LVal::Num; v.num = value; return v; }
static LVal Str(const char* text) { LVal v; v.kind = LVal::Str; v.text = text ? text : ""; return v; }
static LVal Bool(bool value) { LVal v; v.kind = LVal::Bool; v.b = value; return v; }
static LVal Tab() { LVal v; v.kind = LVal::TableK; v.table = std::make_shared<LTable>(); return v; }
static void Field(LVal table, const char* name, LVal value) { table.table->Set(Str(name), value); }
static void Item(LVal table, LVal value) { table.table->Set(Num(table.table->Length() + 1), value); }
static LVal Arr(std::initializer_list<LVal> values) {
    LVal table = Tab();
    for (const LVal& value : values) Item(table, value);
    return table;
}

// =====================================================================================================================
// Fakes for the seams.
// =====================================================================================================================
struct FakeUnknown final : cIGZUnknown {
    std::uint32_t refs = 1;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != kGZIID_cISC4Occupant) return false;
        *out = static_cast<cIGZUnknown*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
};

// A cISC4Occupant with every operation stubbed; only QueryInterface / refcount matter to the commands.
struct FakeOccupant final : cISC4Occupant {
    std::uint32_t refs = 1;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != kGZIID_cISC4Occupant) return false;
        *out = static_cast<cISC4Occupant*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    cISCPropertyHolder* AsPropertyHolder() override { return nullptr; }
    std::uint32_t GetType() const override { return 0; }
    void GetPosition(float* xyz) override { xyz[0] = xyz[1] = xyz[2] = 0.0f; }
    void SetPosition(const float*) override {}
    void GetBoundingBox(float* minXyz, float* maxXyz) override {
        minXyz[0] = minXyz[1] = minXyz[2] = 0.0f;
        maxXyz[0] = maxXyz[1] = maxXyz[2] = 0.0f;
    }
    void GetBoundingCityCells(SC4CellRect& rect) override { rect = SC4CellRect{}; }
    void SetRemovalFlags(int) override {}
    void UnsetRemovalFlags(int) override {}
    bool CanRemove(int) override { return true; }
    void PostOccupantMessage(std::uint32_t, std::uint32_t) override {}
    std::uint32_t GetHighlight() const override { return 0; }
    void SetHighlight(int, bool) override {}
    void SetVisibility(bool, bool) override {}
    cISC43DPlaceableObject* GetPlaceableObject() const override { return nullptr; }
    cISC43DPlaceableObject* GetOrCreatePlaceableObject() override { return nullptr; }
    void SetPlaceableObject(cISC43DPlaceableObject*) override {}
    bool IsOccupantGroup(std::uint32_t) const override { return false; }
    void AddOccupantGroup(std::uint32_t) override {}
    bool GetOccupantGroups(std::set<std::uint32_t>&) const override { return false; }
    bool GetOccupantManagerBBox(std::uint8_t*) const override { return false; }
    void SetOccupantManagerBBox(const std::uint8_t*) override {}
    bool GetLotTag(std::uint32_t&) const override { return false; }
    void SetLotTag(std::uint32_t) override {}
    void SetFlag(std::uint32_t) override {}
    void SetAllFlags(std::uint32_t) override {}
    void ClearFlag(std::uint32_t) override {}
    bool IsFlagSet(std::uint32_t) override { return false; }
    std::uint32_t GetFlags() override { return 0; }
};

// cISC4GameDataRegistry: entries with a node / handle, a command function and a context.
struct FakeRegistry final : sc4::cISC4GameDataRegistry {
    struct Entry {
        cIGZUnknown* node = nullptr;
        void* handle = nullptr;
        std::string name;
        GameDataValueFn value = nullptr;
        GameDataCommandFn command = nullptr;
        int flags = 0;
        void* context = nullptr;
        EntryId parent = 0;
        bool isNode = false;
    };
    std::uint32_t refs = 0;
    EntryId nextId = 1;
    std::map<EntryId, Entry> entries;
    EntryId root = 0;

    FakeRegistry() {
        Entry rootEntry;
        rootEntry.name = "";
        rootEntry.isNode = true;
        root = nextId++;
        entries[root] = rootEntry;
    }

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cISC4GameDataRegistry && iid != 1) return false;
        *out = static_cast<cISC4GameDataRegistry*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }

    EntryId AddChildEntry(cIGZUnknown* node, const char* name, EntryId parent, int flags, void* context) override {
        Entry entry;
        entry.node = node;
        entry.handle = node;
        entry.name = name ? name : "";
        entry.flags = flags;
        entry.context = context;
        entry.parent = parent;
        entry.isNode = true;
        const EntryId id = nextId++;
        entries[id] = entry;
        return id;
    }
    EntryId AddChildEntry(void* handle, const char* name, GameDataValueFn fn, EntryId parent, int flags, void* context) override {
        Entry entry;
        entry.handle = handle;
        entry.name = name ? name : "";
        entry.value = fn;
        entry.flags = flags;
        entry.context = context;
        entry.parent = parent;
        const EntryId id = nextId++;
        entries[id] = entry;
        return id;
    }
    EntryId AddChildEntry(void* handle, const char* name, GameDataCommandFn fn, EntryId parent, int flags, void* context) override {
        Entry entry;
        entry.handle = handle;
        entry.name = name ? name : "";
        entry.command = fn;
        entry.flags = flags;
        entry.context = context;
        entry.parent = parent;
        const EntryId id = nextId++;
        entries[id] = entry;
        return id;
    }
    bool RemoveChildEntry(cIGZUnknown* node) override {
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->second.node == node) {
                entries.erase(it);
                return true;
            }
        }
        return false;
    }
    bool RemoveChildEntry(void* handle) override {
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->second.handle == handle) {
                entries.erase(it);
                return true;
            }
        }
        return false;
    }
    EntryId GetEntryID(cIGZUnknown* node) const override {
        for (const auto& entry : entries) {
            if (entry.second.node == node) return entry.first;
        }
        return 0;
    }
    EntryId GetEntryID(void* handle) const override {
        for (const auto& entry : entries) {
            if (entry.second.handle == handle) return entry.first;
        }
        return 0;
    }
    cIGZUnknown* GetNode(EntryId entry) const override {
        auto it = entries.find(entry);
        return it == entries.end() ? nullptr : it->second.node;
    }
    void* GetValue(EntryId entry) const override {
        auto it = entries.find(entry);
        return it == entries.end() ? nullptr : it->second.handle;
    }
    bool IsValidEntry(EntryId entry) const override { return entries.find(entry) != entries.end(); }
    bool IsNodeEntry(EntryId entry) const override {
        auto it = entries.find(entry);
        return it != entries.end() && it->second.isNode;
    }
    const cRZString* GetEntryName(EntryId) const override { return nullptr; }
    bool DoValueExchange(EntryId entry, cIGZVariant& value, bool write) override {
        auto it = entries.find(entry);
        if (it == entries.end() || !it->second.value) return false;
        return it->second.value(it->second.handle, value, write, this);
    }
    bool DoValueCall(EntryId entry, cIGZCommandParameterSet* in, cIGZCommandParameterSet* out) override {
        auto it = entries.find(entry);
        if (it == entries.end() || !it->second.command) return false;
        return it->second.command(it->second.handle, in, out, this);
    }
    bool SetEntryContext(EntryId entry, void* context) override {
        auto it = entries.find(entry);
        if (it == entries.end()) return false;
        it->second.context = context;
        return true;
    }
    void* GetEntryContext(EntryId entry) const override {
        auto it = entries.find(entry);
        return it == entries.end() ? nullptr : it->second.context;
    }
    bool CheckEntryFlags(EntryId entry, int flags) const override {
        auto it = entries.find(entry);
        return it != entries.end() && (it->second.flags & flags) == flags;
    }
    EntryId GetParentNodeEntry(EntryId entry) const override {
        auto it = entries.find(entry);
        return it == entries.end() ? 0 : it->second.parent;
    }
    EntryId GetRootNodeEntry() const override { return root; }
    bool VisitBreadthFirst(EntryId, EntryVisitFn, void*) override { return false; }
    bool VisitImmediateChildren(EntryId, EntryVisitFn, void*) override { return false; }
    bool AddNotificationTarget(cISC4GameDataRegistryNotificationTarget*) override { return false; }
    bool AddNotificationTarget(cIGZMessageTarget2*) override { return false; }
    bool RemoveNotificationTarget(cISC4GameDataRegistryNotificationTarget*) override { return false; }
    bool RemoveNotificationTarget(cIGZMessageTarget2*) override { return false; }
    void DebugDumpRecursive(EntryId) override {}

    EntryId FindNamed(const std::string& name) const {
        for (const auto& entry : entries) {
            if (entry.second.name == name) return entry.first;
        }
        return 0;
    }
};

// cIGZCommandParameterSet backed by a map of cRZVariant.
struct FakeParamSet final : cIGZCommandParameterSet {
    std::uint32_t refs = 0;
    std::map<std::uint32_t, cRZVariant> values;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZCommandParameterSet && iid != 1) return false;
        *out = static_cast<cIGZCommandParameterSet*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    std::uint32_t GetParameterCount() const override { return static_cast<std::uint32_t>(values.size()); }
    void SetParameterCount(std::uint32_t) override {}
    cIGZVariant& operator[](std::uint32_t id) override { return values[id]; }
    cIGZVariant* GetParameter(std::uint32_t id) override {
        auto it = values.find(id);
        return it == values.end() ? nullptr : &it->second;
    }
    void SetParameter(std::uint32_t id, const cIGZVariant& value) override { values[id] = cRZVariant(value); }
    void AppendParameter(const cIGZVariant& value) override {
        std::uint32_t id = 0;
        while (values.find(id) != values.end()) ++id;
        values[id] = cRZVariant(value);
    }
    void RemoveParameter(std::uint32_t id) override { values.erase(id); }
    void SetStatusParameterValue(std::int32_t) override {}
    bool CompareParameterString(std::uint32_t, const char*, bool) override { return false; }
    bool ValidateParameter(std::uint32_t id, std::uint32_t type) override {
        auto it = values.find(id);
        return it != values.end() && it->second.GetType() == type;
    }
    std::uint32_t GetFirstParameterID() const override { return values.empty() ? 0 : values.begin()->first; }
    std::uint32_t GetNextParameterID(std::uint32_t id) const override {
        auto it = values.upper_bound(id);
        return it == values.end() ? 0 : it->first;
    }
};

struct FakeHost final : IAutomataScriptHost {
    FakeLua* lua = nullptr;
    FakeRegistry* registry = nullptr;
    int loadResult = 0;
    std::uint32_t loadedPackage = 0;
    int loadCalls = 0;
    int tickAdds = 0, tickRemoves = 0;
    std::vector<cIGZMessageTarget2*> tickTargets;
    int tickChannel = -1;
    bool driveMap = false;
    std::map<int, std::vector<std::uint32_t>> driveKeys;
    std::set<int> sourceManagers;
    std::map<int, std::uint32_t> sourceCounts;
    std::vector<std::string> reports;

    FakeUnknown controller;
    FakeUnknown automaton;
    int automatonStarts = 0;
    std::uint32_t automatonGroup = 0;
    float automatonPosition[3] = {0, 0, 0};
    std::uint32_t automatonFlags = 0;
    std::uint32_t automatonKey[3] = {0, 0, 0};
    std::uint8_t automatonExtra = 0;

    struct ControllerCall {
        enum Kind { kAdd, kAddGroup, kRemove, kRemoveGroup } kind;
        std::string name;
        cISC4Occupant* occupant = nullptr;
        float position[3] = {0, 0, 0};
        bool hasPosition = false;
    };
    std::vector<ControllerCall> controllerCalls;
    FakeUnknown controllerResult;

    cISCLua* CreateLua() override { return lua; }
    void RegisterLuaExtensions(cISCLua* state) override {
        if (!state) return;
        state->Register(reinterpret_cast<cISCLua::CFunction>(0x1), "print");
        state->Register(reinterpret_cast<cISCLua::CFunction>(0x2), "_dofile_from_resource");
    }
    int LoadPackageScripts(cISCLua*, std::uint32_t package, int) override {
        ++loadCalls;
        loadedPackage = package;
        return loadResult;
    }
    sc4::cISC4GameDataRegistry* Registry() override { return registry; }
    void AddTickNotification(cIGZMessageTarget2* target, int channel) override {
        ++tickAdds;
        if (tickTargets.empty()) tickTargets.push_back(target);
        tickChannel = channel;
    }
    void RemoveTickNotification(cIGZMessageTarget2* target, int channel) override {
        ++tickRemoves;
        (void)target;
        (void)channel;
    }
    cIGZUnknown* AddControllerFromTemplateName(cISC4Occupant* occupant, const cIGZString& name, const float* position) override {
        ControllerCall call;
        call.kind = ControllerCall::kAdd;
        call.name = std::string(name.ToChar(), name.Strlen());
        call.occupant = occupant;
        if (position) {
            call.hasPosition = true;
            for (int i = 0; i < 3; ++i) call.position[i] = position[i];
        }
        controllerCalls.push_back(call);
        return &controllerResult;
    }
    bool AddControllersFromOccupantGroup(const cIGZString& group, cISC4Occupant* occupant, const float* position) override {
        ControllerCall call;
        call.kind = ControllerCall::kAddGroup;
        call.name = std::string(group.ToChar(), group.Strlen());
        call.occupant = occupant;
        if (position) {
            call.hasPosition = true;
            for (int i = 0; i < 3; ++i) call.position[i] = position[i];
        }
        controllerCalls.push_back(call);
        return true;
    }
    bool RemoveController(cISC4Occupant* occupant, const cIGZString& name) override {
        ControllerCall call;
        call.kind = ControllerCall::kRemove;
        call.name = std::string(name.ToChar(), name.Strlen());
        call.occupant = occupant;
        controllerCalls.push_back(call);
        return true;
    }
    bool RemoveControllersFromOccupantGroup(const cIGZString& group, cISC4Occupant* occupant) override {
        ControllerCall call;
        call.kind = ControllerCall::kRemoveGroup;
        call.name = std::string(group.ToChar(), group.Strlen());
        call.occupant = occupant;
        controllerCalls.push_back(call);
        return true;
    }
    cIGZUnknown* CreateAutomaton(std::uint32_t group, const float position[3], std::uint32_t flags, const std::uint32_t key[3],
                                 std::uint8_t extra) override {
        automatonGroup = group;
        for (int i = 0; i < 3; ++i) automatonPosition[i] = position[i];
        automatonFlags = flags;
        for (int i = 0; i < 3; ++i) automatonKey[i] = key[i];
        automatonExtra = extra;
        return &automaton;
    }
    void StartScriptedAutomaton(cIGZUnknown* object) override {
        if (object == &automaton) ++automatonStarts;
    }
    bool SourceManagerPresent(int index) override { return sourceManagers.count(index) != 0; }
    std::uint32_t SourceBuildingCount(int index, std::uint32_t) override {
        auto it = sourceCounts.find(index);
        return it == sourceCounts.end() ? 0 : it->second;
    }
    bool DriveKeyMapAvailable() override { return driveMap; }
    void GetDriveKeyCodes(int command, std::uint32_t out[8], std::uint32_t& count) override {
        auto it = driveKeys.find(command);
        if (it == driveKeys.end()) return;
        const std::size_t n = std::min<std::size_t>(it->second.size(), 8);
        for (std::size_t i = 0; i < n; ++i) out[i] = it->second[i];
        count = static_cast<std::uint32_t>(n);
    }
    void Report(const std::string& text) override { reports.push_back(text); }
};

// =====================================================================================================================
// The fixture: a small "_config.lua".
// =====================================================================================================================
struct Fixture {
    LVal attractorTable, generatorTable, automataGroupTable, occupantGroupTable, trackTable;
    LVal pedestrianTable, vehicleTable, shopperTable, filterTable, airTable, boatTable;
};

static Fixture BuildScripts(FakeLua& lua) {
    Fixture f;

    // ---- occupant / automata groups ------------------------------------------------------------------------------
    f.pedestrianTable = Tab();
    Field(f.pedestrianTable, "group_id", Str("0x4001"));
    Field(f.pedestrianTable, "class_id", Str("0x896e75af"));
    Field(f.pedestrianTable, "percentage", Num(0.75));
    LVal anims = Tab();
    Field(anims, "walk", Str("0x10"));
    Field(anims, "run", Num(32));
    Field(f.pedestrianTable, "anims", anims);
    Field(f.pedestrianTable, "csi_image", Str("0x1234"));
    Field(f.pedestrianTable, "source_building", Str("9876"));

    LVal airports = Tab();
    Field(airports, "Mass", Num(1.5));
    Field(airports, "WingArea", Num(2.5));
    Field(airports, "MaxSpeed", Num(60));
    LVal playerdrive = Tab();
    Field(playerdrive, "MinZoom", Num(2));
    Field(playerdrive, "StartingZoom", Num(3));
    Field(playerdrive, "MaxSpeed", Num(40));
    Field(playerdrive, "OffRoadTurnAngleRange", Arr({Num(0.25), Num(0.75)}));
    LVal effect1 = Tab();
    Field(effect1, "cmd", Num(1));
    Field(effect1, "fx", Str("fx1"));
    Field(effect1, "bone", Str("b1"));
    Field(effect1, "controller", Str("c1"));
    Field(effect1, "hold", Bool(true));
    Field(effect1, "one_shot", Bool(true));
    Field(effect1, "hard_transition", Bool(false));
    Field(effect1, "override", Bool(false));
    LVal effect2 = Tab();
    Field(effect2, "cmd", Num(2));
    Field(effect2, "bone", Str("b2"));
    LVal keyEffects = Arr({effect1, effect2});
    LVal model = Arr({Str("0x11111111"), Str("0x22222222"), Str("0x33333333")});
    LVal models = Arr({model});

    f.vehicleTable = Tab();
    Field(f.vehicleTable, "group_id", Str("0x4002"));
    Field(f.vehicleTable, "class_id", Str("0x896e714a"));
    Field(f.vehicleTable, "percentage", Num(0.5));
    Field(f.vehicleTable, "controllers", Arr({Str("car_gen")}));
    Field(f.vehicleTable, "models", models);
    Field(f.vehicleTable, "trailers", Arr({Str("pedestrian")}));
    Field(f.vehicleTable, "kph", Arr({Num(10), Num(20)}));
    Field(f.vehicleTable, "airplane", airports);
    Field(f.vehicleTable, "playerdrive", playerdrive);
    Field(f.vehicleTable, "csi_image", Str("0x1234"));
    Field(f.vehicleTable, "source_building", Str("9876"));
    Field(f.vehicleTable, "key_effects", keyEffects);

    f.shopperTable = Tab();
    Field(f.shopperTable, "group_id", Str("0x4005"));
    Field(f.shopperTable, "_parent", f.pedestrianTable);   // the same Lua table: it carries the pedestrian group handle

    f.filterTable = Tab();   // no group id: a special filter

    LVal helicopter = Tab();
    Field(helicopter, "Mass", Num(3.5));
    Field(helicopter, "Power", Num(4.5));
    Field(helicopter, "MaxSpeed", Num(80));
    f.airTable = Tab();
    Field(f.airTable, "group_id", Str("0x4003"));
    Field(f.airTable, "helicopter", helicopter);

    LVal watercraft = Tab();
    Field(watercraft, "DecelPerSecond", Num(1.5));
    Field(watercraft, "TurnsPerSecond", Num(2.5));
    Field(watercraft, "MaxMillisecondDelta", Num(100));
    f.boatTable = Tab();
    Field(f.boatTable, "group_id", Str("0x4004"));
    Field(f.boatTable, "WatercraftDriveParams", watercraft);

    f.automataGroupTable = Tab();
    Field(f.automataGroupTable, "pedestrian", f.pedestrianTable);
    Field(f.automataGroupTable, "vehicle", f.vehicleTable);
    Field(f.automataGroupTable, "shopper", f.shopperTable);
    Field(f.automataGroupTable, "filter_group", f.filterTable);
    Field(f.automataGroupTable, "airgroup", f.airTable);
    Field(f.automataGroupTable, "boat", f.boatTable);

    f.occupantGroupTable = Tab();
    Field(f.occupantGroupTable, "pedestrian", f.pedestrianTable);

    // ---- controller templates ------------------------------------------------------------------------------------
    LVal behavior1 = Tab();
    Field(behavior1, "state", Num(0));
    Field(behavior1, "radius", Num(5));
    Field(behavior1, "priority", Num(1));
    Field(behavior1, "percentage", Num(0.5));
    Field(behavior1, "anims", Arr({Str("a1")}));
    Field(behavior1, "sfx", Arr({Str("0x11"), Str("22")}));
    LVal randomWalk = Tab();
    Field(randomWalk, "delay", Arr({Num(1), Num(2)}));
    Field(randomWalk, "turn", Arr({Num(0.3), Num(0.6)}));
    Field(randomWalk, "strength", Arr({Num(2), Num(3)}));
    Field(randomWalk, "idle_delay", Arr({Num(4), Num(5)}));
    Field(randomWalk, "idle_time", Arr({Num(6), Num(7)}));
    Field(randomWalk, "deceleration", Num(0.9));
    Field(randomWalk, "kph", Arr({Num(1), Num(2)}));
    Field(randomWalk, "acceleration", Num(0.7));
    Field(behavior1, "random_walk", randomWalk);

    LVal behavior2 = Tab();
    Field(behavior2, "state", Num(4));
    Field(behavior2, "radius", Num(2));
    Field(behavior2, "priority", Num(1));
    Field(behavior2, "timeout", Num(3.5));
    Field(behavior2, "final", Bool(true));
    Field(behavior2, "ignore_paths", Bool(true));
    Field(behavior2, "ignore_roads", Bool(true));
    Field(behavior2, "ignore_terrain", Bool(true));
    Field(behavior2, "ignore_attractors", Bool(true));
    Field(behavior2, "avoid_lots", Bool(true));
    Field(behavior2, "avoid_roads", Bool(true));
    Field(behavior2, "path_offset", Num(0.5));
    Field(behavior2, "kph_multiply", Arr({Num(2), Num(3)}));
    Field(behavior2, "kph", Arr({Num(4), Num(5)}));
    Field(behavior2, "acceleration", Num(0.6));

    LVal timeOfDay = Tab();
    timeOfDay.table->Set(Num(0), Num(1));
    timeOfDay.table->Set(Num(6), Num(0.25));
    timeOfDay.table->Set(Num(12), Num(0.5));
    timeOfDay.table->Set(Num(24), Num(1));

    f.attractorTable = Tab();
    LVal shop = Tab();
    Field(shop, "radius", Num(20));
    Field(shop, "strength", Arr({Num(50), Num(10)}));
    Field(shop, "automata", Arr({Str("pedestrian")}));
    Field(shop, "time_of_day", timeOfDay);
    Field(shop, "calendar", Arr({Num(1), Num(2)}));
    Field(shop, "behavior", Arr({behavior1, behavior2}));
    Field(shop, "effect_trigger", Arr({Str("0xaa"), Str("0xbb")}));
    Field(shop, "deactivate_trigger", Arr({Str("0xcc")}));
    Field(shop, "lifetime", Num(30));
    Field(shop, "delay", Num(2));
    Field(shop, "use_lot_facing", Bool(true));
    Field(shop, "transient", Bool(true));
    Field(shop, "sfx", Arr({Str("0x1234")}));
    Field(shop, "ignore_states", Arr({Num(3), Num(5)}));
    Field(shop, "exclusive_radius", Num(8));
    Field(shop, "max_count", Num(4));
    Field(f.attractorTable, "shop_attractor", shop);

    f.generatorTable = Tab();
    LVal carGen = Tab();
    Field(carGen, "radius", Arr({Num(10), Num(25)}));
    Field(carGen, "population_pct", Num(0.5));
    Field(carGen, "max_count", Num(6));
    Field(carGen, "rate", Num(2.5));
    Field(carGen, "rate_scale", Num(3));
    Field(carGen, "event", Num(0x77));
    Field(carGen, "random_chance", Num(0.25));
    Field(carGen, "lifetime_count", Num(9));
    Field(carGen, "group_count", Arr({Num(2), Num(4)}));
    Field(carGen, "priority", Num(300));
    Field(carGen, "automata", Arr({Str("vehicle")}));
    Field(carGen, "time_of_day", timeOfDay);
    Field(carGen, "calendar", Arr({Num(3)}));
    LVal generatorBehavior = Tab();
    Field(generatorBehavior, "state", Num(0));
    Field(generatorBehavior, "radius", Num(1));
    Field(generatorBehavior, "priority", Num(1));
    Field(carGen, "behavior", Arr({generatorBehavior}));
    Field(carGen, "effect_trigger", Arr({Str("0x1")}));
    Field(carGen, "lifetime", Num(7));
    Field(carGen, "delay", Num(1));
    Field(carGen, "sfx", Arr({Str("0x22")}));
    Field(carGen, "ignore_states", Arr({Num(1)}));
    Field(f.generatorTable, "car_gen", carGen);

    f.trackTable = Arr({Str("0x10"), Num(32), Str("bad")});

    lua.globals["attractor"] = f.attractorTable;
    lua.globals["generator"] = f.generatorTable;
    lua.globals["automata_group"] = f.automataGroupTable;
    lua.globals["occupant_group"] = f.occupantGroupTable;
    lua.globals["track_buildings"] = f.trackTable;
    return f;
}

// =====================================================================================================================
// main
// =====================================================================================================================
int main() {
    FakeLua lua;
    FakeRegistry registry;
    FakeHost host;
    host.lua = &lua;
    host.registry = &registry;
    host.driveMap = true;
    host.driveKeys[3] = {0x31, 0x32};
    host.sourceManagers.insert(0);
    host.sourceManagers.insert(1);
    host.sourceCounts[1] = 7;
    SetAutomataScriptHost(&host);
    const Fixture fixture = BuildScripts(lua);

    // ---- identity / refcount ---------------------------------------------------------------------------------------
    auto* system = new cSC4AutomataScriptSystem();
    CHECK_EQ(system->Initialized(), false);
    CHECK_EQ(system->AddRef(), 1u);
    void* view = nullptr;
    CHECK(system->QueryInterface(kGZIID_cISC4AutomataScriptSystem, &view));
    CHECK(view == static_cast<IControllerScriptSystem*>(system));
    static_cast<cIGZUnknown*>(view)->Release();
    CHECK(system->QueryInterface(1, &view) && view != nullptr);
    static_cast<cIGZUnknown*>(view)->Release();
    CHECK(system->QueryInterface(0x452294aa, &view));
    static_cast<cIGZUnknown*>(view)->Release();
    CHECK(system->QueryInterface(0xe98b2f57, &view));
    static_cast<cIGZUnknown*>(view)->Release();
    CHECK(system->QueryInterface(0xe4fda3d4, &view));
    static_cast<cIGZUnknown*>(view)->Release();
    CHECK(!system->QueryInterface(0x12345678, &view));
    CHECK_EQ(system->GetGZCLSID(), kGZCLSID_cSC4AutomataScriptSystem);
    cGZMessage done;
    done.type = kMessageScriptSystemDone;
    CHECK(system->DoMessage(done));
    done.type = 0;
    CHECK(!system->DoMessage(done));

    // ---- variants --------------------------------------------------------------------------------------------------
    {
        cRZVariant hex;
        hex.SetValString(cRZString("0x4a5e8f3f"));
        CHECK_EQ(cSC4AutomataScriptSystem::GetHexFromVariant(hex), 0x4a5e8f3fu);
        cRZVariant number;
        number.SetValUint32(0x4002);
        CHECK_EQ(cSC4AutomataScriptSystem::GetHexFromVariant(number), 0x4002u);
        cRZVariant converted;
        converted.SetValFloat32(1.5f);   // the variant converts a float to uint32 (1); the original does the same
        CHECK_EQ(cSC4AutomataScriptSystem::GetHexFromVariant(converted), 1u);
    }

    // ---- stream guard ----------------------------------------------------------------------------------------------
    {
        PortMemoryOStream out;
        CHECK(system->Write(out));
        CHECK_EQ(out.bytes().size(), std::size_t(2));
        PortMemoryIStream in(out.bytes().data(), out.bytes().size());
        CHECK(system->Read(in));
        PortMemoryIStream wrong(out.bytes().data(), out.bytes().size());
        auto* other = new cSC4AutomataScriptSystem();
        std::uint16_t badVersion = 2;
        PortMemoryOStream badOut;
        badOut.SetUint16(badVersion);
        PortMemoryIStream badIn(badOut.bytes().data(), badOut.bytes().size());
        CHECK(!other->Read(badIn));
        delete other;
        CHECK(system->Lua() == nullptr);
    }

    // ---- Init: default groups, scripts, registry commands ----------------------------------------------------------
    CHECK(system->Init());
    CHECK(system->Initialized());
    CHECK_EQ(host.tickAdds, 1);
    CHECK_EQ(host.tickChannel, 0xc0);
    CHECK_EQ(host.loadCalls, 1);
    CHECK_EQ(host.loadedPackage, kAutomataScriptPackage);
    CHECK_EQ(lua.openCalls, 1);
    CHECK_EQ(lua.openFlags, static_cast<int>(kLuaOpenAll));
    CHECK_EQ(lua.closeCalls, 1);
    CHECK(system->Lua() == nullptr);
    CHECK_EQ(lua.registered.size(), std::size_t(2));
    if (lua.registered.size() == 2) {
        CHECK(lua.registered[0].second == "print");
        CHECK(lua.registered[1].second == "_dofile_from_resource");
    }
    {
        bool sawInGame = false, sawBeta = false;
        for (const std::string& name : lua.globalNames) {
            if (name == "_IN_GAME") sawInGame = true;
            if (name == "_BETA_BUILD") sawBeta = true;
        }
        CHECK(sawInGame);
        CHECK(sawBeta);
    }
    CHECK_EQ(host.reports.size(), std::size_t(0));

    // registry node + the seven commands
    const EntryId automataNode = registry.FindNamed("automata");
    CHECK(automataNode != 0);
    CHECK(registry.IsNodeEntry(automataNode));
    static const char* const kCommandNames[] = {
        "create_automaton", "create_controller", "attach_controller", "attach_controller_group",
        "detach_controller", "detach_controller_group", "get_source_building_count",
    };
    for (const char* name : kCommandNames) CHECK(registry.FindNamed(name) != 0);
    CHECK_EQ(registry.entries.size(), std::size_t(9));   // the root node + "automata" + 7 commands

    // ---- default groups + scripted groups --------------------------------------------------------------------------
    cSC4OccupantGroupTemplate* pedestrian = system->GetOccupantGroup(0x4001);
    cSC4OccupantGroupTemplate* vehicle = system->GetOccupantGroup(0x4002);
    cSC4OccupantGroupTemplate* shopper = system->GetOccupantGroup(0x4005);
    cSC4OccupantGroupTemplate* air = system->GetOccupantGroup(0x4003);
    cSC4OccupantGroupTemplate* boat = system->GetOccupantGroup(0x4004);
    CHECK(pedestrian != nullptr);
    CHECK(vehicle != nullptr);
    CHECK(shopper != nullptr);
    CHECK(air != nullptr);
    CHECK(boat != nullptr);
    CHECK_EQ(pedestrian->classId, kClassIdPedestrianGroup);   // the script does not override it
    CHECK_EQ(vehicle->classId, kClassIdVehicleGroup);
    CHECK_EQ(system->GetOccupantGroupNameMap().size(), std::size_t(6));
    CHECK_EQ(system->GetAutomataGroupMap().size(), std::size_t(6));
    CHECK_EQ(system->GetOccupantGroupIDMap().size(), std::size_t(5));
    CHECK_EQ(system->GetSpecialFilterOccupantGroups().size(), std::size_t(1));
    if (!system->GetSpecialFilterOccupantGroups().empty()) {
        CHECK(system->GetSpecialFilterOccupantGroups().front() == system->GetOccupantGroup(cRZString("filter_group")));
    }
    CHECK(system->GetOccupantGroup(cRZString("filter_group")) != nullptr);

    // ---- templates -------------------------------------------------------------------------------------------------
    CHECK_EQ(system->GetControllerTemplateMap().size(), std::size_t(2));
    tControllerTemplate* shop = system->GetControllerTemplate(cRZString("shop_attractor"));
    tControllerTemplate* carGen = system->GetControllerTemplate(cRZString("car_gen"));
    CHECK(shop != nullptr);
    CHECK(carGen != nullptr);
    if (shop) {
        CHECK_EQ(shop->controllerType, kControllerTypeAttractor);
        CHECK_EQ(shop->classKey, kControllerTypeAttractor);
        CHECK_NEAR(shop->radius, 20.0f);
        CHECK_NEAR(shop->lifetime, 30.0f);
        CHECK_NEAR(shop->startDelay, 2.0f);
        CHECK_EQ(shop->maxAutomata, 4u);
        CHECK_EQ(shop->flags, 0x1au);   // 2 | transient (8) | use_lot_facing (0x10)
        CHECK_NEAR(shop->uniqueRadius, 8.0f);
        CHECK_EQ(shop->field60, 6);     // calendar {1,2}
        CHECK_EQ(shop->groups.size(), std::size_t(1));
        if (!shop->groups.empty()) CHECK_EQ(shop->groups[0], 0x4001u);
        CHECK(shop->groupNames.empty());
        CHECK_EQ(shop->effectsOn.size(), std::size_t(2));
        CHECK_EQ(shop->effectsOff.size(), std::size_t(1));
        CHECK_EQ(shop->sounds.size(), std::size_t(1));
        if (!shop->sounds.empty()) CHECK_EQ(shop->sounds[0], 0x1234u);
        CHECK_EQ(shop->ignoreStates.size(), std::size_t(2));

        std::vector<SC4Point> curve;
        shop->curve.GetResponseCurveData(curve);
        CHECK_EQ(curve.size(), std::size_t(4));
        if (curve.size() == 4) {
            CHECK_NEAR(curve[0].x, 0.0f);
            CHECK_NEAR(curve[0].y, 1.0f);
            CHECK_NEAR(curve[1].x, 6.0f);
            CHECK_NEAR(curve[1].y, 0.25f);
            CHECK_NEAR(curve[2].x, 12.0f);
            CHECK_NEAR(curve[3].x, 24.0f);
        }
        auto* attractor = static_cast<tAttractorTemplate*>(shop);
        std::vector<SC4Point> strength;
        attractor->strengthCurve.GetResponseCurveData(strength);
        CHECK_EQ(strength.size(), std::size_t(2));
        if (strength.size() == 2) {
            CHECK_NEAR(strength[0].x, 0.0f);
            CHECK_NEAR(strength[0].y, 50.0f);
            CHECK_NEAR(strength[1].x, 20.0f);
            CHECK_NEAR(strength[1].y, 10.0f);
        }
        CHECK_EQ(shop->behaviors.size(), std::size_t(2));
        if (shop->behaviors.size() == 2) {
            // sorted by radius (equal priority): the radius 2 behaviour first
            CHECK_NEAR(shop->behaviors[0].radius, 2.0f);
            CHECK_EQ(shop->behaviors[0].state, 4u);
            CHECK(shop->behaviors[0].final);
            CHECK(shop->behaviors[0].ignorePaths);
            CHECK(shop->behaviors[0].avoidRoads);
            CHECK_NEAR(shop->behaviors[0].timeout, 3.5f);
            CHECK_NEAR(shop->behaviors[0].pathOffset, 0.5f);
            CHECK_NEAR(shop->behaviors[0].kphMultiply[0], 2.0f);
            CHECK_NEAR(shop->behaviors[0].kphMultiply[1], 3.0f);
            CHECK_EQ(shop->behaviors[0].kph[0], 4);
            CHECK_NEAR(shop->behaviors[0].acceleration, 0.6f);
            CHECK_NEAR(shop->behaviors[0].delay[0], 10.0f);   // defaults kept

            const tBehaviorTemplate& walk = shop->behaviors[1];
            CHECK_NEAR(walk.radius, 5.0f);
            CHECK_NEAR(walk.percentage, 0.5f);
            CHECK_EQ(walk.priority, 1u);
            CHECK_EQ(walk.anims.size(), std::size_t(1));
            CHECK_EQ(walk.sfx.size(), std::size_t(2));
            if (walk.sfx.size() == 2) CHECK_EQ(walk.sfx[1], 0x22u);
            CHECK_NEAR(walk.delay[0], 1.0f);
            CHECK_NEAR(walk.delay[1], 2.0f);
            CHECK_NEAR(walk.turn[0], 0.3f);
            CHECK_NEAR(walk.turn[1], 0.6f);
            CHECK_NEAR(walk.strength[1], 3.0f);
            CHECK_NEAR(walk.idleDelay[0], 4.0f);
            CHECK_NEAR(walk.idleTime[1], 7.0f);
            CHECK_NEAR(walk.deceleration, 0.9f);
            CHECK_NEAR(walk.acceleration, 0.7f);
        }
    }
    if (carGen) {
        CHECK_EQ(carGen->controllerType, kControllerTypeGenerator);
        auto* generator = static_cast<tGeneratorTemplate*>(carGen);
        CHECK_NEAR(generator->radius, 10.0f);
        CHECK_NEAR(generator->radius2, 25.0f);
        CHECK_NEAR(generator->countScale, 0.5f);
        CHECK_EQ(generator->countMode, 2);
        CHECK_EQ(generator->maxAutomata, 6u);
        CHECK_NEAR(generator->rate, 2.5f);
        CHECK_EQ(generator->periodUnit, 3);
        CHECK_EQ(generator->eventId, 0x77u);
        CHECK_NEAR(generator->probability, 0.25f);
        CHECK_EQ(generator->lifetimeAutomataLimit, 9u);
        CHECK_EQ(generator->groupSizeMin, 2);
        CHECK_EQ(generator->groupSizeMax, 4);
        CHECK_EQ(generator->spawnArgument, 0xff);   // priority 300 clamps to 0xff
        CHECK_EQ(generator->field60, 8);            // calendar {3}
        CHECK_NEAR(generator->lifetime, 7.0f);
        CHECK_EQ(generator->groups.size(), std::size_t(1));
        if (!generator->groups.empty()) CHECK_EQ(generator->groups[0], 0x4002u);
        CHECK_EQ(generator->behaviors.size(), std::size_t(1));
    }

    // ---- group fields ----------------------------------------------------------------------------------------------
    if (pedestrian) {
        CHECK_NEAR(pedestrian->chance_, 0.75f);
        CHECK_EQ(pedestrian->animOffsets.size(), std::size_t(2));
        CHECK_EQ(pedestrian->animOffsets["walk"], 0x10u);
        CHECK_EQ(pedestrian->animOffsets["run"], 32u);
        CHECK_EQ(pedestrian->csiImage, 0x1234u);
        CHECK_EQ(pedestrian->sourceBuilding, 0x9876u);
    }
    if (vehicle) {
        CHECK_NEAR(vehicle->chance_, 0.5f);
        CHECK_EQ(vehicle->controllerTemplates_.size(), std::size_t(1));
        if (!vehicle->controllerTemplates_.empty()) CHECK(vehicle->controllerTemplates_.front() == carGen);
        CHECK_EQ(vehicle->resourceKeys.size(), std::size_t(1));
        if (!vehicle->resourceKeys.empty()) {
            CHECK_EQ(vehicle->resourceKeys.front().type, 0x11111111u);
            CHECK_EQ(vehicle->resourceKeys.front().group, 0x22222222u);
            CHECK_EQ(vehicle->resourceKeys.front().instance, 0x33333333u);
        }
        CHECK_NEAR(vehicle->kphMin, 10.0f);
        CHECK_NEAR(vehicle->kphMax, 20.0f);
        CHECK(vehicle->driveParams != nullptr);
        if (vehicle->driveParams) {
            CHECK_NEAR(vehicle->driveParams->At(0x00), 1.5f);
            CHECK_NEAR(vehicle->driveParams->At(0x04), 2.5f);
            CHECK_NEAR(vehicle->driveParams->At(0x0c), 60.0f);
        }
        CHECK(vehicle->playerDriveParams != nullptr);
        if (vehicle->playerDriveParams) {
            CHECK_EQ(vehicle->playerDriveParams->I(0x00), 2);
            CHECK_EQ(vehicle->playerDriveParams->I(0x04), 3);
            CHECK_NEAR(vehicle->playerDriveParams->F(0x08), 40.0f);
            CHECK_NEAR(vehicle->playerDriveParams->F(0x24), 0.25f);
            CHECK_NEAR(vehicle->playerDriveParams->F(0x28), 0.75f);
        }
        CHECK_EQ(vehicle->keyEffects.size(), std::size_t(1));   // the second effect has no "fx"
        auto effect = vehicle->keyEffects.find(1);
        CHECK(effect != vehicle->keyEffects.end());
        if (effect != vehicle->keyEffects.end()) {
            CHECK(effect->second.fx.ToChar() == std::string("fx1"));
            CHECK(effect->second.bone.ToChar() == std::string("b1"));
            CHECK(effect->second.controller.ToChar() == std::string("c1"));
            CHECK_EQ(effect->second.value, 0x10u | 0x20u | 1u | 2u);
        }
        CHECK_EQ(vehicle->trailerGroupIds.size(), std::size_t(1));
        if (!vehicle->trailerGroupIds.empty()) CHECK_EQ(vehicle->trailerGroupIds.front(), 0x4001u);
        CHECK(vehicle->trailerNames.empty());
    }
    if (air && air->driveParams) CHECK_NEAR(air->driveParams->At(0x00), 3.5f);
    if (boat && boat->driveParams) {
        CHECK_NEAR(boat->driveParams->At(0x00), 1.5f);
        CHECK_NEAR(boat->driveParams->At(0x8c), 100.0f);
    }
    if (shopper) {
        CHECK(shopper->parent == pedestrian);
        CHECK_EQ(shopper->parentRef, -2);
        CHECK_EQ(shopper->selfRef, -2);
    }
    if (pedestrian) {
        CHECK_EQ(pedestrian->parentRef, -2);
        CHECK_EQ(pedestrian->selfRef, -2);
    }
    CHECK_EQ(lua.NonNilRefCount(), 0);   // every _parent / self reference was unref'd by the link pass
    // the link pass cleared the "__group_handle" user data from the shared pedestrian table
    {
        LVal handle;
        handle.kind = LVal::Str;
        handle.text = "__group_handle";
        CHECK(fixture.pedestrianTable.table->Get(handle).kind == LVal::Nil);
    }

    // ---- drive commands / track buildings --------------------------------------------------------------------------
    CHECK_EQ(system->GetDriveCommand(0x31), 3);
    CHECK_EQ(system->GetDriveCommand(0x32), 3);
    CHECK_EQ(system->GetDriveCommand(0x99), kPlayerDriveCommandNone);
    CHECK_EQ(system->GetTrackBuildingGroups().size(), std::size_t(3));
    if (system->GetTrackBuildingGroups().size() == 3) {
        auto it = system->GetTrackBuildingGroups().begin();
        CHECK_EQ(*it++, 0x10u);
        CHECK_EQ(*it++, 32u);
        CHECK_EQ(*it++, 0xbadu);
    }

    // ---- registry commands -----------------------------------------------------------------------------------------
    FakeOccupant occupant;   // outlives the parameter sets below: checked after they released their references
    {
        // create_automaton: group, position, flags, resource key, extra
        FakeParamSet in, out;
        in.SetParameter(0, cRZVariant(cRZString("0x4002")));
        in.SetParameter(1, cRZVariant(1.0f));
        in.SetParameter(2, cRZVariant(2.0f));
        in.SetParameter(3, cRZVariant(3.0f));
        in.SetParameter(4, cRZVariant(0x20u));
        in.SetParameter(5, cRZVariant(cRZString("0x11111111")));
        in.SetParameter(6, cRZVariant(cRZString("0x22222222")));
        in.SetParameter(7, cRZVariant(cRZString("0x33333333")));
        in.SetParameter(8, cRZVariant(std::uint8_t(9)));
        CHECK(registry.DoValueCall(registry.FindNamed("create_automaton"), &in, &out));
        CHECK_EQ(host.automatonGroup, 0x4002u);
        CHECK_NEAR(host.automatonPosition[0], 1.0f);
        CHECK_NEAR(host.automatonPosition[1], 2.0f);
        CHECK_NEAR(host.automatonPosition[2], 3.0f);
        CHECK_EQ(host.automatonFlags, 0x22u);   // param 4 | 2
        CHECK_EQ(host.automatonKey[0], 0x11111111u);
        CHECK_EQ(host.automatonKey[1], 0x22222222u);
        CHECK_EQ(host.automatonKey[2], 0x33333333u);
        CHECK_EQ(host.automatonExtra, 9);
        CHECK_EQ(host.automatonStarts, 1);
        CHECK(out[1].GetValUnknown() == static_cast<cIGZUnknown*>(&host.automaton));

        // without a parameter 4 the default flags are 0x2000002
        FakeParamSet in2, out2;
        in2.SetParameter(0, cRZVariant(cRZString("0x4002")));
        in2.SetParameter(1, cRZVariant(0.0f));
        in2.SetParameter(2, cRZVariant(0.0f));
        in2.SetParameter(3, cRZVariant(0.0f));
        CHECK(registry.DoValueCall(registry.FindNamed("create_automaton"), &in2, &out2));
        CHECK_EQ(host.automatonFlags, 0x2000002u);

        // create_controller
        host.controllerCalls.clear();
        FakeParamSet in3, out3;
        in3.SetParameter(0, cRZVariant(cRZString("shop_attractor")));
        in3.SetParameter(1, cRZVariant(4.0f));
        in3.SetParameter(2, cRZVariant(5.0f));
        in3.SetParameter(3, cRZVariant(6.0f));
        CHECK(registry.DoValueCall(registry.FindNamed("create_controller"), &in3, &out3));
        CHECK_EQ(host.controllerCalls.size(), std::size_t(1));
        if (!host.controllerCalls.empty()) {
            CHECK(host.controllerCalls[0].kind == FakeHost::ControllerCall::kAdd);
            CHECK(host.controllerCalls[0].name == "shop_attractor");
            CHECK(host.controllerCalls[0].occupant == nullptr);
            CHECK(host.controllerCalls[0].hasPosition);
            CHECK_NEAR(host.controllerCalls[0].position[0], 4.0f);
            CHECK_NEAR(host.controllerCalls[0].position[2], 6.0f);
        }
        CHECK(out3[1].GetValUnknown() == static_cast<cIGZUnknown*>(&host.controllerResult));

        // attach_controller / attach_controller_group / detach_controller / detach_controller_group
        host.controllerCalls.clear();
        FakeParamSet in4, out4;
        in4.SetParameter(0, cRZVariant(cRZString("shop_attractor")));
        in4.SetParameter(1, cRZVariant(static_cast<cIGZUnknown*>(&occupant)));
        CHECK(registry.DoValueCall(registry.FindNamed("attach_controller"), &in4, &out4));
        CHECK_EQ(host.controllerCalls.size(), std::size_t(1));
        if (!host.controllerCalls.empty()) {
            CHECK(host.controllerCalls[0].kind == FakeHost::ControllerCall::kAdd);
            CHECK(host.controllerCalls[0].occupant == &occupant);
            CHECK(!host.controllerCalls[0].hasPosition);
        }

        host.controllerCalls.clear();
        FakeParamSet in5, out5;
        in5.SetParameter(0, cRZVariant(cRZString("vehicle")));
        in5.SetParameter(1, cRZVariant(static_cast<cIGZUnknown*>(&occupant)));
        CHECK(registry.DoValueCall(registry.FindNamed("attach_controller_group"), &in5, &out5));
        CHECK_EQ(host.controllerCalls.size(), std::size_t(1));
        if (!host.controllerCalls.empty()) {
            CHECK(host.controllerCalls[0].kind == FakeHost::ControllerCall::kAddGroup);
            CHECK(host.controllerCalls[0].name == "vehicle");
            CHECK(host.controllerCalls[0].occupant == &occupant);
        }

        host.controllerCalls.clear();
        FakeParamSet in6, out6;
        in6.SetParameter(0, cRZVariant(cRZString("shop_attractor")));
        in6.SetParameter(1, cRZVariant(static_cast<cIGZUnknown*>(&occupant)));
        CHECK(registry.DoValueCall(registry.FindNamed("detach_controller"), &in6, &out6));
        CHECK_EQ(host.controllerCalls.size(), std::size_t(1));
        if (!host.controllerCalls.empty()) {
            CHECK(host.controllerCalls[0].kind == FakeHost::ControllerCall::kRemove);
            CHECK(host.controllerCalls[0].name == "shop_attractor");
            CHECK(host.controllerCalls[0].occupant == &occupant);
        }

        host.controllerCalls.clear();
        FakeParamSet in7, out7;
        in7.SetParameter(0, cRZVariant(cRZString("vehicle")));
        in7.SetParameter(1, cRZVariant(static_cast<cIGZUnknown*>(&occupant)));
        CHECK(registry.DoValueCall(registry.FindNamed("detach_controller_group"), &in7, &out7));
        CHECK_EQ(host.controllerCalls.size(), std::size_t(1));
        if (!host.controllerCalls.empty()) {
            CHECK(host.controllerCalls[0].kind == FakeHost::ControllerCall::kRemoveGroup);
            CHECK(host.controllerCalls[0].name == "vehicle");
            CHECK(host.controllerCalls[0].occupant == &occupant);
        }

        // get_source_building_count
        FakeParamSet in8, out8;
        in8.SetParameter(0, cRZVariant(0x4002u));
        CHECK(registry.DoValueCall(registry.FindNamed("get_source_building_count"), &in8, &out8));
        std::uint32_t count = 0;
        CHECK(out8[1].GetValUint32(count));
        CHECK_EQ(count, 7u);
    }
    CHECK_EQ(occupant.refs, 1u);   // every command and every parameter set released the interface

    // ---- direct importer quirks ------------------------------------------------------------------------------------
    {
        FakeLua local;
        FakeHost localHost;
        localHost.lua = &local;
        SetAutomataScriptHost(&localHost);
        auto* other = new cSC4AutomataScriptSystem();
        other->AddRef();
        other->SetLua(&local);
        CHECK_EQ(local.GetTop(), 0);

        // track_buildings stays on the stack (quirk)
        local.globals["track_buildings"] = Arr({Num(7)});
        CHECK(other->ImportTrackBuildingGroups());
        CHECK_EQ(local.GetTop(), 1);
        CHECK_EQ(other->GetTrackBuildingGroups().size(), std::size_t(1));
        local.SetTop(0);
        CHECK_EQ(local.GetTop(), 0);

        // ImportSFXList leaves an existing sound list alone when the table has no "sfx"
        tControllerTemplate* tmpl = other->CreateControllerTemplate(cRZString("tmpl"), kControllerTypeGenerator);
        CHECK(tmpl != nullptr);
        if (tmpl) {
            tmpl->sounds.push_back(0x1234);
            LVal empty = Tab();
            local.stack.push_back(empty);
            CHECK(other->ImportSFXList(1, tmpl));
            CHECK_EQ(tmpl->sounds.size(), std::size_t(1));
            local.Pop(1);
            LVal withSfx = Tab();
            Field(withSfx, "sfx", Arr({Str("0x99")}));
            local.stack.push_back(withSfx);
            CHECK(other->ImportSFXList(1, tmpl));
            CHECK_EQ(tmpl->sounds.size(), std::size_t(1));
            if (!tmpl->sounds.empty()) CHECK_EQ(tmpl->sounds[0], 0x99u);
            local.Pop(1);
        }
        other->SetLua(nullptr);
        other->ClearTemplates();   // Shutdown leaves the controller templates to their owner
        other->Shutdown();
        CHECK_EQ(other->Release(), 0u);   // FinalRelease deletes it
        SetAutomataScriptHost(&host);
    }

    // ---- ExecuteScripts error path ---------------------------------------------------------------------------------
    {
        FakeLua errorLua;
        FakeHost errorHost;
        FakeRegistry errorRegistry;
        errorHost.lua = &errorLua;
        errorHost.registry = &errorRegistry;
        errorHost.loadResult = 7;
        SetAutomataScriptHost(&errorHost);
        auto* failing = new cSC4AutomataScriptSystem();
        failing->AddRef();
        CHECK(failing->Init());
        CHECK_EQ(errorHost.reports.size(), std::size_t(1));
        if (!errorHost.reports.empty()) CHECK(errorHost.reports[0].find("Error code 7 executing automata _config.lua script!") == 0);
        CHECK(failing->GetControllerTemplateMap().empty());
        CHECK_EQ(failing->GetAutomataGroupMap().size(), std::size_t(2));   // the default groups are automata groups too
        CHECK_EQ(failing->GetOccupantGroupIDMap().size(), std::size_t(2));   // the two default groups were still created
        CHECK_EQ(failing->Release(), 0u);
        SetAutomataScriptHost(&host);
    }

    // ---- Shutdown --------------------------------------------------------------------------------------------------
    CHECK(system->Shutdown());
    CHECK(!system->Initialized());
    CHECK_EQ(host.tickRemoves, 1);
    CHECK_EQ(lua.closeCalls, 1);   // ExecuteScripts closed and released the state; Shutdown has nothing to close
    CHECK_EQ(registry.FindNamed("automata"), EntryId(0));
    CHECK(system->GetOccupantGroupNameMap().empty());
    CHECK(system->GetAutomataGroupMap().empty());
    CHECK(system->GetOccupantGroupIDMap().empty());
    CHECK(system->GetSpecialFilterOccupantGroups().empty());
    CHECK(system->GetControllerTemplateMap().empty());
    CHECK_EQ(system->Release(), 0u);   // Shutdown already ran; FinalRelease deletes

    SetAutomataScriptHost(nullptr);
    if (g_failures == 0) std::printf("all checks passed\n");
    else std::printf("%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
