// cSC4MySim — the player's own sim ("Mayor Bob"): the most visible advisor. 125 functions (cSC4Advisor's 35 plus 90
// of its own; the 86 non-virtual thunks are compiler-generated), 0x1006bbbe2..0x1007aae18, class id 0x4a1dbbbf,
// interfaces cISC4Advisor 0xca09f544, cIGZSerializable 0xe4fda3d4, cIGZMessageTarget2 0x452294aa (all from the base),
// cISC4MySim 0x8bc01ffe (the primary vtable) and 0x0a1da42f (the +0x148 view).
//
// The sim lives in the city as an occupant-less "agent": it owns a residence and a job (building occupants), commutes
// between them through cSC4MySimDispatch (a separate object that drives a cISC4Automaton along the traffic network),
// keeps the script variables of the "mysim" Lua table up to date for the UI, shows speech balloons for its advices and
// reacts to the city's messages (buildings removed, fires, disasters, dispatch tool events).
//
// Original layout (the first four subobjects come from cSC4Advisor):
//   +0x00 cRZUnknown +0x10 cISC4Advisor +0x18 cIGZSerializable +0x20 cIGZMessageTarget2
//   +0x148 cISC4MySim vptr (the 69-slot interface, iid 0x0a1da42f)
//   +0x150 u8  slot (the "my sim slot" shown in the UI)          +0x154 int activity state (0 = not in the city)
//   +0x158 cRZAutoRefCount<cISC4BuildingOccupant> residence       +0x160 int residence cell x, +0x164 cell z
//   +0x168 float x, +0x16c float y, +0x170 float z (city position of the sim)
//   +0x178 cRZAutoRefCount<cSC4Lot> residence lot                 +0x180 job building (local)
//   +0x188 job building across a neighbour connection             +0x190..0x19c int[4] job/home cell rect
//   +0x1a0 auto-ref (unidentified, released in Shutdown)          +0x1a8 int unemployed days
//   +0x1ac..0x1b4 cGZPersistResourceKey vehicle (car/ped model)   +0x1b8 int wealth 0..3
//   +0x1bc u16 generation (starts at 1)                           +0x1be u8 zodiac sign, +0x1bf u8 male
//   +0x1c0 cRZString bio                                          +0x1e8 u8 age, +0x1e9 u8 birth month
//   +0x1ec int bitmap id                                          +0x1f0 cRZString bitmap file
//   +0x218 cRZAutoRefCount<IMySimCallout> residence callout box   +0x220 bool callout shown
//   +0x228 auto-ref (unidentified)                                +0x230 int dispatch view track handle
//   +0x234 int last commute time (minutes)                        +0x238 bool retired
//   +0x23c int last local disaster id                             +0x240 bool residence needs a commute check
//   +0x244 int Lua registry reference of the "mysim" table        +0x248 cRZString "home_building" value
//   +0x250 cRZString "job_building" value                         +0x258 auto-ref (unidentified)
//   +0x260 cRZString "job_city" value                             +0x268 cRZString "_generation" personal name
//   +0x270 float education quotient, +0x274 float health quotient
//   +0x278/+0x280/+0x288 auto-refs (0x288 = the "MySimGhost" effect instance)
//   +0x290 cRZRandom                                              +0x2a8 float eq history[6]
//   +0x2c0 float hq history[6]                                    +0x2d8 u8 eq index, +0x2d9 u8 hq index
//   +0x2dc float delta eq, +0x2e0 float delta hq                  +0x2e4 bool balloons/registry initialized
//   +0x2e8 std::map<u32,u32>* occupant-group distances (raw pointer)
//   +0x2f0 auto-ref sound object                                  +0x2f8 cISCLua* (city script state)
//   +0x300 cRZString empty "name" value                           +0x308 cRZString "no home" value
//   +0x310 cRZString "no job" value                               +0x318 int current balloon start time
//   +0x31c int pedestrian bitmap id                               +0x320 int car bitmap id
//   +0x328 cRZAutoRefCount<IMySimDispatch> dispatch               +0x330 bool balloons enabled
//   +0x338 std::vector<cBalloonInfo> (advice balloons)            +0x350 std::map<int,int> event id -> balloon index
//   +0x368 std::list<int> balloon queue (advice)                  +0x380 std::list<int> balloon queue (events)
//   +0x398 int balloon round-robin index                          +0x3a0 cRZAutoRefCount<IMySimBalloonWinProc>
//   +0x3a8 int default car model instance                         +0x3ac int trip local area size
//   +0x3b0 bool dispatch tool active, +0x3b1 bool pylon locked   +0x3b8 cRZAutoRefCount<IMySimPylon>
//   +0x3c0 car model key, +0x3cc pedestrian model key (cGZPersistResourceKey each)
//   +0x3d8 int last trip day (-1)                                 +0x3e0 cRZTimer pylon timer
//   +0x3f8 cRZTimer balloons timer                                +0x410 int last player trip day
//   +0x414 int last home sound time                               +0x418 cRZTimer sound timer
//
// Serialized record (cSC4MySim::Write/Read, version 0x11): activity state; when non-zero slot, residence, residence
// cells, job, the four job/home cells, unemployed days, vehicle key, wealth, generation, zodiac, male, bio, age, birth
// month, bitmap id, bitmap file, track handle, retired, last local disaster, position, eq/hq history, delta eq, keys,
// bitmap ids, dispatch, pylon (flag + serializable), pylon-locked flag and last player trip day; then the advisor base.
//
// The port keeps the original's *relative* layout of its own members, but cSC4Advisor's port members are wider than
// the originals (cSLString carries its resource key), so the cISC4MySim subobject sits at sizeof(cSC4Advisor) instead
// of +0x148; every member comment above gives the original offset.
//
// Quirks kept:
//  * GetPersonalName only uses the "named" form from generation 2 on and only loads a "<first name> <suffix>" resource
//    for generations below 0xb; the suffix is appended to the base personal name, and generation 1 returns the base
//    name unchanged.
//  * MoveIn writes the slot/name/sex/zodiac/bitmap before the new input control is created; a failed tool Init leaves
//    the sim half moved in.
//  * QuitJob only leaves the "at work" states (0x154 becomes 2, except from state 5); it does not touch the dispatch.
//  * ChangeResidence re-runs CommandWouldTrigger while the new residence is already set and rolls back to the old one
//    when an advice would trigger.
//  * LeaveForWork/ReturnHome dispatch a commute and only then change the state; a failed dispatch plays the "no way"
//    sound (LeaveForWork) instead of changing state.
//  * Spawn keeps the generation dependent personal name of the old sim alive (the cached string at +0x268 is not
//    refreshed unless GetPersonalName is called again).
//  * init_balloons erases the base class's event entries for advices that do not answer the balloon interface.
//  * simulate_day_events keys its balloon pushes off the *last* queued event but runs once per queued event, so a
//    queue of N identical events pushes N balloons; the base class then pops one event (its own quirk).
//  * GetJobTitle's wealth 1 special case ORs 1 into the base string id for job kind 5 (so kinds 5 and 6 share the id
//    family 0x8a538a5e/0x8a538a5f).
//  * GetCurrentScreenLocationXY puts the icon 0x19 pixels below the top of the dispatch view rect.
//  * DispatchToolEnd drops the pylon unless the current destination is the locked pylon; DispatchToolLockPylon always
//    returns true, even when it could not go to the pylon.
//  * Everything the original reaches through SL::sp* / spCity / spSimulator / GZCOM() goes through IMySimHost; the
//    cSC4MySimDispatch and cSC4MySimBalloonWinProc neighbours (neither recovered) are IMySimDispatch /
//    IMySimBalloonWinProc seams. The script state is the recovered cISCLua, so the Lua calls keep their original slots.
#pragma once
#include <cstdint>
#include <list>
#include <map>
#include <vector>
#include <functional>

#include "gz/Buffer.h"
#include "gz/cGZPersistResourceKey.h"
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
#include "sc4/AdviceSubject.h"
#include "sc4/Advisor.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/Exemplar.h"
#include "sc4/MySimBalloonAdvice.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4MySim = 0x4a1dbbbf;             // GetGZCLSID @ 0x1007aade4
constexpr std::uint32_t kGZIID_cISC4MySim = 0x8bc01ffe;              // the primary view (QueryInterface @ 0x10079f7e8)
constexpr std::uint32_t kGZIID_cISC4MySimLink = 0x0a1da42f;          // the +0x148 view (also the pylon link iid)
constexpr std::uint16_t kMySimStreamVersion = 0x11;                  // Write @ 0x1007a27ce

// Activity states (+0x154).
enum MySimState : std::int32_t {
    kMySimStateNone = 0,          // not in the city
    kMySimStateAtHome = 1,        // no valid commute; idles at home
    kMySimStateIdle = 2,          // at home, not working (the "home" location)
    kMySimStateAtWork = 3,
    kMySimStateGoingToWork = 4,
    kMySimStateGoingHome = 5,
    kMySimStateAtPylon = 6,
};

