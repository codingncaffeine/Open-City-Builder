// cSC4AutomataScriptSystem — see AutomataScriptSystem.h for the class layout, the vtable order and the script format.
//
// This file is the whole class: the lifecycle (Init registers the class with the game-data registry, Shutdown unregisters it and drops
// the Lua state), the registry commands the Lua scripts call (create_automaton, create_controller, attach_controller, ...), the importers
// that turn the tables of "_config.lua" into templates / groups / key maps, and the linking passes that resolve the script's names.
//
// Every engine neighbour is reached through IAutomataScriptHost (AutomataScriptHost()/SetAutomataScriptHost()); the Lua stack through
// cISCLua and the SCLuaHelp / SCLuaTableRef helpers, both recovered.
#include "sc4/AutomataScriptSystem.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include "lua/SCLuaHelp.h"
#include "lua/SCLuaTableRef.h"
#include "rz/cRZTimer.h"
#include "rz/cRZVariant.h"
#include "sc4/AutomataController.h"
#include "sc4/GameDataRegistryHelpers.h"

namespace sc4 {

namespace {

IAutomataScriptHost* g_host = nullptr;

constexpr int kAutomataScriptTickChannel = 0xc0;   // SL::ATM() channel the class ticks on (Init / Shutdown)
constexpr std::uint16_t kVariantString = 0x800e;   // kTypeRZChar | kTypeArrayFlag: a Lua string handed through a variant
constexpr std::uint32_t kOutParameterID = 1;       // every command answers its result under parameter 1
constexpr std::uint32_t kOccupantIID = 0x26bb767e; // cISC4Occupant, asked of the attach/detach command parameters
constexpr std::uint32_t kIIDMessageTarget2 = 0x452294aa;   // cIGZMessageTarget2 view (+8)
constexpr std::uint32_t kIIDSerializable = 0xe4fda3d4;     // cIGZSerializable view (+0x18)

std::string KeyOf(const cRZString& text) { return std::string(text.ToChar(), text.Strlen()); }
std::string KeyOf(const char* text) { return text ? std::string(text) : std::string(); }

std::uint32_t HexFromText(const char* text) {
    return static_cast<std::uint32_t>(std::strtoul(text ? text : "", nullptr, 16));
}

// The original builds these texts into a cRZString with Sprintf and drops the string again; the seam lets a test see them.
#if defined(__clang__) || defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
void Reportf(const char* format, ...) {
    if (!g_host) return;
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    g_host->Report(std::string(buffer));
}

// The two outputs the script importers read numbers from: a Lua number truncated through a 64-bit integer (unsigned) and a Lua number
// rounded to nearest (signed) are the SCLuaHelp rules; a hex string is parsed by strtoul, as in the original.
std::uint32_t ScriptUint32(cISCLua* lua, int index) {
    if (lua->IsNumber(index)) return static_cast<std::uint32_t>(static_cast<std::int64_t>(lua->ToNumber(index)));
    if (lua->IsString(index)) return HexFromText(lua->ToString(index));
    return 0;
}

cSC4OccupantGroupTemplate* AsGroup(IOccupantGroupTemplate* group) {
    return static_cast<cSC4OccupantGroupTemplate*>(group);
}

// The registry commands are dispatched with the registered handle (the command function pointer) as their first argument; the script
// system is the entry's context, which is how the original recovers `this`.
cSC4AutomataScriptSystem* SystemOf(cISC4GameDataRegistry* registry, void* handle) {
    if (!registry) return nullptr;
    const EntryId entry = registry->GetEntryID(handle);
    return static_cast<cSC4AutomataScriptSystem*>(registry->GetEntryContext(entry));
}

cIGZUnknown* CommandObject(cIGZCommandParameterSet* in, std::uint32_t id) {
    if (!in) return nullptr;
    void* object = nullptr;
    if (!(*in)[id].GetValUnknown(kOccupantIID, &object)) return nullptr;
    return static_cast<cIGZUnknown*>(object);
}

}  // namespace

IAutomataScriptHost* AutomataScriptHost() { return g_host; }
void SetAutomataScriptHost(IAutomataScriptHost* host) { g_host = host; }

cSC4AutomataScriptSystem::~cSC4AutomataScriptSystem() {
    // The destructor releases the Lua state only; the group templates are released by Shutdown (ClearOccupantGroups), exactly like the
    // original (the containers' destructors do not touch their pointees).
    if (lua_) {
        cISCLua* lua = lua_;
        lua_ = nullptr;
        lua->Release();
    }
}

void cSC4AutomataScriptSystem::SetLua(cISCLua* lua) {
    if (lua == lua_) return;
    if (lua_) lua_->Release();
    lua_ = lua;
    if (lua_) lua_->AddRef();
}

// ---- identity ---------------------------------------------------------------------------------------------------------
bool cSC4AutomataScriptSystem::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    switch (iid) {
        case kGZIID_cISC4AutomataScriptSystem:
            *out = static_cast<IControllerScriptSystem*>(this);
            break;
        case kIIDMessageTarget2:
            *out = static_cast<cIGZMessageTarget2*>(this);
            break;
        case kGZIID_cGZMessageTargetLegacy:
            *out = static_cast<cIGZMessageTarget*>(this);
            break;
        case kIIDSerializable:
        case 1:   // cIGZUnknown / cIGZSerializable: the original answers its +0x18 view for both
            *out = static_cast<cIGZSerializable*>(this);
            break;
        default:
            return false;
    }
    AddRef();
    return true;
}

std::uint32_t cSC4AutomataScriptSystem::Release() {
    if (refCount_ > 1) return --refCount_;
    FinalRelease();
    return 0;
}

void cSC4AutomataScriptSystem::FinalRelease() {
    Shutdown();
    delete this;
}

// ---- lifecycle --------------------------------------------------------------------------------------------------------
bool cSC4AutomataScriptSystem::Init() {
    if (initialized_) return true;
    IAutomataScriptHost* host = g_host;
    if (!host) return false;   // the original dereferences SL::ATM()/GZCOM here; a missing seam is a hard error in the port
    host->AddTickNotification(static_cast<cIGZMessageTarget2*>(this), kAutomataScriptTickChannel);

    CreateDefaultOccupantGroups();
    scripting_ = false;
    ExecuteScripts();

    cISC4GameDataRegistry* registry = host->Registry();
    nsISC4GameDataRegistry::AddNode<cSC4AutomataScriptSystem, cIGZUnknown>(this, "automata", nullptr, 0, nullptr, registry);

    struct CommandEntry {
        GameDataCommandFn fn;
        const char* name;
    };
    static const CommandEntry kCommands[] = {
        {sCreateAutomaton, "create_automaton"},
        {sCreateController, "create_controller"},
        {sAttachController, "attach_controller"},
        {sAttachControllerGroup, "attach_controller_group"},
        {sDetachController, "detach_controller"},
        {sDetachControllerGroup, "detach_controller_group"},
        {sGetSourceBuildingCount, "get_source_building_count"},
    };
    for (const CommandEntry& command : kCommands) {
        // The original registers the command function pointer itself as the entry handle.
        nsISC4GameDataRegistry::AddValue(reinterpret_cast<void*>(command.fn), command.name, command.fn, this, 0, this, registry);
    }

    initialized_ = true;
    return true;
}

bool cSC4AutomataScriptSystem::Shutdown() {
    if (!initialized_) return true;
    IAutomataScriptHost* host = g_host;
    if (host) {
        if (cISC4GameDataRegistry* registry = host->Registry()) {
            cIGZUnknown* node = nullptr;
            QueryInterface(1, reinterpret_cast<void**>(&node));
            registry->RemoveChildEntry(node);
            if (node) node->Release();
        }
        host->RemoveTickNotification(static_cast<cIGZMessageTarget2*>(this), kAutomataScriptTickChannel);
    }
    if (lua_) {
        lua_->Close();
        cISCLua* lua = lua_;
        lua_ = nullptr;
        lua->Release();
    }
    ClearTemplates();
    ClearOccupantGroups();
    initialized_ = false;
    return true;
}

