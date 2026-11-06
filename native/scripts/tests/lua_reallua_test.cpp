// Tests the scripting host on the real Lua 5.0 interpreter (lua5::CreateSCLua) and the package script loader
// (SCLuaHelp::LoadPackageScripts / LoadResourceScript / _dofile_from_resource / print) against lua-host.md 3, 5, 7 and 8.4.
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "lua/SCLuaInstance.h"
#include "lua/SCLuaScripts.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {

constexpr std::uint32_t kGroup = 0x4a5e8f3f, kOtherGroup = 0x4a5e8ef6;

struct Resources final : SCLuaHelp::IScriptResources {
    std::vector<std::pair<cGZPersistResourceKey, std::string>> scripts;   // in list order
    void Add(std::uint32_t group, const char* name, const std::string& text) { scripts.emplace_back(SCLuaHelp::ScriptKey(group, name), text); }
    void ListScriptKeys(std::vector<cGZPersistResourceKey>& out) override {
        for (const auto& script : scripts) out.push_back(script.first);
    }
    bool ReadScript(const cGZPersistResourceKey& key, std::vector<char>& text) override {
        for (const auto& script : scripts) {
            if (!(script.first == key)) continue;
            text.assign(script.second.begin(), script.second.end());
            return true;
        }
        return false;
    }
};

std::string GlobalString(cISCLua* lua, const char* name) {
    lua->GetGlobal(name);
    const char* text = lua->IsString(-1) ? lua->ToString(-1) : nullptr;
    std::string result = text ? text : "<none>";
    lua->Pop(1);
    return result;
}
double GlobalNumber(cISCLua* lua, const char* name) {
    lua->GetGlobal(name);
    const double result = lua->IsNumber(-1) ? lua->ToNumber(-1) : -12345.0;
    lua->Pop(1);
    return result;
}
bool GlobalIsNil(cISCLua* lua, const char* name) {
    lua->GetGlobal(name);
    const bool result = lua->IsNil(-1);
    lua->Pop(1);
    return result;
}

cISCLua* NewOpenLua() {
    cISCLua* lua = lua5::CreateSCLua();
    CHECK(lua->Open(0x7e));
    lua->Register(SCLuaHelp::LUAExtension_print, "print");
    lua->Register(SCLuaHelp::LUAExtension_dofile_from_resource, "_dofile_from_resource");
    return lua;
}

void TestKeys() {
    CHECK(SCLuaHelp::ScriptBaseName("C:\\x\\advisors\\Taxi.lua") == "Taxi");
    CHECK(SCLuaHelp::ScriptBaseName("adv_const.lua") == "adv_const");
    CHECK(SCLuaHelp::ScriptBaseName("dir/sub/_config.lua") == "_config");
    CHECK(SCLuaHelp::ScriptBaseName("noextension") == "noextension");
    const cGZPersistResourceKey key = SCLuaHelp::ScriptKey(kGroup, "_package_startup.lua");
    CHECK(key.type == 0xca63e2a3 && key.group == kGroup);
    CHECK(key.instance == 0xff823a6d);                                            // the shipped automata start-up script
    CHECK(SCLuaHelp::ScriptKey(kGroup, "x/_package_startup.txt").instance == key.instance);   // directory and extension dropped
    CHECK(SCLuaHelp::ScriptKey(kGroup, "_Package_Startup.lua").instance != key.instance);     // case-sensitive
}

