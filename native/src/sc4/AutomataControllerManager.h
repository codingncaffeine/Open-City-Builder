// cSC4AutomataControllerManager — owns every automata controller of the city (56 symbols, 0x1000f3efc..0x1000fbc26): it creates
// controllers from script templates for occupants (buildings, props, networks), keeps them in a spatial grid, ticks them with the
// city clock, turns them on/off with the camera and effect triggers, answers attraction queries for the automata and saves them.
// Class id / interface 0xca14ca03 / 0xca14c9fc; message target 0x452294aa; serializable 0xe4fda3d4 (also answered for iid 1).
// Original layout (sub-objects: +0 manager interface, +8 legacy message target (cheat codes), +0x10 cIGZMessageTarget2, +0x18 serializable):
//   +0x20 initialised  +0x21 "post city init pending" (set by Read)  +0x24 refcount  +0x28 clock*  +0x30 cSC4ControllerGrid
//   +0x58 tControllerTime now  +0x60 tControllerTime last tick  +0x68 script system*  +0x70 last view position  +0x7c scrolling
//   +0x7d enabled (cheat "Controllers on|off")  +0x80 map<occupant ptr, list<controller*>>  +0xa8 map<occupant ptr, tControllerQInfo>.
// Controllers without an occupant live under the key 0. The map keys are counted references to the occupants.
//
// Messages handled by DoMessage(cIGZMessage2*): 0x86ad10ee tick (data2, data3 as float), 0x89d3e9f3 (queue points invalid), 0x99ef1142
// occupant inserted (AddControllers unless it carries property 0x29244db), 0x99ef1143 occupant removed (queue points invalid when the
// occupant answers iid 0xa821ef94; RemoveControllers), 0xa6b79602 / 0xa6b79612 camera changed (the latter only after moving farther than the
// per-zoom granularity), 0xcad61e00 / 0xead61e16 scroll start / end, 0xaa1d07f3 source data changed, 0xaa375fe8 effect triggers changed,
// 0x44778230 hour passed, 0x26d31ec1 post city init, 0x09d92fc3 automaton removed (data2 == 3), 0xebff18a3 occupant group added.
//
// Quirks kept: RemoveController(occupant, name) answers false even when it removed controllers; removing the last controller of an occupant
// frees its queue info too; ClearControllers empties the grid (it has no tracts until the next Init); Read replaces the clock pointer
// without releasing the old one and drops the controllers of an occupant that already has an entry; DoAnimationTick only advances
// controllers while the enabled flag is set and one of the two city automata layers is running; GetCompositeAttractionEffect divides by the
// distance without a zero check (a controller exactly on the occupant gives NaN components); RemoveControllers(null occupant)
// dereferences null in the original (guarded here).
#pragma once
#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>

#include "gz/WinInterfaces.h"
#include "gz/cGZMessageServer.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZSerializable.h"
#include "sc4/AutomataAttractor.h"
#include "sc4/AutomataControllerGrid.h"
#include "sc4/OccupantManager.h"   // kGZIID_cIGZMessageTarget2

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4AutomataControllerManager = 0xca14ca03;
constexpr std::uint32_t kGZIID_cISC4AutomataControllerManager = 0xca14c9fc;
constexpr std::uint32_t kAutomataCheatControllers = 0xaba2b5bb;      // cheat code "Controllers"
#ifndef SC4_MESSAGE_CHEAT_CODE_DEFINED   // defined by both automata halves (AutomataControllerManager.h, AutomataManager.h)
#define SC4_MESSAGE_CHEAT_CODE_DEFINED
constexpr std::uint32_t kMessageCheatCode = 0x230e27ac;
#endif
constexpr std::uint32_t kPropertyNoControllers = 0x29244db;           // an occupant with this property gets no automatic controllers
constexpr std::uint32_t kPropertyControllerGroups = 0xaa1dd396;       // occupant property: occupant group ids that name controller groups
constexpr std::uint32_t kMessageControllerRemoved = 0xea298d50;       // posted after a controller is removed (void1 = the controller)
constexpr std::uint32_t kOccupantIidQueueReset = 0xa821ef94;

// Messages (the first 12 are registered by Init with the message server)
constexpr std::uint32_t kCtrlMsgTick = 0x86ad10ee, kCtrlMsgQueueReset = 0x89d3e9f3, kCtrlMsgOccupantInserted = 0x99ef1142,
                        kCtrlMsgOccupantRemoved = 0x99ef1143, kCtrlMsgCameraMoved = 0xa6b79602, kCtrlMsgCameraChanged = 0xa6b79612,
                        kCtrlMsgScrollStart = 0xcad61e00, kCtrlMsgScrollEnd = 0xead61e16, kCtrlMsgSourceDataChanged = 0xaa1d07f3,
                        kCtrlMsgEffectTriggers = 0xaa375fe8, kCtrlMsgHourPassed = 0x44778230, kCtrlMsgPostCityInit = 0x26d31ec1,
                        kCtrlMsgAutomatonRemoved = 0x09d92fc3, kCtrlMsgOccupantGroupAdded = 0xebff18a3;
