// The two facts the script host reads from inside the interpreter's state (nsGZLua5ThreadLib reads lua_State::nCcalls and
// the global state's main thread directly). Kept apart from RealLuaApi.cpp because the interpreter's internal headers define
// macros (iscfunction, ...) that share their names with the API slots.
extern "C" {
#include "lua.h"
#include "lstate.h"
}

namespace lua5 {

int RealLuaCcalls(::lua_State* L) { return L->nCcalls; }
::lua_State* RealLuaMainThread(::lua_State* L) { return G(L)->mainthread; }

}  // namespace lua5
