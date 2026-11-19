// Tests the Lua host stack (cGZLua5, ThreadRegistry, thread library, Lua-4 compatibility, cSCLua) on MiniLua.
#include <cstdio>

#include "lua/SCLua.h"
#include "lua_minilua.h"

using namespace lua5;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static int g_alerts = 0;
static int fnAlert(lua_State*) { ++g_alerts; return 0; }
static int g_reports = 0, g_reportStatus = 0;
static void reporter(cGZLua5Thread*, int status, const char*) { ++g_reports; g_reportStatus = status; }

struct Factory : IThreadFactory {
    ILuaApi& api; ILuaCompat& compat; int created = 0;
    Factory(ILuaApi& a, ILuaCompat& c) : api(a), compat(c) {}
    cGZLua5Thread* CreateThreadObject(unsigned) override { ++created; auto* t = new cGZLua5Thread(api, compat); t->AddRef(); return t; }
};

struct ParamSet : ICommandParameterSet {
    int refs = 0; static int live;
    ParamSet() { ++live; }
    void AddRef() override { ++refs; }
    void Release() override { if (--refs <= 0) { --live; delete this; } }
};
int ParamSet::live = 0;
struct Host : ISCLuaHost {
    int registered = 0, unregistered = 0;
    ICommandParameterSet* NewParameterSet() override { return new ParamSet; }
    void RegisterRegistryListener(cSCLua*) override { ++registered; }
    void UnregisterRegistryListener(cSCLua*) override { ++unregistered; }
};

// Helpers calling a global Lua function with a prepared stack.
static std::string Str(const char* s) { return s ? s : "<null>"; }

