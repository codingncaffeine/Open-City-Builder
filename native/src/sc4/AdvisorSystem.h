// cSC4AdvisorSystem — the city's advisor system: owns the advisor objects (cSC4Advisor), feeds them the events the
// rest of the game pushes, relays the advice messages and keeps the advisor window. Recovered from Sim City 4 Deluxe
// Edition 1.2.1 (46 member functions at 0x100294eb0..0x100297fbc, object 0xf8 bytes, class id 0x4a0dd616, interface
// cISC4AdvisorSystem 0x4a0dd617). It is created by GZCOM (GZCOM_CreatecSC4AdvisorSystem @ 0x100294ffb allocates 0xf8
// bytes and calls the constructor) and registered by the city as its "AdvisorSystem" component.
//
// Original object layout (the five vtable views at +0..+0x2f):
//   +0x00 cISC4AdvisorSystem vptr     +0x08 cIGZMessageTarget2 vptr (DoMessage(cIGZMessage2*))
//   +0x10 cIGZMessageTarget vptr      (DoMessage(cGZMessage&))
//   +0x18 cIGZSerializable vptr       +0x20 cRZUnknown vptr (its refcount lives at +0x28)
//   +0x2c int advice popup priority thresholds[3] (easy / medium / hard; init_advice_popup_priority_thresholds sets
//          them to 100, keyed by the "low" / "medium" / "high" rows of ADVICE_POPUP_PRIORITIES_BY_SKILL_LEVEL)
//   +0x38 bool loaded from a save         +0x39 bool initialized
//   +0x40 cRZRandom random (seed 0xffffffff)      +0x58 int last date number the yearly tick ran for
//   +0x60 std::map<int, cRZAutoRefCount<cISC4Advisor>> advisors (keyed by the advisor's "id")
//   +0x78 cISCLua* scripting context (counted)    +0x80 cSC4AdvisorUIAdvisorWinProc* advisor window (counted)
//   +0x88 bool scripts were reloaded (checked by the tick, only a reload sets it)   +0x90 cIGZStringDetokenizer*
//   +0x98 std::map<int, std::set<cISC4Advisor*>> event subscribers (event id -> advisors)
//   +0xb0 std::vector<cRZAutoRefCount<cISC4Advice>> context advice stack (PushContextAdvice / PopContextAdvice)
//   +0xc8 std::vector<int> Lua tags created in Init (GetDataTag)
//   +0xe0 cRZTimer tick timer (milliseconds)
//
// The advisor messages (the ids DoMessage routes, compared as signed 32-bit values like the original's jump table):
//   0x26d31ec1 city started: once the city is established, tick and (before the first dated tick) simulate a month
//   0x26d31ec4 another city-started flavour: push event 0xaa31a210, then the same month simulation
//   0xca5501f2 the periodic tick (posted to itself): yearly/28th-of-month re-evaluation, otherwise at most every 501 ms
//   0x230e27ac a cheat code was typed: data1 = cheat id, void2 = the typed text ("guids", "trigger", "print",
//              "refresh", "reload" switches; the DollyLlama cheat toggles the llama head)
// The advisor notifications AddAdvisor / RemoveAdvisor / kill_advisors post:
//   0x8a09fa6a advisor added (object = cISC4Advisor)   0x8a09fa69 advisor removed   0x8a09fa6b all advisors killed
// The tick broadcasts the advice triggers the advisors evaluate through cISC4Advisor::Simulate; the frequency / timeout
// limits themselves live in cSC4Advice (GetFrequencyInDays / GetTimeOutInDays), not here.
//
// Quirks kept:
//  * Init ends by releasing any existing advisor window and creating a fresh one, also when it returns early because
//    the system was already initialized.
//  * reload_data looks up the global "_on_reload" and calls it only when it is a function; a non-function value is left
//    on the Lua stack (the original never pops it).
//  * Shutdown unregisters the advice detokenizer with the Lua context instead of the `this` context it registered with
//    (harmless: the detokenizer set is ordered by the method pointer).
//  * load_advisor inserts the advisor straight into the map (it does not call AddAdvisor), notifies "advisor added"
//    and returns 0 whatever happened.
//  * the last-date field (+0x58) holds the *julian day number* of the last ticked day, not a year, and the yearly
//    re-evaluation runs on any new day whose day-of-month is the 28th.
//  * GetAdvicePopupPriorityThreshold indexes thresholds by the city difficulty value (1 = medium, 2 = hard, anything
//    else easy) without bounds checks, and the three thresholds are uninitialised until Init.
//  * gc_tag_method reads the thread object from the slot immediately before the lua_State, queries it for cISCLua and
//    releases the userdata of the two tags created in Init (the "gc" tag method that keeps reference-counted objects
//    alive from Lua).
//  * the process-wide instance is a plain static that every constructor overwrites and every destructor clears, so
//    creating a second system steals the instance from the first (the original's spInstance has no guard).
//  * the port keeps the original's stack restore with GetTop / SetTop on the scripting context; the original wraps it
//    in nsIGZLua5::cStackPosSaver (thread view of the Lua state).
//
// Everything the original reaches through SL::sp* / GZCOM() / SCLuaHelp goes through IAdvisorSystemHost; the advisor,
// advice and message interfaces are the recovered ones (sc4/Advisor.h, sc4/Advice.h, gz/cIGZMessage2.h). A null host
// service is treated as absent (the original dereferences spLua / spMS2 / spCity unconditionally).
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "gz/cGZMessageServer.h"
#include "gz/cIGZIStream.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZOStream.h"
#include "gz/cIGZSerializable.h"
#include "gz/cIGZString.h"
#include "lua/ISCLua.h"
#include "rz/cRZAutoRefCount.h"
#include "rz/cRZRandom.h"
#include "rz/cRZString.h"
#include "rz/cRZTimer.h"
#include "rz/cRZUnknown.h"
#include "sc4/Advice.h"
#include "sc4/Advisor.h"
#include "sc4/StringDetokenizer.h"

