// lua5::ILuaApi on the real Lua 5.0 interpreter (third_party/lua-5.0.2, built as C with LUA_USERSTATE = void*, which
// reserves the pointer just before every lua_State that the original's patched Lua keeps there: the state's thread object).
// The recovered wrappers (cGZLua5Thread, cGZLua5, cSCLua, the thread registry and the Lua 4 compatibility library) only
// ever see the opaque lua5::lua_State; this file is the one place that knows it is the interpreter's own lua_State.
#include "lua/RealLuaApi.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

namespace lua5 {

int RealLuaCcalls(::lua_State* L);             // RealLuaInternals.cpp
::lua_State* RealLuaMainThread(::lua_State* L);

namespace {

inline ::lua_State* R(lua_State* L) { return reinterpret_cast<::lua_State*>(L); }
inline lua_State* W(::lua_State* L) { return reinterpret_cast<lua_State*>(L); }
inline cGZLua5Thread*& Slot(::lua_State* L) { return reinterpret_cast<cGZLua5Thread**>(L)[-1]; }

class RealApi final : public ILuaApi {
public:
    int gettop(lua_State* L) override { return lua_gettop(R(L)); }
    void settop(lua_State* L, int i) override { lua_settop(R(L), i); }
    void pushvalue(lua_State* L, int i) override { lua_pushvalue(R(L), i); }
    void remove(lua_State* L, int i) override { lua_remove(R(L), i); }
    void insert(lua_State* L, int i) override { lua_insert(R(L), i); }
    void replace(lua_State* L, int i) override { lua_replace(R(L), i); }
    void xmove(lua_State* from, lua_State* to, int n) override { lua_xmove(R(from), R(to), n); }
    int checkstack(lua_State* L, int n) override { return lua_checkstack(R(L), n); }
    int type(lua_State* L, int i) override { return lua_type(R(L), i); }
    const char* type_name(lua_State* L, int tp) override { return lua_typename(R(L), tp); }
    int isnumber(lua_State* L, int i) override { return lua_isnumber(R(L), i); }
    int isstring(lua_State* L, int i) override { return lua_isstring(R(L), i); }
    int iscfunction(lua_State* L, int i) override { return lua_iscfunction(R(L), i); }
    int isuserdata(lua_State* L, int i) override { return lua_isuserdata(R(L), i); }
    int equal(lua_State* L, int a, int b) override { return lua_equal(R(L), a, b); }
    int rawequal(lua_State* L, int a, int b) override { return lua_rawequal(R(L), a, b); }
    int lessthan(lua_State* L, int a, int b) override { return lua_lessthan(R(L), a, b); }
    double tonumber(lua_State* L, int i) override { return lua_tonumber(R(L), i); }
    int toboolean(lua_State* L, int i) override { return lua_toboolean(R(L), i); }
    const char* tostring(lua_State* L, int i) override { return lua_tostring(R(L), i); }
    std::size_t strlen(lua_State* L, int i) override { return lua_strlen(R(L), i); }
    lua_CFunction tocfunction(lua_State* L, int i) override { return reinterpret_cast<lua_CFunction>(lua_tocfunction(R(L), i)); }
    void* touserdata(lua_State* L, int i) override { return lua_touserdata(R(L), i); }
    lua_State* tothread(lua_State* L, int i) override { return W(lua_tothread(R(L), i)); }
    void pushnil(lua_State* L) override { lua_pushnil(R(L)); }
    void pushnumber(lua_State* L, double n) override { lua_pushnumber(R(L), n); }
    void pushlstring(lua_State* L, const char* s, std::size_t n) override { lua_pushlstring(R(L), s, n); }
    void pushstring(lua_State* L, const char* s) override { lua_pushstring(R(L), s); }
    void pushcclosure(lua_State* L, lua_CFunction f, int n) override { lua_pushcclosure(R(L), reinterpret_cast<::lua_CFunction>(f), n); }
    void pushboolean(lua_State* L, int b) override { lua_pushboolean(R(L), b); }
    void pushlightuserdata(lua_State* L, void* p) override { lua_pushlightuserdata(R(L), p); }
    void* newuserdata(lua_State* L, std::size_t n) override { return lua_newuserdata(R(L), n); }
    void gettable(lua_State* L, int i) override { lua_gettable(R(L), i); }
    void rawget(lua_State* L, int i) override { lua_rawget(R(L), i); }
    void rawgeti(lua_State* L, int i, int n) override { lua_rawgeti(R(L), i, n); }
    void newtable(lua_State* L) override { lua_newtable(R(L)); }
    void getfenv(lua_State* L, int i) override { lua_getfenv(R(L), i); }
    int getmetatable(lua_State* L, int i) override { return lua_getmetatable(R(L), i); }
    void settable(lua_State* L, int i) override { lua_settable(R(L), i); }
    void rawset(lua_State* L, int i) override { lua_rawset(R(L), i); }
    void rawseti(lua_State* L, int i, int n) override { lua_rawseti(R(L), i, n); }
    int setfenv(lua_State* L, int i) override { return lua_setfenv(R(L), i); }
    int setmetatable(lua_State* L, int i) override { return lua_setmetatable(R(L), i); }
    void call(lua_State* L, int nargs, int nresults) override { lua_call(R(L), nargs, nresults); }
    int pcall(lua_State* L, int nargs, int nresults, int errfunc) override { return lua_pcall(R(L), nargs, nresults, errfunc); }
    int dofile(lua_State* L, const char* file) override { return lua_dofile(R(L), file); }
    int dostring(lua_State* L, const char* s) override { return lua_dostring(R(L), s); }
    int dobuffer(lua_State* L, const char* b, std::size_t n, const char* name) override { return lua_dobuffer(R(L), b, n, name); }
    int load(lua_State* L, lua_Chunkreader reader, void* data, const char* name) override {
        return lua_load(R(L), reinterpret_cast<::lua_Chunkreader>(reader), data, name);
    }
    int loadfile(lua_State* L, const char* file) override { return luaL_loadfile(R(L), file); }
    int loadbuffer(lua_State* L, const char* b, std::size_t n, const char* name) override { return luaL_loadbuffer(R(L), b, n, name); }
    // cISCLua::Error ignores its message: lua_error raises the value on top of the stack (lua-host.md 5, slot 65).
    int error(lua_State* L, const char*) override { return lua_error(R(L)); }
    int next(lua_State* L, int i) override { return lua_next(R(L), i); }
    void concat(lua_State* L, int n) override { lua_concat(R(L), n); }
    int ref(lua_State* L, int t) override { return luaL_ref(R(L), t); }
    void unref(lua_State* L, int t, int r) override { luaL_unref(R(L), t, r); }
    int yield(lua_State* L, int nresults) override { return lua_yield(R(L), nresults); }

