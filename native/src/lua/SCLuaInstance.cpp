#include "lua/SCLuaInstance.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include "gz/CommandParameterSet.h"
#include "gz/cIGZUnknown.h"
#include "lua/RealLuaApi.h"
#include "lua/SCLua.h"
#include "rz/cRZString.h"
#include "rz/cRZVariant.h"
#include "sc4/GameDataRegistry.h"

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

// ---- the game-data registry bridge (cSCLua, i386 0x0040b392..0x0040c22a) ----------------------------------------------
// Every registry NODE is a Lua table: the root is the global `sc4game`, the others sit in their parent's table under the entry's
// name (the entry pointer as light userdata when it has none) and carry their entry in field "_e"; Lua's registry maps each
// node entry (light userdata) to its table. Every VALUE / COMMAND entry is a C closure over (registry, entry, cSCLua): a
// command (entry flag 1) calls DoValueCall with the arguments as variants and returns the results; a value exchanges one
// variant (read with no argument, write with one).
using GDRegistry = sc4::cISC4GameDataRegistry;
using sc4::EntryId;
constexpr char kGameDataRootNS[] = "sc4game";   // _kGameDataRootNS (0x005e09fc)
constexpr char kGameDataEntryLink[] = "_e";     // _kGameDataEntryLink (0x005e09f8)
constexpr int kTypeNil = 0, kTypeBoolean = 1, kTypeNumber = 3, kTypeString = 4, kTypeTable = 5;
constexpr std::uint32_t kStatusParameterId = 0x2b999d2b;   // the call's status parameter, not returned to Lua
GDRegistry* g_gameDataRegistry = nullptr;

inline int UpvalueIndex(int i) { return kGlobalsIndex - i; }
inline void* LightUserData(EntryId id) { return reinterpret_cast<void*>(id); }

// A Lua argument as a variant (GDRCallExtention's argument switch; a table is the node its "_e" names).
void ArgumentToVariant(ILuaApi& api, lua_State* L, int index, GDRegistry* registry, cRZVariant& out) {
    switch (api.type(L, index)) {
    case kTypeNil: out.SetValUnknown(nullptr); break;
    case kTypeBoolean: out.SetValBool(api.toboolean(L, index) != 0); break;
    case kTypeNumber: out.SetValFloat64(api.tonumber(L, index)); break;
    case kTypeString: { cRZString text(api.tostring(L, index)); out.SetValString(text); break; }
    case kTypeTable: {
        api.pushstring(L, kGameDataEntryLink);
        api.rawget(L, index < 0 ? index - 1 : index);
        const auto id = reinterpret_cast<EntryId>(api.touserdata(L, -1));
        cIGZUnknown* node = id && registry->IsValidEntry(id) && registry->IsNodeEntry(id) ? registry->GetNode(id) : nullptr;
        out.SetValUnknown(node);
        api.settop(L, -2);
        break;
    }
    default: out.SetValFloat64(0.0); break;
    }
}

// A result variant pushed for Lua: bool, numbers, a node as its table, anything else as its string.
void PushVariant(ILuaApi& api, lua_State* L, cIGZVariant& value, GDRegistry* registry) {
    const std::uint16_t type = value.GetType();
    if (type == cIGZVariant::kTypeBool) {
        api.pushboolean(L, value.GetValBool() ? 1 : 0);
    } else if (type >= 2 && type < 0xf) {
        api.pushnumber(L, value.GetValFloat64());
    } else if (type == cIGZVariant::kTypeUnknown) {
        const EntryId id = registry->GetEntryID(value.GetValUnknown());
        if (id == 0) {
            api.pushnil(L);
        } else {
            api.pushlightuserdata(L, LightUserData(id));
            api.rawget(L, kRegistryIndex);
        }
    } else {
        cRZString text;
        value.GetValString(text);
        api.pushstring(L, text.ToChar());
    }
}

struct Machine;
int GDRCallExtention(lua_State* L);
int GDRExchangeExtention(lua_State* L);

// cSCLua's registry-notification view (+0x80).
class RegistryBridge final : public sc4::cISC4GameDataRegistryNotificationTarget {
public:
    explicit RegistryBridge(Machine* owner) : owner_(owner) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }    // owned by the cSCLua
    std::uint32_t Release() override { return 1; }
    void OnRegistryEvent(GDRegistry* registry, int event, EntryId entry) override;   // 0x0040b54e

private:
    Machine* owner_;
};

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
    void RegisterRegistryListener(cSCLua* lua) override;
    void UnregisterRegistryListener(cSCLua* lua) override;
};