class cGZCheatCodeManager;
class cIGZDate;
struct lua_State;

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4AdvisorSystem = 0x4a0dd616;   // GetGZCLSID @ 0x100297fb0
constexpr std::uint32_t kGZIID_cISC4AdvisorSystem = 0x4a0dd617;    // the +0x00 view
constexpr std::uint32_t kGZIID_cGZMessageTargetLegacy = 0xe98b2f57;   // the +0x10 view
constexpr std::uint32_t kAdvisorPackageId = 0x4a5e8ef6;            // the "advisors" script package
constexpr const char* kAdvisorTableNameText = "advisors";
constexpr std::uint32_t kAdvisorCheatLlamaHead = 0xaab7f42a;       // registered with the text "DollyLlama"
constexpr std::uint32_t kAdvisorCheatCheats = 0xa919d25e;          // registered with the text "Adv" (debug builds)
constexpr const char* kAdvisorCheatLlamaHeadText = "DollyLlama";
constexpr const char* kAdvisorCheatCheatsText = "Adv";
constexpr int kLuaOpenAllLibraries = 0x7e;                         // cISCLua::Open argument in Init
constexpr std::uint32_t kAdvisorMessageCityStarted = 0x26d31ec1;
constexpr std::uint32_t kAdvisorMessageCityStarted2 = 0x26d31ec4;
constexpr std::uint32_t kAdvisorMessageTick = 0xca5501f2;
constexpr std::uint32_t kAdvisorMessageCheat = 0x230e27ac;
constexpr std::uint32_t kAdvisorMessageTriggered = 0xea917750;     // data1 = show text keys
constexpr std::uint32_t kAdvisorMessageAdviceChanged = 0x4a91dd6e; // data1 = advice id
constexpr std::uint32_t kAdvisorMessageAdviceDump = 0x4a92ecef;    // data1 = advice id
constexpr std::uint32_t kAdvisorMessageAdviceReset = 0x8a917759;
constexpr std::uint32_t kAdvisorNotifyAdvisorAdded = 0x8a09fa6a;
constexpr std::uint32_t kAdvisorNotifyAdvisorRemoved = 0x8a09fa69;
constexpr std::uint32_t kAdvisorNotifyAdvisorsKilled = 0x8a09fa6b;
constexpr std::uint32_t kAdvisorCityStartedEvent = 0xaa31a210;     // PushEvent id in the 0x26d31ec4 handler
constexpr int kAdvisorYearlyDayOfMonth = 0x1c;                     // the 28th
constexpr std::uint32_t kAdvisorTickIntervalMs = 0x1f5;            // at most one advisor pass per 501 ms
constexpr int kAdvisorDefaultPopupPriority = 100;
constexpr std::uint16_t kAdvisorStreamVersion = 1;

using AdvisorMap = std::map<int, cRZAutoRefCount<cISC4Advisor>>;
using AdvisorEventMap = std::map<int, std::set<cISC4Advisor*>>;