void cSC4AutomataScriptSystem::CreateDefaultOccupantGroups() {
    // Both default groups are also automata groups (the original passes addToAutomataGroups = true to both).
    if (cSC4OccupantGroupTemplate* vehicle = CreateOccupantGroup(cRZString("vehicle"), kOccupantGroupVehicle, true)) {
        vehicle->classId = kClassIdVehicleGroup;
    }
    if (cSC4OccupantGroupTemplate* pedestrian = CreateOccupantGroup(cRZString("pedestrian"), kOccupantGroupPedestrian, true)) {
        pedestrian->classId = kClassIdPedestrianGroup;
    }
}

void cSC4AutomataScriptSystem::ClearTemplates() {
    for (auto& entry : templates_) {
        delete entry.second;
        entry.second = nullptr;
    }
    templates_.clear();
}

void cSC4AutomataScriptSystem::ClearOccupantGroups() {
    for (auto& entry : occupantGroups_) {
        if (entry.second) entry.second->Release();
    }
    occupantGroups_.clear();
    for (auto& entry : automataGroups_) {
        if (entry.second) entry.second->Release();
    }
    automataGroups_.clear();
    for (auto& entry : groupsById_) {
        if (entry.second) entry.second->Release();
    }
    groupsById_.clear();
    for (IOccupantGroupTemplate* group : specialFilterGroups_) {
        if (group) AsGroup(group)->Release();
    }
    specialFilterGroups_.clear();
}

bool cSC4AutomataScriptSystem::Write(cIGZOStream& stream) const {
    stream.SetUint16(kAutomataScriptVersion);
    return !stream.GetError();
}

bool cSC4AutomataScriptSystem::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != kAutomataScriptVersion) return false;
    return !stream.GetError();
}

bool cSC4AutomataScriptSystem::DoMessage(cIGZMessage2* message) {
    if (message) message->GetType();   // the original reads and discards the type
    return true;
}

bool cSC4AutomataScriptSystem::DoMessage(cGZMessage& message) {
    return message.type == kMessageScriptSystemDone;
}

// ---- lookups ----------------------------------------------------------------------------------------------------------
cSC4OccupantGroupTemplate* cSC4AutomataScriptSystem::GetAutomataGroup(const cRZString& name) {
    auto it = automataGroups_.find(KeyOf(name));
    return it == automataGroups_.end() ? nullptr : it->second;
}

cSC4OccupantGroupTemplate* cSC4AutomataScriptSystem::GetOccupantGroup(const cRZString& name) {
    auto it = occupantGroups_.find(KeyOf(name));
    return it == occupantGroups_.end() ? nullptr : it->second;
}

cSC4OccupantGroupTemplate* cSC4AutomataScriptSystem::GetOccupantGroup(std::uint32_t id) {
    auto it = groupsById_.find(id);
    return it == groupsById_.end() ? nullptr : it->second;
}

cSC4OccupantGroupTemplate* cSC4AutomataScriptSystem::CreateOccupantGroup(const cRZString& name, std::uint32_t id, bool addToAutomataGroups) {
    cSC4OccupantGroupTemplate* group = new cSC4OccupantGroupTemplate();
    group->groupId = id;
    const std::string key = KeyOf(name);
    if (occupantGroups_.find(key) != occupantGroups_.end()) {
        Reportf("Couldn't create occupant group named \"%s\"", name.ToChar());
        delete group;
        return nullptr;
    }
    occupantGroups_.emplace(key, group);
    group->AddRef();
    if (addToAutomataGroups) {
        automataGroups_.emplace(key, group);   // keeps an existing group under that name
        group->AddRef();                       // unconditional: a duplicate automata-group name leaks one reference (quirk kept)
    }
    if (id == 0) {
        specialFilterGroups_.push_back(group);   // the final AddRef below belongs to this list
    } else {
        groupsById_.emplace(id, group);          // the final AddRef below belongs to this map (a duplicate id leaks one, as in the original)
    }
    group->AddRef();
    return group;   // borrowed: the caller does not get its own reference
}

tControllerTemplate* cSC4AutomataScriptSystem::CreateControllerTemplate(const cRZString& name, std::uint32_t type) {
    tControllerTemplate* tmpl = cSC4AutomataController::CreateTemplateInstance(name, type);   // the type picks the template
    if (!tmpl) return nullptr;
    if (!templates_.emplace(KeyOf(name), tmpl).second) {
        Reportf("Couldn't create controller template named \"%s\".  Name already exists?", name.ToChar());
        delete tmpl;
        return nullptr;
    }
    return tmpl;
}

tControllerTemplate* cSC4AutomataScriptSystem::GetControllerTemplate(const cRZString& name) {
    auto it = templates_.find(KeyOf(name));
    return it == templates_.end() ? nullptr : it->second;
}

IOccupantGroupTemplate* cSC4AutomataScriptSystem::FindGroupTemplate(const cIGZString& name) {
    cRZString text(name);
    return GetOccupantGroup(text);
}

IOccupantGroupTemplate* cSC4AutomataScriptSystem::FindGroupTemplate(std::uint32_t id) { return GetOccupantGroup(id); }

tControllerTemplate* cSC4AutomataScriptSystem::FindControllerTemplate(const cIGZString& name) {
    cRZString text(name);
    return GetControllerTemplate(text);
}

int cSC4AutomataScriptSystem::GetDriveCommand(std::uint32_t keyCode) const {
    auto it = driveCommands_.find(keyCode);
    return it == driveCommands_.end() ? kPlayerDriveCommandNone : it->second;
}

// ---- variants ---------------------------------------------------------------------------------------------------------
// @ 0x1001ddc44: a hex string (type 0x800e) goes through strtoul; anything else has to convert to uint32, else 0.
std::uint32_t cSC4AutomataScriptSystem::GetHexFromVariant(cIGZVariant& value) {
    if (value.GetType() == kVariantString) {
        cRZString text;
        value.GetValString(text);
        return HexFromText(text.ToChar());
    }
    std::uint32_t result = 0;
    if (value.GetValUint32(result)) return result;
    return 0;
}

// ---- registry commands ------------------------------------------------------------------------------------------------
// @ 0x1001d6542
bool cSC4AutomataScriptSystem::sCreateAutomaton(void* handle, cIGZCommandParameterSet* in, cIGZCommandParameterSet* out,
                                                cISC4GameDataRegistry* registry) {
    cIGZUnknown* automaton = nullptr;
    if (SystemOf(registry, handle) && in && g_host) {
        const std::uint32_t count = in->GetParameterCount();
        float position[3] = {0.0f, 0.0f, 0.0f};
        std::uint32_t flags = 0x2000002;   // the caller's flags are OR-ed with 2; without one this default is used
        std::uint32_t key[3] = {0, 0, 0};
        std::uint8_t extra = 0;
        const std::uint32_t group = GetHexFromVariant((*in)[0]);
        (*in)[1].GetValFloat32(position[0]);
        (*in)[2].GetValFloat32(position[1]);
        (*in)[3].GetValFloat32(position[2]);
        if (count > 4) {
            (*in)[4].GetValUint32(flags);
            flags |= 2;
            if (count > 7) {
                key[0] = GetHexFromVariant((*in)[5]);
                key[1] = GetHexFromVariant((*in)[6]);
                key[2] = GetHexFromVariant((*in)[7]);
                if (count != 8) (*in)[8].GetValUint8(extra);
            }
        }
        automaton = g_host->CreateAutomaton(group, position, flags, key, extra);
        if (automaton) g_host->StartScriptedAutomaton(automaton);
    }
    if (out) out->SetParameter(kOutParameterID, cRZVariant(automaton));
    return true;
}