class Instance;
// The recovered host, tied back to the cISCLua object that owns it; it carries the bridge's two parameter sets (+0x98 / +0xa0).
struct Machine final : cSCLua {
    Machine(Instance* outer, IThreadFactory* factory, ISCLuaHost& host)
        : cSCLua(*RealLuaApi(), Compat(), factory, reinterpret_cast<const char*>(kCompatScript), host), outer(outer), bridge(this) {
        callIn.AddRef();
        callOut.AddRef();
    }
    Instance* outer;
    RegistryBridge bridge;
    GDRegistry* registry = nullptr;   // the registry the bridge listens to
    cGZCommandParameterSet callIn, callOut;
};

void Host::RegisterRegistryListener(cSCLua* lua) {
    auto* machine = static_cast<Machine*>(lua);
    if (!g_gameDataRegistry || machine->registry) return;
    machine->registry = g_gameDataRegistry;
    machine->registry->AddRef();
    machine->registry->AddNotificationTarget(&machine->bridge);   // replays every entry as "added"
}

void Host::UnregisterRegistryListener(cSCLua* lua) {
    auto* machine = static_cast<Machine*>(lua);
    if (!machine->registry) return;
    machine->registry->RemoveNotificationTarget(&machine->bridge);
    machine->registry->Release();
    machine->registry = nullptr;
}

// GDRUnregisterNodeVisitor (0x0040b392): a node's registry slot goes.
struct UnregisterContext { ILuaApi* api; lua_State* L; GDRegistry* registry; };
bool UnregisterNodeVisitor(EntryId entry, void* context) {
    auto* c = static_cast<UnregisterContext*>(context);
    if (c->registry->IsNodeEntry(entry)) {
        c->api->pushlightuserdata(c->L, LightUserData(entry));
        c->api->pushnil(c->L);
        c->api->rawset(c->L, kRegistryIndex);
    }
    return true;
}

// The entry's key in its parent's table: its name, or the entry itself when it has none.
void PushEntryKey(ILuaApi& api, lua_State* L, GDRegistry* registry, EntryId entry) {
    const cRZString* name = registry->GetEntryName(entry);
    const char* text = name ? name->ToChar() : nullptr;
    if (!text || !*text) api.pushlightuserdata(L, LightUserData(entry));
    else api.pushstring(L, text);
}

void RegistryBridge::OnRegistryEvent(GDRegistry* registry, int event, EntryId entry) {
    ILuaApi& api = *RealLuaApi();
    lua_State* L = owner_->GetContext();
    if (!L || !registry) return;
    const int top = api.gettop(L);
    const EntryId root = registry->GetRootNodeEntry();
    const bool shutdown = event == sc4::kRegistryShutdown || (event == sc4::kRegistryEntryRemoved && entry == root);
    if (shutdown) {
        api.pushstring(L, kGameDataRootNS);
        api.pushnil(L);
        api.rawset(L, kGlobalsIndex);
        UnregisterContext context{&api, L, registry};
        registry->VisitBreadthFirst(root, &UnregisterNodeVisitor, &context);
        api.setgcthreshold(L, 0);
    } else if (event == sc4::kRegistryEntryAdded && entry == root) {
        api.newtable(L);
        api.pushstring(L, kGameDataRootNS);
        api.pushvalue(L, -2);
        api.rawset(L, kGlobalsIndex);
        api.pushlightuserdata(L, LightUserData(entry));
        api.pushvalue(L, -2);
        api.rawset(L, kRegistryIndex);
    } else if (event == sc4::kRegistryEntryAdded || event == sc4::kRegistryEntryRemoved) {
        api.pushlightuserdata(L, LightUserData(registry->GetParentNodeEntry(entry)));
        api.rawget(L, kRegistryIndex);
        if (api.type(L, -1) == kTypeTable) {
            PushEntryKey(api, L, registry, entry);
            if (event == sc4::kRegistryEntryRemoved) {
                api.pushnil(L);
                api.rawset(L, -3);
                if (registry->IsNodeEntry(entry)) {
                    UnregisterContext context{&api, L, registry};
                    registry->VisitBreadthFirst(entry, &UnregisterNodeVisitor, &context);
                }
            } else if (registry->IsNodeEntry(entry)) {
                api.newtable(L);
                api.pushstring(L, kGameDataEntryLink);
                api.pushlightuserdata(L, LightUserData(entry));
                api.rawset(L, -3);
                api.pushlightuserdata(L, LightUserData(entry));
                api.pushvalue(L, -2);
                api.rawset(L, kRegistryIndex);
                api.rawset(L, -3);
            } else {
                api.pushlightuserdata(L, registry);
                api.pushlightuserdata(L, LightUserData(entry));
                api.pushlightuserdata(L, owner_);
                api.pushcclosure(L, registry->CheckEntryFlags(entry, 1) ? &GDRCallExtention : &GDRExchangeExtention, 3);
                api.rawset(L, -3);
            }
        }
    }
    api.settop(L, top);
}