constexpr int kControllerManagerChannel = 0xc0;                       // SL::ATM() notification channel (the tick source)

// scripts: occupant group templates (cSC4OccupantGroupTemplate) and the controller templates they list
class IOccupantGroupTemplate {
public:
    virtual ~IOccupantGroupTemplate() = default;
    virtual bool Matches(cISC4Occupant* occupant) = 0;                   // vslot 0x18
    virtual float SpawnChance() = 0;                                     // +0x40 (0..1)
    virtual const std::list<tControllerTemplate*>& ControllerTemplates() = 0;   // +0x48
};
class IControllerScriptSystem : public cIGZUnknown {                     // SL::spCity vslot 0x180 (cSC4AutomataScriptSystem)
public:
    virtual const std::list<IOccupantGroupTemplate*>& OccupantGroupTemplates() = 0;   // vslot 0x50
    virtual IOccupantGroupTemplate* FindGroupTemplate(const cIGZString& name) = 0;    // vslot 0x58
    virtual IOccupantGroupTemplate* FindGroupTemplate(std::uint32_t id) = 0;          // vslot 0x60
    virtual tControllerTemplate* FindControllerTemplate(const cIGZString& name) = 0;  // vslot 0x88
};
class IControllerClock : public cIGZUnknown {                            // SL::spCity vslot 0x2b0 (24 hour clock)
public:
    virtual int Hour() = 0;                                              // vslot 0x50
    virtual int Minute() = 0;                                            // vslot 0x68
};
class INetworkOccupantView {                                             // cISC4NetworkOccupant vslot 0x180 -> object with vslot 0x40
public:
    virtual ~INetworkOccupantView() = default;
    virtual void GetWorldPosition(float* xyz) = 0;
};

class IControllerManagerHost {
public:
    virtual ~IControllerManagerHost() = default;
    virtual IControllerClock* AcquireClock() = 0;                        // SL::spCity vslot 0x2b0 (the manager AddRefs it)
    virtual IControllerScriptSystem* AcquireScriptSystem() = 0;          // SL::spCity vslot 0x180 (the manager AddRefs it)
    virtual cIGZMessageServer2* MessageServer() = 0;                     // SL::spMS2 (null: none)
    virtual void AddTickNotification(cIGZMessageTarget2* target, int channel) = 0;      // SL::ATM() vslot 0x28
    virtual void RemoveTickNotification(cIGZMessageTarget2* target, int channel) = 0;   // SL::ATM() vslot 0x30
    virtual bool CheatManagerAvailable() = 0;                            // SL::spCCM != 0
    virtual bool DebugFunctionalityEnabled() = 0;                        // SL::spSC4 vslot 0xa8
    virtual void RegisterCheat(std::uint32_t id, const char* name) = 0;  // spCCM vslot 0x18
    virtual void UnregisterCheat(std::uint32_t id) = 0;                  // spCCM vslot 0x20
    virtual void AddCheatListener(cIGZMessageTarget* target) = 0;        // spCCM vslot 0x28
    virtual void RemoveCheatListener(cIGZMessageTarget* target) = 0;     // spCCM vslot 0x30
    virtual bool AutomataLayersRunning() = 0;                            // city vslot 0x158 / 0x160 objects, vslot 0x30 (either)
    virtual const float* ViewPosition() = 0;                             // SL::spViewUtilities + 0x44
    virtual int ViewZoom() = 0;                                          // SL::spViewUtilities + 0x14
    virtual cSC4AutomataController* CreateController(std::uint32_t classKey) = 0;   // GZCOM()->CreateInstance(classKey, 0x6a1857f8)
    virtual const void* LotOf(cISC4Occupant* occupant) = 0;              // city vslot 0x108 -> vslot 0x70(occupant); identity only
    virtual double RandomDouble() = 0;                                   // cRZRandom(0xffffffff).RandomDoubleUniform()
    virtual void Report(const std::string& message) { (void)message; }   // the error texts the original formats and drops
};
IControllerManagerHost* ControllerManagerHost();
void SetControllerManagerHost(IControllerManagerHost* host);