// @ 0x1001d6752
bool cSC4AutomataScriptSystem::sCreateController(void* handle, cIGZCommandParameterSet* in, cIGZCommandParameterSet* out,
                                                 cISC4GameDataRegistry* registry) {
    cIGZUnknown* controller = nullptr;
    if (SystemOf(registry, handle) && in && g_host) {
        cRZString name;
        (*in)[0].GetValString(name);
        float position[3] = {0.0f, 0.0f, 0.0f};
        (*in)[1].GetValFloat32(position[0]);
        (*in)[2].GetValFloat32(position[1]);
        (*in)[3].GetValFloat32(position[2]);
        controller = g_host->AddControllerFromTemplateName(nullptr, name, position);
    }
    if (out) out->SetParameter(kOutParameterID, cRZVariant(controller));
    return true;
}

// @ 0x1001d68ec
bool cSC4AutomataScriptSystem::sAttachController(void* handle, cIGZCommandParameterSet* in, cIGZCommandParameterSet* out,
                                                 cISC4GameDataRegistry* registry) {
    cIGZUnknown* controller = nullptr;
    if (SystemOf(registry, handle) && in && g_host) {
        cRZString name;
        (*in)[0].GetValString(name);
        cIGZUnknown* occupant = CommandObject(in, 1);
        if (occupant) {
            controller = g_host->AddControllerFromTemplateName(static_cast<cISC4Occupant*>(occupant), name, nullptr);
            occupant->Release();
        }
    }
    if (out) out->SetParameter(kOutParameterID, cRZVariant(controller));
    return true;
}

// @ 0x1001d6a7a (no answer parameter)
bool cSC4AutomataScriptSystem::sAttachControllerGroup(void* handle, cIGZCommandParameterSet* in, cIGZCommandParameterSet* out,
                                                      cISC4GameDataRegistry* registry) {
    (void)out;
    if (SystemOf(registry, handle) && in && g_host) {
        cRZString name;
        (*in)[0].GetValString(name);
        cIGZUnknown* occupant = CommandObject(in, 1);
        if (occupant) {
            g_host->AddControllersFromOccupantGroup(name, static_cast<cISC4Occupant*>(occupant), nullptr);
            occupant->Release();
        }
    }
    return true;
}

// @ 0x1001d6bb6 (no answer parameter)
bool cSC4AutomataScriptSystem::sDetachController(void* handle, cIGZCommandParameterSet* in, cIGZCommandParameterSet* out,
                                                 cISC4GameDataRegistry* registry) {
    (void)out;
    if (SystemOf(registry, handle) && in && g_host) {
        cRZString name;
        (*in)[0].GetValString(name);
        cIGZUnknown* occupant = CommandObject(in, 1);
        if (occupant) {
            g_host->RemoveController(static_cast<cISC4Occupant*>(occupant), name);
            occupant->Release();
        }
    }
    return true;
}

// @ 0x1001d6cf0 (no answer parameter)
bool cSC4AutomataScriptSystem::sDetachControllerGroup(void* handle, cIGZCommandParameterSet* in, cIGZCommandParameterSet* out,
                                                      cISC4GameDataRegistry* registry) {
    (void)out;
    if (SystemOf(registry, handle) && in && g_host) {
        cRZString name;
        (*in)[0].GetValString(name);
        cIGZUnknown* occupant = CommandObject(in, 1);
        if (occupant) {
            g_host->RemoveControllersFromOccupantGroup(name, static_cast<cISC4Occupant*>(occupant));
            occupant->Release();
        }
    }
    return true;
}

// @ 0x1001d6e2a
bool cSC4AutomataScriptSystem::sGetSourceBuildingCount(void* handle, cIGZCommandParameterSet* in, cIGZCommandParameterSet* out,
                                                       cISC4GameDataRegistry* registry) {
    std::uint32_t count = 0;
    if (SystemOf(registry, handle) && in && g_host) {
        std::uint32_t group = 0;
        (*in)[0].GetValUint32(group);
        for (int index = 0; index < 4; ++index) {
            if (!g_host->SourceManagerPresent(index)) continue;
            count = g_host->SourceBuildingCount(index, group);
            if (count != 0) break;
        }
    }
    if (out) out->SetParameter(kOutParameterID, cRZVariant(count));
    return true;
}

// ---- scripts ----------------------------------------------------------------------------------------------------------
// @ 0x1001d8210
bool cSC4AutomataScriptSystem::InitScripts() {
    IAutomataScriptHost* host = g_host;
    if (!host) return false;
    if (!lua_) {
        lua_ = host->CreateLua();
        if (!lua_) return false;   // the original dereferences the new state without a check
        lua_->Open(static_cast<int>(kLuaOpenAll));
    }
    host->RegisterLuaExtensions(lua_);
    // The four build flags the game scripts test. _BETA_BUILD and _MASTER_DEBUG are nil.
    lua_->PushNumber(1.0);
    lua_->SetGlobal("_IN_GAME");
    lua_->PushNumber(1.0);
    lua_->SetGlobal("_FINAL_RELEASE");
    lua_->PushNumber(1.0);
    lua_->SetGlobal("_MASTER_RELEASE");
    lua_->PushNil();
    lua_->SetGlobal("_BETA_BUILD");
    lua_->PushNil();
    lua_->SetGlobal("_MASTER_DEBUG");
    return true;
}

// @ 0x1001d61e6: creates the Lua state, runs the package's "_config.lua", imports its tables and closes the state again.
bool cSC4AutomataScriptSystem::ExecuteScripts() {
    cRZTimer timer(cRZTimer::kMilliseconds);
    timer.SetUnits(cRZTimer::kMilliseconds);
    timer.Start();

    InitScripts();
    if (!lua_) return false;   // no scripting host: the original would crash
    const int error = g_host->LoadPackageScripts(lua_, kAutomataScriptPackage, 0);   // SCLuaHelp::LoadPackageScripts: this seam needs the resource manager
    if (error == 0) {
        ImportAttractorTemplates();
        ImportGeneratorTemplates();
        ImportAutomataGroups();
        ImportOccupantGroups();
        LinkControllerTemplates();
        LinkOccupantGroups();
        ImportDriveCommandKeyMap();
        ImportTrackBuildingGroups();
    } else {
        Reportf("Error code %d executing automata _config.lua script!", error);
    }
    if (std::getenv("SC4_DEBUG"))
        std::fprintf(stderr, "automata scripts: status %d, %zu occupant groups (%zu with an id), %zu automata groups, %zu controller templates, %zu track building groups\n",
                     error, occupantGroups_.size(), groupsById_.size(), automataGroups_.size(), templates_.size(), trackBuildings_.size());
    scripting_ = false;
    if (lua_) {
        lua_->Close();
        cISCLua* lua = lua_;
        lua_ = nullptr;
        lua->Release();
    }
    timer.Stop();
    float elapsed = 0.0f;
    timer.GetElapsedTimeFloat(elapsed);   // read and dropped, like the original
    (void)elapsed;
    return true;
}

