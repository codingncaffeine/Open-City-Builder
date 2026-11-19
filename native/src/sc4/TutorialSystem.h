// cSC4TutorialSystem — the city's tutorial driver: it owns the tutorial UI window procedure, keeps the current
// tutorial GUID and task number, starts / ends a tutorial, and routes the messages the tutorial win proc needs.
// Recovered from Sim City 4 Deluxe Edition 1.2.1 (49 member symbols at 0x10005f0d4..0x100060145, object 0x68 bytes,
// class id 0xaa3836fb, interface cISC4TutorialSystem 0xaa3836f6). It is created by GZCOM
// (GZCOM_CreatecSC4TutorialSystem @ 0x10005f086 allocates 0x68 bytes and calls the constructor) and registered by the
// city as its "TutorialSystem" component (cSC4City's table, iid 0xaa3836f6); SL::spCity vslot 0x290 hands it out
// (cSC4City::GetTutorialSystem @ 0x100048922) and cSC4RegionalCity asks it for the tutorial GUID (vslot 0x60,
// CityTutorialGUID @ 0x1003b2df6, see src/sc4/RegionalCity.cpp).
//
// Original object layout (the five vtable views at +0..+0x2f, cRZUnknown last). Every offset below is the original
// one; in the port the members start one word later because the recovered cRZUnknown block is 0x10 bytes where the
// original's is 0xc (nothing reads these fields by offset, so the +4 shift is harmless):
//   +0x00 cISC4TutorialSystem vptr (iid 0xaa3836f6; 28 slots, see the interface below)
//   +0x08 cIGZMessageTarget2 vptr (iid 0x452294aa, DoMessage(cIGZMessage2*))
//   +0x10 cIGZMessageTarget vptr  (iid 0xe98b2f57, DoMessage(cGZMessage&))
//   +0x18 cIGZSerializable vptr   (iid 0xe4fda3d4, Write / Read / GetGZCLSID)
//   +0x20 cRZUnknown vptr (+0x28 its refcount, 4 bytes; the original's cRZUnknown is 0xc bytes)
//   +0x2c three bytes no recovered function touches (the constructor clears them with the same 8-byte store that
//         clears +0x30 and +0x2f; the port keeps them as padding so the later offsets read like the original's)
//   +0x2f bool "a tutorial is running" (IsTutorial, startTutorial sets 1, endTutorial sets 0)
//   +0x30 int current task number (GetTutorialTaskNumber / ResetTutorialIndex / IncreaseTutorialNumber)
//   +0x34 int current tutorial GUID (GetTutorialGUID), -1 = none (the constructor's default)
//   +0x38 int initialised to 3 by the constructor and never read or written by any recovered function (purpose
//         unknown; kept so the field order is preserved)
//   +0x3c bool "the tutorial was ended without saving": set to !save by endTutorial, cleared by startTutorial and
//         consumed by the city-loaded message (0x26c63343), which then ends the tutorial with save = true
//   +0x3d bool "the city-view-started message was already handled" (the one-shot latch of 0xea8ae29a)
//   +0x40 cISCLua* scripting context cache (GetScriptingContext; referenced, released by the destructor)
//   +0x48 std::vector (3 pointers, never grown by any recovered function; the destructor frees the block, so the
//         element type is trivially destructible and cannot be recovered)
//   +0x60 cSC4TutorialUIWinProc* tutorial window procedure (referenced, see ITutorialUIWinProc)
//
// The four messages the class subscribes to (Init) and routes:
//   0xaa3840e6 "a tutorial finished" (posted by cSC4TutorialUIWinProc::finish_tutorial with data1 = !save): when a
//              window procedure exists, endTutorial(data1 == 0)
//   0x26c63343 "city loaded" (cSC4ModelMaker's kMessageCityLoaded): when +0x3c is set, endTutorial(true) — this is
//              how a tutorial that was quit without saving is marked inactive in the registry after the reload
//   0xea8ae29a "city view started" (cSC4App state 2): once (+0x3d), when a tutorial is running, look the current
//              tutorial GUID up in the registry and start its first task when the registry item is active
//   0x230e27ac a cheat code was typed: param1 = cheat id, param2 = the typed text as a cIGZString. The "Tutorial"
//              cheat (id 0x6a382baf, registered by Init only when debug functionality is enabled) understands
//              "reset [guid]", "save [guid]", "step <index>", "next" and "quit".
//
// Quirks kept:
//  * startTutorial takes two references on the freshly created window procedure (the assignment block adds one and
//    the Init call adds another), so endTutorial / Shutdown release it twice (0x10005f2fc, 0x10005f3ae, 0x10005fe5a).
//  * The window procedure's creation path is dead the second time: it is only created when +0x60 is null, and the
//    assignment block's "release the old object" branch can therefore never run (0x10005f329).
//  * endTutorial releases the window procedure even when its Shutdown already ran, and clears +0x60 only between the
//    two releases, so the second release uses the saved pointer (0x10005f3d6).
//  * ResetTutorialNumAndIndex stores its *first* argument (the tutorial GUID) into +0x34 and the value the window
//    procedure returns (the new task number) into +0x30; the second argument is only forwarded (0x10005f75).
//  * The "save <guid>" cheat refuses GUID 0 and 0xffffffff but nothing else (0x10005f92e), works with a stale /
//    current task number and writes the city's tutorial GUID before asking for the save.
//  * The two-argument "save" branch dereferences SL::spSC4 without a null check (0x10005f981), unlike the two
//    "reset" branches; the port treats a missing host as absent.
//  * FilterMessage forwards the *low 32 bits* of the message's third word (the catalog item id) for command
//    0x49d993d4 and of the second word (the button id) for the two button commands (0x10005ff15), and ignores every
//    other message type / sender.
//  * Read only accepts version 2; a wrong version returns false without setting the stream error, and it does not
//    skip the payload (0x10005fc1a). The local version variable is read uninitialised in the original before
//    GetUint16 fills it.
//  * The process-wide instance is a plain static that every constructor overwrites and every destructor clears, so a
//    second system steals the instance from the first (0x10005f1a1 / 0x10005f1cc, no guard).
//  * IsEdgeScrollingAllowed answers true when there is no window procedure or no current task (0x100060058).
//  * nsSC4TutorialSystem::FilterUICommandMessage (0x10006008e) answers true without a city, and otherwise asks the
//    tutorial system only when it reports a running tutorial; that is the gate cSC4View3DWin and the building-select
//    panel use before executing a UI command, and it lives in this file in the original.
//
// Everything the original reaches through SL::sp* / the tutorial registry goes through ITutorialSystemHost (the
// global / vslot / address of every original call is in the comment). The tutorial window procedure is the
// unrecovered cSC4TutorialUIWinProc, reduced here to ITutorialUIWinProc.
#pragma once
#include "sc4/CommonIIDs.h"
#include <cstdint>
#include <vector>