class cISC4AutomataControllerManager : public cIGZUnknown {
public:
    virtual bool Init() = 0;                                              // 0x18
    virtual bool Shutdown() = 0;                                          // 0x20
    virtual cSC4AutomataController* AddControllerFromTemplateName(cISC4Occupant* occupant, const cIGZString& name, const float* position) = 0;   // 0x28
    virtual cSC4AutomataController* AddControllerFromTemplate(cISC4Occupant* occupant, tControllerTemplate* tmpl, const float* position) = 0;   // 0x30
    virtual bool RemoveController(cISC4Occupant* occupant, const cIGZString& name) = 0;     // 0x38
    virtual bool RemoveController(cSC4AutomataController* controller) = 0;                  // 0x40
    virtual bool AddControllersFromOccupantGroup(const cIGZString& group, cISC4Occupant* occupant, const float* position) = 0;   // 0x48
    virtual bool AddControllersFromOccupantGroup(std::uint32_t group, cISC4Occupant* occupant, const float* position) = 0;       // 0x50
    virtual bool RemoveControllersFromOccupantGroup(const cIGZString& group, cISC4Occupant* occupant) = 0;   // 0x58
    virtual bool RemoveControllersFromOccupantGroup(std::uint32_t group, cISC4Occupant* occupant) = 0;       // 0x60
    virtual bool AddControllers(cISC4Occupant* occupant) = 0;             // 0x68
    virtual bool RemoveControllers(cISC4Occupant* occupant) = 0;          // 0x70
    virtual bool MoveControllers(cISC4Occupant* occupant) = 0;            // 0x78
    virtual bool MoveController(cSC4AutomataController* controller) = 0; // 0x80
    virtual cSC4AutomataController* GetController(cISC4Occupant* occupant, const cIGZString& name) = 0;       // 0x88
    virtual cSC4AutomataController* GetController(cISC4Occupant* occupant, const tControllerTemplate* tmpl) = 0;   // 0x90
    virtual cSC4AutomataController* GetController(const cS3DVector3& position, const cIGZString& name, float radius) = 0;   // 0x98
    virtual bool GetRelevantControllersByWorldPos(float x, float z, std::uint32_t controllerType, std::list<cSC4AutomataController*>& out) = 0;   // 0xa0
    virtual void ReleaseControllerList(std::list<cSC4AutomataController*>& list) = 0;      // 0xa8
    virtual cSC4AutomataController* GetCompositeAttractionEffect(cISC4Occupant* occupant, cS3DVector3& direction, float& distance, float& strength) = 0;   // 0xb0
    virtual float GetCompositeAttractionCommotion(INetworkOccupantView* networkOccupant) = 0;   // 0xb8
    virtual const ControllerTime& GetControllerTime() const = 0;          // 0xc0
};