// @ 0x1001d8364
bool cSC4AutomataScriptSystem::ImportAttractorTemplates() {
    cISCLua* lua = lua_;
    if (!lua) return false;
    SCLuaTableRef table(lua, "attractor");
    for (table.begin(); !table.Finished(); table.next()) {
        const char* keyText = lua->ToString(-2);
        const cRZString name(keyText);
        const int tableIndex = lua->GetTop();
        tControllerTemplate* tmpl = GetControllerTemplate(name);
        if (!tmpl) {
            tmpl = CreateControllerTemplate(name, kControllerTypeAttractor);
        } else if (tmpl->controllerType != kControllerTypeAttractor) {
            Reportf("Couldn't create attractor template \"%s\", it already exists of type %d!", name.ToChar(), tmpl->controllerType);
        }
        if (!tmpl) continue;   // the original would overwrite a null template here; a failed insert only happens for duplicates
        tmpl->name = name;
        SCLuaHelp::ReadTableField(lua, "radius", tmpl->radius, tableIndex);

        // "strength" is a list of weights spread evenly over 0..radius; without one the strength falls from 100 at 0 to 0 at radius.
        std::vector<SC4Point> points;
        if (!SCLuaHelp::GetField(lua, "strength", tableIndex)) {
            points.resize(2);
            points[0] = SC4Point{0.0f, 100.0f};
            points[1] = SC4Point{tmpl->radius, 0.0f};
        } else {
            std::vector<float> strengths;
            SCLuaHelp::ReadTableAsArray(lua, strengths, -1);
            if (strengths.size() < 2) {
                const float strength = strengths.empty() ? 0.0f : strengths[0];   // the original dereferences begin() of an empty list
                points.resize(2);
                points[0] = SC4Point{0.0f, strength};
                points[1] = SC4Point{tmpl->radius, strength};
            } else {
                points.resize(strengths.size());
                const float step = tmpl->radius / static_cast<float>(strengths.size() - 1);
                float x = 0.0f;
                for (std::size_t i = 0; i < strengths.size(); ++i) {
                    points[i] = SC4Point{x, strengths[i]};
                    x += step;
                }
            }
            lua->Pop(1);
        }
        // The attractor's strength curve lives at +0x128 of its template. The original stores it there even when the name belonged to a
        // template of another type (where that address is the generator's count scale); writing a vector there would corrupt it, so the
        // write is skipped after the mismatch report above.
        if (tmpl->controllerType == kControllerTypeAttractor) {
            static_cast<tAttractorTemplate*>(tmpl)->strengthCurve.SetResponseCurveData(points);
        }

        ImportAutomataGroupNameList(tableIndex, tmpl->groupNames);
        ImportTimeOfDayCurve(tableIndex, tmpl);
        ImportCalendarField(tableIndex, tmpl);
        ImportBehaviorTable(tableIndex, tmpl);
        ImportEffectTriggerList(tableIndex, tmpl);
        ImportControllerLifetime(tableIndex, tmpl);
        ImportLotFlags(tableIndex, tmpl);
        ImportSFXList(tableIndex, tmpl);
        ImportIgnoreStateList(tableIndex, tmpl);
        tmpl->uniqueRadius = 0.0f;
        SCLuaHelp::ReadTableField(lua, "exclusive_radius", tmpl->uniqueRadius, tableIndex);
        if (!SCLuaHelp::ReadTableField(lua, "max_count", tmpl->maxAutomata, tableIndex)) tmpl->maxAutomata = 0;
    }
    lua->Pop(1);
    return true;
}

// @ 0x1001d88be
bool cSC4AutomataScriptSystem::ImportGeneratorTemplates() {
    cISCLua* lua = lua_;
    if (!lua) return false;
    SCLuaTableRef table(lua, "generator");
    for (table.begin(); !table.Finished(); table.next()) {
        const char* keyText = lua->ToString(-2);
        const cRZString name(keyText);
        const int tableIndex = lua->GetTop();
        tControllerTemplate* base = GetControllerTemplate(name);
        if (!base) {
            base = CreateControllerTemplate(name, kControllerTypeGenerator);
        } else if (base->controllerType != kControllerTypeGenerator) {
            // The original reports the clash and then writes the generator fields through the other template; the downcast is
            // undefined for that object, so the entry is skipped here.
            Reportf("Couldn't create generator template \"%s\", it already exists of type %d!", name.ToChar(), base->controllerType);
            continue;
        }
        if (!base) continue;
        auto* tmpl = static_cast<tGeneratorTemplate*>(base);   // guaranteed by the type check above
        tmpl->name = name;

        ImportAutomataGroupNameList(tableIndex, tmpl->groupNames);
        ImportTimeOfDayCurve(tableIndex, tmpl);
        ImportCalendarField(tableIndex, tmpl);
        ImportBehaviorTable(tableIndex, tmpl);
        ImportEffectTriggerList(tableIndex, tmpl);
        ImportControllerLifetime(tableIndex, tmpl);
        ImportLotFlags(tableIndex, tmpl);
        ImportSFXList(tableIndex, tmpl);
        ImportIgnoreStateList(tableIndex, tmpl);
        tmpl->uniqueRadius = 0.0f;
        SCLuaHelp::ReadTableField(lua, "exclusive_radius", tmpl->uniqueRadius, tableIndex);

        if (SCLuaHelp::GetField(lua, "radius", tableIndex)) {
            if (!lua->IsTable(-1)) {
                const float radius = static_cast<float>(lua->ToNumber(-1));
                tmpl->radius = radius;
                tmpl->radius2 = radius;
            } else {
                std::vector<float> radii;
                SCLuaHelp::ReadTableAsArray(lua, radii, -1);
                if (!radii.empty()) tmpl->radius = radii[0];
                if (radii.size() >= 2) tmpl->radius2 = radii[1];
            }
            lua->Pop(1);
        }

        if (SCLuaHelp::GetField(lua, "population_pct", tableIndex)) {
            tmpl->countScale = static_cast<float>(lua->ToNumber(-1));
            tmpl->countMode = 2;
            lua->Pop(1);
        } else if (SCLuaHelp::GetField(lua, "occupancy_pct", tableIndex)) {
            tmpl->countScale = static_cast<float>(lua->ToNumber(-1));
            tmpl->countMode = 1;
            lua->Pop(1);
        } else if (SCLuaHelp::GetField(lua, "count", tableIndex)) {
            tmpl->countScale = static_cast<float>(lua->ToNumber(-1));
            tmpl->countMode = 0;
            lua->Pop(1);
        } else {
            tmpl->countScale = 0.0f;
            tmpl->countMode = 0;
        }

        if (!SCLuaHelp::ReadTableField(lua, "max_count", tmpl->maxAutomata, tableIndex)) tmpl->maxAutomata = 0;
        SCLuaHelp::ReadTableField(lua, "rate", tmpl->rate, tableIndex);
        std::uint32_t rateScale = 0;
        SCLuaHelp::ReadTableField(lua, "rate_scale", rateScale, tableIndex);
        tmpl->periodUnit = static_cast<std::int32_t>(rateScale);
        std::uint32_t event = 0;
        if (!SCLuaHelp::ReadTableField(lua, "event", event, tableIndex)) event = 0;
        tmpl->eventId = event;
        if (!SCLuaHelp::ReadTableField(lua, "random_chance", tmpl->probability, tableIndex)) tmpl->probability = 1.0f;
        tmpl->lifetimeAutomataLimit = 0;
        SCLuaHelp::ReadTableField(lua, "lifetime_count", tmpl->lifetimeAutomataLimit, tableIndex);
        tmpl->groupSizeMin = 1;
        tmpl->groupSizeMax = 1;
        SCLuaHelp::ReadTableRange(lua, "group_count", &tmpl->groupSizeMin, tableIndex);
        std::uint32_t priority = 0;
        if (!SCLuaHelp::ReadTableField(lua, "priority", priority, tableIndex)) {
            priority = 0x19;
        } else if (priority > 0xfe) {
            priority = 0xff;
        }
        tmpl->spawnArgument = static_cast<std::uint8_t>(priority);
    }
    lua->Pop(1);
    return true;
}