void TestHost() {
    cISCLua* lua = NewOpenLua();
    CHECK(lua->Open(0x7e));                      // already open: the stored flag
    CHECK(lua->GetTop() == 0);
    CHECK(lua->AsIGZLua5() != nullptr);

    // the Lua 4 names of the compatibility script and the tag emulation
    CHECK(lua->DoString("x = getn({1,2,3}) y = strsub('hello', 2, 3) z = tinsert ~= nil and foreach ~= nil") == 0);
    CHECK(GlobalNumber(lua, "x") == 3.0);
    CHECK(GlobalString(lua, "y") == "el");
    CHECK(lua->DoString("t = {} tg = newtag() settag(t, tg) r = tag(t) u = tag({}) g = globals() == _G") == 0);
    CHECK(GlobalNumber(lua, "tg") >= 1.0);
    CHECK(GlobalNumber(lua, "r") == GlobalNumber(lua, "tg"));
    CHECK(GlobalNumber(lua, "u") == -1.0);
    lua->GetGlobal("g"); CHECK(lua->IsBoolean(-1) && lua->ToBoolean(-1)); lua->Pop(1);
    CHECK(lua->DoString("settagmethod(tg, 'index', function(t, k) return k .. '!' end) v = t.hello") == 0);
    CHECK(GlobalString(lua, "v") == "hello!");
    lua->SetTop(0);

    // a boxed pointer (the script system's "__group_handle")
    int object = 7;
    lua->PushUserData(&object, 0);
    CHECK(lua->IsUserData(-1));
    CHECK(lua->ToUserTag(-1) == &object);
    lua->Pop(1);
    lua->PushNumber(1.0);
    CHECK(lua->ToUserTag(-1) == nullptr);
    lua->Pop(1);

    // GetN, references, numbers
    CHECK(lua->DoString("list = {4, 5, 6, 7}") == 0);
    lua->GetGlobal("list");
    CHECK(lua->IsTable(-1) && lua->GetN(-1) == 4);
    const int ref = lua->Ref(1);
    CHECK(lua->GetTop() == 0);
    lua->GetRef(ref);
    CHECK(lua->IsTable(-1));
    lua->Pop(1);
    lua->Unref(ref);
    lua->PushNumber(4294967295.0);
    CHECK(lua->ToUint32(-1) == 0xffffffffu);
    lua->Pop(1);

    // a protected call that fails: the status comes back and the message is popped
    CHECK(lua->DoString("function boom() error('no') end function three() return 3 end") == 0);
    lua->GetGlobal("boom");
    CHECK(lua->CallProtected(0, 0) != 0);
    CHECK(lua->GetTop() == 0);
    lua->GetGlobal("three");
    CHECK(lua->CallProtected(0, 1) == 0);
    CHECK(lua->ToNumber(-1) == 3.0);
    lua->Pop(1);

    // print outputs nothing, takes any values, and raises when tostring does not answer a string
    CHECK(lua->DoString("print('a', 1, nil, {})") == 0);
    lua->SetTop(0);
    CHECK(lua->DoString("tostring = function() return {} end print(1)") != 0);
    lua->SetTop(0);

    CHECK(lua->Close());
    CHECK(lua->Release() == 0);
}

