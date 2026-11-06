// The automata script system's services on the native application (sc4::IAutomataScriptHost), and the script resources
// every Lua host loads from (SCLuaHelp::IScriptResources on the game's resource manager). Without this host
// cSC4AutomataScriptSystem::Init stopped before running the automata scripts, so the city had no automata groups, no
// controller templates and no group classes: every automaton was created as a plain vehicle.
//   PARTIAL: the game-data registry is not part of the native game yet, so the "automata" node and its seven script
//   commands (create_automaton, attach_controller, ...) are not published and the command seams answer neutrally; the
//   player-drive key map is not read.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "app/NativeAnimationTicks.h"
#include "app/NativeUi.h"
#include "gz/cGZPersistResourceManager.h"
#include "gz/cIGZPersistDBSegment.h"
#include "gz/cIGZPersistResourceKeyList.h"
#include "lua/SCLuaInstance.h"
#include "lua/SCLuaScripts.h"
#include "sc4/AutomataScriptSystem.h"
#include "sc4/ServiceLocator.h"

namespace native {
namespace {
using namespace sc4;

class ScriptResources final : public SCLuaHelp::IScriptResources {
public:
    void ListScriptKeys(std::vector<cGZPersistResourceKey>& out) override {
        if (!SL::spRM) return;
        cIGZPersistResourceKeyList* list = nullptr;
        SL::spRM->GetAvailableResourceListForType(&list, SCLuaHelp::kScriptResourceType);
        if (!list) return;
        for (std::uint32_t i = 0; i < list->Size(); ++i) out.push_back(*list->GetKey(i));
        list->Release();
    }
    bool ReadScript(const cGZPersistResourceKey& key, std::vector<char>& text) override {
        text.clear();
        cIGZPersistDBSegment* segment = nullptr;
        if (SL::spRM && SL::spRM->FindDBSegment(key, &segment) && segment) {
            const std::uint32_t size = segment->GetRecordSize(key);
            bool read = false;
            if (size != 0xffffffffu) {
                text.assign(size, 0);
                std::uint32_t got = size;
                read = size == 0 || (segment->ReadRecord(key, text.data(), got) != 0xffffffffu && got == size);
            }
            segment->Release();
            if (read) return true;
        }
        std::vector<std::uint8_t> bytes;   // the game's packages read directly
        if (!LoadResourceBytes(key.type, key.group, key.instance, bytes)) return false;
        text.assign(bytes.begin(), bytes.end());
        return true;
    }
};

class ScriptHost final : public IAutomataScriptHost {
public:
    cISCLua* CreateLua() override { return lua5::CreateSCLua(); }
    void RegisterLuaExtensions(cISCLua* lua) override {
        lua->Register(SCLuaHelp::LUAExtension_print, "print");
        lua->Register(SCLuaHelp::LUAExtension_dofile_from_resource, "_dofile_from_resource");
    }
    int LoadPackageScripts(cISCLua* lua, std::uint32_t package, int flags) override {
        return SCLuaHelp::LoadPackageScripts(lua, package, static_cast<std::uint32_t>(flags));
    }
    sc4::cISC4GameDataRegistry* Registry() override { return nullptr; }
    void AddTickNotification(cIGZMessageTarget2* target, int channel) override {
        AnimationTickServerView()->AddNotification(target, static_cast<std::uint32_t>(channel));
    }
    void RemoveTickNotification(cIGZMessageTarget2* target, int channel) override {
        AnimationTickServerView()->RemoveNotification(target, static_cast<std::uint32_t>(channel));
    }
    cIGZUnknown* AddControllerFromTemplateName(cISC4Occupant*, const cIGZString&, const float*) override { return nullptr; }
    bool AddControllersFromOccupantGroup(const cIGZString&, cISC4Occupant*, const float*) override { return false; }
    bool RemoveController(cISC4Occupant*, const cIGZString&) override { return false; }
    bool RemoveControllersFromOccupantGroup(const cIGZString&, cISC4Occupant*) override { return false; }
    cIGZUnknown* CreateAutomaton(std::uint32_t, const float[3], std::uint32_t, const std::uint32_t[3], std::uint8_t) override { return nullptr; }
    void StartScriptedAutomaton(cIGZUnknown*) override {}
    bool SourceManagerPresent(int) override { return false; }
    std::uint32_t SourceBuildingCount(int, std::uint32_t) override { return 0; }
    bool DriveKeyMapAvailable() override { return false; }
    void GetDriveKeyCodes(int, std::uint32_t[8], std::uint32_t& count) override { count = 0; }
    void Report(const std::string& text) override {
        if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "automata scripts: %s\n", text.c_str());
    }
};

}  // namespace

void InstallScriptResources() {
    static ScriptResources resources;
    SCLuaHelp::SetScriptResources(&resources);
    SCLuaHelp::SetScriptHostLookup(lua5::SCLuaFromState);
}

// SC4_NOAUTOMATASCRIPTS=1 leaves the script system without its host (no groups, no templates): the A/B switch for anything
// the scripts change.
void InstallAutomataScriptHost() {
    static ScriptHost host;
    InstallScriptResources();
    if (std::getenv("SC4_NOAUTOMATASCRIPTS")) return;
    sc4::SetAutomataScriptHost(&host);
}

}  // namespace native