// Things the sim sends/hears. The values are the original message types and event ids.
constexpr std::uint32_t kMessageMySimSimulate = 0x8ad610ee;          // DoMessage "the simulator ticked" (=-0x7952ef12)
constexpr std::uint32_t kMessageMySimToolMessage = 0xbb04126;        // create the dispatch tool
constexpr std::uint32_t kMessageMySimChanged = 0x4bc42c48;           // "this sim changed", data1 = cISC4MySim*
constexpr std::uint32_t kMessageMySimHideAll = 0x26d31ec1;           // hide residence/location/pylon
constexpr std::uint32_t kMessageMySimInput = 0xab6f3e2d;             // cancel the view input for this sim
constexpr std::uint32_t kMessageMySimInput2 = 0x4b6f3ebf;            // (same, second trigger)
constexpr std::uint32_t kMessageMySimNoop = 0xb6f3e27;               // ignored (balloon window proc bookkeeping)
constexpr std::uint32_t kMessageMySimDispatchNotify = 0x2ba06b99;    // dispatch notification, data1 = event below
constexpr std::uint32_t kDispatchNotifyIdleOffset = 0xeba06bd0;      // pick an idle animation offset
constexpr std::uint32_t kDispatchNotifyLocation = 0xeba06bd4;        // the current location changed
constexpr std::uint32_t kDispatchNotifyArrived = 0xeba06bf4;         // arrived at the current destination
constexpr std::uint32_t kDispatchNotifyPylon = 0xeba06c12;           // pylon reached / arrived nearby
constexpr std::uint32_t kDispatchNotifyTripEnd = 0x4bb2d020;         // trip finished (data2/data3 = leg state)
constexpr std::uint32_t kDispatchNotifyTripLeg = 0x4bb2cfc0;
constexpr std::uint32_t kDispatchNotifyNeighbor = 0x4bb71a7e;
constexpr std::uint32_t kEventMySimNoResidence = 0x6a57dd94;         // no free residence found
constexpr std::uint32_t kEventMySimMoved = 0xaa3edbb1;               // moved into/out of a building
constexpr std::uint32_t kEventMySimMovedIn = 0xca3e87a8;             // AnnounceMoveIn's PushEvent (0x1007a934f)
constexpr std::uint32_t kEventMySimCommuteFailed = 0xca4553b7;       // no route to work
constexpr std::uint32_t kEventMySimLeftWork = 0xec107df9;            // left a neighbour job
constexpr std::uint32_t kEventMySimBuildingAdded = 0xca4e8986;       // a building appeared nearby
constexpr std::uint32_t kEventMySimFire = 0xea5e4e89;
constexpr std::uint32_t kEventMySimPipeBurst = 0x6a5e598c;
constexpr std::uint32_t kEventMySimTornado = 0x0a5e8243;
constexpr std::uint32_t kEventMySimDisasterA = 0x8ac11a9b;           // disaster 0xca380b06
constexpr std::uint32_t kEventMySimDisasterB = 0x2ac14492;           // disaster 0xc4c90997
// UpdateTripVariables: the landmark occupant group, the landmark budget purpose and the disaster ids it looks for.
constexpr std::uint32_t kOccupantGroupLandmark = 0x150a;
constexpr std::uint32_t kBudgetPurposeLandmark = 0xaa59670c;
constexpr std::uint32_t kDisasterIdFire = 0x621cda33;
constexpr std::uint32_t kDisasterIdRiot = 0xca380b06;
constexpr std::uint32_t kMessageMySimsChanged = 0xaa25827c;          // SendMySimsChangedMessage @ 0x1007a787b
constexpr std::uint32_t kMessageMySimMovedIn = 0xea3ada33;           // AnnounceMoveIn
// Script commands (DoScriptCommand @ 0x1007a93a0).
constexpr std::uint32_t kScriptMySimChangeHouse = 0x6a47e79b;
constexpr std::uint32_t kScriptMySimSpawn = 0x6a495f7f;
constexpr std::uint32_t kScriptMySimIncreaseWealth = 0x6a4a5e9e;
constexpr std::uint32_t kScriptMySimQuitJob = 0x6a5e6860;
constexpr std::uint32_t kScriptMySimRetire = 0xaa4934b4;             // -0x55b6cb4c: quit the job and turn retired
constexpr std::uint32_t kScriptMySimDie = 0xea4d269c;                // -0x15b2d964
constexpr std::uint32_t kScriptMySimLeaveCity = 0x0a57de1f;
// Home/night sounds (PlayHomeSound callers).
constexpr std::uint32_t kSoundNightMale = 0x6a5fc4f2;
constexpr std::uint32_t kSoundNightFemale = 0x4a5fc52a;
constexpr std::uint32_t kSoundNoCommuteMale = 0x2a5f9cdc;
constexpr std::uint32_t kSoundNoCommuteFemale = 0x6a5fb141;
// Job titles ("job title" strings, group 0xea231e96; GetJobTitle @ 0x1007a7e36).
constexpr std::uint32_t kJobTitleGroup = 0xea231e96;
constexpr std::uint32_t kJobTitleWealth1Kind5To8 = 0x8a538a5e;       // | (kind == 5)
constexpr std::uint32_t kJobTitleWealth1Other = 0x8a538a61;          // - (kind == 2)
constexpr std::uint32_t kJobTitleWealth2Kind5To8 = 0x8a538a62;
constexpr std::uint32_t kJobTitleWealth2Other = 0x8a538a64;          // - (kind == 2)
constexpr std::uint32_t kJobTitleWealth3Kind5To8 = 0x8a538a65;
constexpr std::uint32_t kJobTitleWealth3Other = 0x8a538a67;          // - (kind == 2)
// HQ text thresholds (GetHQAsText @ 0x1007a83e4).
constexpr std::uint32_t kHQTextLow = 0xa5fe2df, kHQTextMid = 0xaa317340, kHQTextHigh = 0x6a5fe294;
constexpr std::uint32_t kHQTextGroupLow = 0xea5524eb, kHQTextGroupMid = 0x6a231eaa;
// Default car / pedestrian exemplar keys (static initialiser @ 0x1007ab77a).
constexpr cGZPersistResourceKey kDefaultCarExemplarKey{0x6534284a, 0x096e6739, 0x10e70000};
constexpr cGZPersistResourceKey kDefaultPedExemplarKey{0x6534284a, 0xc96e6806, 0xac0c8dad};
constexpr std::uint32_t kMySimExemplarIid = 0xa52160f5;              // spRM vslot 0x18 iid for the default exemplars
constexpr std::uint32_t kPropertyModelResourceKey = 0x09c80416;      // cGZPersistResourceKey property (SetCar/SetPedestrian)
constexpr std::uint32_t kPropertyBitmapId = 0xebfc5e5e;              // int property: the model button bitmap
constexpr std::uint32_t kCalloutImageType = 0x46a006b0;              // cSLBuffer image of the residence callout
constexpr std::uint32_t kCalloutImageGroup = 0x856ddbac;
constexpr std::uint32_t kCalloutImageInstance = 0x14416190;

class cISC4MySim;
class cSC4MySim;
class cISC4TrafficSimulator;
class cISC4Occupant;
class cISC4OccupantManager;
class cSC4Lot;
class cISC4MySimBalloonAdvice;
class IMySimHomeSound;
class IMySimDispatchVehicleView;
class IMySimDispatchAutomaton;

// cSC4MySimDispatch::cLocation (0x30 bytes; ctor @ 0x1006ac5fc). The dispatch neighbour is not recovered yet, so the
// location it is addressed by is mirrored here and used by both sides of the seam.
struct MySimLocation {
    cRZAutoRefCount<cISC4Occupant> occupant;   // +0x00
    float x = -1000.0f;                        // +0x08 (0xc1800000 = -1000)
    float z = -1000.0f;                        // +0x0c
    std::int32_t cellX = -1;                   // +0x10
    std::int32_t cellZ = -1;                   // +0x14
    std::int32_t lotCellX = -1;                // +0x18
    std::int32_t lotCellZ = -1;                // +0x1c
    bool hasOccupant = true;                   // +0x20 (ctor stores 1)
    bool commuteLocation = false;              // +0x21 (SetByCommute @ 0x1006ac7c4)
    std::int32_t data24 = 0;                   // +0x24
    std::int32_t data28 = 0;                   // +0x28

    MySimLocation() = default;                                        // @ 0x1006ac5fc
    bool SetByOccupant(cISC4Occupant* occupant);                      // @ 0x1006ac638 (resets, then fills from the occupant)
    bool SetByCommute() { commuteLocation = true; return true; }      // @ 0x1006ac7c4
    bool operator==(const MySimLocation& other) const;                // @ 0x1006acaac
    bool IsValid(class IMySimHost& host) const;                       // @ 0x1006ac7da
    void SetCellPosition(std::int32_t x, std::int32_t z);             // @ 0x1006ac5c6
};