class cSC4AutomataControllerManager : public cISC4AutomataControllerManager, public cIGZMessageTarget, public cIGZMessageTarget2,
                                      public cIGZSerializable {
public:
    cSC4AutomataControllerManager() = default;                            // @ 0x1000f3efc
    virtual ~cSC4AutomataControllerManager();                             // @ 0x1000f3fc8

    // cIGZUnknown (4 views, one count)
    bool QueryInterface(std::uint32_t iid, void** out) override;          // @ 0x1000f8278 (thunks 0x1000f82be, 0x1000f82cc, 0x1000f82da)
    std::uint32_t AddRef() override { return ++refCount_; }               // @ 0x1000f82e8 (thunks 0x1000f82f6, 0x1000f8304, 0x1000f8312)
    std::uint32_t Release() override;                                     // @ 0x1000f8320 (thunks 0x1000f837e, 0x1000f839e, 0x1000f83be)
    void FinalRelease();                                                  // @ 0x1000f833c (Shutdown, then delete)

    bool Init() override;                                                 // @ 0x1000f4076
    bool Shutdown() override;                                             // @ 0x1000f43b8
    cSC4AutomataController* AddControllerFromTemplateName(cISC4Occupant* occupant, const cIGZString& name, const float* position) override;   // @ 0x1000f6740
    cSC4AutomataController* AddControllerFromTemplate(cISC4Occupant* occupant, tControllerTemplate* tmpl, const float* position) override;   // @ 0x1000f6796
    bool RemoveController(cISC4Occupant* occupant, const cIGZString& name) override;     // @ 0x1000f6fcc
    bool RemoveController(cSC4AutomataController* controller) override;                  // @ 0x1000f71c8
    bool AddControllersFromOccupantGroup(const cIGZString& group, cISC4Occupant* occupant, const float* position) override;   // @ 0x1000f6124
    bool AddControllersFromOccupantGroup(std::uint32_t group, cISC4Occupant* occupant, const float* position) override;       // @ 0x1000f6198
    bool RemoveControllersFromOccupantGroup(const cIGZString& group, cISC4Occupant* occupant) override;   // @ 0x1000f620c
    bool RemoveControllersFromOccupantGroup(std::uint32_t group, cISC4Occupant* occupant) override;       // @ 0x1000f63fc
    bool RemoveControllersFromOccupantGroup(IOccupantGroupTemplate* group, cISC4Occupant* occupant);     // @ 0x1000f6246 (null group = every controller)
    bool AddControllers(cISC4Occupant* occupant) override;                // @ 0x1000f5fee
    bool RemoveControllers(cISC4Occupant* occupant) override;             // @ 0x1000f646e
    bool MoveControllers(cISC4Occupant* occupant) override;               // @ 0x1000f6480
    bool MoveController(cSC4AutomataController* controller) override;     // @ 0x1000f6710
    cSC4AutomataController* GetController(cISC4Occupant* occupant, const cIGZString& name) override;       // @ 0x1000f727a
    cSC4AutomataController* GetController(cISC4Occupant* occupant, const tControllerTemplate* tmpl) override;   // @ 0x1000f72bc
    cSC4AutomataController* GetController(const cS3DVector3& position, const cIGZString& name, float radius) override;   // @ 0x1000f7332
    bool GetRelevantControllersByWorldPos(float x, float z, std::uint32_t controllerType, std::list<cSC4AutomataController*>& out) override;   // @ 0x1000f7550
    void ReleaseControllerList(std::list<cSC4AutomataController*>& list) override;      // @ 0x1000f7b36
    cSC4AutomataController* GetCompositeAttractionEffect(cISC4Occupant* occupant, cS3DVector3& direction, float& distance, float& strength) override;   // @ 0x1000f762c
    float GetCompositeAttractionCommotion(INetworkOccupantView* networkOccupant) override;   // @ 0x1000f7a08
    const ControllerTime& GetControllerTime() const override { return now_; }            // @ 0x1000fbc10

    tControllerQInfo* GetControllerQInfo(cISC4Occupant* occupant, bool create);          // @ 0x1000f7b66
    void ClearControllerQInfo();                                          // @ 0x1000f454c
    void ClearControllers();                                              // @ 0x1000f459a
    void HandlePostCityInit();                                            // @ 0x1000f4642
    void HandleViewChanged();                                             // @ 0x1000f5a68
    void DoAnimationTick(std::uint32_t data, float deltaMilliseconds);    // @ 0x1000f5b12
    void UpdateControllers();                                             // @ 0x1000f5e78
    void SendControllerMessage(std::uint32_t type, cSC4AutomataController* controller);   // @ 0x1000f5ef2

    bool Write(cIGZOStream& stream) const override;                       // @ 0x1000f48de (thunk 0x1000f4aea)
    bool Read(cIGZIStream& stream) override;                              // @ 0x1000f4af8 (thunk 0x1000f53e8)
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4AutomataControllerManager; }   // @ 0x1000fbc1a (thunk 0x1000fbc26)
    bool DoMessage(cGZMessage& message) override;                         // @ 0x1000f53f6 (thunk 0x1000f5548)
    bool DoMessage(cIGZMessage2* message) override;                       // @ 0x1000f5556 (thunk 0x1000f5d66)

    cSC4ControllerGrid& Grid() { return grid_; }
    bool Enabled() const { return enabled_; }
    void SetEnabled(bool on) { enabled_ = on; }
    bool Initialized() const { return initialized_; }
    std::size_t OccupantEntryCount() const { return controllers_.size(); }
    std::size_t QueueEntryCount() const { return queues_.size(); }
    const std::list<cSC4AutomataController*>* ControllersOf(cISC4Occupant* occupant) const {
        auto it = controllers_.find(reinterpret_cast<std::uintptr_t>(occupant));
        return it == controllers_.end() ? nullptr : &it->second.controllers;
    }

private:
    struct ControllerEntry { cISC4Occupant* occupant = nullptr; std::list<cSC4AutomataController*> controllers; };
    struct QueueEntry { cISC4Occupant* occupant = nullptr; tControllerQInfo info; };
    static std::uintptr_t KeyOf(const cISC4Occupant* occupant) { return reinterpret_cast<std::uintptr_t>(occupant); }
    void DropControllerEntry(cISC4Occupant* occupant);
    ControllerTime ElapsedSinceLastTick(const ControllerTime& at) const;
    void EraseController(std::list<cSC4AutomataController*>& list, std::list<cSC4AutomataController*>::iterator it);

    bool initialized_ = false;                       // +0x20
    bool postCityInitPending_ = false;               // +0x21
    std::uint32_t refCount_ = 0;                     // +0x24
    IControllerClock* clock_ = nullptr;              // +0x28
    cSC4ControllerGrid grid_;                        // +0x30
    ControllerTime now_;                             // +0x58
    ControllerTime last_;                            // +0x60
    IControllerScriptSystem* script_ = nullptr;      // +0x68
    float viewPosition_[3] = {0, 0, 0};              // +0x70
    bool scrolling_ = false;                         // +0x7c
    bool enabled_ = true;                            // +0x7d
    std::unordered_map<std::uintptr_t, ControllerEntry> controllers_;   // +0x80
    std::unordered_map<std::uintptr_t, QueueEntry> queues_;             // +0xa8
};

}  // namespace sc4
