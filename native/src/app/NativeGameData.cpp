// The game-data registry (see NativeGameData.h).
//   Port: the registry is not joined to a framework as a system service (nothing looks it up there natively); the hosts that
//   ask for it get this one.
#include "app/NativeGameData.h"

#include <cstdio>
#include <string>

#include "app/NativeCity.h"
#include "lua/ISCLua.h"
#include "lua/SCLuaInstance.h"
#include "sc4/GameDataRegistry.h"
#include "sc4/cSC4City.h"

namespace native {
namespace {
using sc4::EntryId;
using GDRegistry = sc4::cISC4GameDataRegistry;

struct DumpContext { GDRegistry* registry; int shown; };
bool DumpEntry(EntryId entry, void* context) {
    auto* c = static_cast<DumpContext*>(context);
    int depth = 0;
    for (EntryId e = c->registry->GetParentNodeEntry(entry); e != 0; e = c->registry->GetParentNodeEntry(e)) ++depth;
    const cRZString* name = c->registry->GetEntryName(entry);
    std::printf("  %*s%s%s\n", depth * 2, "", name ? name->ToChar() : "?",
                c->registry->IsNodeEntry(entry) ? "/" : (c->registry->CheckEntryFlags(entry, 1) ? "()" : ""));
    ++c->shown;
    return true;
}
}  // namespace

sc4::cISC4GameDataRegistry* NativeGameDataRegistry() {
    static sc4::cSC4GameDataRegistry* registry = [] {
        auto* r = new sc4::cSC4GameDataRegistry();
        r->AddRef();
        lua5::SetGameDataRegistry(r);
        return r;
    }();
    return registry;
}

void NativeRunCityLua(const char* code) {
    // A fresh scripting host, opened like the game's (it mirrors the registry as `sc4game` when it opens); the city's
    // automata VM itself only lives while _config.lua runs.
    NativeGameDataRegistry();
    cISCLua* lua = lua5::CreateSCLua();
    if (!lua) { std::printf("lua: no scripting host\n"); return; }
    lua->Open(0x7e);   // the libraries the game opens (kLuaOpenAll)
    const int top = lua->GetTop();
    const std::string chunk = std::string("return ") + code;
    int status = lua->DoString(chunk.c_str());
    if (status != 0) { lua->SetTop(top); status = lua->DoString(code); }
    std::printf("lua: %s -> status %d:", code, status);
    for (int i = top + 1; i <= lua->GetTop(); ++i) {
        const char* text = lua->IsString(i) || lua->IsNumber(i) ? lua->ToString(i) : lua->TypeName(lua->Type(i));
        std::printf(" %s", text ? text : "?");
    }
    std::printf("\n");
    lua->Close();
    lua->Release();
}

void NativeRegistryReport() {
    GDRegistry* registry = NativeGameDataRegistry();
    DumpContext context{registry, 0};
    std::printf("registry:\n");
    registry->VisitBreadthFirst(registry->GetRootNodeEntry(), &DumpEntry, &context);
    std::printf("registry: %d entries\n", context.shown);
}

}  // namespace native
