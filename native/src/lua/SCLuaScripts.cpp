#include "lua/SCLuaScripts.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include "rz/cRZCmdLine.h"
#include "rz/nRZHash.h"

namespace SCLuaHelp {
namespace {

IScriptResources* g_resources = nullptr;
cISCLua* (*g_lookup)(lua_State*) = nullptr;

constexpr int kLuaTypeString = 4;
// A package header is looked for in scripts longer than kPackageSignaturePrefixLen + 10 bytes, within their first 512.
constexpr int kPackageHeaderMinSize = 23;
constexpr std::size_t kPackageHeaderWindow = 0x200;

// The id of a package script ("--#-package:<hex>#" at its very start), or 0.
std::uint32_t PackageId(const std::vector<char>& text) {
    if (static_cast<int>(text.size()) <= kPackageHeaderMinSize || text[0] != '-' || text[1] != '-' || text[2] != '#') return 0;
    // the original ends the window with a NUL at min(size, 512) - 1 and scans "--#%[^#]s"
    const std::size_t window = (text.size() < kPackageHeaderWindow ? text.size() : kPackageHeaderWindow) - 1;
    std::string header;
    for (std::size_t i = 3; i < window && text[i] != '#' && text[i] != '\0'; ++i) header.push_back(text[i]);
    if (header.empty()) return 0;
    cRZCmdLine commandLine(header.c_str());
    cRZString value;
    if (!commandLine.IsSwitchPresent(cRZString("package"), value, true)) return 0;
    return static_cast<std::uint32_t>(std::strtoul(value.ToChar(), nullptr, 16));
}

}  // namespace

void SetScriptResources(IScriptResources* resources) { g_resources = resources; }
void SetScriptHostLookup(cISCLua* (*lookup)(lua_State*)) { g_lookup = lookup; }

std::string ScriptBaseName(const char* path) {
    std::string name = path ? path : "";
    const std::size_t slash = name.find_last_of("/\\:");
    if (slash != std::string::npos) name.erase(0, slash + 1);
    const std::size_t dot = name.rfind('.');
    if (dot != std::string::npos) name.erase(dot);
    return name;
}

cGZPersistResourceKey ScriptKey(std::uint32_t group, const char* name) {
    cGZPersistResourceKey key;
    key.type = kScriptResourceType;
    key.group = group;
    key.instance = nRZHash::CRC24(ScriptBaseName(name).c_str(), false) | 0xff000000u;   // SC4GetResourceIDFromString
    return key;
}

int LoadResourceScript(cISCLua* lua, const cGZPersistResourceKey& key, const char* chunkName) {
    std::vector<char> text;
    if (!g_resources || !g_resources->ReadScript(key, text)) return 2;
    if (text.empty()) return 0;
    return lua->DoBuffer(text.data(), static_cast<int>(text.size()), chunkName);
}

int LoadResourceScript(cISCLua* lua, std::uint32_t group, const char* name) {
    const std::string base = ScriptBaseName(name);
    return LoadResourceScript(lua, ScriptKey(group, name), base.c_str());
}

int LoadPackageScripts(cISCLua* lua, std::uint32_t group, std::uint32_t firstPackage) {
    lua->PushNumber(static_cast<double>(group));
    lua->SetGlobal(kResourceGroupGlobal);

    std::vector<cGZPersistResourceKey> keys;
    if (g_resources) g_resources->ListScriptKeys(keys);
    std::map<std::int32_t, std::size_t> packages;   // signed id -> index of the first key that carries it
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keys[i].group != group) continue;
        std::vector<char> text;
        if (!g_resources->ReadScript(keys[i], text)) continue;
        const std::uint32_t id = PackageId(text);
        if (id != 0) packages.emplace(static_cast<std::int32_t>(id), i);
    }

    int loaded = 0;
    if (firstPackage != 0) {
        auto first = packages.find(static_cast<std::int32_t>(firstPackage));
        if (first != packages.end() && LoadResourceScript(lua, keys[first->second], "") == 0) {
            packages.erase(first);
            loaded = 1;
        }
    }
    for (const auto& package : packages) {
        if (LoadResourceScript(lua, keys[package.second], "") == 0) ++loaded;
    }
    // The original subtracts from the map's size after the erase above, so a first package that ran is counted against the
    // result (-1 when everything ran). Its two callers pass no first package.
    return static_cast<int>(packages.size()) - loaded;
}

int LUAExtension_print(lua_State* state) {
    cISCLua* lua = g_lookup ? g_lookup(state) : nullptr;
    if (!lua) return 0;
    static const bool echo = std::getenv("SC4_LUADEBUG") != nullptr;
    const int count = lua->GetTop();
    lua->GetGlobal("tostring");
    for (int i = 1; i <= count; ++i) {
        lua->PushValue(-1);
        lua->PushValue(i);
        lua->RawCall(1, 1);
        const char* text = lua->ToString(-1);
        if (!text) lua->Error("`tostring' must return a string to `print'");
        if (echo) std::fprintf(stderr, "lua: print: %s\n", text);   // the original outputs nothing
        lua->Pop(1);
    }
    return 0;
}

int LUAExtension_dofile_from_resource(lua_State* state) {
    cISCLua* lua = g_lookup ? g_lookup(state) : nullptr;
    if (!lua) return 0;
    if (lua->GetTop() > 0 && lua->Type(-1) == kLuaTypeString) {
        lua->GetGlobal(kResourceGroupGlobal);
        const std::uint32_t group = lua->IsNumber(-1) ? static_cast<std::uint32_t>(static_cast<std::int64_t>(lua->ToNumber(-1))) : 0;
        lua->Pop(1);
        const char* name = lua->ToString(-1);
        if (name && LoadResourceScript(lua, group, name) == 0) {
            lua->PushNumber(0.0);
            return 1;
        }
    }
    lua->PushNil();
    return 1;
}

}  // namespace SCLuaHelp