// @ 0x1001d8f52
bool cSC4AutomataScriptSystem::ImportAutomataGroups() {
    cISCLua* lua = lua_;
    if (!lua) return false;
    SCLuaTableRef table(lua, "automata_group");
    for (table.begin(); !table.Finished(); table.next()) {
        const char* keyText = lua->ToString(-2);
        const cRZString name(keyText);
        const int tableIndex = lua->GetTop();
        cSC4OccupantGroupTemplate* group = ImportSingleOccupantGroup(name, static_cast<short>(tableIndex));
        if (!group) continue;

        if (automataGroups_.emplace(KeyOf(name), group).second) group->AddRef();

        if (SCLuaHelp::GetField(lua, "trailers", tableIndex)) {
            SCLuaHelp::ReadTableAsList(lua, group->trailerNames, -1);
            lua->Pop(1);
        }
        group->kphMin = 0.0f;
        group->kphMax = 0.0f;
        if (SCLuaHelp::GetField(lua, "kph", tableIndex)) {
            std::vector<float> kph;
            SCLuaHelp::ReadTableAsArray(lua, kph, -1);
            if (!kph.empty()) group->kphMin = kph[0];
            if (kph.size() >= 2) group->kphMax = kph[1];
            lua->Pop(1);
        }

        group->driveParams.reset();
        if (SCLuaHelp::GetField(lua, "airplane", tableIndex)) {
            group->driveParams = std::make_unique<tAutomataDriveParams>();
            ImportAirplaneParams(lua->GetTop(), group->driveParams.get());
            lua->Pop(1);
        } else if (SCLuaHelp::GetField(lua, "helicopter", tableIndex)) {
            group->driveParams = std::make_unique<tAutomataDriveParams>();
            ImportHelicopterParams(lua->GetTop(), group->driveParams.get());
            lua->Pop(1);
        } else if (SCLuaHelp::GetField(lua, "WatercraftDriveParams", tableIndex)) {
            group->driveParams = std::make_unique<tAutomataDriveParams>();
            ImportWatercraftParams(lua->GetTop(), group->driveParams.get());
            lua->Pop(1);
        }

        group->playerDriveParams.reset();
        if (SCLuaHelp::GetField(lua, "playerdrive", tableIndex)) {
            group->playerDriveParams = std::make_unique<tAutomataPlayerDriveParams>();
            ImportPlayerDriveParams(lua->GetTop(), group->playerDriveParams.get());
            lua->Pop(1);
        }

        cRZString image;
        SCLuaHelp::ReadTableField(lua, "csi_image", image, tableIndex);
        group->csiImage = HexFromText(image.ToChar());
        cRZString sourceBuilding;
        SCLuaHelp::ReadTableField(lua, "source_building", sourceBuilding, tableIndex);
        group->sourceBuilding = HexFromText(sourceBuilding.ToChar());

        if (SCLuaHelp::GetField(lua, "key_effects", tableIndex)) {
            ImportKeyEffectsTable(lua->GetTop(), group);
            lua->Pop(1);
        }
    }
    lua->Pop(1);
    return true;
}

// @ 0x1001d99c4
bool cSC4AutomataScriptSystem::ImportOccupantGroups() {
    cISCLua* lua = lua_;
    if (!lua) return false;
    SCLuaTableRef table(lua, "occupant_group");
    for (table.begin(); !table.Finished(); table.next()) {
        const char* keyText = lua->ToString(-2);
        const cRZString name(keyText);
        ImportSingleOccupantGroup(name, static_cast<short>(lua->GetTop()));
    }
    lua->Pop(1);
    return true;
}

// @ 0x1001d9b9e: resolve every template's "automata" names into the group ids it should spawn / attract, then drop the names.
bool cSC4AutomataScriptSystem::LinkControllerTemplates() {
    for (auto& entry : templates_) {
        tControllerTemplate* tmpl = entry.second;
        if (!tmpl) continue;
        if (tmpl->controllerType != kControllerTypeAttractor && tmpl->controllerType != kControllerTypeGenerator) continue;
        tmpl->groups.clear();
        for (const cRZString& name : tmpl->groupNames) {
            if (cSC4OccupantGroupTemplate* group = GetAutomataGroup(name)) tmpl->groups.push_back(group->groupId);
        }
        tmpl->groupNames.clear();
    }
    return true;
}

// @ 0x1001d9c74: two passes over the Lua reference pairs (parent link, then clear the handle) and the trailer names -> group ids.
bool cSC4AutomataScriptSystem::LinkOccupantGroups() {
    cISCLua* lua = lua_;
    if (!lua) return false;
    for (auto& entry : occupantGroups_) {
        cSC4OccupantGroupTemplate* group = entry.second;
        if (!group || group->parentRef == -2) continue;
        lua->GetRef(group->parentRef);
        if (!lua->IsNil(-1)) {
            const int tableIndex = lua->GetTop();
            if (lua->IsTable(tableIndex) && SCLuaHelp::GetField(lua, "__group_handle", tableIndex)) {
                group->parent = static_cast<cSC4OccupantGroupTemplate*>(lua->ToUserTag(-1));
                lua->Pop(1);
            }
            lua->Pop(1);
        }
        lua->Unref(group->parentRef);
        group->parentRef = -2;
    }
    for (auto& entry : occupantGroups_) {
        cSC4OccupantGroupTemplate* group = entry.second;
        if (!group || group->selfRef == -2) continue;
        lua->GetRef(group->selfRef);
        if (!lua->IsNil(-1)) {
            const int tableIndex = lua->GetTop();
            if (lua->IsTable(tableIndex)) {
                lua->PushString("__group_handle");
                lua->PushNil();
                lua->SetTable(tableIndex);
            }
            lua->Pop(1);
        }
        lua->Unref(group->selfRef);
        group->selfRef = -2;
    }
    for (auto& entry : occupantGroups_) {
        cSC4OccupantGroupTemplate* group = entry.second;
        if (!group || group->trailerNames.empty()) continue;
        for (const cRZString& name : group->trailerNames) {
            auto it = occupantGroups_.find(KeyOf(name));
            if (it == occupantGroups_.end() || !it->second) continue;
            if (it->second->groupId != 0) group->trailerGroupIds.push_back(it->second->groupId);
        }
        group->trailerNames.clear();
    }
    return true;
}

// @ 0x1001d9f84: the drive key map resource (key type 0x42e3ea4b, record 0x42e967be) gives the key codes bound to each drive command.
bool cSC4AutomataScriptSystem::ImportDriveCommandKeyMap() {
    driveCommands_.clear();
    IAutomataScriptHost* host = g_host;
    if (!host || !host->DriveKeyMapAvailable()) return true;
    for (int command = 0; command < kDriveCommandCount; ++command) {
        std::uint32_t keys[kDriveKeyCodesPerCommand] = {};
        std::uint32_t count = kDriveKeyCodesPerCommand;
        host->GetDriveKeyCodes(command, keys, count);
        if (count > kDriveKeyCodesPerCommand) count = kDriveKeyCodesPerCommand;   // the original trusts the record's count
        for (std::uint32_t i = 0; i < count; ++i) driveCommands_.emplace(keys[i], command);   // the first command for a key wins
    }
    return true;
}

// @ 0x1001da37c
bool cSC4AutomataScriptSystem::ImportTrackBuildingGroups() {
    cISCLua* lua = lua_;
    if (!lua) return false;
    SCLuaTableRef table(lua, "track_buildings");
    for (table.begin(); !table.Finished(); table.next()) {
        std::int32_t value = 0;
        if (lua->IsNumber(-1)) {
            value = static_cast<std::int32_t>(lua->ToUint32(-1));
        } else if (lua->IsString(-1)) {
            value = static_cast<std::int32_t>(HexFromText(lua->ToString(-1)));
        } else {
            continue;
        }
        if (value != 0) trackBuildings_.push_back(static_cast<std::uint32_t>(value));
    }
    // Quirk kept: the original never pops the "track_buildings" table from the Lua stack.
    return true;
}