// One balloon entry (+0x338, 0x10 bytes): the advice the balloon shows, the sim day it may reappear and the start time.
struct MySimBalloonInfo {
    cRZAutoRefCount<cISC4MySimBalloonAdvice> advice;   // +0x00
    std::int32_t nextDay = 0;                          // +0x08
    std::int32_t startTime = 0;                        // +0x0c
};

// cSC4MySimDispatch (70 functions at 0x1006acb3c..0x1006b3280, object size 0x210) is not recovered yet: the seam
// mirrors the calls cSC4MySim makes into it (the cSC4MySimDispatch:: address of each one).
class IMySimDispatch : public cIGZUnknown {
public:
    virtual bool Init() = 0;                                                        // @ 0x1006ad11a
    virtual bool Shutdown() = 0;                                                    // @ 0x1006ad48a
    virtual void SetDefaultDestination(const MySimLocation& location) = 0;          // @ 0x1006b00de
    virtual bool SetCurrentCommuteDestination(const MySimLocation& location, bool commute) = 0;   // @ 0x1006b26fe
    virtual bool AddDestinationLocation(const MySimLocation& location, int flags) = 0;            // @ 0x1006afdd8
    virtual bool GoTo(const MySimLocation& location) = 0;                           // @ 0x1006afb8c
    virtual void TeleportToCommuteLocation() = 0;                                   // @ 0x1006b2860
    virtual const MySimLocation& GetCurrentLocation() const = 0;                    // @ 0x1006b26ca
    virtual const MySimLocation& GetCurrentDestination() const = 0;                 // @ 0x1006b26d4
    virtual const MySimLocation& GetCurrentDefaultDestination() const = 0;          // @ 0x1006b26e2
    virtual const MySimLocation& GetCurrentCommuteDestination() const = 0;          // @ 0x1006b26f0
    virtual bool CheckFlags(int flags) const = 0;                                   // @ 0x1006afa4a
    virtual int GetState() const = 0;                                               // @ 0x1006b0244
    virtual int GetTravelType() const = 0;                                          // @ 0x1006b024e
    virtual int GetTransitSwitchWaitTimeMunutes() = 0;                              // @ 0x1006b2dae
    virtual void SetModelOffset(int modelType, int offset, bool snap) = 0;          // @ 0x1006b000a
    virtual void SetModelBaseResKey(int modelType, const cGZPersistResourceKey& key, int arg) = 0;   // @ 0x1006aff0e
    virtual void StartTracking(int bitmapId, const cRZString& text) = 0;            // @ 0x1006b0266
    virtual void StopTracking() = 0;                                                // @ 0x1006b01d2
    virtual int TrackHandle() const = 0;                                            // field +0x4c
    virtual void SetFollowCamera(bool follow) = 0;                                  // byte field +0x20
    virtual bool CanUDriveIt() = 0;                                                 // @ 0x1006b2dd6
    virtual bool EnterUDriveIt() = 0;                                               // @ 0x1006b2e9c
    virtual bool ExitUDriveIt() = 0;                                                // @ 0x1006b30a6
    // Tuning constants (InitFromTuning @ 0x1007a191c); the original writes the fields directly.
    virtual void SetDestinationApproachDistance(int value) = 0;                     // +0x24
    virtual void SetDestinationProximityMin(int value) = 0;                         // +0x28
    virtual void SetMinTimeIdling(int value) = 0;                                   // +0x2c
    virtual void SetMinTimeAtLocation(int value) = 0;                               // +0x30
    virtual void SetMinTimeAtSwitch(int value) = 0;                                 // +0x34
};

// cSC4MySimBalloonWinProc (0x1006ab464, object size 0x80) is not recovered yet; the seam mirrors the four duration
// setters cSC4MySim::InitFromTuning and the Init/Shutdown calls.
class IMySimBalloonWinProc : public cIGZUnknown {
public:
    virtual bool Init() = 0;                       // @ 0x1006ab5b4
    virtual bool Shutdown() = 0;                   // @ 0x1006ab7e4
    virtual void SetHiddenDuration(int value) = 0; // 0x40
    virtual void SetShownDuration(int value) = 0;  // 0x48
    virtual void SetFadeDuration(int value) = 0;   // 0x50
};

// cSC4WinCalloutBox (ctor @ 0x1003c5f5c, object size 0x278), the callout over the sim's residence.
class IMySimCallout : public cIGZUnknown {
public:
    virtual void SetStyle(std::uint32_t style) = 0;                                  // 0x58
    virtual void SetOptions(int options) = 0;                                        // 0x60
    virtual void SetImage(const cGZPersistResourceKey& key) = 0;                     // 0x20 (a cSLBuffer image)
    virtual void SetText(const cIGZString& text) = 0;                                // 0x40
    virtual void SetScreenPosition(const float* xy) = 0;                             // 0x78
    virtual void AddToView() = 0;                                                    // spView3DWin 0x70(callout->0x18)
    virtual void Show() = 0;                                                         // callout->0x18 window 0x228
    virtual void Activate() = 0;                                                     // callout->0x18 window 0xc0
    virtual void Hide() = 0;                                                         // callout->0x18 window 0x230
};

// The "MySimGhost" effect instance StartGhostEffect creates through the effects manager.
class IMySimEffect : public cIGZUnknown {
public:
    virtual void SetActive(bool active) = 0;                     // 0x18
    virtual void SetTransform(const float* matrix) = 0;          // 0x38 (4x4 matrix)
};

// The sound object PlayHomeSound creates (cSCsound/cGZSnd, at +0x2f0; the original keeps the object it got from
// SL::spPropManager vslot 0x80 and talks to the resource behind its vslot 0x18).
class IMySimHomeSound : public cIGZUnknown {
public:
    virtual void SetPosition(const float* xyz) = 0;   // 0x18 -> vslot 0x48
    virtual bool IsPlaying() const = 0;               // 0x18 -> vslot 0x100(1)
};

// cSC4PylonOccupant (ctor @ 0x10022def8, object size 0x228), the call box the dispatch tool places.
class IMySimPylon : public cIGZUnknown {
public:
    virtual bool Init() = 0;                                     // 0x20
    virtual bool Shutdown() = 0;                                 // 0x28
    virtual void SetState(int state) = 0;                        // 0x30
    virtual void SetBitmap(int bitmapId, const cIGZString& name) = 0;   // 0x40
    virtual void SetFlags(int flags) = 0;                        // 0x48
    virtual void SetKind(int kind) = 0;                          // 0x58
    virtual void SetLinkedObject(cISC4Occupant* occupant) = 0;   // 0x88 (null clears)
    virtual cISC4Occupant* GetLinkedOccupant() = 0;              // 0x90
    virtual void SetLinkedAgent(cIGZUnknown* agent) = 0;         // 0x98 (the sim itself, iid 1)
    virtual bool IsDead() const = 0;                             // 0x80
    virtual cISC4Occupant* AsOccupant() = 0;                     // 0x18
    virtual void SetVisibility(bool visible) = 0;                // occupant 0x90 (SetVisibility(visible, true))
};

