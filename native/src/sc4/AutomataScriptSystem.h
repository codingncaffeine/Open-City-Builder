// cSC4AutomataScriptSystem — loads the automata configuration scripts (package 0x4a5e8f3f, "_config.lua") and keeps what they define:
// controller templates (attractor / generator), occupant groups and automata groups, the player-drive key map and the track building groups.
// 63 symbols at 0x1001d5a90..0x1001de310. Class id 0x4a1b664e, interface 0x8a1b6647 (the primary view); +8 cIGZMessageTarget2 (0x452294aa),
// +0x10 legacy message target (0xe98b2f57), +0x18 serializable (0xe4fda3d4, also iid 1).
// Layout: +0x20 initialised, +0x24 refcount, +0x28 Lua (counted), +0x30 byte (cleared after the scripts ran), +0x38 map<name, group> occupant
// groups, +0x60 map<name, group> automata groups, +0x88 map<id, group>, +0xb0 list<group> groups without an id ("special filters"), +0xc8
// map<name, controller template>, +0xf0 list<u32> track building groups, +0x108 map<key code, drive command>.
// Every group lives in the containers it was inserted in; ClearOccupantGroups releases one reference per container (name map, automata map,
// id map, special list).
//
// Original primary vtable: [0] QueryInterface, [1] AddRef, [2] Release, [3] Init, [4] Shutdown, [5] GetAutomataGroupMap,
// [6] GetAutomataGroup(name), [7] GetAutomataGroup(id), [8] GetOccupantGroupNameMap, [9] GetOccupantGroupIDMap,
// [10] GetSpecialFilterOccupantGroups, [11] GetOccupantGroup(name), [12] GetOccupantGroup(id), [13] CreateOccupantGroup,
// [14] GetDriveCommand, [15] GetControllerTemplateMap, [16] CreateControllerTemplate, [17] GetControllerTemplate,
// [18] GetTrackBuildingGroups, [19] Write, [20] Read, [21] GetGZCLSID, [22] DoMessage(cIGZMessage2*), [23] DoMessage(cGZMessage&).
// This port reaches every one of them by name (the bytes at +8/+0x10/+0x18 are the other three views), so the C++ inheritance order
// (IControllerScriptSystem, cIGZMessageTarget2, cIGZMessageTarget, cIGZSerializable) only has to preserve those subobject views.
//
// Script format (read by the Import* functions; numbers may be given as hex strings where noted):
//   attractor{ name = { radius, strength, automata, time_of_day, calendar, behavior, effect_trigger, deactivate_trigger, lifetime, delay,
//   lot flags, sfx, ignore_states, exclusive_radius, max_count } }  generator{ ... + population_pct / occupancy_pct / count, rate, rate_scale,
//   event, random_chance, lifetime_count, group_count, priority }  occupant_group / automata_group { name = { group_id, percentage,
//   controllers, models, anims, _parent, class_id, trailers, kph, airplane|helicopter|WatercraftDriveParams, playerdrive, csi_image,
//   source_building, key_effects } }  track_buildings{ ... }.
//
// Quirks kept: CreateOccupantGroup hands the group an extra reference when it is already in the automata map; a failed group/template
// insert deletes the new object and answers null; an existing controller template of another type is reported but still overwritten;
// ImportSingleOccupantGroup reuses an existing group of the same name and (re)creates the Lua "self" reference; ReadGUIDTable takes the
// three hex strings as (type, group, instance); the behaviour list is sorted with tBehaviorTemplate's operator<; a strength array with
// fewer than two values makes a flat curve; the "_parent" / "__group_handle" link is a Lua reference pair that the link pass clears again;
// ImportSFXList leaves the sound list untouched when the script has no "sfx" field; ImportTrackBuildingGroups and ImportKeyEffectsTable
// leave their table on the Lua stack (the caller pops the latter); GetHexFromVariant only accepts the string (0x800e) and uint32 types.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <list>
#include <string>
#include <unordered_map>