    lua_State* open() override {
        ::lua_State* L = lua_open();
        if (L) Slot(L) = nullptr;
        return W(L);
    }
    void close(lua_State* L) override { lua_close(R(L)); }
    void openlib(lua_State* L, int lib) override {
        switch (lib) {
            case 0: luaopen_base(R(L)); break;
            case 1: luaopen_io(R(L)); break;
            case 2: luaopen_string(R(L)); break;
            case 3: luaopen_math(R(L)); break;
            case 4: luaopen_debug(R(L)); break;
            case 5: luaopen_table(R(L)); break;
            default: break;
        }
    }
    int getgccount(lua_State* L) override { return lua_getgccount(R(L)); }
    int getgcthreshold(lua_State* L) override { return lua_getgcthreshold(R(L)); }
    void setgcthreshold(lua_State* L, int n) override { lua_setgcthreshold(R(L), n); }
    lua_State* newthread(lua_State* L) override {
        ::lua_State* thread = lua_newthread(R(L));
        if (thread) Slot(thread) = nullptr;
        return W(thread);
    }
    int resume(lua_State* L, int nargs) override { return lua_resume(R(L), nargs); }
    bool getstack(lua_State* L, int level) override {
        lua_Debug scratch;
        return lua_getstack(R(L), level, &scratch) != 0;
    }
    int ccalls(lua_State* L) override { return RealLuaCcalls(R(L)); }
    lua_State* main_state(lua_State* L) override { return W(RealLuaMainThread(R(L))); }
    cGZLua5Thread* get_thread_slot(lua_State* L) override { return Slot(R(L)); }
    void set_thread_slot(lua_State* L, cGZLua5Thread* thread) override { Slot(R(L)) = thread; }
    void pushfstring(lua_State* L, const char* fmt, int arg) override { lua_pushfstring(R(L), fmt, arg); }
    void lerror(lua_State* L, const char* msg) override { luaL_error(R(L), "%s", msg); }
    void checktype(lua_State* L, int narg, int t) override { luaL_checktype(R(L), narg, t); }
    void typerror(lua_State* L, int narg, const char* tname) override { luaL_typerror(R(L), narg, tname); }
};

}  // namespace

ILuaApi* RealLuaApi() {
    static RealApi api;
    return &api;
}

}  // namespace lua5