#include "gz/cGZMessageServer.h"
#include "gz/cIGZIStream.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZOStream.h"
#include "gz/cIGZSerializable.h"
#include "lua/ISCLua.h"
#include "rz/cRZUnknown.h"

namespace sc4 {

class cISC4TutorialTask;        // sc4/TutorialTask.h (recovered)
class cISC4WinCatalogView;      // sc4/WinCatalogView.h (recovered)

constexpr std::uint32_t kGZCLSID_cSC4TutorialSystem = 0xaa3836fb;          // GetGZCLSID @ 0x10006011a
constexpr std::uint32_t kGZIID_cISC4TutorialSystem = 0xaa3836f6;           // the +0x00 view (cSC4City's component)
constexpr std::uint32_t kGZIID_cGZMessageTargetLegacy = 0xe98b2f57;        // the +0x10 view
constexpr std::uint32_t kTutorialMessageFinished = 0xaa3840e6;             // endTutorial(data1 == 0)
constexpr std::uint32_t kTutorialMessageCityLoaded = 0x26c63343;           // endTutorial(true) when +0x3c
constexpr std::uint32_t kTutorialMessageCityViewStarted = 0xea8ae29a;      // start the registry's task once
constexpr std::uint32_t kTutorialMessageCheatCode = 0x230e27ac;            // cGZCheatCodeManager's typed-cheat message
constexpr std::uint32_t kTutorialCheatId = 0x6a382baf;                     // "Tutorial"
constexpr const char* kTutorialCheatText = "Tutorial";                     // _kSC4CheatCodeTutorialString @ 0x1009ef1ef
constexpr std::uint16_t kTutorialStreamVersion = 2;                        // Write / Read
// The three command ids FilterMessage forwards: a catalog item was chosen (word 3 = the item id, see
// WinCatalogView.h kCatalogMsgItemChosen) and the button "activated" / "on changed" commands (word 2 = the button id).
constexpr std::uint64_t kTutorialCommandCatalogItem = 0x49d993d4;
constexpr std::uint64_t kTutorialCommandButtonChanged = 0x287259f7;
constexpr std::uint64_t kTutorialCommandButtonActivated = 0x287259f6;
// The button / command ids the cheat verbs are compared against (0x1009ef1f8..0x1009ef20d).
constexpr const char* kTutorialCheatReset = "reset";
constexpr const char* kTutorialCheatSave = "save";
constexpr const char* kTutorialCheatStep = "step";
constexpr const char* kTutorialCheatNext = "next";
constexpr const char* kTutorialCheatQuit = "quit";
// cSC4TutorialRegistry::WhichToGUID @ 0x100643874 maps the two stored tutorial numbers to their city GUIDs.
constexpr int kTutorialGUIDDefault = static_cast<int>(0x8a5b7a6c);         // stored 0
constexpr int kTutorialGUIDSecond = static_cast<int>(0xea5d6dc8);          // stored 1
constexpr int kTutorialGUIDNone = -1;                                      // +0x34 default

// cSC4TutorialUIWinProc (unrecovered: 55 symbols at 0x1000b697c..0x1000bbb4c, object 0xc0 bytes, vtable 0x100a6ee30).
// The tutorial system only owns it, forwards the messages / calls above and asks it for the current task. Every slot
// offset / address below is the original one.
class ITutorialUIWinProc {
public:
    virtual ~ITutorialUIWinProc() = default;
    virtual std::uint32_t AddRef() = 0;                                           // vslot 0x08 (the one count of every view)
    virtual std::uint32_t Release() = 0;                                          // vslot 0x10
    virtual void Init() = 0;                                                      // @ 0x1000b6d8c
    virtual void Shutdown() = 0;                                                  // @ 0x1000b7470
    virtual void handleButtons(std::uint32_t command) = 0;                        // vslot 0x48 @ 0x1000bb0a0
    virtual void ShowUIHintsForUsers() = 0;                                       // vslot 0x50 @ 0x1000bb28e
    virtual void HandleCatalogView(cISC4WinCatalogView* view) = 0;                // vslot 0x58 @ 0x1000bb114
    virtual cISC4TutorialTask* GetCurrentTask() = 0;                              // vslot 0x60 @ 0x1000b8812 (borrowed)
    virtual bool FilterLeftMouseClick(int& x, int& y, int flags) = 0;             // vslot 0x68 @ 0x1000b78c4
    virtual int ResetTutorialNumAndIndex(int guid, int index) = 0;                // vslot 0x70 @ 0x1000ba668
    virtual void moveToNext() = 0;                                                // @ 0x1000b83ee
    virtual bool OnTutorialEvent(std::uint32_t event) = 0;                        // @ 0x1000b883e (true)
};

// cISC4TutorialSystem — the +0x00 interface. The original declares the whole 28-slot surface on it, including the
// destructor (slots 0x98 / 0xa0) and then the serialisation / message / script slots 0xa8..0xd8: the class itself
// declares no virtual of its own, and the secondary vtable groups hold pointers to thunks of these same functions
// (so one C++ override here also overrides the cIGZSerializable / message-target base functions).
class cISC4TutorialSystem : public cIGZUnknown {
public:
    // The tutorial events the scripts / UI raise through OnTutorialEvent. Only value 1 is decoded anywhere in the
    // recovered code: cSC4TutorialUIWinProc::OnTutorialEvent opens the current task's "try_again_msg" document (and
    // shows its hints) when the task's whichFunction is also 1. The other values are forwarded untouched.
    enum eTutorialEvent : std::uint32_t { kTutorialEventOpenTaskLink = 1 };

