// The game-data registry bridge (cSCLua 0x0040b392..0x0040c22a) on the real Lua 5.0 interpreter: a real cSC4GameDataRegistry
// mirrored as `sc4game`, nodes as tables (field "_e"), commands and values as closures, removal and shutdown.
#include <cstdio>
#include <string>

#include "gz/CommandParameterSet.h"
#include "lua/SCLuaInstance.h"
#include "rz/cRZString.h"
#include "rz/cRZVariant.h"
#include "sc4/GameDataRegistry.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {
using sc4::EntryId;
using Registry = sc4::cISC4GameDataRegistry;

struct Node final : cIGZUnknown {
    int refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
};

// What the command saw and answers.
struct Seen {
    int calls = 0;
    std::uint32_t count = 0;
    double number = 0;
    std::string text;
    bool flag = false;
    cIGZUnknown* node = nullptr;
    bool nilWasUnknown = false;
    double value = 7.5;   // the exchanged value
} g_seen;

bool Command(void*, cIGZCommandParameterSet* in, cIGZCommandParameterSet* out, Registry*) {
    ++g_seen.calls;
    g_seen.count = in->GetParameterCount();
    if (g_seen.count >= 5) {
        g_seen.number = (*in)[0].GetValFloat64();
        cRZString text;
        (*in)[1].GetValString(text);
        g_seen.text = text.ToChar();
        g_seen.flag = (*in)[2].GetValBool();
        g_seen.node = (*in)[3].GetValUnknown();
        g_seen.nilWasUnknown = (*in)[4].GetType() == cIGZVariant::kTypeUnknown && !(*in)[4].GetValUnknown();
    }
    out->SetParameter(0, cRZVariant(42.0));
    cRZString answer("yes");
    out->SetParameter(1, cRZVariant(answer));
    cRZVariant truth;
    truth.SetValBool(true);
    out->SetParameter(2, truth);
    out->SetParameter(0x2b999d2b, cRZVariant(99.0));   // the status parameter is not returned
    return true;
}

bool Value(void*, cIGZVariant& value, bool write, Registry*) {
    if (write) g_seen.value = value.GetValFloat64();
    else value.SetValFloat64(g_seen.value);
    return true;
}

std::string Run(cISCLua* lua, const char* code) {
    const int top = lua->GetTop();
    const int status = lua->DoString((std::string("return ") + code).c_str());
    std::string result = status != 0 ? "<error>" : "";
    for (int i = top + 1; status == 0 && i <= lua->GetTop(); ++i) {
        if (i > top + 1) result += ",";
        result += lua->IsString(i) || lua->IsNumber(i) ? lua->ToString(i) : lua->TypeName(lua->Type(i));
    }
    lua->SetTop(top);
    return result;
}
}  // namespace

int main() {
    auto* registry = new sc4::cSC4GameDataRegistry();
    registry->AddRef();
    Node automata, inner;
    int handleA = 0, handleB = 0;
    const EntryId root = registry->GetRootNodeEntry();
    const EntryId automataId = registry->AddChildEntry(&automata, "automata", root, 0, nullptr);
    registry->AddChildEntry(&handleA, "do_it", &Command, automataId, 1, nullptr);
    registry->AddChildEntry(&inner, "inner", automataId, 0, nullptr);
    lua5::SetGameDataRegistry(registry);

    // entries that exist when the VM opens are replayed into it
    cISCLua* lua = lua5::CreateSCLua();
    lua->Open(0x7e);
    CHECK(Run(lua, "type(sc4game)") == "table");
    CHECK(Run(lua, "type(sc4game.automata), type(sc4game.automata._e), type(sc4game.automata.inner)") == "table,userdata,table");
    CHECK(Run(lua, "type(sc4game.automata.do_it)") == "function");

    // a command: arguments as variants (number, string, bool, a node table, nil), results back (status left out)
    CHECK(Run(lua, "(function(a, b, c, d) return a, b, tostring(c), tostring(d) end)(sc4game.automata.do_it(3.5, 'abc', true, sc4game.automata.inner, nil))") ==
          "42,yes,true,nil");
    CHECK(g_seen.calls == 1 && g_seen.count == 5 && g_seen.number == 3.5 && g_seen.text == "abc" && g_seen.flag);
    CHECK(g_seen.node == &inner && g_seen.nilWasUnknown);

    // entries added later appear at once; a value reads with no argument and writes with one
    registry->AddChildEntry(&handleB, "speed", &Value, automataId, 0, nullptr);
    CHECK(Run(lua, "sc4game.automata.speed()") == "7.5");
    Run(lua, "sc4game.automata.speed(12)");
    CHECK(g_seen.value == 12.0);

    // removing a node takes its table out of the parent and its registry slot
    registry->RemoveChildEntry(static_cast<cIGZUnknown*>(&inner));
    CHECK(Run(lua, "type(sc4game.automata.inner)") == "nil");
    registry->RemoveChildEntry(static_cast<cIGZUnknown*>(&automata));
    CHECK(Run(lua, "type(sc4game.automata)") == "nil");

    // the registry's shutdown removes the global
    registry->Shutdown();
    CHECK(Run(lua, "type(sc4game)") == "nil");

    lua->Close();
    lua->Release();
    lua5::SetGameDataRegistry(nullptr);
    registry->Release();
    std::puts(g_fail ? "lua_gdrbridge: FAILED" : "lua_gdrbridge ok");
    return g_fail != 0;
}