int main() {
    MiniLua api;
    SetLuaApi(&api);
    LuaCompat compat(fnAlert);
    Factory factory(api, compat);

    // ---- cGZLua5 open / threads ------------------------------------------------------------------------
    auto* lua = new cGZLua5(api, compat, &factory, "-- compat script");
    lua->AddRef();
    CHECK(!lua->Close() == false);                       // closing an unopened VM is fine
    CHECK(lua->Open(kOpenString | kOpenCompat4));
    CHECK((api.libs == std::vector<int>{0, 2}));
    CHECK(api.dostrings.size() == 1 && api.dostrings[0] == "-- compat script");
    CHECK(!lua->Open(0));                                // already open
    CHECK(lua->GetTop() == 0);
    CHECK(lua->GetID() == 0 && lua->GetThread(0) == lua && lua->GetThread(5) == nullptr);
    CHECK(lua->GetContext() != nullptr);
    CHECK(lua->IsValid());
    CHECK(lua->GetGCCount() == 42 && lua->GetGCThreshold() == 100);
    lua->SetGCThreshold(7); CHECK(api.gcThreshold == 7);
    lua_State* L = lua->GetContext();
    // library registration
    lua->GetGlobal("tag"); CHECK(lua->IsFunction(-1)); lua->Pop(1);
    lua->GetGlobal("globals"); CHECK(lua->IsCFunction(-1)); lua->Pop(1);
    lua->GetGlobal("coroutine"); CHECK(lua->IsTable(-1));
    lua->PushString("resume"); lua->RawGet(-2); CHECK(lua->IsFunction(-1)); lua->SetTop(0);
    lua->GetGlobalTable(); lua->PushString("_ALERT"); lua->RawGet(-2); CHECK(lua->IsCFunction(-1)); lua->SetTop(0);

    // error reporter selection
    CHECK(lua->GetErrorReporter() == &cGZLua5::DefaultErrorReporter);
    lua->SetErrorReporter(reporter, false); CHECK(lua->GetErrorReporter() == reporter);
    lua->SetErrorReporter(nullptr, false); CHECK(lua->GetErrorReporter() == nullptr);
    lua->SetErrorReporter(nullptr, true); CHECK(lua->GetErrorReporter() == &cGZLua5::DefaultErrorReporter);
    lua->SetErrorReporter(reporter, true); CHECK(lua->GetErrorReporter() == reporter);

    // ---- Lua 4 compatibility --------------------------------------------------------------------------
    // NewTag reads the result from stack index 1, so it must run on an empty stack (as in the game).
    lua->GetGlobal("newtag"); CHECK(lua->CallProtected(0, 1, 0, true) == 0);
    CHECK(lua->ToSint32(-1) == 1);
    lua->Pop(1);
    CHECK(lua->GetTop() == 0);
    CHECK(lua->NewTag() == 2);
    int t3 = lua->NewTag(), t4 = lua->NewTag(), t5 = lua->NewTag();
    CHECK(t3 == 3 && t4 == 4 && t5 == 5);

    lua->PushNewTable();                                       // object at index 1
    CHECK(lua->Tag(1) == -1);                                  // untagged object
    lua->PushValue(1); lua->SetTag(2);                         // lua_SetTag: tag the copy (same table)
    lua->SetTop(1);
    CHECK(lua->Tag(1) == 2);
    lua->GetGlobal("tag"); lua->PushValue(1); CHECK(lua->CallProtected(1, 1, 0, true) == 0);
    CHECK(lua->ToSint32(-1) == 2); lua->SetTop(1);

    // settagmethod / gettagmethod round trip
    lua->PushString("handler");
    lua->SetTagMethod(2, "gettable");
    CHECK(lua->GetTop() == 1);
    lua->GetTagMethod(2, "gettable"); CHECK(Str(lua->ToString(-1)) == "handler"); lua->Pop(1);
    lua->GetTagMethod(2, "index"); CHECK(Str(lua->ToString(-1)) == "handler"); lua->Pop(1);  // same __index slot
    lua->GetTagMethod(2, "bogus"); CHECK(lua->IsNil(-1)); lua->Pop(1);
    lua->GetTagMethod(99, "index"); CHECK(lua->IsNil(-1)); lua->Pop(1);
    CHECK(Str(compat4::Lua4EventToMetaMethod("function")) == "__call");
    CHECK(Str(compat4::Lua4EventToMetaMethod("settable")) == "__newindex");
    CHECK(compat4::Lua4EventToMetaMethod("nope") == nullptr && compat4::Lua4EventToMetaMethod(nullptr) == nullptr);
    CHECK(lua->CopyTagMethods(t5, 2) == t5);
    lua->GetTagMethod(t5, "index"); CHECK(Str(lua->ToString(-1)) == "handler"); lua->Pop(1);

    // user tags
    int target = 77;
    lua->PushUserTag(&target, 2);
    CHECK(lua->IsUserData(-1) && lua->Tag(-1) == 2);
    CHECK(lua->ToUserTag(-1) == reinterpret_cast<std::uint64_t>(&target));
    lua->SetTop(0);
    CHECK(lua->CloneTag(2) == 6);                              // new tag 6 with tag 2's methods
    lua->GetTagMethod(6, "index"); CHECK(Str(lua->ToString(-1)) == "handler");
    lua->SetTop(0);

    // ---- thread library -------------------------------------------------------------------------------
    int bodyRuns = 0;
    {
        // create(fn)  -> id 1
        api.pushnumber(L, 0); api.settop(L, 0);
        L->stack.push_back(api.MakeLuaFn([&](lua_State* T) { ++bodyRuns; api.pushnumber(T, 5); return api.yield(T, 1); }));
        CHECK(threadlib::Create(L) == 1);
        CHECK(lua->IsNumber(-1) && lua->ToSint32(-1) == 1);
        CHECK(factory.created == 1);
        lua->SetTop(0);
        CHECK(lua->GetRegistry()->Count() == 2);
        CHECK(lua->GetRegistry()->IsValidThread(1));
        CHECK(lua->GetThread(1) != nullptr && lua->GetThread(1)->GetID() == 1);
        CHECK(lua->GetRegistry()->GetThreadStatus(1) == 1);   // fresh coroutine still holds its body: suspended
    }
    // status of the fresh thread: stack holds the function so gettop != 0 -> suspended (1)
    CHECK(lua->GetThread(1)->Status() == 1);
    // resume(id): runs the body, which yields
    api.settop(L, 0);
    api.pushnumber(L, 1);
    CHECK(threadlib::Resume(L) == 2);                        // true, yielded value
    CHECK(bodyRuns == 1);
    CHECK(lua->ToBoolean(-2) && lua->ToNumber(-1) == 5);
    api.settop(L, 0);
    // ID() on the main thread, Valid, Status names, Class, Parent
    CHECK(threadlib::ID(L) == 1 && lua->ToSint32(-1) == 0); api.settop(L, 0);
    api.pushnumber(L, 1); CHECK(threadlib::Valid(L) == 1 && lua->ToBoolean(-1)); api.settop(L, 0);
    api.pushnumber(L, 99); CHECK(threadlib::Valid(L) == 1 && !lua->ToBoolean(-1)); api.settop(L, 0);
    api.pushnumber(L, 0); threadlib::Status(L); CHECK(Str(lua->ToString(-1)) == "running"); api.settop(L, 0);
    api.pushnumber(L, 99); threadlib::Status(L); CHECK(Str(lua->ToString(-1)) == "invalid"); api.settop(L, 0);
    api.pushnumber(L, 1); threadlib::Class(L); CHECK(lua->ToSint32(-1) == static_cast<int>(kDefaultThreadClassId)); api.settop(L, 0);
    api.pushnumber(L, 1); threadlib::Parent(L); CHECK(lua->ToSint32(-1) == -1); api.settop(L, 0);
    api.pushnumber(L, 7); CHECK(threadlib::Resume(L) == 2 && !lua->ToBoolean(-2)); // bad id -> false + message
    CHECK(Str(lua->ToString(-1)) == "Resuming Lua thread with invalid ID: 7."); api.settop(L, 0);
    // destroy errors
    api.pushnumber(L, 0); CHECK(threadlib::Destroy(L) == 2);
    CHECK(!lua->ToBoolean(-2) && Str(lua->ToString(-1)) == "Destruction of the main Lua thread is disallowed."); api.settop(L, 0);
    api.pushnumber(L, 42); CHECK(threadlib::Destroy(L) == 2);
    CHECK(Str(lua->ToString(-1)) == "Bad Lua thread ID: 42"); api.settop(L, 0);
    // destroy thread 1 from the main thread
    api.pushnumber(L, 1); CHECK(threadlib::Destroy(L) == 1 && lua->ToBoolean(-1)); api.settop(L, 0);
    CHECK(lua->GetRegistry()->Count() == 1 && lua->GetThread(1) == nullptr);

    // create with flags: 'P' (child), 'R' (parent resumes), 'L' (keep alive); class id quirk reads argument 2
    L->stack.push_back(api.MakeLuaFn([](lua_State*) { return 0; }));
    api.pushstring(L, "PRL");
    CHECK(threadlib::Create(L) == 1);
    int childId = lua->ToSint32(-1); CHECK(childId == 2);   // ids are never reused
    api.settop(L, 0);
    const ThreadRegistry::Thread* ct = lua->GetRegistry()->Find(childId);
    CHECK(ct && ct->flags == (kThreadFlagChild | kThreadFlagParentResumes | kThreadFlagKeepAlive) && ct->parent == L);
    CHECK(lua->GetRegistry()->GetParentID(childId) == 0);
    api.pushnumber(L, childId); threadlib::Parent(L); CHECK(lua->ToSint32(-1) == 0); api.settop(L, 0);
    // only the parent may resume it; a different thread cannot
    {
        cGZLua5Thread* other = nullptr;
        L->stack.push_back(api.MakeLuaFn([](lua_State*) { return 0; }));
        int oid = lua->CreateThread(&other, 0, kDefaultThreadClassId);
        CHECK(oid == 3 && other != nullptr && other->GetID() == 3);
        lua_State* OS = other->GetContext();
        CHECK(lua->GetRegistry()->ResumeThread(OS, childId, 0) == 1);
        CHECK(Str(api.tostring(OS, -1)) == "Lua thread 2 is only resumable by its parent.");
        other->Release();
        lua->DestroyThread(oid);
        api.settop(L, 0);
    }
    // bad first parameter -> error value, id -1 not produced because lerror throws
    api.pushnumber(L, 3);
    bool threw = false;
    try { threadlib::Create(L); } catch (LuaError&) { threw = true; }
    CHECK(threw); api.settop(L, 0);

    // destroying the parent destroys the child first
    int destroyedBefore = lua->GetRegistry()->Count();
    CHECK(destroyedBefore >= 2);
    lua->DestroyThread(childId);
    CHECK(lua->GetThread(childId) == nullptr);

    // ---- close -----------------------------------------------------------------------------------------
    CHECK(lua->Close());
    CHECK(api.closed.size() == 1 && api.closed[0] == L);
    CHECK(lua->GetContext() == nullptr && lua->GetID() == -1 && !lua->IsValid());
    CHECK(lua->QueryInterface(kIID_cIGZLua5) == LuaInterface::Lua5);
    CHECK(lua->QueryInterface(kIID_cIGZLua5Thread) == LuaInterface::Thread);
    CHECK(lua->QueryInterface(0x1234) == LuaInterface::None);
    lua->Release(); lua->Release(); lua->Release();
    // (remaining references are released by the QueryInterface calls above)

    // ---- cSCLua ------------------------------------------------------------------------------------------
    {
        MiniLua api2; SetLuaApi(&api2);
        Factory f2(api2, compat); Host host;
        auto* sc = new cSCLua(api2, compat, &f2, "x", host);
        sc->AddRef();
        CHECK(!sc->IsOpen() && sc->Close());
        CHECK(sc->Open(kOpenCompat4));
        CHECK(sc->IsOpen() && host.registered == 1 && ParamSet::live == 2);
        CHECK(sc->CallParameters() && sc->ResultParameters() && sc->CallParameters() != sc->ResultParameters());
        CHECK(sc->Open(0) && host.registered == 1);            // second open is a no-op returning true
        CHECK(sc->QueryInterface(kIID_GameDataRegistryListener) == LuaInterface::GameDataListener);
        CHECK(sc->QueryInterface(kIID_cIGZLua5) == LuaInterface::Lua5);
        // GetN: global "getn" is called with the table; a non-table gives 0
        sc->PushNewTable();
        CHECK(sc->GetN(1) == 0);                                // no getn function installed
        Val getn = api2.MakeLuaFn([&](lua_State* T) { api2.pushnumber(T, 3.9); return 1; });
        MiniLua::Set(*sc->GetContext()->globals, MiniLua::Str("getn"), getn);
        CHECK(sc->GetN(1) == 3);
        sc->PushNumber(4); CHECK(sc->GetN(2) == 0);
        sc->SetTop(0);
        CHECK(sc->Close() && !sc->IsOpen() && host.unregistered == 1 && ParamSet::live == 0);
        CHECK(sc->CallParameters() == nullptr);
        sc->Release(); sc->Release(); sc->Release();  // plus the two QueryInterface references
        SetLuaApi(&api);
    }
    if (!g_fail) std::printf("lua_luasystem ok\n");
    return g_fail != 0;
}