    virtual bool Init() = 0;                                              // 0x18
    virtual bool Shutdown() = 0;                                          // 0x20
    virtual void startTutorial() = 0;                                     // 0x28
    virtual void endTutorial(bool saveTutorial) = 0;                      // 0x30
    virtual cISCLua* GetScriptingContext() = 0;                           // 0x38
    virtual bool IsTutorial() const = 0;                                  // 0x40
    virtual bool IsEdgeScrollingAllowed() const = 0;                      // 0x48
    virtual cISC4TutorialTask* GetCurrentTask() const = 0;                // 0x50 (borrowed)
    virtual std::int32_t GetTutorialTaskNumber() const = 0;               // 0x58
    virtual std::int32_t GetTutorialGUID() const = 0;                     // 0x60
    virtual bool FilterMessage(cGZMessage& message) = 0;                  // 0x68
    virtual void ShowUIHintsForUsers() = 0;                               // 0x70
    virtual bool HandleCatalogMenu(cISC4WinCatalogView* view) = 0;        // 0x78
    virtual bool FilterLeftMouseClick(int& x, int& y, int flags) = 0;     // 0x80
    virtual bool ResetTutorialNumAndIndex(int guid, int index) = 0;       // 0x88
    virtual bool OnTutorialEvent(eTutorialEvent event) = 0;               // 0x90 (the original returns true)
    virtual ~cISC4TutorialSystem() = default;                             // 0x98 / 0xa0
    virtual bool Write(cIGZOStream& stream) const = 0;                    // 0xa8
    virtual bool Read(cIGZIStream& stream) = 0;                           // 0xb0
    virtual std::uint32_t GetGZCLSID() const = 0;                         // 0xb8
    virtual bool DoMessage(cIGZMessage2* message) = 0;                    // 0xc0
    virtual bool DoMessage(cGZMessage& message) = 0;                      // 0xc8
    virtual void ResetTutorialIndex(int index) = 0;                       // 0xd0 (Lua: reset the task number)
    virtual void IncreaseTutorialNumber() = 0;                            // 0xd8 (Lua: step the task number)
};

// SL::spCCM (the real class is cGZCheatCodeManager, recovered in gz/CheatCodeManager.h): only the four slots this
// class calls. The tutorial system uses the *legacy* message-target view (+0x10), i.e. vslots 0x28 / 0x30.
class ITutorialCheatCodes {
public:
    virtual ~ITutorialCheatCodes() = default;
    virtual void RegisterCheatCode(std::uint32_t id, const char* text) = 0;                // vslot 0x18
    virtual void UnregisterCheatCode(std::uint32_t id) = 0;                                // vslot 0x20
    virtual void AddNotification(cIGZMessageTarget* target, std::uint32_t id) = 0;         // vslot 0x28
    virtual void RemoveNotification(cIGZMessageTarget* target, std::uint32_t id) = 0;      // vslot 0x30
};

// What cSC4TutorialSystem needs from the running game; one method per original global / call chain.
class ITutorialSystemHost {
public:
    virtual ~ITutorialSystemHost() = default;
    virtual cIGZMessageServer2* Messages() = 0;                          // SL::spMS2 (borrowed, may be null)
    virtual ITutorialCheatCodes* CheatCodes() = 0;                       // SL::spCCM (borrowed, may be null)
    virtual bool DebugFunctionalityEnabled() = 0;                        // SL::spSC4 vslot 0xa8
    // cSC4TutorialRegistry (unrecovered) through SL::spSC4 vslot 0x178 (cISC4App::GetTutorialRegistry):
    virtual bool TutorialRegistrySetActive(int guid, bool active) = 0;   // SetActive @ 0x1006433ee
    virtual bool TutorialItemActive(int guid) = 0;                       // GetItem @ 0x10064307a, item byte +0x10
    virtual int TutorialGUIDFromStoredValue(int value) = 0;              // WhichToGUID @ 0x100643874
    // SL::spCity vslot 0x288 -> vslot 0x50 (the city's scripting server): the cISCLua view, borrowed.
    virtual cISCLua* ScriptingContext() = 0;
    // SL::spView3D vslot 0x1e0 (CloseTertiaryMenu) then vslot 0x1d8 (CloseSecondaryMenu), no-ops without a view.
    virtual void CloseTertiaryMenu() = 0;
    virtual void CloseSecondaryMenu() = 0;
    // SL::spSC4 vslot 0x118 (GetRegion) -> vslot 0xb0 (ResetTutorialCity).
    virtual void ResetTutorialCity(int guid) = 0;
    // SL::spSC4 vslot 0x120 (GetRegionalCity) -> vslot 0x210 (SetTutorialGUID).
    virtual void SetRegionalCityTutorialGUID(int guid) = 0;
    // SL::spSC4 vslot 0x48 (RequestSaveCity(true, false)).
    virtual void RequestSaveCity() = 0;
    // new cSC4TutorialUIWinProc (0xc0 bytes, constructor @ 0x1000b697c), returned with reference count 0.
    virtual ITutorialUIWinProc* CreateTutorialUIWinProc() = 0;
};
ITutorialSystemHost* TutorialSystemHost();
void SetTutorialSystemHost(ITutorialSystemHost* host);

class cSC4TutorialSystem final : public cISC4TutorialSystem,
                                 public cIGZMessageTarget2,
                                 public cIGZMessageTarget,
                                 public cIGZSerializable,
                                 public cRZUnknown {
public:
    cSC4TutorialSystem();                                                // @ 0x10005f0d4
    ~cSC4TutorialSystem() override;                                      // @ 0x10005f1bc (aliases 0x10005f2a8 / 0x10005f2c0,
                                                                         //   thunks 0x10005f2b2 / 0x10005f2ee)

    // cIGZUnknown (all five views share the reference count of the +0x20 subobject)
    bool QueryInterface(std::uint32_t iid, void** out) override;         // @ 0x10005fa92 (thunks 0x10005faea..0x10005fb14)
    std::uint32_t AddRef() override;                                     // @ 0x10005fb22 (thunks 0x10005fb30..0x10005fb5a)
    std::uint32_t Release() override;                                    // @ 0x10005fb64 (thunks 0x10005fb72..0x10005fb9c)

    // cISC4TutorialSystem
    bool Init() override;                                                // @ 0x10005fcc8
    bool Shutdown() override;                                            // @ 0x10005fe5a
    void startTutorial() override;                                       // @ 0x10005f2fc
    void endTutorial(bool saveTutorial) override;                        // @ 0x10005f3ae
    cISCLua* GetScriptingContext() override;                             // @ 0x10005ffd2
    bool IsTutorial() const override;                                    // @ 0x1000600e0
    bool IsEdgeScrollingAllowed() const override;                        // @ 0x100060058
    cISC4TutorialTask* GetCurrentTask() const override;                  // @ 0x1000600ee
    std::int32_t GetTutorialTaskNumber() const override;                 // @ 0x100060106
    std::int32_t GetTutorialGUID() const override;                       // @ 0x100060110
    bool FilterMessage(cGZMessage& message) override;                    // @ 0x10005ff0c
    void ShowUIHintsForUsers() override;                                 // @ 0x10005ff88
    bool HandleCatalogMenu(cISC4WinCatalogView* view) override;          // @ 0x10005ffba
    bool FilterLeftMouseClick(int& x, int& y, int flags) override;       // @ 0x10005ff9e
    bool ResetTutorialNumAndIndex(int guid, int index) override;         // @ 0x10005ff56
    bool OnTutorialEvent(eTutorialEvent event) override;                 // @ 0x100060042 (always true)
    void ResetTutorialIndex(int index) override;                         // @ 0x100060126
    void IncreaseTutorialNumber() override;                              // @ 0x100060130

    // cIGZSerializable
    bool Write(cIGZOStream& stream) const override;                      // @ 0x10005fba6 (thunk 0x10005fc0c)
    bool Read(cIGZIStream& stream) override;                             // @ 0x10005fc1a (thunk 0x10005fcba)
    std::uint32_t GetGZCLSID() const override;                           // @ 0x10006011a (thunk 0x10006013a)

    // cIGZMessageTarget2 / cIGZMessageTarget
    bool DoMessage(cIGZMessage2* message) override;                      // @ 0x10005f452 (thunk 0x10005f516)
    bool DoMessage(cGZMessage& message) override;                        // @ 0x10005f528 (thunk 0x10005fa80)

    // process-wide instance (the original's spInstance at 0x100b214f0)
    static cSC4TutorialSystem* Instance() { return instance_; }

    // test / glue accessors (no counterparts in the original; the fields are private there and the tutorial window
    // procedure reaches them through the object layout)
    bool TutorialActive() const { return tutorialActive_; }        // +0x2f
    std::int32_t TaskNumber() const { return taskNumber_; }        // +0x30
    std::int32_t TutorialGUID() const { return tutorialGUID_; }    // +0x34
    bool QuitPending() const { return quitPending_; }              // +0x3c
    bool CityViewHandled() const { return cityViewHandled_; }      // +0x3d
    cISCLua* ScriptingContextCache() const { return scriptingContext_; }   // +0x40
    ITutorialUIWinProc* WinProc() const { return winProc_; }       // +0x60
    std::int32_t Unknown38() const { return unknown38_; }          // +0x38 (the constructor's 3)

private:
    static cSC4TutorialSystem* instance_;
    [[maybe_unused]] std::uint8_t reserved2c_[3] = {};   // +0x2c (never accessed)
    bool tutorialActive_ = false;               // +0x2f
    std::int32_t taskNumber_ = 0;               // +0x30
    std::int32_t tutorialGUID_ = kTutorialGUIDNone;   // +0x34
    std::int32_t unknown38_ = 3;                // +0x38 (never accessed; the constructor's value)
    bool quitPending_ = false;                  // +0x3c
    bool cityViewHandled_ = false;              // +0x3d
    cISCLua* scriptingContext_ = nullptr;       // +0x40 (referenced)
    std::vector<std::uint32_t> unused48_;       // +0x48 (never grown; see the layout note)
    ITutorialUIWinProc* winProc_ = nullptr;     // +0x60 (two references while non-null)
};

// nsSC4TutorialSystem::FilterUICommandMessage @ 0x10006008e — the free function of the original's
// SC4TutorialSystem.cpp that cSC4View3DWin and cSC4BuildingSelectWinProc call before executing a UI command: without
// a city (or with a tutorial system that is not running) the command always passes, otherwise the tutorial window
// procedure gets a veto through cISC4TutorialSystem::FilterMessage (vslot 0x68).
bool FilterUICommandMessage(cGZMessage& message);
// The seam for SL::spCity vslot 0x290 (cSC4City::GetTutorialSystem); null when there is no city.
cISC4TutorialSystem* TutorialFilterSystem();
void SetTutorialFilterSystem(cISC4TutorialSystem* system);

}  // namespace sc4