// What cSC4MySim needs from the running game. One method per original call chain (the global / vslot chain is in the
// comment; the concrete class behind each is in docs/SEAMS.md).
class IMySimHost {
public:
    virtual ~IMySimHost() = default;
    // --- script state ---------------------------------------------------------------------------------------
    virtual cISCLua* Lua() = 0;                                             // spCity vslot 0x288 -> vslot 0x50
    virtual int LuaUserDataTag(bool stringValue) = 0;                       // spCity 0x288 -> vslot 0xb0(0/1)
    virtual bool LoadString(std::uint32_t group, std::uint32_t instance, cIGZString& out) = 0;  // SC4LoadString
    // --- city -----------------------------------------------------------------------------------------------
    virtual void ResidenceCandidates(std::vector<cISC4BuildingOccupant*>& out) = 0;
        // spCity vslot 0x110 -> GetOccupantsByOccupantManagerCells(cMySimBuildingFilter(true)); borrowed
    virtual bool IsResidenceCandidate(cISC4BuildingOccupant* building) = 0;
        // cMySimBuildingFilter(true).IsOccupantIncluded(building->AsOccupant()) @ 0x1007aaf18
    virtual void BuildingsNearResidence(int cellX, int cellZ, int radius,
                                        std::vector<cISC4BuildingOccupant*>& out) = 0;
        // spCity vslot 0x110 (GetOccupantManager) -> vslot 0x148 GetOccupantsByStandardCityCells(rect +- radius,
        // cMySimBuildingFilter(false)) (i386 0x88 -> 0xa4, UpdateLocalBuildings 0x00547eea)
    virtual cISC4Occupant* OccupantAtCell(int x, int z) = 0;                // spSC4 vslot 0x118 -> vslot 0x58(x, z)
    virtual cSC4Lot* LotByBuilding(cISC4BuildingOccupant* building) = 0;    // spLotManager vslot 0x70(lot key)
    virtual cSC4Lot* LotAtCell(int x, int z) = 0;                           // spLotManager vslot 0x28(x, z, 0)
    virtual cSC4MySim* MySimAtBuilding(cISC4BuildingOccupant* building) = 0;   // spCity vslot 0x238 -> vslot 0x48
    virtual bool AnyTravelConnection(int x, int z) = 0;                     // AnyTravelConnectionExists (cLocation::IsValid)
    virtual bool FindCommutePath(const int homeRect[4], const int jobRect[4], int& minutes) = 0;
        // spCity vslot 0x228 -> vslot 0x30 (path finder) -> 0x28(1), 0x48(3), 0x90(1), 0x98/0xa0(cell rects),
        // 0xb8() (found) and 0xd0() (minutes)
    virtual float TerrainHeight(float x, float z) = 0;                      // spTerrain vslot 0x60
    virtual float TerrainSeaLevel() = 0;                                    // spTerrain vslot 0xe8 (DispatchToolCanDispatch)
    virtual void StandardCellToPosition(int x, int z, float& px, float& pz) = 0;   // spCity vslot 0x308
    virtual int CityCellsX() = 0;                                           // spCity vslot 0x2e8
    virtual int CityCellsZ() = 0;                                           // spCity vslot 0x2f0
    virtual int ResidentialAgeForWealth(int wealth) = 0;                    // spResidentialSim vslot 0x150 GetAverageNewAgeByWealth
    virtual int TrafficCongestion(int x, int z) = 0;
        // GetCongestion @ 0x1007a60fc (i386 0x0054150a): traffic GetCapacity(1, x, z) != 0 -> GetCongestionMap -> cell value
    virtual int ResidentialAgeAtCell(int x, int z) = 0;                     // spResidentialSim 0x148 -> vslot 0x38
    virtual float ResidentialEducationQuotient(int x, int z, int wealth) = 0;   // spResidentialSim vslot 0x128
    virtual float ResidentialHealthQuotient(int x, int z, int wealth) = 0;      // spResidentialSim vslot 0x130
    virtual int HourOfDay() = 0;                                            // sp24HourClock vslot 0x50
    virtual bool SimulatorTickEnabled() = 0;                                // spSimulator vslots 0x78/0x80/0x88 (any true)
    virtual void SpecialBuildings(std::vector<cISC4Occupant*>& out) = 0;
        // spCity 0x1a8 GetBuildingDevelopmentSimulator -> vslot 0x160 GetSpecialBuildings (borrowed; i386 0xd4 -> 0xb0)
    virtual void BuildingOccupantGroups(std::uint32_t buildingType, std::vector<std::uint32_t>& out) = 0;
        // spBuildingDevelopmentSim vslot 0x68(type, key) then cSC4Occupant::GetOccupantGroups(key, out)
    virtual void ResidentialServiceBuildings(bool hospital, std::vector<cISC4Occupant*>& out) = 0;
        // spResidentialSim GetSchoolBuildings / GetHospitalBuildings(list, no groups) (i386 0xbc / 0xc4); borrowed
    virtual bool ResidentialServiceGrade(bool hospital, cISC4Occupant* building, float& grade) = 0;
        // spResidentialSim GetSchoolQueryData / GetHospitalQueryData (i386 0x68 / 0x6c) -> the grade of tQueryData (+0x20)
    // --- UpdateVariables (i386 0x005481e6) ------------------------------------------------------------------
    virtual int ResidentialLifeExpectancy(int x, int z) = 0;                // spResidentialSim GetCellLifeExpectancy (i386 0x74)
    virtual std::uint8_t BuildingCondition(cISC4BuildingOccupant* building) = 0;   // GetBuildingCondition (i386 0x005422a6)
    virtual bool OccupantPowered(cISC4Occupant* occupant) = 0;              // spPowerSim IsOccupantPowered (i386 0x7c)
    virtual bool BuildingWatered(cISC4Occupant* occupant) = 0;              // spPlumbingSim IsBuildingWatered (i386 0x24)
    virtual int CrimeAtCell(int x, int z) = 0;                              // spCrimeSim GetCrimeGrid (0x14) -> GetCellValue (0x1c)
    virtual int ProtectionCoverage(bool police, int x, int z, int width, int height) = 0;
        // spPoliceSim GetPolicePowerGrid / spFireProtectionSim GetFireProtectionGrid (i386 0x14) -> GetAverageValueInCellRect (0x20)
    virtual int PollutionAtCell(int type, int x, int z) = 0;                // spPollutionSim GetPollutionGrid(type) (0x14) -> GetCellValue
    virtual bool DesirabilityAtCell(std::uint32_t developerType, int x, int z, int& value) = 0;
        // spBuildingDevelopmentSim GetDeveloper(type) (i386 0x84) -> GetDesirabilityMap (0x34) -> GetCellValue; false without one
    struct Station { float position[3]; bool funded; float funding; };
    virtual void ProtectionStations(bool police, std::vector<Station>& out) = 0;
        // spPoliceSim GetPoliceStations (i386 0x54) / spFireProtectionSim GetFireStations (0x58): each station's position and
        // GetStationFunding(station, null, &fraction) (0x1c / 0x20)
    // --- the surroundings of a trip (UpdateTripVariables, i386 0x00546994) ----------------------------------------
    // spTrafficSim, spPollutionSim and spCrimeSim are all present (the original samples nothing otherwise).
    virtual bool TripSimulatorsPresent() = 0;
    // The grids UpdateTripVariables reads at a world position: grid vslot 0x4c PositionToTract, then 0x58 GetTractValue.
    enum class TripGrid {
        Congestion,         // spTrafficSim 0x28 GetCongestionMap (uint8)
        PollutionAir,       // spPollutionSim 0x14 GetPollutionGrid(0) (int16)
        PollutionWater,     // GetPollutionGrid(1)
        PollutionGarbage,   // GetPollutionGrid(2)
        Crime,              // spCrimeSim 0x14 GetCrimeGrid (uint8)
        MayorRating,        // spAuraSim 0x30 GetAuraGrid (int8)
        EducationQuotient,  // spResidentialSim 0x54 GetAverageEQGrid (a new float grid per call)
        HealthQuotient,     // spResidentialSim 0x58 GetAverageHQGrid
    };
    virtual double TripGridValue(TripGrid grid, float x, float z) = 0;
    virtual float TravelTimeRatio(int cellX, int cellZ, int travelType) = 0;   // spTrafficSim 0x60 GetTravelTimeRatio
    // One entry of the lot manager's GetLotsInCellRect (iid 0x681905bd vslot 0x1c, unique = false: one entry per cell):
    // the lot's building (lot 0xc0, borrowed, may be null), GetCondition (0x8c) and GetOccupantWealth (0xa4).
    struct TripLot { cISC4BuildingOccupant* building; int condition; int wealth; };
    virtual void LotsInCellRect(int x0, int z0, int x1, int z1, std::vector<TripLot>& out) = 0;
    // spBudgetSim 0xcc GetBudgetItemForPurpose(building's property holder, purpose, item): the item's local funding.
    virtual float BuildingLocalFunding(cISC4BuildingOccupant* building, std::uint32_t purpose) = 0;
    // spDisasterLayer 0x40 GetDisasterLocationCount / 0x44 GetDisasterLocationByIndex: each location's disaster id
    // (instance 0x14 GetID) and cell.
    struct TripDisaster { std::uint32_t id; int x; int z; };
    virtual void DisasterLocations(std::vector<TripDisaster>& out) = 0;
    virtual bool NeighborJobCityName(cIGZUnknown* city, cIGZString& out) = 0;   // the regional city's GetCityName (i386 0x84)
    virtual void FitTextToWidth(cIGZString& text, int width) = 0;
        // RZ::WCM() SystemFont(0x68963c4c) -> FormatForEllipsis(text, DefaultText(0x5377be31), {0, 0, width, line height})
    virtual bool GamePaused() = 0;                                          // RZGetFramework 0xa417445e -> vslot 0x150
    // --- UI / effects ---------------------------------------------------------------------------------------
    virtual bool CameraLookAt(const float pos[3], bool zoomIfFar) = 0;
        // spRenderer vslot 0x80 -> camera control; spRenderProperties -> +0x18 -> +0x978; SetZoom @ 0x10076bbe2 and
        // SetViewTargetPosition @ 0x10076bf2a
    virtual void NotificationDialog(const cIGZString& title, const cIGZString& body) = 0;   // 0x10030b2da
    virtual bool CityName(cIGZString& out) = 0;                             // spSC4 vslot 0x1d8
    virtual IMySimCallout* CreateCallout() = 0;                             // cSC4WinCalloutBox ctor, one reference
    virtual bool LoadMySimBitmap(std::uint32_t bitmapId, const cIGZString* file, cIGZBuffer** out) = 0;
        // GZCOM CreateInstance(0xa2ffb5d3, 0x2ffbb60) + vslot 0x20(file, buffer, 8), or spRM vslot 0x18 with key
        // {0x46a006b0, 0x856ddbac, bitmapId} + cIGZBuffer vslot 0x18(0x20732180); one reference
    virtual bool CalloutImageAvailable() = 0;   // cSLBuffer ctor (0x100043bce) with {0x46a006b0, 0x856ddbac, 0x14416190}
    virtual void ProjectPointToScreen(float xyz[3]) = 0;   // SC4ViewUtilities::ProjectPointToViewportCoordinates2D
    virtual void ReportSimEvent(const cIGZString& text, const float pos[3]) = 0;   // spCity 0x178 -> vslot 0x48
    virtual cIGZMessageServer2* Messages() = 0;             // SL::spMS2
    virtual void PostMessage2(std::uint32_t type, std::uint32_t data1, std::uint32_t data2,
                              std::uint32_t data3, std::uint32_t data4, cIGZUnknown* unknown) = 0;   // SC4PostMessage2
    virtual bool LoadExemplar(const cGZPersistResourceKey& key, std::uint32_t iid, cIGZUnknown** out) = 0;  // spRM 0x18
    virtual bool PickRandomWalkModelKey(cGZPersistResourceKey& out) = 0;    // spCity vslot 0x160 -> vslot 0x78(...)
    virtual IMySimHomeSound* CreateHomeSound(std::uint32_t soundId) = 0;    // spPropManager 0x80(key, &out), one reference
    virtual void DestroyHomeSound(IMySimHomeSound* sound) = 0;              // spPropManager vslot 0xe8
    virtual void PlayCitySound(IMySimHomeSound* sound, const float pos[3]) = 0;   // spCity 0x110 -> vslot 0xa0(sound, 0)
    virtual IMySimEffect* CreateGhostEffect() = 0;                          // spEffectsManager vslot 0x30("MySimGhost")
    virtual bool DispatchIconRect(int trackHandle, int rect[4]) = 0;        // spDispatchManager 0xb0 -> GetLastScreenIconRect
    // --- cSC4MyStreetSim ------------------------------------------------------------------------------------
    virtual IMySimDispatchVehicleView* DispatchVehicleView() { return nullptr; }   // spDispatchManager 0xb0 (borrowed)
    virtual bool SimulatorAnyPaused() { return false; }                      // spSimulator IsAnyPaused (i386 vslot 0x48)
    virtual std::uint32_t ViewInputControlId() { return 0; }                // spView3D 0x60 (current control) -> vslot 0x14
    // --- objects --------------------------------------------------------------------------------------------
    virtual IMySimDispatch* CreateDispatch(cSC4MySim* owner) = 0;           // cSC4MySimDispatch ctor @ 0x1006acb3c
    virtual IMySimBalloonWinProc* CreateBalloonWinProc(cISC4MySim* owner) = 0;   // cSC4MySimBalloonWinProc @ 0x1006ab464
    virtual IMySimPylon* CreatePylon() = 0;                                 // cSC4PylonOccupant ctor, one reference
    // --- view input controls --------------------------------------------------------------------------------
    virtual cIGZUnknown* CurrentViewInputControl() = 0;                     // mspViewInputControl (borrowed)
    virtual void SetCurrentViewInputControl(cIGZUnknown* control) = 0;       // assigns the global auto-ref
    virtual cIGZUnknown* ViewCurrentInputControl() = 0;                     // spView3D vslot 0xc0
    virtual void ViewSetInputControl(cIGZUnknown* control) = 0;             // spView3D vslot 0xd0(control)
    virtual void ViewPushInputControl(cIGZUnknown* control) = 0;            // spView3D vslot 0xc8(control, 2)
    virtual cIGZUnknown* CreateMoveInTool(cSC4MySim* owner) = 0;            // cSC4MySimInputControlMoveIn @ 0x1006b8a1c
    virtual cIGZUnknown* CreateDispatchToolControl(cSC4MySim* owner) = 0;   // cSC4MySimInputControlDispatch @ 0x1006b788c
    virtual cIGZUnknown* CreateChangeHouseTool(cSC4MySim* owner) = 0;       // cSC4MySimInputControlChangeHouse
    virtual cIGZUnknown* CreateChangeJobTool(cSC4MySim* owner) = 0;         // cSC4MySimInputControlChangeJob
    // --- message plumbing -----------------------------------------------------------------------------------
    virtual void RegisterInputControlTarget(cIGZMessageTarget2* target) = 0;     // SL::ATM() vslot 0x28(target, 0xff)
    virtual void UnregisterInputControlTarget(cIGZMessageTarget2* target) = 0;   // SL::ATM() vslot 0x30(target, 0xff)
    virtual bool AddNotification(cIGZMessageTarget2* target, std::uint32_t type) = 0;      // spMS2 0x28
    virtual bool RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) = 0;   // spMS2 0x30
    // --- game data registry (init_balloons / Shutdown) ------------------------------------------------------
    virtual void AddToGameDataRegistry(cSC4MySim& sim) = 0;   // AddNode("mysims") + AddValue("age"/"name")
    virtual void RemoveFromGameDataRegistry() = 0;            // registry vslot 0x30(0)
    // --- occupant serialization -----------------------------------------------------------------------------
    virtual void WriteOccupant(cIGZOStream& stream, cISC4BuildingOccupant* occupant) = 0;      // SC4WriteGZSerializable
    virtual bool ReadOccupant(cIGZIStream& stream, cISC4BuildingOccupant** out) = 0;           // SC4ReadGZSerializable(0x87dda3a9)
    virtual bool ReadPylon(cIGZIStream& stream, IMySimPylon** out) = 0;                        // SC4ReadGZSerializable(0xc9b6d7d6)
    virtual void WritePylon(cIGZOStream& stream, IMySimPylon* pylon) = 0;                      // SC4WriteGZSerializable
    virtual void WriteDispatch(cIGZOStream& stream, IMySimDispatch* dispatch) = 0;             // SC4WriteGZSerializable
    virtual bool ReadDispatch(cIGZIStream& stream, IMySimDispatch** out) = 0;                  // SC4ReadGZSerializable(0xeb8b6f9a)
    virtual cISC4AdviceSubject* CreateLuaSubject(cISC4Occupant* occupant, bool string) = 0;
        // GZCOM CreateInstance(0x6a26d049, 0x6a26d050) + vslot 0x38(occupant) for buildings; one reference
};
IMySimHost* MySimHost();
void SetMySimHost(IMySimHost* host);