// The advisor window (cSC4AdvisorUIAdvisorWinProc, sc4/AdvisorUIAdvisorWinProc.h): the system only owns it, shows / hides it and asks
// whether it is visible. Original calls: Init @ 0x1002b5502, Shutdown @ 0x1002b581c, ShowWindow @ 0x1002b9634,
// HideWindow @ 0x1002b6dd0, IsVisible @ 0x1002b9712.
class IAdvisorWinProc {
public:
    virtual ~IAdvisorWinProc() = default;
    virtual void AddRef() = 0;
    virtual void Release() = 0;
    virtual void Init() = 0;
    virtual void Shutdown() = 0;
    virtual void ShowWindow() = 0;
    virtual void HideWindow() = 0;
    virtual bool IsVisible() = 0;
};

// What cSC4AdvisorSystem needs from the running game (one method per original global / engine call).
class IAdvisorSystemHost {
public:
    virtual ~IAdvisorSystemHost() = default;
    virtual cISCLua* Lua() = 0;                                        // SL::spLua (borrowed cISCLua view)
    virtual cIGZMessageServer2* Messages() = 0;                        // SL::spMS2
    virtual cGZCheatCodeManager* CheatCodeManager() = 0;               // SL::spCCM (iid 0xa1085722, borrowed)
    virtual cSCStringDetokenizer* Detokenizer() = 0;                   // SL::spStringDetokenizer (borrowed)
    virtual IAdvisorWinProc* CreateAdvisorWinProc() = 0;               // new cSC4AdvisorUIAdvisorWinProc, refcount 0
    virtual cIGZUnknown* City() = 0;                                   // SL::spCity (borrowed cISC4City), may be null
    virtual bool CityEstablished(cIGZUnknown* city) = 0;               // city vslot 0xb0 (GetEstablished)
    virtual int CityDifficultyLevel() = 0;                             // city vslot 0xc0 (GetDifficultyLevel)
    virtual bool DebugFunctionalityEnabled() = 0;                      // SL::spSC4 vslot 0xa8
    virtual void GetDataDirectory(cIGZString& out, int language) = 0;  // SL::spSC4 vslot 0x198
    virtual void GetPluginDirectory(cIGZString& out) = 0;              // SL::spSC4 vslot 0x1a8
    virtual void SetTutorialRegistryLua(cISCLua* lua) = 0;             // SL::spSC4 vslot 0x178 -> SetLua
    virtual bool CreateAdvisor(std::uint32_t classId, std::uint32_t iid, cISC4Advisor** out) = 0;   // GZCOM vslot 0x18
    virtual DetokenizeMethod AdviceDetokenizerFunction() = 0;          // gAdviceDetokenizerFunction @ 0x1003fb0a0
    // nsAdviceAPI::gLUAExtensions (filled by cLUAExtensionsCollector; not recovered): the script extension table.
    struct LuaExtension {
        const char* name;            // +0x00 the table key / global name
        cISCLua::CFunction function; // +0x08 the C function registered as "_tf_"
        bool global;                 // +0x10 true: registered in the globals on Init, false: in "game" on reload
    };
    virtual const LuaExtension* LuaExtensions(std::size_t& count) = 0;
    virtual void UpdateResourcesFromDirectory(std::uint32_t package, const char* directory) = 0;   // SCLuaHelp
    virtual void LoadPackageScripts(cISCLua* lua, std::uint32_t package, int flags) = 0;           // SCLuaHelp
    virtual cIGZDate* SimDate() = 0;                                   // SL::spSimulator vslot 0x30 (borrowed)
    virtual void SimStartDate(cIGZDate& out) = 0;                      // simulator vslot 0x28
    virtual bool SimulatorAnyPaused() = 0;                             // simulator vslot 0x90
    virtual void ToggleLlamaCheat() = 0;                               // cSC4AdvisorUIImageWinProc::mbLlamaCheatActive ^= 1
    // The scripting host that owns `state` with one reference (the gc tag method). The original reads the object stored in
    // the pointer before the state and asks it for 0xea10c4a2; the default does that, a host whose Lua keeps something else
    // there (the vendored Lua 5.0 keeps its thread object) answers through its own lookup.
    virtual cISCLua* LuaFromState(lua_State* state);
};
IAdvisorSystemHost* AdvisorSystemHost();
void SetAdvisorSystemHost(IAdvisorSystemHost* host);

