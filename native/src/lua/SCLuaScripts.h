// SCLuaHelp — running Lua scripts that live in the game's packages (lua-host.md 7 and 8.4; Mac i386 0x00161d88
// LoadResourceScript by key, 0x00162266 by name, 0x00161e54 LoadPackageScripts, 0x0016230e _dofile_from_resource,
// 0x0016196c print). A script is a resource of type 0xca63e2a3; its instance is 0xff000000 | CRC24 of the file name
// without directory and extension, case kept.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "gz/cGZPersistResourceKey.h"
#include "lua/ISCLua.h"

namespace SCLuaHelp {

constexpr std::uint32_t kScriptResourceType = 0xca63e2a3;
constexpr const char* kResourceGroupGlobal = "current_resource_group_id_ca636e36";

// The resource manager as the loader uses it (SL::spRM: slot 0x48 lists the keys, slot 0xc reads a script as 0xca63d8e9).
class IScriptResources {
public:
    virtual ~IScriptResources() = default;
    // Every key of type 0xca63e2a3, in the resource manager's list order.
    virtual void ListScriptKeys(std::vector<cGZPersistResourceKey>& out) = 0;
    // The script's text; false when there is no such resource.
    virtual bool ReadScript(const cGZPersistResourceKey& key, std::vector<char>& text) = 0;
};
void SetScriptResources(IScriptResources* resources);
// How a Lua C function finds its host (the original reads the pointer stored before the state and asks it for 0xea10c4a2).
void SetScriptHostLookup(cISCLua* (*lookup)(lua_State*));

// The script's key for `name` in `group`.
cGZPersistResourceKey ScriptKey(std::uint32_t group, const char* name);
// The file name of `path` without its directory and its last extension.
std::string ScriptBaseName(const char* path);

// 0 = ran (or the script is empty), 2 = no such resource, otherwise the DoBuffer status.
int LoadResourceScript(cISCLua* lua, const cGZPersistResourceKey& key, const char* chunkName);
int LoadResourceScript(cISCLua* lua, std::uint32_t group, const char* name);
// Runs the package scripts of `group` (the ones that start with "--#-package:<hex>#"), `firstPackage` first when it is not 0,
// the rest in ascending order of their signed id. Answers how many did not run.
int LoadPackageScripts(cISCLua* lua, std::uint32_t group, std::uint32_t firstPackage);

int LUAExtension_print(lua_State* state);
int LUAExtension_dofile_from_resource(lua_State* state);

}  // namespace SCLuaHelp