// The free helpers of the original MySim unit (kept as free functions like the originals).
void MySimSetTableString(cISCLua* lua, const char* key, cIGZString* value);             // @ 0x1007a5ce4
void MySimSetTableNumber(cISCLua* lua, const char* key, long value);                    // SetTableNumber (i386)
void MySimSetTableBuilding(cISCLua* lua, const char* key, cISC4BuildingOccupant* value); // @ 0x1007a5be9
void MySimSetTableOccupantSubject(cISCLua* lua, const char* key, cISC4Occupant* value);  // @ 0x1007a61d0
int MySimGetCongestion(cISC4TrafficSimulator* traffic, int x, int z);                   // @ 0x1007a60fc

// cISC4MySim — the 69-slot interface object at (+0x148 in the original). Slot numbers are the original vslots.
class cISC4MySim : public cIGZUnknown {
public:
    virtual cISC4Advisor* AsAdvisor() = 0;                                              // 0x18
    virtual void MoveIn(std::uint8_t slot, cRZString& name, bool male, std::uint8_t zodiac, cRZString& bio,
                        std::uint32_t bitmapId, cRZString& bitmapFile) = 0;             // 0x20
    virtual bool FindNewResidence() = 0;                                                // 0x28
    virtual bool ChangeResidence(cISC4BuildingOccupant* building) = 0;                  // 0x30
    virtual bool TestForResidence(cISC4BuildingOccupant* building, bool checkFilter, bool checkWealth) = 0;  // 0x38
    virtual bool ChangeJob(cISC4BuildingOccupant* building) = 0;                        // 0x40
    virtual bool DeleteFromGame() = 0;                                                  // 0x48
    virtual std::uint8_t GetSlot() const = 0;                                           // 0x50
    virtual cISC4BuildingOccupant* GetResidence() const = 0;                            // 0x58
    virtual cISC4BuildingOccupant* GetJob() const = 0;                                  // 0x60
    virtual void SetResidence(cISC4BuildingOccupant* building) = 0;                     // 0x68
    virtual bool GetLocation(float* xyz) const = 0;                                     // 0x70
    virtual cGZPersistResourceKey GetVehicleKey() const = 0;                            // 0x78
    virtual std::uint8_t GetSign() const = 0;                                           // 0x80
    virtual std::uint8_t GetAge() const = 0;                                            // 0x88
    virtual bool IsMale() const = 0;                                                    // 0x90
    virtual int GetWealth() const = 0;                                                  // 0x98
    virtual bool IsActive() const = 0;                                                  // 0xa0
    virtual const cRZString& GetBio() const = 0;                                        // 0xa8
    virtual std::uint32_t GetBitmapID() const = 0;                                      // 0xb0
    virtual const cRZString& GetBitmapFile() const = 0;                                 // 0xb8
    virtual bool GetBitmapBuffer(cIGZBuffer** out) const = 0;                           // 0xc0
    virtual const cRZString& GetHomeName() const = 0;                                   // 0xc8
    virtual const cRZString& GetJobName() const = 0;                                    // 0xd0
    virtual const cRZString& GetJobTitle() const = 0;                                   // 0xd8
    virtual bool GetEQAsText(cRZString& out) = 0;                                       // 0xe0
    virtual bool GetHQAsText(cRZString& out) = 0;                                       // 0xe8
    virtual void Find() = 0;                                                            // 0xf0
    virtual void ShowResidence() = 0;                                                   // 0xf8
    virtual void HideResidence() = 0;                                                   // 0x100
    virtual void ShowLocation() = 0;                                                    // 0x108
    virtual void HideLocation() = 0;                                                    // 0x110
    virtual void SetPylonVisibility(bool visible) = 0;                                  // 0x118
    virtual void UpdateResidence() = 0;                                                 // 0x120
    virtual void LeaveForWork() = 0;                                                    // 0x128
    virtual void ReturnHome() = 0;                                                      // 0x130
    virtual void AnnounceMoveIn() = 0;                                                  // 0x138
    virtual void DoScriptCommand(int command) = 0;                                      // 0x140
    virtual void DoBuildingAdded(cISC4BuildingOccupant* building) = 0;                  // 0x148
    virtual void DoBuildingChanged(cISC4BuildingOccupant* building) = 0;                // 0x150
    virtual void DoBuildingRemoved(cISC4BuildingOccupant* building, std::uint32_t data) = 0;   // 0x158
    virtual void DoFireAdded(float x, float z) = 0;                                     // 0x160
    virtual void DoPipeBurst(float x, float z) = 0;                                     // 0x168
    virtual void DoTornado(float x, float z) = 0;                                       // 0x170
    virtual void DoDisaster(std::uint32_t disaster, int x, int z) = 0;                  // 0x178
    virtual void DoNeighborConnectionRemoved(int cellX, int cellZ) = 0;                 // 0x180
    virtual int DistanceToClosest(std::uint32_t occupantGroup) = 0;                     // 0x188
    virtual std::uint32_t DistanceToClosestSpecial(std::uint32_t buildingType) = 0;     // 0x190
    virtual float LocalSchoolGrade(std::uint32_t occupantGroup) = 0;                    // 0x198
    virtual float LocalHospitalGrade(std::uint32_t occupantGroup) = 0;                  // 0x1a0
    virtual void PlayNightSound() = 0;                                                  // 0x1a8
    virtual bool GetCurrentScreenLocationXY(int* x, int* y) = 0;                        // 0x1b0
    virtual void GetJobTitle(cISC4BuildingOccupant* building, cIGZString& out) = 0;     // 0x1b8
    virtual bool RequestToPlaySound() = 0;                                              // 0x1c0
    virtual bool SetCar(cISCResExemplar* exemplar) = 0;                           // 0x1c8
    virtual bool SetPedestrian(cISCResExemplar* exemplar) = 0;                    // 0x1d0
    virtual std::uint32_t GetModelButtonBitmapID(bool pedestrian) const = 0;            // 0x1d8
    virtual cISC4MySimBalloonAdvice* GetNextBalloonAdvice() = 0;                        // 0x1e0
    virtual cISC4MySimBalloonAdvice* GetNextBalloonEventAdvice() = 0;                   // 0x1e8
    virtual cISC4MySimBalloonAdvice* GetCurrentBalloonAdvice() const = 0;               // 0x1f0
    virtual cISC4MySimBalloonAdvice* GetCurrentBalloonEventAdvice() const = 0;          // 0x1f8
    virtual bool CanUDriveIt() = 0;                                                     // 0x200
    virtual bool EnterUDriveIt() = 0;                                                   // 0x208
    virtual bool ExitUDriveIt() = 0;                                                    // 0x210
    virtual bool CreateDispatchTool() = 0;                                              // 0x218
    virtual bool ActivateLuaContext() = 0;                                              // 0x220
};