#include "gz/CommandParameterSet.h"
#include "gz/cGZMessageServer.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZSerializable.h"
#include "gz/cIGZVariant.h"
#include "lua/ISCLua.h"
#include "rz/cRZString.h"
#include "sc4/AutomataAttractor.h"
#include "sc4/AutomataControllerManager.h"
#include "sc4/GameDataRegistry.h"
#include "sc4/OccupantGroupTemplate.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4AutomataScriptSystem = 0x4a1b664e;
constexpr std::uint32_t kGZIID_cISC4AutomataScriptSystem = 0x8a1b6647;
constexpr std::uint32_t kGZIID_cGZMessageTargetLegacy = 0xe98b2f57;
constexpr std::uint32_t kAutomataScriptPackage = 0x4a5e8f3f;
constexpr int kPlayerDriveCommandNone = 0xd;                     // GetDriveCommand for an unmapped key
constexpr std::uint32_t kOccupantGroupVehicle = 0x4002, kOccupantGroupPedestrian = 0x4001;
constexpr std::uint32_t kClassIdVehicleGroup = 0x896e714a, kClassIdPedestrianGroup = 0x896e75af;
constexpr std::uint32_t kMessageScriptSystemDone = 0x87163754;   // DoMessage(cGZMessage&) answers true for this type
constexpr std::uint32_t kLuaOpenAll = 0x7e;
constexpr std::uint16_t kAutomataScriptVersion = 1;              // Write/Read stream version
constexpr int kDriveCommandCount = 0xd;                          // commands 0..0xc have a key map entry
constexpr int kDriveKeyCodesPerCommand = 8;                      // the record's key array
constexpr std::uint16_t kLuaGroupHandleTag = 0;                  // PushUserData tag of "__group_handle"

// ---- seam -----------------------------------------------------------------------------------------------------------
// Everything the original reached through a process-wide global or the framework.
class IAutomataScriptHost {
public:
    virtual ~IAutomataScriptHost() = default;
    virtual cISCLua* CreateLua() = 0;                                              // GZCOM()->CreateInstance(0xea10c4a2, 0xea10c4a2), counted
    virtual void RegisterLuaExtensions(cISCLua* lua) = 0;                          // Register(LUAExtension_print, "print"), "_dofile_from_resource"
    virtual int LoadPackageScripts(cISCLua* lua, std::uint32_t package, int flags) = 0;   // SCLuaHelp::LoadPackageScripts (0 = ok)
    virtual cISC4GameDataRegistry* Registry() = 0;                                 // RZGetFramework() system service 0xebe0e860 / 0xe898ed03 (borrowed)
    // SL::ATM(): the automata manager the scripts tick from; vslots 0x28 (add) / 0x30 (remove), channel 0xc0.
    virtual void AddTickNotification(cIGZMessageTarget2* target, int channel) = 0;
    virtual void RemoveTickNotification(cIGZMessageTarget2* target, int channel) = 0;
    // SL::spCity vslot 0x178 (cSC4AutomataControllerManager); a null manager makes the create/attach commands answer null and the detach
    // commands do nothing. The five calls mirror the manager's vslots 0x28, 0x38, 0x48, 0x58.
    virtual cIGZUnknown* AddControllerFromTemplateName(cISC4Occupant* occupant, const cIGZString& name, const float* position) = 0;
    virtual bool AddControllersFromOccupantGroup(const cIGZString& group, cISC4Occupant* occupant, const float* position) = 0;
    virtual bool RemoveController(cISC4Occupant* occupant, const cIGZString& name) = 0;
    virtual bool RemoveControllersFromOccupantGroup(const cIGZString& group, cISC4Occupant* occupant) = 0;
    // cSC4AutomataManager::StaticCreateAutomaton(group, position, flags, resource key, null, 0): a new automaton (counted) or null; started
    // afterwards with vslot 0x220.
    virtual cIGZUnknown* CreateAutomaton(std::uint32_t group, const float position[3], std::uint32_t flags, const std::uint32_t key[3], std::uint8_t extra) = 0;
    virtual void StartScriptedAutomaton(cIGZUnknown* automaton) = 0;               // automaton vslot 0x220
    // SL::spVehicleManager / spPedestrianManager / spAircraftManager / spWatercraftManager, in that order: vslot 0x120(group).
    virtual bool SourceManagerPresent(int index) = 0;
    virtual std::uint32_t SourceBuildingCount(int index, std::uint32_t group) = 0;
    // The player-drive key map resource (key 0x6a231eaa a2e3d533 0x0c0eaad0, type 0x42e3ea4b, record 0x42e967be): the key codes bound to command
    // `command` (up to 8). False when the resource is missing.
    virtual bool DriveKeyMapAvailable() = 0;
    virtual void GetDriveKeyCodes(int command, std::uint32_t out[8], std::uint32_t& count) = 0;   // record vslot 0x68(command, out, &count)
    virtual void Report(const std::string& text) { (void)text; }                   // the Sprintf'd error texts (the original formats and drops them)
};
IAutomataScriptHost* AutomataScriptHost();
void SetAutomataScriptHost(IAutomataScriptHost* host);