void TestPackages() {
    Resources resources;
    resources.Add(kGroup, "pkg_b.lua", "--#-package:00000002#\norder = (order or '') .. 'b'\n");
    resources.Add(kGroup, "pkg_a.lua",
                  "--#-package:00000001#\norder = (order or '') .. 'a'\ndofile = _dofile_from_resource\n"
                  "r1 = dofile('sub/inc.lua')\nr2 = dofile('missing.lua')\nr3 = dofile(5)\n");
    resources.Add(kGroup, "inc.lua", "included = (included or 0) + 1\n");
    resources.Add(kGroup, "pkg_b_again.lua", "--#-package:00000002#\norder = (order or '') .. 'X'\n");   // a repeated id: the first wins
    resources.Add(kGroup, "pkg_n.lua", "--#-package:80000001#\norder = (order or '') .. 'n'\n");         // negative as a signed id: first
    resources.Add(kGroup, "plain.lua", "-- no package header here, so it only runs through dofile\norder = (order or '') .. 'P'\n");
    resources.Add(kGroup, "short.lua", "--#-package:9#\nq=1\n    ");                                    // 23 bytes: too short to be looked at
    resources.Add(kGroup, "enough.lua", "--#-package:8#\nw=1\n     ");                                  // 24 bytes: a package
    resources.Add(kGroup, "zero.lua", "--#-package:00000000#\norder = (order or '') .. 'Z'\n");          // id 0 is no package
    resources.Add(kOtherGroup, "other.lua", "--#-package:00000003#\norder = (order or '') .. 'O'\n");    // another group
    SCLuaHelp::SetScriptResources(&resources);

    {
        cISCLua* lua = NewOpenLua();
        CHECK(SCLuaHelp::LoadPackageScripts(lua, kGroup, 0) == 0);
        CHECK(GlobalString(lua, "order") == "nab");
        CHECK(GlobalNumber(lua, "included") == 1.0);
        CHECK(GlobalIsNil(lua, "q"));
        CHECK(GlobalNumber(lua, "w") == 1.0);
        CHECK(GlobalNumber(lua, "r1") == 0.0);
        CHECK(GlobalIsNil(lua, "r2"));
        CHECK(GlobalIsNil(lua, "r3"));
        CHECK(GlobalNumber(lua, SCLuaHelp::kResourceGroupGlobal) == double(kGroup));
        lua->Close();
        CHECK(lua->Release() == 0);
    }
    {   // a first package runs first; the original then answers -1 when everything ran
        cISCLua* lua = NewOpenLua();
        CHECK(SCLuaHelp::LoadPackageScripts(lua, kGroup, 2) == -1);
        CHECK(GlobalString(lua, "order") == "bna");
        lua->Close();
        CHECK(lua->Release() == 0);
    }
    {   // a first package that is not there changes nothing
        cISCLua* lua = NewOpenLua();
        CHECK(SCLuaHelp::LoadPackageScripts(lua, kGroup, 0x77) == 0);
        CHECK(GlobalString(lua, "order") == "nab");
        lua->Close();
        CHECK(lua->Release() == 0);
    }
    {   // a package that fails is counted and the others still run
        resources.Add(kGroup, "pkg_bad.lua", "--#-package:00000004#\nthis is not Lua at all\n");
        cISCLua* lua = NewOpenLua();
        CHECK(SCLuaHelp::LoadPackageScripts(lua, kGroup, 0) == 1);
        CHECK(GlobalString(lua, "order") == "nab");
        lua->Close();
        CHECK(lua->Release() == 0);
    }

    // by key and by name
    {
        cISCLua* lua = NewOpenLua();
        CHECK(SCLuaHelp::LoadResourceScript(lua, kGroup, "inc.lua") == 0);
        CHECK(GlobalNumber(lua, "included") == 1.0);
        CHECK(SCLuaHelp::LoadResourceScript(lua, kGroup, "nothing.lua") == 2);
        CHECK(SCLuaHelp::LoadResourceScript(lua, kOtherGroup, "inc.lua") == 2);
        resources.Add(kGroup, "empty.lua", "");
        CHECK(SCLuaHelp::LoadResourceScript(lua, kGroup, "empty.lua") == 0);
        CHECK(SCLuaHelp::LoadResourceScript(lua, SCLuaHelp::ScriptKey(kGroup, "pkg_bad.lua"), "") != 0);
        lua->SetTop(0);
        lua->Close();
        CHECK(lua->Release() == 0);
    }
    SCLuaHelp::SetScriptResources(nullptr);
    {   // no resource manager: nothing loads, nothing fails
        cISCLua* lua = NewOpenLua();
        CHECK(SCLuaHelp::LoadPackageScripts(lua, kGroup, 0) == 0);
        CHECK(GlobalIsNil(lua, "order"));
        CHECK(SCLuaHelp::LoadResourceScript(lua, kGroup, "inc.lua") == 2);
        lua->Close();
        CHECK(lua->Release() == 0);
    }
}

}  // namespace

int main() {
    SCLuaHelp::SetScriptHostLookup(lua5::SCLuaFromState);
    TestKeys();
    TestHost();
    TestPackages();
    TestHost();   // a second host after the first is gone
    std::printf(g_fail ? "lua_reallua: %d FAILED\n" : "lua_reallua: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