// cSC4MySim's primary vtable (0x100aeef70) as cSC4MySimDispatch uses it: DoMessage delivers the 0x2ba06b99
// notifications, GetWealth (0x170) and IsMale (0x178) pick the walking automata group. The original hands the dispatch the
// sim's primary interface (iid 0x8bc01ffe); cSC4MySim answers that iid with this view so a dispatch read back from a
// save reaches its owner through the same three slots.
class IMySimDispatchOwner : public cIGZUnknown {
public:
    virtual bool DoMessage(cIGZMessage2* message) = 0;   // 0xc8 @ 0x1007a3364
    virtual int GetWealth() const = 0;                   // 0x170 @ 0x1007a7f7c
    virtual bool IsMale() const = 0;                     // 0x178 @ 0x1007a7f74 (walking group: 0x4149 / 0x414a)
};

class cSC4MySim : public cSC4Advisor, public cISC4MySim, public IMySimDispatchOwner {
public:
    cSC4MySim();                                       // @ 0x10079ebcc
    ~cSC4MySim() override;                             // @ 0x10079f23c (deleting 0x10079f7ba, thunks)

    // cIGZUnknown
    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x10079f7e8 (thunks 0x10079f822/830/83e/84c)
    std::uint32_t AddRef() override;                               // @ 0x10079f85e (thunks ...868/876/884/892)
    std::uint32_t Release() override;                              // @ 0x10079f8a4 (thunks ...8ae/8bc/8ca/8d8)

    // cSC4Advisor overrides
    bool Init(int advisorType) override;                           // @ 0x10079f8ea (thunk 0x1007a1bb8)
    bool Shutdown() override;                                      // @ 0x1007a1fe8 (thunk 0x1007a25a0)
    cIGZString& GetPersonalName() override;                        // @ 0x1007a261a (thunk 0x1007a27be)
    bool CommandWouldTrigger(std::uint32_t commandId) override;    // @ 0x1007a8f4e (thunk 0x1007a8f86)
    bool DoMessage(cIGZMessage2* message) override;                // @ 0x1007a3364 (thunk 0x1007a5778)
    bool Write(cIGZOStream& stream) const override;                // @ 0x1007a27ce (thunk 0x1007a2b0e)
    bool Read(cIGZIStream& stream) override;                       // @ 0x1007a2b1c (thunk 0x1007a2fb4)
    std::uint32_t GetGZCLSID() const override;                     // @ 0x1007aade4 (thunk 0x1007aae08)
    void simulate_day() override;                                  // @ 0x1007a6344
    void simulate_day_events() override;                           // @ 0x1007a6676
    void simulate_month() override;                                // @ 0x1007a6e26