class cSC4AutomataScriptSystem : public IControllerScriptSystem, public cIGZMessageTarget2, public cIGZMessageTarget, public cIGZSerializable {
public:
    cSC4AutomataScriptSystem() = default;                                         // @ 0x1001d5a90 (0x1001d5bcc twin)
    virtual ~cSC4AutomataScriptSystem();                                          // @ 0x1001d5bd6

    bool QueryInterface(std::uint32_t iid, void** out) override;                  // @ 0x1001ddd30 (thunks ..d86, d94, da2)
    std::uint32_t AddRef() override { return ++refCount_; }                       // @ 0x1001dddb0
    std::uint32_t Release() override;                                             // @ 0x1001ddde8
    void FinalRelease();                                                          // @ 0x1001dde04

    bool Init();                                                                  // @ 0x1001d5d0a
    bool Shutdown();                                                              // @ 0x1001d6f5a
    void CreateDefaultOccupantGroups();                                           // @ 0x1001d60d4
    bool ExecuteScripts();                                                        // @ 0x1001d61e6
    bool InitScripts();                                                           // @ 0x1001d8210

    // registry commands (context = the script system)
    static bool sCreateAutomaton(void* handle, cIGZCommandParameterSet* in, cIGZCommandParameterSet* out, cISC4GameDataRegistry* registry);    // @ 0x1001d6542
    static bool sCreateController(void* handle, cIGZCommandParameterSet* in, cIGZCommandParameterSet* out, cISC4GameDataRegistry* registry);  // @ 0x1001d6752
    static bool sAttachController(void* handle, cIGZCommandParameterSet* in, cIGZCommandParameterSet* out, cISC4GameDataRegistry* registry);  // @ 0x1001d68ec
    static bool sAttachControllerGroup(void* handle, cIGZCommandParameterSet* in, cIGZCommandParameterSet* out, cISC4GameDataRegistry* registry);   // @ 0x1001d6a7a
    static bool sDetachController(void* handle, cIGZCommandParameterSet* in, cIGZCommandParameterSet* out, cISC4GameDataRegistry* registry);  // @ 0x1001d6bb6
    static bool sDetachControllerGroup(void* handle, cIGZCommandParameterSet* in, cIGZCommandParameterSet* out, cISC4GameDataRegistry* registry);   // @ 0x1001d6cf0
    static bool sGetSourceBuildingCount(void* handle, cIGZCommandParameterSet* in, cIGZCommandParameterSet* out, cISC4GameDataRegistry* registry);  // @ 0x1001d6e2a

