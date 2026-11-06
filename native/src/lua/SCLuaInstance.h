// The scripting host as game code holds it: a cISCLua whose slots forward to a recovered cSCLua running on the real Lua 5.0
// interpreter (lua-host.md 5: slots 6-74 are thin forwards to the main thread). One object per Lua VM, as in the original
// (GZCOM class 0xea10c4a2).
//   PARTIAL: the game-data registry bridge (the `sc4game` mirror, ToDataRegistry*) is not recovered; the three slots answer
//   false and no registry listener is registered.
#pragma once
#include "lua/ISCLua.h"

namespace lua5 {

// A new, unopened scripting host with one reference for the caller.
cISCLua* CreateSCLua();
// The host that owns `state` (any of its threads), or null: how a Lua C function finds its cISCLua (lua-host.md 1.3). Borrowed.
cISCLua* SCLuaFromState(::lua_State* state);

}  // namespace lua5