// @ 0x1001da596
cSC4OccupantGroupTemplate* cSC4AutomataScriptSystem::ImportSingleOccupantGroup(const cRZString& name, short tableIndex) {
    cISCLua* lua = lua_;
    if (!lua) return nullptr;
    std::uint32_t groupId = 0;
    if (SCLuaHelp::GetField(lua, "group_id", tableIndex)) {
        groupId = ScriptUint32(lua, -1);
        lua->Pop(1);
    }

    cSC4OccupantGroupTemplate* group = nullptr;
    auto existing = occupantGroups_.find(KeyOf(name));
    if (existing == occupantGroups_.end()) {
        group = CreateOccupantGroup(name, groupId, false);
    } else {
        group = existing->second;   // reuse the group of the same name (its id is not updated)
    }
    if (!group) return nullptr;

    if (!SCLuaHelp::GetField(lua, "percentage", tableIndex)) {
        group->chance_ = 1.0f;
    } else {
        group->chance_ = static_cast<float>(lua->ToNumber(-1));
        lua->Pop(1);
    }

    if (!SCLuaHelp::GetField(lua, "controllers", tableIndex)) {
        group->controllerTemplates_.clear();
    } else {
        std::vector<cRZString> names;
        SCLuaHelp::ReadTableAsArray(lua, names, -1);
        for (const cRZString& controllerName : names) {
            if (tControllerTemplate* tmpl = GetControllerTemplate(controllerName)) group->controllerTemplates_.push_back(tmpl);
        }
        lua->Pop(1);
    }

    if (!SCLuaHelp::GetField(lua, "models", tableIndex)) {
        group->resourceKeys.clear();
    } else {
        ReadGUIDTable(-1, group->resourceKeys);
        lua->Pop(1);
    }

    const bool hasAnims = SCLuaHelp::GetField(lua, "anims", tableIndex);
    group->animOffsets.clear();
    if (hasAnims) {
        SCLuaTableRef anims(lua, -1);
        for (anims.begin(); !anims.Finished(); anims.next()) {
            const char* animName = lua->ToString(-2);
            group->animOffsets[KeyOf(animName)] = ScriptUint32(lua, -1);
        }
        lua->Pop(1);
    }

    if (SCLuaHelp::GetField(lua, "_parent", tableIndex)) {
        if (group->parentRef != -2) lua->Unref(group->parentRef);
        group->parentRef = static_cast<std::int16_t>(lua->Ref(false));
    }

    if (!SCLuaHelp::GetField(lua, "class_id", tableIndex)) {
        group->classId = 0;
    } else {
        group->classId = ScriptUint32(lua, -1);
        lua->Pop(1);
    }

    // Publish the group as a user data handle on its own table, and keep a reference to that table as the group's "self": the parent
    // pass of LinkOccupantGroups reads the handle, the self pass clears it again.
    lua->PushString("__group_handle");
    lua->PushUserData(group, kLuaGroupHandleTag);
    lua->SetTable(tableIndex);
    if (group->selfRef != -2) lua->Unref(group->selfRef);
    lua->PushValue(tableIndex);
    group->selfRef = static_cast<std::int16_t>(lua->Ref(false));
    return group;
}

namespace {
// The three drive-parameter importers share their shape: the table is walked in one chain and every field is read from it (the original's
// "index != -1" guard after each read is dead: the bookkeeping cursor at that point is never -1).
void ReadFloatChain(cISCLua* lua, int tableIndex, tAutomataDriveParams* params, const char* const* names, const unsigned* offsets,
                    std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) SCLuaHelp::ReadTableField(lua, names[i], params->At(offsets[i]), tableIndex);
}
}  // namespace

// @ 0x1001daec4
bool cSC4AutomataScriptSystem::ImportAirplaneParams(int tableIndex, tAutomataDriveParams* params) {
    static const char* const kNames[] = {
        "Mass", "WingArea", "TakeOffSpeed", "MaxSpeed", "ElevatorMax", "ElevatorChangeRate", "AileronMax", "AileronChangeRate",
        "RudderMax", "RudderChangeRate", "ThrottleMax", "ThrottleChangeRate", "FlapsMax", "FlapsChangeRate", "StallAngle",
        "LiftFactorLevelFlight", "LiftFactorMaxClimb", "LiftFactorFullStall",
    };
    static const unsigned kOffsets[] = {
        0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c, 0x40, 0x44,
    };
    cISCLua* lua = lua_;
    if (!lua || !params) return false;
    SCLuaTableRef table(lua, tableIndex);
    if (table.Finished() || table.TableIndex() == -1) return true;
    ReadFloatChain(lua, table.TableIndex(), params, kNames, kOffsets, std::size(kNames));
    return true;
}

// @ 0x1001db158
bool cSC4AutomataScriptSystem::ImportHelicopterParams(int tableIndex, tAutomataDriveParams* params) {
    static const char* const kNames[] = {
        "Mass", "Power", "SpeedMultiplier", "MaxSpeed", "DragFactor", "RotationalDragFactor", "BankMax", "BankChangeRate",
        "PitchMax", "PitchChangeRate", "YawRateMax", "BankMultiplier", "PitchMultiplier",
    };
    static const unsigned kOffsets[] = {0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30};
    cISCLua* lua = lua_;
    if (!lua || !params) return false;
    SCLuaTableRef table(lua, tableIndex);
    if (table.Finished() || table.TableIndex() == -1) return true;
    ReadFloatChain(lua, table.TableIndex(), params, kNames, kOffsets, std::size(kNames));
    return true;
}

// @ 0x1001db34c
bool cSC4AutomataScriptSystem::ImportWatercraftParams(int tableIndex, tAutomataDriveParams* params) {
    static const char* const kNames[] = {
        "DecelPerSecond", "TurnsPerSecond", "RestoreTurnsPerSecond", "DecaySpeedPerSecond", "SpeedModifier",
        "SwitchTurnDirectionsModifier", "MinSpeed", "MaxSpeed", "MinTurn", "MaxTurn", "MaxSpeedChangePerMillisecond",
        "BaseForwardTurnRadius", "BaseBackwardTurnRadius", "AIMaxSpeedChangePerMillisecond", "AIMaxTurnChangePerMillisecond",
        "AICrashSpeedAnimateCollisionThreshold", "AICrashSpeedInanimateCollisionThreshold", "ForwardMotionPitchModifier",
        "ForwardMotionPitchMaxAngle", "SideMotionPitchModifier", "SideMotionPitchMaxAngle", "CrashSequenceMillisecondMax",
        "CrashPitchTotal", "CrashRunAgroundSpeedReductionModifier", "CrashRunAgroundRollTotal",
        "CrashClimableTerrainAngleThreshold", "CrashMaxAltitudeIncrease", "CrashSpeedAnimateCollisionThreshold",
        "CrashSpeedInanimateCollisionThreshold", "CrashAnimateCollisionSpeedScalingFactor", "MinAcceleration", "MaxAcceleration",
        "ReverseInertialTime", "ForwardInertialTime", "TurnInPlacePerSec", "MaxMillisecondDelta", "FerryQueueDistance",
        "FerryDockDistance", "FerryMaxDockingOrientationChangePerMillisecond", "FerryMaxDockingPositionChangePerMillisecond",
    };
    static const unsigned kOffsets[] = {
        0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c, 0x40, 0x44, 0x48, 0x4c,
        0x50, 0x54, 0x58, 0x5c, 0x60, 0x64, 0x68, 0x6c, 0x70, 0x74, 0x78, 0x7c, 0x80, 0x84, 0x88, 0x8c, 0x90, 0x94, 0x98, 0x9c,
    };
    cISCLua* lua = lua_;
    if (!lua || !params) return false;
    SCLuaTableRef table(lua, tableIndex);
    if (table.Finished() || table.TableIndex() == -1) return true;
    ReadFloatChain(lua, table.TableIndex(), params, kNames, kOffsets, std::size(kNames));
    return true;
}