    // The primary vtable's own slots, in the binary's declaration order (0x100..0x348).
    cISC4Advisor* AsAdvisor() override;                             // 0x100 @ 0x1007aadf0
    void MoveIn(std::uint8_t slot, cRZString& name, bool male, std::uint8_t zodiac, cRZString& bio,
                std::uint32_t bitmapId, cRZString& bitmapFile) override;   // 0x108 @ 0x1007a6fa6
    bool FindNewResidence() override;                               // 0x110 @ 0x1007a718c
    bool ChangeResidence(cISC4BuildingOccupant* building) override; // 0x118 @ 0x1007a7556
    bool ChangeJob(cISC4BuildingOccupant* building) override;       // 0x120 @ 0x1007a768a
    bool TestForResidence(cISC4BuildingOccupant* building, bool checkFilter, bool checkWealth) override;   // 0x128
    bool DeleteFromGame() override;                                 // 0x130 @ 0x1007a7722
    std::uint8_t GetSlot() const override;                          // 0x138 @ 0x1007a7f64
    cISC4BuildingOccupant* GetResidence() const override;           // 0x140 @ 0x1007a7e06
    cISC4BuildingOccupant* GetJob() const override;                 // 0x148 @ 0x1007a7e1e
    void SetResidence(cISC4BuildingOccupant* building) override;    // 0x150 @ 0x1007a7918
    cGZPersistResourceKey GetVehicleKey() const override;           // 0x158 @ 0x1007a7ef2
    std::uint8_t GetAge() const override;                           // 0x160 @ 0x1007a7f48
    std::uint8_t GetSign() const override;                          // 0x168 @ 0x1007a7f30
    int GetWealth() const override;                                 // 0x170 @ 0x1007a7f7c
    bool IsMale() const override;                                   // 0x178 @ 0x1007a845c
    bool IsActive() const override;                                 // 0x180 @ 0x1007a8472
    const cRZString& GetBio() const override;                       // 0x188 @ 0x1007a8490
    std::uint32_t GetBitmapID() const override;                     // 0x190 @ 0x1007a7f92
    const cRZString& GetBitmapFile() const override;                // 0x198 @ 0x1007a7faa
    bool GetBitmapBuffer(cIGZBuffer** out) const override;          // 0x1a0 @ 0x1007a7fc6
    const cRZString& GetHomeName() const override;                  // 0x1a8 @ 0x1007a81f8
    const cRZString& GetJobName() const override;                   // 0x1b0 @ 0x1007a8240
    const cRZString& GetJobTitle() const override;                  // 0x1b8 @ 0x1007a82ea
    bool GetEQAsText(cRZString& out) override;                      // 0x1c0 @ 0x1007a8394
    bool GetHQAsText(cRZString& out) override;                      // 0x1c8 @ 0x1007a83e4
    void Find() override;                                           // 0x1d0 @ 0x1007a84a8
    void ShowResidence() override;                                  // 0x1d8 @ 0x1007a8876
    void HideResidence() override;                                  // 0x1e0 @ 0x1007a8af8
    void ShowLocation() override;                                   // 0x1e8 @ 0x1007a8c6c
    void HideLocation() override;                                   // 0x1f0 @ 0x1007a8d5c
    void SetPylonVisibility(bool visible) override;                 // 0x1f8 @ 0x1007a8dba
    void UpdateResidence() override;                                // 0x200 @ 0x1007a8b64
    void LeaveForWork() override;                                   // 0x208 @ 0x1007a9182
    void ReturnHome() override;                                     // 0x210 @ 0x1007a9202
    void AnnounceMoveIn() override;                                 // 0x218 @ 0x1007a928e
    void DoScriptCommand(int command) override;                     // 0x220 @ 0x1007a93a0
    void DoBuildingAdded(cISC4BuildingOccupant* building) override; // 0x228 @ 0x1007a96b2
    void DoBuildingChanged(cISC4BuildingOccupant* building) override;   // 0x230 @ 0x1007a9804
    void DoBuildingRemoved(cISC4BuildingOccupant* building, std::uint32_t data) override;   // 0x238 @ 0x1007a9858
    void DoFireAdded(float x, float z) override;                    // 0x240 @ 0x1007a9a34
    void DoPipeBurst(float x, float z) override;                    // 0x248 @ 0x1007a9b1a
    void DoTornado(float x, float z) override;                      // 0x250 @ 0x1007a9c00
    void DoDisaster(std::uint32_t disaster, int x, int z) override; // 0x258 @ 0x1007a9ce6
    void DoNeighborConnectionRemoved(int cellX, int cellZ) override;   // 0x260 @ 0x1007a9e64
    int DistanceToClosest(std::uint32_t occupantGroup) override;    // 0x268 @ 0x1007a6830
    std::uint32_t DistanceToClosestSpecial(std::uint32_t buildingType) override;   // 0x270 @ 0x1007a6896
    float LocalSchoolGrade(std::uint32_t occupantGroup) override;   // 0x278 @ 0x1007a69ee (i386 0x00543a48)
    float LocalHospitalGrade(std::uint32_t occupantGroup) override; // 0x280 @ 0x1007a6c06 (i386 0x0054392a)
    void PlayNightSound() override;                                 // 0x288 @ 0x1007a912a
    bool GetLocation(float* xyz) const override;                    // 0x290 @ 0x1007aa520
    bool RequestToPlaySound() override;                             // 0x298 @ 0x1007a8fc2
    cISC4MySimBalloonAdvice* GetNextBalloonAdvice() override;       // 0x2a0 @ 0x1007a3260
    cISC4MySimBalloonAdvice* GetNextBalloonEventAdvice() override;  // 0x2a8 @ 0x1007a3286
    cISC4MySimBalloonAdvice* GetCurrentBalloonAdvice() const override;   // 0x2b0 @ 0x1007a32ac
    cISC4MySimBalloonAdvice* GetCurrentBalloonEventAdvice() const override;   // 0x2b8 @ 0x1007a3308
    bool CanUDriveIt() override;                                    // 0x2c0 @ 0x1007aac66
    bool EnterUDriveIt() override;                                  // 0x2c8 @ 0x1007aaca2
    bool ExitUDriveIt() override;                                   // 0x2d0 @ 0x1007aad14
    bool ActivateLuaContext() override;                             // 0x2d8 @ 0x10079e9c6 (thunk 0x10079ea94)
    void* DispatchToolBegin();                                      // 0x2e0 @ 0x1007a9e9e (returns the pylon)
    bool DispatchToolEnd();                                         // 0x2e8 @ 0x1007aa0fa
    bool DispatchToolLockPylon(bool lock);                          // 0x2f0 @ 0x1007aa262
    bool DispatchToolIsPylonLocked();                               // 0x2f8 @ 0x1007aa4ae (const in the binary)
    IMySimPylon* DispatchToolGetPylon();                            // 0x300 @ 0x1007aa4ba
    void DispatchToolSetFollowCamera(bool follow);                  // 0x308 @ 0x1007aa4c8
    bool DispatchToolCanDispatch(float x, float z);                 // 0x310 @ 0x1007aa4da (const in the binary)
    bool CreateDispatchTool() override;                             // 0x318 @ 0x1007aa9da
    std::uint32_t GetModelButtonBitmapID(bool pedestrian) const override;   // 0x320 @ 0x1006bbbe2
    bool SetCar(cISCResExemplar* exemplar) override;          // 0x328 @ 0x1007aa5f0
    bool SetPedestrian(cISCResExemplar* exemplar) override;   // 0x330 @ 0x1007aa740
    void GetJobTitle(cISC4BuildingOccupant* building, cIGZString& out) override;   // 0x338 @ 0x1007a7e36
    bool GetCurrentScreenLocationXY(int* x, int* y) override;       // 0x340 @ 0x1007a8e36
    int get_track_handle();                                         // 0x348 @ 0x1007aab7c
    // Not in the vtable: CreateLuaTable/ActivateLuaContext helpers and the create-tool wrappers.
    void CreateLuaTable();                                          // @ 0x10079ea2a
    // derived-class access (cSC4MyStreetSim's constructor writes +0x1ec / +0x230, its Init reads +0x244)
    void SetBitmapId(std::uint32_t id) { bitmapId_ = id; }
    void SetTrackHandle(std::int32_t handle) { trackHandle_ = handle; }
    std::int32_t LuaTableRef() const { return luaTableRef_; }
    bool CreateChangeHouseTool();                                   // @ 0x1007aaab4
    bool CreateChangeJobTool();                                     // @ 0x1007aab9e
    cGZPersistResourceKey GetRandomWalkModelKey();                  // @ 0x1007aa8d0

    // Non-virtual helpers (private in the original; public for the subclass and the test).
    void UpdateVariables();                                        // @ 0x10079fcb2 (i386 0x005481e6)
    void UpdateTripVariables(int newState, int oldState, int newFlags, int oldFlags, bool force);   // @ 0x1007a3e66
    void UpdateQuickVariables();                                   // @ 0x1007a5800
    void UpdateLocalBuildings();                                   // @ 0x1007a5d5e
    std::uint32_t DistanceToPosition(const float* xyz) const;      // DistanceToOccupant (i386 0x0054147a), from the position
    float LocalServiceGrade(bool hospital, std::uint32_t occupantGroup);   // LocalSchoolGrade / LocalHospitalGrade body
    void UpdateJobTitle();                                         // @ 0x1007a6160
    void init_balloons();                                          // @ 0x1007a10f0
    bool InitFromTuning();                                         // @ 0x1007a191c
    void RemoveHomeSound();                                        // @ 0x1007a2502
    void CancelViewInput();                                        // @ 0x1007a25ae
    void empty_all_balloon_queues();                               // @ 0x1007a2fc2
    bool pop_balloon_queue(int queueType, int statusMask);         // @ 0x1007a3038
    void push_balloon_queue(int queueType, int index);             // @ 0x1007a3118
    cISC4MySimBalloonAdvice* get_next_balloon(int queueType);      // @ 0x1007a31b6
    void simulate_balloons(bool force);                            // @ 0x1007a3bd6
    void SampleTripLocalArea(cISCLua* lua, IMySimHost* host, int x0, int z0, int x1, int z1, cIGZString* noLandmarkName);
    int PickIdleOffset();                                          // @ 0x1007a3d3a
    bool is_pylon_location(const MySimLocation& location);         // @ 0x1007a5690
    void FailCommute();                                            // @ 0x1007a56ee
    bool FindNewJob();                                             // @ 0x1007a5786
    void CheckCommute();                                           // @ 0x1007a64f2
    void MoveOut();                                                // @ 0x1007a7112
    void ResetAdvisorState();                                      // @ 0x1007a77b8
    bool DispatchCommute();                                        // @ 0x1007a7bec
    void QuitJob();                                                // @ 0x1007a8ebe
    void PlayHomeSound(std::uint32_t soundId);                     // @ 0x1007a9042
    void PostChangedMessage();   // SC4PostMessage2(0x4bc42c48, 0, 0, 0, 0, the cISC4MySim view)
    void Spawn();                                                  // @ 0x1007a9480
    void IncreaseWealth();                                         // @ 0x1007a94fe
    void Die();                                                    // @ 0x1007a953e
    void LeaveCity();                                              // @ 0x1007a9600
    void StartGhostEffect();                                       // @ 0x1007a9934
    void SendMySimsChangedMessage();                               // free function @ 0x1007a787b

