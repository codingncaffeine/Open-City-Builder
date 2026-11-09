// The automata script system's services on the native application (sc4::IAutomataScriptHost), and the script resources
// every Lua host loads from (SCLuaHelp::IScriptResources on the game's resource manager). Without this host
// cSC4AutomataScriptSystem::Init stopped before running the automata scripts, so the city had no automata groups, no
// controller templates and no group classes: every automaton was created as a plain vehicle.
// The seven "automata" registry commands reach the city's controller manager (SL::spCity vslot 0x178) and the automata manager
// that takes the group (cSC4AutomataManager::StaticCreateAutomaton); a scripted automaton then registers its own node.
//   PARTIAL: the player-drive key map is not read; the watercraft manager is not asked for a group.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "app/NativeAnimationTicks.h"
#include "app/NativeAutomataViews.h"
#include "app/NativeCity.h"
#include "app/NativeGameData.h"
#include "app/NativeUi.h"
#include "gz/cGZPersistResourceManager.h"
#include "gz/cIGZPersistDBSegment.h"
#include "gz/cIGZPersistResourceKeyList.h"
#include "lua/SCLuaInstance.h"
#include "lua/SCLuaScripts.h"
#include "sc4/AutomataControllerManager.h"
#include "sc4/AutomataManager.h"
#include "sc4/AutomataScriptSystem.h"
#include "sc4/Automaton.h"
#include "sc4/ServiceLocator.h"
#include "sc4/cSC4City.h"

namespace native {
sc4::cSC4AutomataManager* NativeAutomataManagerAt(int index);   // NativeAutomataManagerHost.cpp
sc4::cSC4AutomataManager* NativeAutomataManagerForGroup(std::uint32_t group);
namespace {
using namespace sc4;

bool Trace() {
    static const bool on = std::getenv("SC4_SCRIPTCMDDEBUG") != nullptr;
    return on;
}
cSC4AutomataControllerManager* ControllerManager() {
    cSC4City* city = HostCity();
    return city ? dynamic_cast<cSC4AutomataControllerManager*>(city->GetComponent(CityComponent::AutomataControllerManager)) : nullptr;
}
cSC4AutomataScriptSystem* ScriptSystem() {
    cSC4City* city = HostCity();
    return city ? dynamic_cast<cSC4AutomataScriptSystem*>(city->GetComponent(CityComponent::AutomataScriptSystem)) : nullptr;
}

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
    cISCLua* CreateLua() override {
        NativeGameDataRegistry();   // the mirror needs the registry before the VM opens
        return lua5::CreateSCLua();
    }
    void RegisterLuaExtensions(cISCLua* lua) override {
        lua->Register(SCLuaHelp::LUAExtension_print, "print");
        lua->Register(SCLuaHelp::LUAExtension_dofile_from_resource, "_dofile_from_resource");
    }
    int LoadPackageScripts(cISCLua* lua, std::uint32_t package, int flags) override {
        return SCLuaHelp::LoadPackageScripts(lua, package, static_cast<std::uint32_t>(flags));
    }
    sc4::cISC4GameDataRegistry* Registry() override { return NativeGameDataRegistry(); }
    void AddTickNotification(cIGZMessageTarget2* target, int channel) override {
        AnimationTickServerView()->AddNotification(target, static_cast<std::uint32_t>(channel));
    }
    void RemoveTickNotification(cIGZMessageTarget2* target, int channel) override {
        AnimationTickServerView()->RemoveNotification(target, static_cast<std::uint32_t>(channel));
    }
    cIGZUnknown* AddControllerFromTemplateName(cISC4Occupant* occupant, const cIGZString& name, const float* position) override {
        cSC4AutomataControllerManager* manager = ControllerManager();
        cSC4AutomataController* made = manager ? manager->AddControllerFromTemplateName(occupant, name, position) : nullptr;
        if (Trace()) std::fprintf(stderr, "script commands: controller \"%s\" on %p -> %p\n", name.ToChar(), static_cast<void*>(occupant), static_cast<void*>(made));
        return made;
    }
    bool AddControllersFromOccupantGroup(const cIGZString& group, cISC4Occupant* occupant, const float* position) override {
        cSC4AutomataControllerManager* manager = ControllerManager();
        const bool added = manager && manager->AddControllersFromOccupantGroup(group, occupant, position);
        if (Trace()) std::fprintf(stderr, "script commands: controller group \"%s\" on %p -> %d\n", group.ToChar(), static_cast<void*>(occupant), added ? 1 : 0);
        return added;
    }
    bool RemoveController(cISC4Occupant* occupant, const cIGZString& name) override {
        cSC4AutomataControllerManager* manager = ControllerManager();
        if (Trace()) std::fprintf(stderr, "script commands: detach controller \"%s\" from %p\n", name.ToChar(), static_cast<void*>(occupant));
        return manager && manager->RemoveController(occupant, name);
    }
    bool RemoveControllersFromOccupantGroup(const cIGZString& group, cISC4Occupant* occupant) override {
        cSC4AutomataControllerManager* manager = ControllerManager();
        const bool removed = manager && manager->RemoveControllersFromOccupantGroup(group, occupant);
        if (Trace()) std::fprintf(stderr, "script commands: detach controller group \"%s\" from %p -> %d\n", group.ToChar(), static_cast<void*>(occupant), removed ? 1 : 0);
        return removed;
    }
    cIGZUnknown* CreateAutomaton(std::uint32_t group, const float position[3], std::uint32_t flags, const std::uint32_t key[3], std::uint8_t extra) override {
        cSC4AutomataManager* manager = NativeAutomataManagerForGroup(group);
        const cS3DVector3 where{position[0], position[1], position[2]};
        const cGZPersistResourceKey modelKey{key[0], key[1], key[2]};
        IAutomataAutomaton* view = manager ? manager->CreateAutomaton(group, where, flags, modelKey, nullptr, extra, nullptr) : nullptr;
        cSC4Automaton* automaton = view ? AutomatonOf(view) : nullptr;
        if (Trace()) std::fprintf(stderr, "script commands: automaton group 0x%x at (%.1f, %.1f, %.1f) flags 0x%x -> %p\n", group, position[0], position[1],
                                  position[2], flags, static_cast<void*>(automaton));
        return automaton ? static_cast<cISC4Occupant*>(automaton) : nullptr;
    }
    void StartScriptedAutomaton(cIGZUnknown* made) override {   // automaton vslot 0x220
        void* automaton = nullptr;
        if (!made || !made->QueryInterface(kAutomatonIid, &automaton)) return;
        static_cast<cSC4Automaton*>(automaton)->RegisterInGameDataRegistry();
        static_cast<cSC4Automaton*>(automaton)->Release();
    }
    bool SourceManagerPresent(int index) override { return NativeAutomataManagerAt(index) != nullptr; }
    std::uint32_t SourceBuildingCount(int index, std::uint32_t group) override {   // manager vslot 0x120
        sc4::cSC4AutomataManager* manager = NativeAutomataManagerAt(index);
        return manager ? manager->GetSourceBuildingCount(group) : 0;
    }
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
    sc4::AutomatonRegistrySource source;
    source.registry = [] { return NativeGameDataRegistry(); };
    source.scriptSystem = []() -> cIGZUnknown* {
        sc4::cSC4AutomataScriptSystem* system = ScriptSystem();
        return system ? static_cast<cIGZSerializable*>(system) : nullptr;
    };
    sc4::SetAutomatonRegistrySource(source);
    if (std::getenv("SC4_NOAUTOMATASCRIPTS")) return;
    sc4::SetAutomataScriptHost(&host);
}

}  // namespace native