// 0x0040b9fa: a registry command called from Lua.
int GDRCallExtention(lua_State* L) {
    ILuaApi& api = *RealLuaApi();
    auto* registry = static_cast<GDRegistry*>(api.touserdata(L, UpvalueIndex(1)));
    const auto entry = reinterpret_cast<EntryId>(api.touserdata(L, UpvalueIndex(2)));
    auto* machine = static_cast<Machine*>(api.touserdata(L, UpvalueIndex(3)));
    if (!registry || !machine || !registry->IsValidEntry(entry)) return 0;
    machine->callIn.SetParameterCount(0);
    machine->callOut.SetParameterCount(0);
    const int count = api.gettop(L);
    for (int i = 1; i <= count; ++i) {
        cRZVariant value;
        ArgumentToVariant(api, L, i, registry, value);
        machine->callIn.SetParameter(static_cast<std::uint32_t>(i - 1), value);
    }
    registry->DoValueCall(entry, &machine->callIn, &machine->callOut);
    int pushed = 0;
    for (std::uint32_t id = machine->callOut.GetFirstParameterID(); id != 0xffffffffu; id = machine->callOut.GetNextParameterID(id)) {
        if (id == kStatusParameterId) continue;
        cIGZVariant* value = machine->callOut.GetParameter(id);
        if (!value) api.pushnil(L);
        else PushVariant(api, L, *value, registry);
        ++pushed;
    }
    return pushed;
}

// 0x0040be78: a registry value read (no argument) or written (one argument) from Lua.
int GDRExchangeExtention(lua_State* L) {
    ILuaApi& api = *RealLuaApi();
    auto* registry = static_cast<GDRegistry*>(api.touserdata(L, UpvalueIndex(1)));
    const auto entry = reinterpret_cast<EntryId>(api.touserdata(L, UpvalueIndex(2)));
    if (!registry || !registry->IsValidEntry(entry)) return 1;
    const bool write = api.gettop(L) > 0;
    cRZVariant value;
    if (write) ArgumentToVariant(api, L, 1, registry, value);
    registry->DoValueExchange(entry, value, write);
    if (!write) PushVariant(api, L, value, registry);
    return 1;
}

// The original's cSCLua is one object whose single vtable starts with QueryInterface / AddRef / Release, so its cISCLua
// pointer also serves as its cIGZUnknown. Here the two are separate C++ bases sharing those three methods: GZCOM callers
// get the cIGZUnknown view (iid 1) and reach the scripting interface with QueryInterface(0xea10c4a2).
class Instance final : public cISCLua, public cIGZUnknown {
public:
    explicit Instance(std::uint32_t refs = 1) : lua_(new Machine(this, &factory_, host_)), refs_(refs) {
        lua_->AddRef();
        lua_->SetErrorReporter(ReportError, false);
    }

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (!out || (iid != 1 && iid != kIID_GameDataRegistryListener)) return false;
        if (iid == 1) *out = static_cast<cIGZUnknown*>(this);
        else *out = static_cast<cISCLua*>(this);
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
    bool ToDataRegistryEntryID(::cISC4GameDataRegistry*, int) override { return false; }
    bool ToDataRegistryNode(::cISC4GameDataRegistry*, int) override { return false; }
    bool ToDataRegistryValue(::cISC4GameDataRegistry*, int) override { return false; }

private:
    ~Instance() {
        lua_->Close();
        lua_->Release();
    }

    ThreadFactory factory_;
    Host host_;
    Machine* lua_;
    std::uint32_t refs_;
};

}  // namespace

void SetGameDataRegistry(sc4::cISC4GameDataRegistry* registry) { g_gameDataRegistry = registry; }

cISCLua* CreateSCLua() {
    SetLuaApi(RealLuaApi());
    return new Instance();
}

cIGZUnknown* MakeSCLuaObject() {
    SetLuaApi(RealLuaApi());
    return static_cast<cIGZUnknown*>(new Instance(0));
}

cISCLua* SCLuaFromState(::lua_State* state) {
    if (!state) return nullptr;
    ILuaApi& api = *RealLuaApi();
    cGZLua5Thread* thread = api.get_thread_slot(api.main_state(W(state)));
    auto* machine = dynamic_cast<Machine*>(thread);
    return machine ? machine->outer : nullptr;
}

}  // namespace lua5