// @ 0x1001db8b8
bool cSC4AutomataScriptSystem::ImportPlayerDriveParams(int tableIndex, tAutomataPlayerDriveParams* params) {
    cISCLua* lua = lua_;
    if (!lua || !params) return false;
    SCLuaTableRef table(lua, tableIndex);
    if (table.Finished()) return true;
    int rangeIndex = -1;
    if (table.TableIndex() != -1) {
        struct IntField {
            const char* name;
            unsigned offset;
        };
        struct FloatField {
            const char* name;
            unsigned offset;
        };
        // The chain order is the script's declaration order in the original source, not the memory order.
        static const IntField kInts[] = {{"MinZoom", 0x00}, {"StartingZoom", 0x04}};
        static const FloatField kFloats[] = {
            {"MaxSpeed", 0x08}, {"MinSpeed", 0x0c}, {"AccelPerSecond", 0x10}, {"DecelPerSecond", 0x14},
            {"DecaySpeedPerSec", 0x1c}, {"MaxTurnAngle", 0x20}, {"TurnPerSec", 0x2c}, {"RestoreTurnPerSec", 0x30},
            {"TurnAngleThreshhold", 0x34}, {"GripPercentage", 0x38}, {"GuardrailDotTolerance", 0x3c}, {"GuardrailDistance", 0x40},
            {"GuardrailPercentage", 0x44}, {"OffRoadSpeedModifier", 0x18}, {"RelativeDensity", 0x48},
            {"TrainDerailmentThrottleDuration", 0x4c}, {"TrainFullThrottlePitchMax", 0x50}, {"MaxTapDuration", 0x54},
            {"TankTurnInPlace", 0x58}, {"HitPoints", 0x5c}, {"AccelRate", 0x60}, {"BankRate", 0x64}, {"ClimbRate", 0x68},
            {"DiveRate", 0x6c}, {"RudderRate", 0x70}, {"ReturnToCenterFactor", 0x74}, {"MinThrottle", 0x78},
            {"FlapRateClimb", 0x7c}, {"FlapRateDive", 0x80}, {"LandingThrottle", 0x84}, {"LandingElevator", 0x88},
            {"TrimRate", 0x8c}, {"MaxTrim", 0x90},
        };
        for (const IntField& field : kInts) {
            int value = 0;
            SCLuaHelp::ReadTableField(lua, field.name, value, table.TableIndex());
            params->SetI(field.offset, value);
        }
        for (const FloatField& field : kFloats) {
            float value = 0.0f;
            SCLuaHelp::ReadTableField(lua, field.name, value, table.TableIndex());
            params->SetF(field.offset, value);
        }
        rangeIndex = table.TableIndex();
    }
    if (SCLuaHelp::GetField(lua, "OffRoadTurnAngleRange", rangeIndex)) {
        std::vector<float> range;
        SCLuaHelp::ReadTableAsArray(lua, range, -1);
        if (!range.empty()) params->SetF(0x24, range[0]);
        if (range.size() >= 2) params->SetF(0x28, range[1]);
        lua->Pop(1);
    }
    return true;
}

// @ 0x1001dbe26
bool cSC4AutomataScriptSystem::ImportKeyEffectsTable(int tableIndex, cSC4OccupantGroupTemplate* group) {
    cISCLua* lua = lua_;
    if (!lua || !group) return false;
    SCLuaTableRef table(lua, tableIndex);
    for (table.begin(); !table.Finished(); table.next()) {
        if (!lua->IsTable(-1)) continue;
        tKeyEffect effect;
        const int rowIndex = lua->GetTop();
        if (rowIndex != -1) {
            std::uint32_t command = 0;
            SCLuaHelp::ReadTableField(lua, "cmd", command, rowIndex);
            SCLuaHelp::ReadTableField(lua, "fx", effect.fx, rowIndex);
            SCLuaHelp::ReadTableField(lua, "bone", effect.bone, rowIndex);
            SCLuaHelp::ReadTableField(lua, "controller", effect.controller, rowIndex);
            std::uint32_t flags = 0;
            if (effect.fx.Strlen() != 0) flags |= 0x10;
            if (effect.controller.Strlen() != 0) flags |= 0x20;
            bool hold = false, oneShot = false, hardTransition = false, override = false;
            SCLuaHelp::ReadTableField(lua, "hold", hold, rowIndex);
            SCLuaHelp::ReadTableField(lua, "one_shot", oneShot, rowIndex);
            SCLuaHelp::ReadTableField(lua, "hard_transition", hardTransition, rowIndex);
            SCLuaHelp::ReadTableField(lua, "override", override, rowIndex);
            if (hold) flags |= 1;
            if (oneShot) flags |= 2;
            if (hardTransition) flags |= 4;
            if (override) flags |= 8;
            effect.value = flags;
            // Only an effect with an "fx" is stored; the first entry for a command wins.
            if (effect.fx.Strlen() != 0) group->keyEffects.emplace(static_cast<std::uint8_t>(command), effect);
        }
    }
    // Quirk kept: this importer leaves its table on the Lua stack (ImportAutomataGroups pops it).
    return true;
}

// @ 0x1001dc52e: every entry is a list of three hex strings: (type, group, instance).
bool cSC4AutomataScriptSystem::ReadGUIDTable(short tableIndex, std::list<cGZPersistResourceKey>& out) {
    cISCLua* lua = lua_;
    if (!lua) return false;
    out.clear();
    SCLuaTableRef table(lua, static_cast<int>(tableIndex));
    for (table.begin(); !table.Finished(); table.next()) {
        std::vector<cRZString> values;
        SCLuaHelp::ReadTableAsArray(lua, values, -1);
        if (values.size() < 3) continue;   // the original reads the three entries unchecked
        cGZPersistResourceKey key;
        key.type = HexFromText(values[0].ToChar());
        key.group = HexFromText(values[1].ToChar());
        key.instance = HexFromText(values[2].ToChar());
        out.push_back(key);
    }
    // Quirk kept: this importer leaves its table on the Lua stack (the caller pops it).
    return true;
}

// @ 0x1001dc6ae
bool cSC4AutomataScriptSystem::ImportAutomataGroupNameList(int tableIndex, std::list<cRZString>& out) {
    cISCLua* lua = lua_;
    if (!lua) return false;
    if (!SCLuaHelp::GetField(lua, "automata", tableIndex)) {
        out.clear();
    } else {
        SCLuaHelp::ReadTableAsList(lua, out, -1);
        lua->Pop(1);
    }
    return true;
}

// @ 0x1001dc704: the time-of-day response curve is a table time -> weight, sorted by time (a repeated time keeps the later entry first).
bool cSC4AutomataScriptSystem::ImportTimeOfDayCurve(int tableIndex, tControllerTemplate* tmpl) {
    cISCLua* lua = lua_;
    if (!lua || !tmpl) return false;
    std::vector<SC4Point> points;
    if (!SCLuaHelp::GetField(lua, "time_of_day", tableIndex)) {
        points.push_back(SC4Point{0.0f, 1.0f});
        points.push_back(SC4Point{24.0f, 1.0f});
    } else {
        SCLuaTableRef table(lua, -1);
        for (table.begin(); !table.Finished(); table.next()) {
            const SC4Point point{static_cast<float>(lua->ToNumber(-2)), static_cast<float>(lua->ToNumber(-1))};
            auto at = std::lower_bound(points.begin(), points.end(), point.x,
                                       [](const SC4Point& existing, float x) { return existing.x < x; });
            points.insert(at, point);
        }
        lua->Pop(1);
    }
    tmpl->curve.SetResponseCurveData(points);
    return true;
}

// @ 0x1001dc902: the day mask (1 << (day & 31)); no "calendar" means -1 (every day).
bool cSC4AutomataScriptSystem::ImportCalendarField(int tableIndex, tControllerTemplate* tmpl) {
    cISCLua* lua = lua_;
    if (!lua || !tmpl) return false;
    if (!SCLuaHelp::GetField(lua, "calendar", tableIndex)) {
        tmpl->field60 = -1;
    } else {
        std::vector<unsigned> days;
        SCLuaHelp::ReadTableAsArray(lua, days, -1);
        std::uint32_t mask = 0;
        for (unsigned day : days) mask |= 1u << (day & 0x1f);
        tmpl->field60 = static_cast<std::int32_t>(mask);
        lua->Pop(1);
    }
    return true;
}

