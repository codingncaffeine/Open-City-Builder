// The scripting host as game code holds it: a cISCLua whose slots forward to a recovered cSCLua running on the real Lua 5.0
// interpreter (lua-host.md 5: slots 6-74 are thin forwards to the main thread). One object per Lua VM, as in the original
// (GZCOM class 0xea10c4a2).
// The game-data registry bridge mirrors the registry as the global `sc4game` (see SCLuaInstance.cpp).
//   PARTIAL: the three ToDataRegistry* slots answer false (no caller in the port).
#pragma once
#include "lua/ISCLua.h"

class cIGZUnknown;
namespace sc4 { class cISC4GameDataRegistry; }

namespace lua5 {

// The registry every scripting host opened from now on mirrors as `sc4game` (borrowed; null = no mirror).
void SetGameDataRegistry(sc4::cISC4GameDataRegistry* registry);
// A new, unopened scripting host with one reference for the caller.
cISCLua* CreateSCLua();
// The same host as a GZCOM class object with NO reference yet (cRZCOMDllGenericClassFactoryT: the factory's
// QueryInterface takes the first). Its cIGZUnknown view; QueryInterface(0xea10c4a2) gives the cISCLua view.
cIGZUnknown* MakeSCLuaObject();
// The host that owns `state` (any of its threads), or null: how a Lua C function finds its cISCLua (lua-host.md 1.3). Borrowed.
cISCLua* SCLuaFromState(::lua_State* state);

}  // namespace lua5