    void ClearTemplates();                                                        // @ 0x1001d7086
    void ClearOccupantGroups();                                                   // @ 0x1001d70c4
    bool Write(cIGZOStream& stream) const override;                               // @ 0x1001d717a (thunk 0x1001d71a6)
    bool Read(cIGZIStream& stream) override;                                      // @ 0x1001d71d2 (thunk 0x1001d7214)
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4AutomataScriptSystem; }   // @ 0x1001de304 (thunk 0x1001de310)
    bool DoMessage(cIGZMessage2* message) override;                               // @ 0x1001d7256 (thunk 0x1001d7268)
    bool DoMessage(cGZMessage& message) override;                                 // @ 0x1001d7280 (thunk 0x1001d7290)

    // lookups (original vslots 0x28..0x90 of the primary view)
    const std::unordered_map<std::string, cSC4OccupantGroupTemplate*>& GetAutomataGroupMap() const { return automataGroups_; }   // @ 0x1001de2b8
    cSC4OccupantGroupTemplate* GetAutomataGroup(const cRZString& name);          // @ 0x1001d72a0
    cSC4OccupantGroupTemplate* GetAutomataGroup(std::uint32_t id) { return GetOccupantGroup(id); }   // @ 0x1001d72c0 (tail call of vslot 0x60)
    const std::unordered_map<std::string, cSC4OccupantGroupTemplate*>& GetOccupantGroupNameMap() const { return occupantGroups_; }   // @ 0x1001de2c2
    const std::unordered_map<std::uint32_t, cSC4OccupantGroupTemplate*>& GetOccupantGroupIDMap() const { return groupsById_; }       // @ 0x1001de2cc
    const std::list<IOccupantGroupTemplate*>& GetSpecialFilterOccupantGroups() const { return specialFilterGroups_; }                // @ 0x1001de2da
    cSC4OccupantGroupTemplate* GetOccupantGroup(const cRZString& name);          // @ 0x1001d773e
    cSC4OccupantGroupTemplate* GetOccupantGroup(std::uint32_t id);               // @ 0x1001d775e
    cSC4OccupantGroupTemplate* CreateOccupantGroup(const cRZString& name, std::uint32_t id, bool addToAutomataGroups);   // @ 0x1001d7798
    int GetDriveCommand(std::uint32_t keyCode) const;                            // @ 0x1001ddcfe
    const std::unordered_map<std::string, tControllerTemplate*>& GetControllerTemplateMap() const { return templates_; }   // @ 0x1001de2e8
    tControllerTemplate* CreateControllerTemplate(const cRZString& name, std::uint32_t type);   // @ 0x1001d72ce
    tControllerTemplate* GetControllerTemplate(const cRZString& name);           // @ 0x1001d771a
    const std::list<std::uint32_t>& GetTrackBuildingGroups() const { return trackBuildings_; }   // @ 0x1001de2f6

    // IControllerScriptSystem (controller manager seam; original vslots 0x50, 0x58, 0x60, 0x88)
    const std::list<IOccupantGroupTemplate*>& OccupantGroupTemplates() override { return specialFilterGroups_; }
    IOccupantGroupTemplate* FindGroupTemplate(const cIGZString& name) override;
    IOccupantGroupTemplate* FindGroupTemplate(std::uint32_t id) override;
    tControllerTemplate* FindControllerTemplate(const cIGZString& name) override;

    // script importers (read the tables of the Lua state, +0x28)
    bool ImportAttractorTemplates();                                              // @ 0x1001d8364
    bool ImportGeneratorTemplates();                                              // @ 0x1001d88be
    bool ImportAutomataGroups();                                                  // @ 0x1001d8f52
    bool ImportOccupantGroups();                                                  // @ 0x1001d99c4
    bool LinkControllerTemplates();                                               // @ 0x1001d9b9e
    bool LinkOccupantGroups();                                                    // @ 0x1001d9c74
    bool ImportDriveCommandKeyMap();                                              // @ 0x1001d9f84
    bool ImportTrackBuildingGroups();                                             // @ 0x1001da37c
    cSC4OccupantGroupTemplate* ImportSingleOccupantGroup(const cRZString& name, short tableIndex);   // @ 0x1001da596
    bool ImportAirplaneParams(int tableIndex, tAutomataDriveParams* params);      // @ 0x1001daec4
    bool ImportHelicopterParams(int tableIndex, tAutomataDriveParams* params);    // @ 0x1001db158
    bool ImportWatercraftParams(int tableIndex, tAutomataDriveParams* params);    // @ 0x1001db34c
    bool ImportPlayerDriveParams(int tableIndex, tAutomataPlayerDriveParams* params);   // @ 0x1001db8b8
    bool ImportKeyEffectsTable(int tableIndex, cSC4OccupantGroupTemplate* group); // @ 0x1001dbe26
    bool ReadGUIDTable(short tableIndex, std::list<cGZPersistResourceKey>& out);  // @ 0x1001dc52e
    bool ImportAutomataGroupNameList(int tableIndex, std::list<cRZString>& out);  // @ 0x1001dc6ae
    bool ImportTimeOfDayCurve(int tableIndex, tControllerTemplate* tmpl);         // @ 0x1001dc704
    bool ImportCalendarField(int tableIndex, tControllerTemplate* tmpl);          // @ 0x1001dc902
    bool ImportBehaviorTable(int tableIndex, tControllerTemplate* tmpl);          // @ 0x1001dc9ce
    bool ImportEffectTriggerList(int tableIndex, tControllerTemplate* tmpl);      // @ 0x1001dd250
    bool ImportControllerLifetime(int tableIndex, tControllerTemplate* tmpl);     // @ 0x1001dd504
    bool ImportLotFlags(int tableIndex, tControllerTemplate* tmpl);               // @ 0x1001dd56c
    bool ImportSFXList(int tableIndex, tControllerTemplate* tmpl);                // @ 0x1001dd7be
    bool ImportIgnoreStateList(int tableIndex, tControllerTemplate* tmpl);        // @ 0x1001dd8ca
    // @ 0x1001ddc44: a hex string (type 0x800e, a Lua string) goes through strtoul; anything else has to convert to uint32, else 0.
    // Inline so the other script commands that take a GUID (the city situation manager's) use it without this class.
    static std::uint32_t GetHexFromVariant(cIGZVariant& value) {
        if (value.GetType() == 0x800e) {
            cRZString text;
            value.GetValString(text);
            return static_cast<std::uint32_t>(std::strtoul(text.ToChar(), nullptr, 16));
        }
        std::uint32_t result = 0;
        if (value.GetValUint32(result)) return result;
        return 0;
    }

    cISCLua* Lua() const { return lua_; }
    void SetLua(cISCLua* lua);                                                    // test hook: adopt a prepared Lua state (counted)
    bool Initialized() const { return initialized_; }

private:
    bool initialized_ = false;      // +0x20
    std::uint32_t refCount_ = 0;    // +0x24
    cISCLua* lua_ = nullptr;        // +0x28
    bool scripting_ = false;        // +0x30
    std::unordered_map<std::string, cSC4OccupantGroupTemplate*> occupantGroups_;      // +0x38
    std::unordered_map<std::string, cSC4OccupantGroupTemplate*> automataGroups_;      // +0x60
    std::unordered_map<std::uint32_t, cSC4OccupantGroupTemplate*> groupsById_;        // +0x88
    std::list<IOccupantGroupTemplate*> specialFilterGroups_;                          // +0xb0 (groups without an id)
    std::unordered_map<std::string, tControllerTemplate*> templates_;                 // +0xc8
    std::list<std::uint32_t> trackBuildings_;                                         // +0xf0
    std::unordered_map<std::uint32_t, int> driveCommands_;                            // +0x108
};

}  // namespace sc4