// @ 0x1001dc9ce: one behavior entry per element of the "behavior" table; the list is sorted afterwards with tBehaviorTemplate's operator<.
bool cSC4AutomataScriptSystem::ImportBehaviorTable(int tableIndex, tControllerTemplate* tmpl) {
    cISCLua* lua = lua_;
    if (!lua || !tmpl) return false;
    tmpl->behaviors.clear();
    if (!SCLuaHelp::GetField(lua, "behavior", tableIndex)) return true;
    SCLuaTableRef table(lua, -1);
    for (table.begin(); !table.Finished(); table.next()) {
        const int rowIndex = lua->GetTop();
        tBehaviorTemplate behavior;   // the defaults are the original's stack initialisers
        std::uint32_t state = 0;
        SCLuaHelp::ReadTableField(lua, "state", state, rowIndex);
        behavior.state = state;

        if (SCLuaHelp::GetField(lua, "anims", rowIndex)) {
            SCLuaHelp::ReadTableAsArray(lua, behavior.anims, -1);
            lua->Pop(1);
        }
        if (SCLuaHelp::GetField(lua, "sfx", rowIndex)) {
            std::list<cRZString> sounds;
            SCLuaHelp::ReadTableAsList(lua, sounds, -1);
            for (const cRZString& sound : sounds) behavior.sfx.push_back(HexFromText(sound.ToChar()));
            lua->Pop(1);
        }
        if (!SCLuaHelp::ReadTableField(lua, "radius", behavior.radius, rowIndex)) {
            behavior.radius = std::numeric_limits<float>::max();
        }
        std::uint32_t priority = behavior.priority;
        SCLuaHelp::ReadTableField(lua, "priority", priority, rowIndex);
        behavior.priority = priority;
        if (!SCLuaHelp::ReadTableField(lua, "percentage", behavior.percentage, rowIndex)) behavior.percentage = 1.0f;
        SCLuaHelp::ReadTableField(lua, "timeout", behavior.timeout, rowIndex);
        SCLuaHelp::ReadTableField(lua, "final", behavior.final, rowIndex);
        SCLuaHelp::ReadTableField(lua, "ignore_paths", behavior.ignorePaths, rowIndex);
        SCLuaHelp::ReadTableField(lua, "ignore_roads", behavior.ignoreRoads, rowIndex);
        SCLuaHelp::ReadTableField(lua, "ignore_terrain", behavior.ignoreTerrain, rowIndex);
        SCLuaHelp::ReadTableField(lua, "ignore_attractors", behavior.ignoreAttractors, rowIndex);
        SCLuaHelp::ReadTableField(lua, "avoid_lots", behavior.avoidLots, rowIndex);
        SCLuaHelp::ReadTableField(lua, "avoid_roads", behavior.avoidRoads, rowIndex);
        behavior.kphMultiply[0] = behavior.kphMultiply[1] = 1.0f;
        SCLuaHelp::ReadTableRange(lua, "kph_multiply", behavior.kphMultiply, rowIndex);
        SCLuaHelp::ReadTableRange(lua, "kph", behavior.kph, rowIndex);
        SCLuaHelp::ReadTableField(lua, "acceleration", behavior.acceleration, rowIndex);
        behavior.pathOffset = 0.0f;
        SCLuaHelp::ReadTableField(lua, "path_offset", behavior.pathOffset, rowIndex);

        if (SCLuaHelp::GetField(lua, "random_walk", rowIndex)) {
            const int walkIndex = lua->GetTop();
            SCLuaHelp::ReadTableRange(lua, "delay", behavior.delay, walkIndex);
            if (SCLuaHelp::GetField(lua, "turn", walkIndex)) {
                behavior.turn[0] = behavior.turn[1] = 0.0f;
                if (lua->IsTable(-1)) {
                    std::vector<float> turns;
                    SCLuaHelp::ReadTableAsArray(lua, turns, -1);
                    if (!turns.empty()) behavior.turn[0] = turns[0];
                    if (turns.size() >= 2) behavior.turn[1] = turns[1];
                } else if (lua->IsNumber(-1)) {
                    behavior.turn[0] = static_cast<float>(lua->ToNumber(-1));
                }
                lua->Pop(1);
            }
            SCLuaHelp::ReadTableRange(lua, "strength", behavior.strength, walkIndex);
            SCLuaHelp::ReadTableRange(lua, "idle_delay", behavior.idleDelay, walkIndex);
            SCLuaHelp::ReadTableRange(lua, "idle_time", behavior.idleTime, walkIndex);
            SCLuaHelp::ReadTableField(lua, "deceleration", behavior.deceleration, walkIndex);
            SCLuaHelp::ReadTableRange(lua, "kph", behavior.kph, walkIndex);
            SCLuaHelp::ReadTableField(lua, "acceleration", behavior.acceleration, walkIndex);
            lua->Pop(1);
        }
        tmpl->behaviors.push_back(behavior);
    }
    lua->Pop(1);
    std::sort(tmpl->behaviors.begin(), tmpl->behaviors.end());
    return true;
}

// @ 0x1001dd250: "effect_trigger" and "deactivate_trigger" are lists of hex effect ids (the parameter must be on / off).
bool cSC4AutomataScriptSystem::ImportEffectTriggerList(int tableIndex, tControllerTemplate* tmpl) {
    cISCLua* lua = lua_;
    if (!lua || !tmpl) return false;
    tmpl->effectsOn.clear();
    if (SCLuaHelp::GetField(lua, "effect_trigger", tableIndex)) {
        std::vector<cRZString> values;
        SCLuaHelp::ReadTableAsArray(lua, values, -1);
        for (const cRZString& value : values) tmpl->effectsOn.push_back(HexFromText(value.ToChar()));
        lua->Pop(1);
    }
    tmpl->effectsOff.clear();
    if (SCLuaHelp::GetField(lua, "deactivate_trigger", tableIndex)) {
        std::vector<cRZString> values;
        SCLuaHelp::ReadTableAsArray(lua, values, -1);
        for (const cRZString& value : values) tmpl->effectsOff.push_back(HexFromText(value.ToChar()));
        lua->Pop(1);
    }
    return true;
}

// @ 0x1001dd504
bool cSC4AutomataScriptSystem::ImportControllerLifetime(int tableIndex, tControllerTemplate* tmpl) {
    cISCLua* lua = lua_;
    if (!lua || !tmpl) return false;
    tmpl->lifetime = 0.0f;
    SCLuaHelp::ReadTableField(lua, "lifetime", tmpl->lifetime, tableIndex);
    tmpl->startDelay = 0.0f;
    SCLuaHelp::ReadTableField(lua, "delay", tmpl->startDelay, tableIndex);
    return true;
}

// @ 0x1001dd56c: the controller flag word (+0xbc). It starts as 2 and every named bool sets or clears one bit.
bool cSC4AutomataScriptSystem::ImportLotFlags(int tableIndex, tControllerTemplate* tmpl) {
    cISCLua* lua = lua_;
    if (!lua || !tmpl) return false;
    struct FlagField {
        const char* name;
        std::uint32_t bit;
    };
    static const FlagField kFlags[] = {
        {"use_lot_facing", 0x10},  {"use_lot_center", 0x20}, {"ignore_lot", 0x40},    {"transient", 0x8},
        {"ground_only", 0x1000},   {"destroy_automata", 0x100}, {"follow_roads", 0x80}, {"follow_rail", 0x800},
        {"persist_offscreen", 0x200},
    };
    std::uint32_t flags = 2;
    for (const FlagField& field : kFlags) {
        bool enabled = false;
        SCLuaHelp::ReadTableField(lua, field.name, enabled, tableIndex);
        if (enabled) flags |= field.bit;
        else flags &= ~field.bit;
    }
    tmpl->flags = flags;
    return true;
}

// @ 0x1001dd7be
bool cSC4AutomataScriptSystem::ImportSFXList(int tableIndex, tControllerTemplate* tmpl) {
    cISCLua* lua = lua_;
    if (!lua || !tmpl) return false;
    if (!SCLuaHelp::GetField(lua, "sfx", tableIndex)) return true;   // quirk: an absent field leaves the old list alone
    std::vector<cRZString> values;
    SCLuaHelp::ReadTableAsArray(lua, values, -1);
    tmpl->sounds.clear();
    for (const cRZString& value : values) tmpl->sounds.push_back(HexFromText(value.ToChar()));
    lua->Pop(1);
    return true;
}

// @ 0x1001dd8ca
bool cSC4AutomataScriptSystem::ImportIgnoreStateList(int tableIndex, tControllerTemplate* tmpl) {
    cISCLua* lua = lua_;
    if (!lua || !tmpl) return false;
    tmpl->ignoreStates.clear();
    if (!SCLuaHelp::GetField(lua, "ignore_states", tableIndex)) return true;
    SCLuaHelp::ReadTableAsArray(lua, tmpl->ignoreStates, -1);
    lua->Pop(1);
    return true;
}

}  // namespace sc4