// cISC4AdvisorSystem (the interface at +0x00; the slot offsets are the original vslots).
class cISC4AdvisorSystem : public cIGZUnknown {
public:
    virtual bool Init() = 0;                                                    // 0x18
    virtual bool Shutdown() = 0;                                                // 0x20
    virtual bool AddAdvisor(cISC4Advisor* advisor) = 0;                         // 0x28
    virtual bool RemoveAdvisor(int id) = 0;                                     // 0x30
    virtual cISC4Advisor* GetAdvisor(int id) = 0;                               // 0x38
    virtual AdvisorMap* GetAdvisors() = 0;                                      // 0x40
    virtual cRZRandom* GetRandom() = 0;                                         // 0x48
    virtual cISCLua* GetScriptingContext() = 0;                                 // 0x50
    virtual bool DoLink(const cIGZString& link) = 0;                            // 0x58
    virtual bool SetGlobalValue(const char* name, double value) = 0;            // 0x60
    virtual bool SetGlobalValue(const char* name, cIGZString* value) = 0;       // 0x68
    virtual bool SetLocalValue(const char* name, double value, cISC4AdviceSubject* subject) = 0;   // 0x70
    virtual bool PushEvent(int eventId, cISC4AdviceSubject* subject) = 0;       // 0x78
    virtual bool SubscribeToEvent(int eventId, cISC4Advisor* advisor) = 0;      // 0x80
    virtual bool UnsubscribeFromEvent(int eventId, cISC4Advisor* advisor) = 0;  // 0x88
    virtual void PushContextAdvice(cISC4Advice* advice) = 0;                    // 0x90
    virtual cISC4Advice* PopContextAdvice() = 0;                                // 0x98
    virtual cISC4Advice* GetContextAdvice() = 0;                                // 0xa0
    virtual int GetAdvicePopupPriorityThreshold() const = 0;                    // 0xa8
    virtual int GetDataTag(int index) = 0;                                      // 0xb0
};