    // Layout / state accessors (for the subclasses and the test).
    std::int32_t State() const { return state_; }                       // +0x154
    std::uint16_t Generation() const { return generation_; }            // +0x1bc
    // The game data registry handles init_balloons registers: the age byte ("age", +0x1e8) and the personal name ("name").
    std::uint8_t* AgeField() { return &age_; }
    void* NameHandle() { return &cSC4Advisor::GetPersonalName(); }
    bool Retired() const { return retired_; }                           // +0x238
    bool BalloonsEnabled() const { return balloonsEnabled_; }           // +0x330
    bool PylonLocked() const { return pylonLocked_; }                   // +0x3b1
    const std::map<int, int>& EventBalloons() const { return eventBalloons_; }    // +0x350
    std::vector<MySimBalloonInfo>& Balloons() { return balloons_; }     // +0x338
    std::list<int>& BalloonQueue() { return balloonQueue_; }            // +0x368
    std::list<int>& EventQueue() { return eventQueue_; }                // +0x380
    IMySimDispatch* Dispatch() const { return dispatch_.get(); }        // +0x328
    IMySimCallout* Callout() const { return callout_.get(); }           // +0x218
    IMySimPylon* Pylon() const { return pylon_.get(); }                 // +0x3b8
    IMySimBalloonWinProc* BalloonWinProc() const { return balloonWinProc_.get(); }   // +0x3a0
    cRZRandom& Random() { return random_; }                             // +0x290
    void SetStateForTest(std::int32_t state) { state_ = state; }
    void SetLastSoundTimeForTest(std::int32_t value) { lastSoundTime_ = value; }
    cRZString& Bio() { return bio_; }                                   // +0x1c0
    cRZString& BitmapFile() { return bitmapFile_; }                     // +0x1f0
    void SetCallout(IMySimCallout* callout) { callout_ = callout; }     // +0x218

protected:   // cSC4MyStreetSim reads the state, the script table, the track handle and the timers
    void BuildStrings();                    // the three lazily created strings of Init
    cRZString& NoJobString();               // +0x308
    cRZString& RetiredString();             // +0x310
    cRZString& EmptyString();               // +0x300

    // +0x150 onwards (original offsets; see the file comment).
    std::uint8_t slot_ = 0;                              // +0x150
    std::int32_t state_ = 0;                             // +0x154
    cRZAutoRefCount<cISC4BuildingOccupant> residence_;   // +0x158
    std::int32_t residenceCellX_ = 0;                    // +0x160
    std::int32_t residenceCellZ_ = 0;                    // +0x164
    float posX_ = 0.0f;                                  // +0x168
    float posY_ = 0.0f;                                  // +0x16c
    float posZ_ = 0.0f;                                  // +0x170
    std::int32_t pad174_ = 0;                            // +0x174 (unused in the recovered functions)
    cRZAutoRefCount<cSC4Lot> lot_;                       // +0x178
    cRZAutoRefCount<cISC4BuildingOccupant> job_;         // +0x180
    cRZAutoRefCount<cISC4BuildingOccupant> neighborJob_; // +0x188
    std::int32_t jobCell_[4] = {0, 0, 0, 0};             // +0x190..0x19c
    cRZAutoRefCount<cIGZUnknown> unknown1a0_;            // +0x1a0
    std::int32_t unemployedDays_ = 0;                    // +0x1a8
    cGZPersistResourceKey vehicleKey_;                   // +0x1ac
    std::int32_t wealth_ = 0;                            // +0x1b8
    std::uint16_t generation_ = 1;                       // +0x1bc
    std::uint8_t zodiac_ = 0;                            // +0x1be
    std::uint8_t male_ = 1;                              // +0x1bf
    cRZString bio_;                                      // +0x1c0
    std::uint8_t age_ = 0;                               // +0x1e8
    std::uint8_t birthMonth_ = 0;                        // +0x1e9
    std::uint32_t bitmapId_ = 0;                         // +0x1ec
    cRZString bitmapFile_;                               // +0x1f0
    cRZAutoRefCount<IMySimCallout> callout_;             // +0x218
    bool calloutShown_ = false;                          // +0x220
    cRZAutoRefCount<cIGZUnknown> unknown228_;            // +0x228
    std::int32_t trackHandle_ = 0;                       // +0x230
    std::int32_t lastCommuteTime_ = 0;                   // +0x234
    bool retired_ = false;                               // +0x238
    std::int32_t lastLocalDisaster_ = 0;                 // +0x23c
    bool commuteCheck_ = false;                          // +0x240
    std::int32_t luaTableRef_ = 0;                       // +0x244
    cRZAutoRefCount<cRZString> homeBuildingText_;        // +0x248
    cRZAutoRefCount<cRZString> jobBuildingText_;         // +0x250
    cRZAutoRefCount<cRZString> jobTitleText_;            // +0x258 (never written by the recovered functions)
    cRZAutoRefCount<cRZString> jobCityText_;             // +0x260
    cRZAutoRefCount<cRZString> generationName_;          // +0x268
    float eq_ = 0.0f;                                    // +0x270
    float hq_ = 0.0f;                                    // +0x274
    cRZAutoRefCount<cIGZUnknown> unknown278_;            // +0x278
    cRZAutoRefCount<cIGZUnknown> unknown280_;            // +0x280
    cRZAutoRefCount<IMySimEffect> ghost_;                // +0x288
    cRZRandom random_{0xffffffff};                       // +0x290
    float eqHistory_[6] = {};                            // +0x2a8
    float hqHistory_[6] = {};                            // +0x2c0
    std::uint8_t eqHistoryIndex_ = 0;                    // +0x2d8
    std::uint8_t hqHistoryIndex_ = 0;                    // +0x2d9
    float deltaEq_ = 0.0f;                               // +0x2dc
    float deltaHq_ = 0.0f;                               // +0x2e0
    bool balloonsInitialized_ = false;                   // +0x2e4
    std::map<std::uint32_t, std::uint32_t>* groupDistances_ = nullptr;   // +0x2e8 (owned)
    cRZAutoRefCount<IMySimHomeSound> homeSound_;         // +0x2f0
    cRZAutoRefCount<cISCLua> lua_;                       // +0x2f8
    cRZAutoRefCount<cRZString> emptyText_;               // +0x300
    cRZAutoRefCount<cRZString> noJobText_;               // +0x308 ("no job")
    cRZAutoRefCount<cRZString> retiredText_;             // +0x310 ("retired")
    std::int32_t balloonStartTime_ = 0;                  // +0x318
    std::uint32_t pedestrianBitmapId_ = 0;               // +0x31c
    std::uint32_t carBitmapId_ = 0;                      // +0x320
    cRZAutoRefCount<IMySimDispatch> dispatch_;           // +0x328
    bool balloonsEnabled_ = false;                       // +0x330
    std::vector<MySimBalloonInfo> balloons_;             // +0x338
    std::map<int, int> eventBalloons_;                   // +0x350
    std::list<int> balloonQueue_;                        // +0x368
    std::list<int> eventQueue_;                          // +0x380
    std::int32_t balloonIndex_ = 0;                      // +0x398
    cRZAutoRefCount<IMySimBalloonWinProc> balloonWinProc_;   // +0x3a0
    std::int32_t defaultCarModel_ = 0;                   // +0x3a8
    std::int32_t localAreaSize_ = 7;                     // +0x3ac (ctor stores 7; tuning default 10)
    bool dispatchToolActive_ = false;                    // +0x3b0
    bool pylonLocked_ = false;                           // +0x3b1
    cRZAutoRefCount<IMySimPylon> pylon_;                 // +0x3b8
    cGZPersistResourceKey carModelKey_;                  // +0x3c0
    cGZPersistResourceKey pedestrianModelKey_;           // +0x3cc
    std::int32_t lastTripDay_ = -1;                      // +0x3d8
    cRZTimer pylonTimer_{cRZTimer::kSeconds};            // +0x3e0
    cRZTimer balloonsTimer_{cRZTimer::kSeconds};         // +0x3f8
    std::int32_t lastPlayerTripDay_ = 0;                 // +0x410
    std::int32_t lastSoundTime_ = 0;                     // +0x414
    cRZTimer soundTimer_{cRZTimer::kSeconds};            // +0x418
};

}  // namespace sc4
