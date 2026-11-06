// The game-data registry on the native application (cSC4GameDataRegistry, the original's system service 0xebe0e860): one per
// process, mirrored into every scripting host as `sc4game`. NativeGameData.cpp.
#pragma once

namespace sc4 { class cISC4GameDataRegistry; }

namespace native {

sc4::cISC4GameDataRegistry* NativeGameDataRegistry();   // created on first use (borrowed)
void NativeRunCityLua(const char* code);                // the kit's `lua CODE`: runs in the city's automata script VM, prints the result
void NativeRegistryReport();                            // the kit's `registry`: the entry tree

}  // namespace native
