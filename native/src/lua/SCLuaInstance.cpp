#include "lua/SCLuaInstance.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include "lua/RealLuaApi.h"
#include "lua/SCLua.h"

namespace lua5 {
namespace {

// The Lua 4 compatibility script the original embeds (4071 bytes at PC 0x00ac9798; it matches etc/compat.lua of Lua 5.0).
const unsigned char kCompatScript[] = {
#include "lua_compat.inc"
    , 0};

inline lua_State* W(::lua_State* L) { return reinterpret_cast<lua_State*>(L); }
inline lua_CFunction WF(cISCLua::CFunction f) { return reinterpret_cast<lua_CFunction>(f); }

// The error reporter (lua-host.md 4.3): the original builds the text and drops it; here it goes to the log under SC4_DEBUG.
void ReportError(cGZLua5Thread* thread, int status, const char* message) {
    static const bool trace = std::getenv("SC4_DEBUG") != nullptr || std::getenv("SC4_LUADEBUG") != nullptr;
    if (!trace) return;
    static const char* const kKinds[] = {"Unknown", "Run-time", "File-related", "Syntax", "Allocation"};
    std::fprintf(stderr, "lua: script error (%s, thread %d): %s\n", kKinds[status >= 1 && status <= 4 ? status : 0],
                 thread ? thread->GetID() : -1, message ? message : "<unknown description>");
}

// _ALERT (PC 0x008b8d70): a string on top of the stack is reported with status 1.
int Alert(lua_State* L) {
    ILuaApi& api = LuaApi();
    if (api.gettop(L) > 0 && api.type(L, -1) == kLuaTypeString) {
        cGZLua5Thread* thread = api.get_thread_slot(L);
        ILua5Owner* owner = thread ? thread->GetLua() : nullptr;
        if (LuaErrorCallback report = owner ? owner->GetErrorCallback() : nullptr) report(thread, 1, api.tostring(L, -1));
    }
    return 0;
}

LuaCompat& Compat() {
    static LuaCompat compat(Alert);
    return compat;
}

struct ThreadFactory final : IThreadFactory {
    cGZLua5Thread* CreateThreadObject(unsigned) override {
        auto* thread = new cGZLua5Thread(*RealLuaApi(), Compat());
        thread->AddRef();
        return thread;
    }
};

struct ParameterSet final : ICommandParameterSet {
    void AddRef() override { ++refs; }
    void Release() override { if (--refs <= 0) delete this; }
    int refs = 0;
};

struct Host final : ISCLuaHost {
    ICommandParameterSet* NewParameterSet() override { return new ParameterSet; }
    void RegisterRegistryListener(cSCLua*) override {}     // PARTIAL: the game-data registry mirror is not recovered
    void UnregisterRegistryListener(cSCLua*) override {}
};

class Instance;
// The recovered host, tied back to the cISCLua object that owns it.
struct Machine final : cSCLua {
    Machine(Instance* outer, IThreadFactory* factory, ISCLuaHost& host)
        : cSCLua(*RealLuaApi(), Compat(), factory, reinterpret_cast<const char*>(kCompatScript), host), outer(outer) {}
    Instance* outer;
};

class Instance final : public cISCLua {
public:
    Instance() : lua_(new Machine(this, &factory_, host_)) {
        lua_->AddRef();
        lua_->SetErrorReporter(ReportError, false);
    }

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (!out || (iid != 1 && iid != kIID_GameDataRegistryListener)) return false;
        *out = static_cast<cISCLua*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (refs_ > 1) return --refs_;
        delete this;
        return 0;
    }
    bool Open(int libraries) override { return lua_->Open(libraries); }
    bool Close() override { return lua_->Close(); }
    void* AsIGZLua5() override { return lua_->AsIGZLua5(); }
    int GetTop() override { return lua_->GetTop(); }
    void SetTop(int index) override { lua_->SetTop(index); }
    void PushValue(int index) override { lua_->PushValue(index); }
    void Remove(int index) override { lua_->Remove(index); }
    void Insert(int index) override { lua_->Insert(index); }
    void Pop(int count) override { lua_->Pop(count); }
    int Type(int index) override { return lua_->Type(index); }
    const char* TypeName(int type) override { return lua_->TypeName(type); }
    bool IsNumber(int index) override { return lua_->IsNumber(index); }
    bool IsString(int index) override { return lua_->IsString(index); }
    bool IsCFunction(int index) override { return lua_->IsCFunction(index); }
    int Tag(int index) override { return lua_->Tag(index); }
    bool IsBoolean(int index) override { return lua_->IsBoolean(index); }
    bool Equal(int a, int b) override { return lua_->Equal(a, b); }
    bool LessThan(int a, int b) override { return lua_->LessThan(a, b); }
    std::uint32_t ToUint32(int index) override { return static_cast<std::uint32_t>(lua_->ToUint32(index)); }
    std::int32_t ToSint32(int index) override { return lua_->ToSint32(index); }
    double ToNumber(int index) override { return lua_->ToNumber(index); }
    const char* ToString(int index) override { return lua_->ToString(index); }
    int Strlen(int index) override { return static_cast<int>(lua_->Strlen(index)); }
    CFunction ToCFunction(int index) override { return reinterpret_cast<CFunction>(lua_->ToCFunction(index)); }
    void* ToUserData(int index) override { return lua_->ToUserData(index); }
    void* ToUserTag(int index) override {   // the pointer boxed by PushUserTag (the original dereferences a null for other values)
        void* box = lua_->ToUserData(index);
        return box ? *static_cast<void**>(box) : nullptr;
    }
    bool ToBoolean(int index) override { return lua_->ToBoolean(index); }
    void PushNil() override { lua_->PushNil(); }
    void PushNumber(double value) override { lua_->PushNumber(value); }
    void PushLString(const char* s, int length) override { lua_->PushLString(s, length); }
    void PushString(const char* s) override { lua_->PushString(s); }
    void PushCClosure(CFunction fn, int upvalues) override { lua_->PushCClosure(WF(fn), upvalues); }
    void PushUserTag(void* p, int tag) override { lua_->PushUserTag(p, tag); }
    void PushBoolean(bool value) override { lua_->PushBoolean(value); }
    void GetGlobal(const char* name) override { lua_->GetGlobal(name); }
    void GetTable(int index) override { lua_->GetTable(index); }
    void RawGet(int index) override { lua_->RawGet(index); }
    void RawGetI(int index, int n) override { lua_->RawGetI(index, n); }
    void GetGlobals() override { lua_->GetGlobals(); }
    void GetTagMethod(int tag, const char* event) override { lua_->GetTagMethod(tag, event); }
    bool GetRef(int ref) override { lua_->GetRef(ref); return true; }
    void NewTable() override { lua_->NewTable(); }
    void SetGlobal(const char* name) override { lua_->SetGlobal(name); }
    void SetTable(int index) override { lua_->SetTable(index); }
    void RawSet(int index) override { lua_->RawSet(index); }
    void RawSetI(int index, int n) override { lua_->RawSetI(index, n); }
    void SetTagMethod(int tag, const char* event) override { lua_->SetTagMethod(tag, event); }
    int Ref(int) override { return lua_->RefValue(); }   // the argument is ignored
    int CallProtected(int args, int results) override { return lua_->CallProtected(args, results); }
    void RawCall(int args, int results) override { lua_->RawCall(args, results); }
    int DoFile(const char* path) override { return lua_->DoFile(path); }
    int DoString(const char* code) override { return lua_->DoString(code); }
    int DoBuffer(const char* buffer, int size, const char* name) override { return lua_->DoBuffer(buffer, size, name); }
    int GetGCCount() override { return lua_->GetGCCount(); }
    int GetGCThreshold() override { return lua_->GetGCThreshold(); }
    void SetGCThreshold(int value) override { lua_->SetGCThreshold(value); }
    int NewTag() override { return lua_->NewTagS(); }
    void CopyTagMethods(int from, int to) override { lua_->CopyTagMethods(from, to); }
    void SetTag(int tag) override { lua_->SetTag(tag); }
    void Unref(int ref) override { lua_->Unref(ref); }
    int Next(int index) override { return lua_->Next(index); }
    int GetN(int index) override { return lua_->GetN(index); }
    void Error(const char* message) override { lua_->Error(message); }
    void Concat(int count) override { lua_->Concat(count); }
    void* NewUserData(int size) override { return lua_->NewUserData(size); }
    void Register(CFunction fn, const char* name) override { lua_->Register(WF(fn), name); }
    void PushUserData(void* p, int tag) override { lua_->PushUserTag(p, tag); }   // the same function as PushUserTag
    int CloneTag(int tag) override { return lua_->CloneTag(tag); }
    bool IsFunction(int index) override { return lua_->IsFunction(index); }
    bool IsTable(int index) override { return lua_->IsTable(index); }
    bool IsUserData(int index) override { return lua_->IsUserData(index); }
    bool IsNil(int index) override { return lua_->IsNil(index); }
    bool ToDataRegistryEntryID(cISC4GameDataRegistry*, int) override { return false; }
    bool ToDataRegistryNode(cISC4GameDataRegistry*, int) override { return false; }
    bool ToDataRegistryValue(cISC4GameDataRegistry*, int) override { return false; }

private:
    ~Instance() {
        lua_->Close();
        lua_->Release();
    }

    ThreadFactory factory_;
    Host host_;
    Machine* lua_;
    std::uint32_t refs_ = 1;
};

}  // namespace

cISCLua* CreateSCLua() {
    SetLuaApi(RealLuaApi());
    return new Instance();
}

cISCLua* SCLuaFromState(::lua_State* state) {
    if (!state) return nullptr;
    ILuaApi& api = *RealLuaApi();
    cGZLua5Thread* thread = api.get_thread_slot(api.main_state(W(state)));
    auto* machine = dynamic_cast<Machine*>(thread);
    return machine ? machine->outer : nullptr;
}

}  // namespace lua5