class cSC4AdvisorSystem final : public cISC4AdvisorSystem,
                                public cIGZMessageTarget2,
                                public cIGZMessageTarget,
                                public cIGZSerializable,
                                public cRZUnknown {
public:
    cSC4AdvisorSystem();                                   // @ 0x100295048
    ~cSC4AdvisorSystem() override;                         // @ 0x10029525e (aliases 0x100295408/0x100295412,
                                                           //   deleting 0x100295420, thunk 0x10029544e)

    // cIGZUnknown (all five views share the reference count of the +0x20 subobject)
    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x1002962e4 (thunks 0x10029633c/34a/358/366)
    std::uint32_t AddRef() override;                               // @ 0x100296374 (thunks 0x100296382/390/39e/3ac)
    std::uint32_t Release() override;                              // @ 0x1002963b6 (thunks 0x1002963c4/3d2/3e0/3ee)

    // cISC4AdvisorSystem
    bool Init() override;                                          // @ 0x10029558e
    bool Shutdown() override;                                      // @ 0x10029613c
    bool AddAdvisor(cISC4Advisor* advisor) override;               // @ 0x100296d2c
    bool RemoveAdvisor(int id) override;                           // @ 0x100296f68
    cISC4Advisor* GetAdvisor(int id) override;                     // @ 0x10029713e
    AdvisorMap* GetAdvisors() override;                            // @ 0x10029717e
    cRZRandom* GetRandom() override;                               // @ 0x100297580
    cISCLua* GetScriptingContext() override;                       // @ 0x100297188
    bool DoLink(const cIGZString& link) override;                  // @ 0x100297192
    bool SetGlobalValue(const char* name, double value) override;  // @ 0x100297ab8
    bool SetGlobalValue(const char* name, cIGZString* value) override;   // @ 0x100297a1c
    bool SetLocalValue(const char* name, double value, cISC4AdviceSubject* subject) override;   // @ 0x100297b30
    bool PushEvent(int eventId, cISC4AdviceSubject* subject) override;          // @ 0x100297c98
    bool SubscribeToEvent(int eventId, cISC4Advisor* advisor) override;         // @ 0x100297d4e
    bool UnsubscribeFromEvent(int eventId, cISC4Advisor* advisor) override;     // @ 0x100297ed8
    void PushContextAdvice(cISC4Advice* advice) override;          // @ 0x100297420
    cISC4Advice* PopContextAdvice() override;                      // @ 0x1002974bc
    cISC4Advice* GetContextAdvice() override;                      // @ 0x100297512
    int GetAdvicePopupPriorityThreshold() const override;          // @ 0x100297530
    int GetDataTag(int index) override;                            // @ 0x100297f88

    // own virtuals (the primary vtable slots 0x19.. of the original, after the two destructor slots)
    IAdvisorWinProc* GetAdvisorWinProc();                          // @ 0x100297fa2
    void startAdvisorWindow();                                     // @ 0x10029758a
    void ShowAdvisorWindow();                                      // @ 0x100297632
    void HideAdvisorWindow();                                      // @ 0x10029764a
    bool IsAdvisorWindowVisible();                                 // @ 0x100297618
    void RemoveWinProcs();                                         // @ 0x100297662

    // cIGZSerializable
    bool Write(cIGZOStream& stream) const override;                // @ 0x1002976a6 (thunk 0x100297756)
    bool Read(cIGZIStream& stream) override;                       // @ 0x100297764 (thunk 0x1002978a0)
    std::uint32_t GetGZCLSID() const override;                     // @ 0x100297fb0 (thunk 0x100297fbc)

    // cIGZMessageTarget2 / cIGZMessageTarget
    bool DoMessage(cIGZMessage2* message) override;                // @ 0x1002964ec (thunk 0x100296d1a)
    bool DoMessage(cGZMessage& message) override;                  // @ 0x100297570 (thunk 0x100297578)

    // helpers (the original's file-static / non-virtual members)
    void kill_advisors();                                          // @ 0x10029545c
    bool register_api(bool open);                                  // @ 0x1002959d0
    bool reload_data();                                            // @ 0x100295ae8
    bool load_advisors();                                          // @ 0x100295d1a
    bool update_advisors();                                        // @ 0x100295dbe
    void init_advice_popup_priority_thresholds();                  // @ 0x100295eea
    void simulate_all_advisors(int period);                        // @ 0x1002963f8
    void tick_self();                                              // @ 0x100296468
    bool load_advisor(int row);                                    // @ 0x1002978b0
    static int gc_tag_method(lua_State* state);                    // @ 0x100294eb0

    // process-wide instance (the original's spInstance; the advisor / advice / city-situation classes reach it)
    static cSC4AdvisorSystem* Instance() { return instance_; }

    // test / glue accessors
    AdvisorMap& Advisors() { return advisors_; }                   // +0x60
    AdvisorEventMap& EventSubscribers() { return eventSubscribers_; }   // +0x98
    std::vector<cRZAutoRefCount<cISC4Advice>>& ContextAdvices() { return contextAdvices_; }   // +0xb0
    const std::vector<int>& DataTags() const { return dataTags_; } // +0xc8
    bool Initialized() const { return initialized_; }              // +0x39
    bool LoadedFromSave() const { return loadedFromSave_; }        // +0x38
    bool DataReloaded() const { return dataReloaded_; }            // +0x88
    int LastDateNumber() const { return lastDateNumber_; }         // +0x58
    void SetDataReloaded(bool value) { dataReloaded_ = value; }
    const int* Thresholds() const { return thresholds_; }           // +0x2c (easy, medium, hard)
    cISCLua* Lua() const { return lua_.get(); }                    // +0x78
    IAdvisorWinProc* WinProc() const { return winProc_.get(); }    // +0x80
    cSCStringDetokenizer* DetokenizerPtr() const { return detokenizer_.get(); }   // +0x90

private:
    int32_t thresholds_[3] = {kAdvisorDefaultPopupPriority, kAdvisorDefaultPopupPriority,
                              kAdvisorDefaultPopupPriority};       // +0x2c
    bool loadedFromSave_ = false;                                  // +0x38
    bool initialized_ = false;                                     // +0x39
    cRZRandom random_{0xffffffffu};                                // +0x40
    int lastDateNumber_ = 0;                                       // +0x58
    AdvisorMap advisors_;                                          // +0x60
    cRZAutoRefCount<cISCLua> lua_;                                 // +0x78
    cRZAutoRefCount<IAdvisorWinProc> winProc_;                     // +0x80
    bool dataReloaded_ = false;                                    // +0x88
    cRZAutoRefCount<cSCStringDetokenizer> detokenizer_;            // +0x90
    AdvisorEventMap eventSubscribers_;                             // +0x98
    std::vector<cRZAutoRefCount<cISC4Advice>> contextAdvices_;     // +0xb0
    std::vector<int> dataTags_;                                    // +0xc8
    cRZTimer timer_{cRZTimer::kMilliseconds};                      // +0xe0

    static cSC4AdvisorSystem* instance_;
};

}  // namespace sc4
