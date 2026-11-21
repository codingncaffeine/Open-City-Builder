// cSC4MySimDispatch — the object that walks one MySim through the city: it owns the sim's current/default/commute
// destinations (cLocation), asks the traffic simulator's path finder for a route whenever the destination changes, turns
// the route into a deque of cISC4PathFinder::RouteData legs, creates/reuses the automaton the sim rides (car, bus, train,
// on foot), tells the tracked car/pedestrian when to switch travel type, and reports every step to cSC4MySim through one
// direct DoMessage call (message 0x2ba06b99) instead of posting to the message server.
//
// 58 original symbols at 0x1006acb3c..0x1006b328c plus cSC4MySimDispatch::cLocation at 0x1006ac5c6..0x1006acaac. The
// complete-object dtor is 0x1006ace62 (base 0x1006ad0e2, deleting 0x1006ad0ec); the 13 non-virtual thunks
// 0x1006ad842/0x1006ad850/0x1006ad868/0x1006ad876/0x1006ad88e/0x1006ad89c/0x1006adce0/0x1006ae2c8/0x1006afdc4/
// 0x1006b328c are compiler-generated. Class id 0xcbc14674, interface 0xeb8b6f9a (cISC4MySimDispatch, the primary vtable
// 0x100ae0da0 with slots 0x38 Write / 0x40 Read / 0x48 GetGZCLSID / 0x50 DoMessage / 0x58 CanUDriveIt /
// 0x60 EnterUDriveIt / 0x68 ExitUDriveIt), 0x452294aa (cIGZMessageTarget2 at +0x10) and 0xe4fda3d4 (cIGZSerializable at
// +0x18); one refcount is shared by the three views.
//
// Original layout (0x210 bytes):
//   +0x00 cISC4MySimDispatch vptr   +0x10 cIGZMessageTarget2 vptr   +0x18 cIGZSerializable vptr
//   (the refcount lives in the cRZUnknown base; the port keeps one count and returns the two interface views by
//   static_cast, like cSC4DispatchManager does)
//   +0x20 bool "follow the sim with the camera" (SetFollowCamera @ 0x1006b27xx, cleared by the view messages)
//   +0x24 destination approach distance (m)      +0x28 destination proximity minimum (cells)
//   +0x2c min time idling                        +0x30 min time at a location      +0x34 min time at a transit switch
//   +0x38 transit-stop occupant group chosen by the travel type (table 0x100a28730)
//   +0x3c end index of the current path leg      +0x40 its start index (+0x3c = one past the leg)
//   +0x44 change counter                         +0x48 counter at the last tracking update
//   +0x4c dispatch-view track handle (-1 = none) +0x50 state (eState)   +0x54 previous state
//   +0x58 flag word (bits below)                 +0x5c sim time in minutes
//   +0x60 travel type (cISC4TrafficSimulator::TravelType, 0xfe walk / 0xff none)
//   +0x64 time the sim reached the transit switch +0x68 neighbour-connection attempt counter
//   +0x70 cLocation current location             +0xa0 cLocation default destination
//   +0xd0 cLocation current destination          +0x100 cLocation commute destination
//   +0x130 counted cISC4Occupant* the dispatch view tracks  +0x138 tracking bitmap id
//   +0x140 cRZString tracking label (vptr, std::string at +0x148, refcount at +0x160)
//   +0x168 counted automaton the sim rides (cISC4Automaton)  +0x170 counted cSC4MySim owner
//   +0x178 counted cISC4GameClock (iid 0x681bd53a)           +0x180 counted cISC4PathFinder
//   +0x188 std::deque<cISC4PathFinder::RouteData> route (libc++ internals: map +0x190, element index +0x1a8, size +0x1b0)
//   +0x1b8 std::map<int, std::pair<cGZPersistResourceKey,int>> model base keys per eModelType (root +0x1c0, size +0x1c8)
//   +0x1d0 default model type (eModelType, -1 = none)
//   +0x1d8 std::deque<cLocation> destination queue (map +0x1e0, element index +0x1f8, size +0x200)
//   +0x208 bool "Read ran before Init": the next 0x26d31ec1 message runs Init
//
// cLocation (0x30 bytes, ctor 0x1006ac5fc): the type is shared with cSC4MySim through sc4::MySimLocation (MySim.h), whose
// layout and SetByOccupant / SetCellPosition / SetByCommute / operator== are already recovered there. Write, Read,
// AnyTravelConnectionExists and IsValid (0x1006ac930 / 0x1006ac9e2 / 0x1006ac84e / 0x1006ac7da) live in MySim.cpp for
// the MySim side; this file implements the serialization halves as free functions because cLocation has no vtable.
//
// Flag bits (+0x58), from the code that sets/tests them:
//   0x1   player drive (U-Drive-It) pending/active        0x2   player drive view running
//   0x4   "this destination is a cell, not an occupant"   0x10  at a transit stop/building
//   0x20  commute trip (checked by switchTravel)          0x40  stuck (setStuck)
//   0x80  a default destination is set (SetDefaultDestination / MySim DoMessage)
//   0x100 tracking through the dispatch view             0x200 path leg dispatched, follow the route
//   0x400 switching travel type                          0x800 switching travel type (enter branch)
//   0x1000 travelling (a path was found)                 0x2000 reachability/teleport marker
//   0x4000 a new path is needed                          0x8000 near the destination
//   0x10000 U-Drive-It active                            0x20000 U-Drive-It requested
//   0x2400/0x440 are set by setStuck together with 0x1.
//
// State machine (eState at +0x50): 3 = idle (waiting at the current location), 2 = travelling (path follow),
// -1 = no trip (Reset / Shutdown), 0xff is the "no state" the trip notifications use.
//
// Quirks kept on purpose (each has its address in the .cpp):
//  * notifyMySim calls the owner's DoMessage directly (owner primary vtable 0xc8) instead of posting through
//    SL::spMS2; MySim's DoMessage is what consumes message 0x2ba06b99.
//  * The destructor clears +0x170 before releasing it and later tests +0x170 again, so the second test always sees null
//    (release order: owner, route/model data, path finder, clock, owner-again, automaton, label, tracked occupant and the
//    four locations).
//  * Init only registers the message notifications when no path finder was stored yet, while it re-acquires the clock and
//    resets the destinations on every call when no Read is pending; the state is reset with setState(3) even when it
//    already was 3 (no notification is posted for an unchanged state).
//  * Read adds to the route/map/destination deques instead of clearing them first, does not restore +0x50/+0x54 through
//    setState, and only arms the deferred Init (0x26d31ec1) when the stream ended without error.
//  * resetPath clears the route and hands it back to the path finder; SetModelOffset is a silent no-op for a model type
//    that has no base resource key and discards the PlayAnim result (the MySim seam declares it void).
//  * setStuck sets the stuck flags and posts the "pylon reached" notification instead of a dedicated stuck message; it
//    only sets 0x2401 when the sim is player-driving or already has a real travel type.
//  * GoTo raises a destination's dwell time (+0x24) to minTimeAtLocation only when the destination has an occupant, and
//    it keeps the destination queue untouched when the sim is already travelling.
//  * BasicDispatch re-creates the automaton when its occupant group differs or when it is marked for removal (releasing
//    the old reference once); a route hand-off failure removes the fresh automaton again.
//  * updateTrackingOccupant's early-out compares changeCounter_ (+0x44) with trackedChangeCounter_ (+0x48), which
//    nothing ever writes except Read, so it only short-circuits while no change has happened at all.
//  * DoMessage's automaton-event case 9 calls ExitUDriveIt (which drops +0x168) and then reads the automaton position;
//    the port keeps the position update guarded on the occupant still being there.
//  * The walking automata group is picked from the owner's IsMale() (0x4149 male / 0x414a female) unless a walking model
//    key is set, which selects the transit group 0x4107 instead.
//  * The five-minute tick message adds 5 to the sim time (+0x5c) whenever a path finder exists, even while the sim idles.
//
// Neighbour seams (the original global / vtable slot is in each comment; docs/SEAMS.md has the hook-up table):
//   IMySimDispatchOwner            cSC4MySim primary vtable: DoMessage 0xc8, GetWealth 0x170
//   IMySimDispatchAutomaton        cISC4Automaton (iid 0xa9b40f05): 0x30/0x68/0xa0/0xf0/0xf8/0x138/0x140/0x198/0x200
//                                  and the cISC4TrafficAutomaton route view (iid 0x8b99ee7c)
//   IMySimDispatchRoute            cISC4TrafficAutomaton: 0x4b8 GetRouteInfo / 0x4c0 SetRouteInfo
//   IMySimDispatchAutomataManager  spVehicleManager / spPedestrianManager: 0x78 CreateAutomaton, 0x68/0x110/0x118 drive
//   IMySimDispatchHost             the SL::sp* globals and city / lot / network / occupant-manager queries
#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <utility>

#include "gz/StreamOps.h"
#include "gz/cGZPersistResourceKey.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "rz/cRZString.h"
#include "sc4/MySim.h"             // IMySimDispatch, MySimLocation, the notification message ids
#include "sc4/Occupant.h"
#include "sc4/PathFinder.h"        // cISC4PathFinder, RouteData

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4MySimDispatch = 0xcbc14674;
constexpr std::uint32_t kGZIID_cISC4MySimDispatch = 0xeb8b6f9a;
constexpr std::uint32_t kMySimDispatchIidTrafficAutomaton = 0x8b99ee7c;   // the route view of the automaton
constexpr std::uint32_t kMySimDispatchIidAutomaton = 0xa9b40f05;          // cISC4Automaton
constexpr std::uint32_t kMySimDispatchIidPylon = 0xc9b6d7d6;             // cISC4PylonOccupant (getOccupantToTrack)
constexpr std::uint32_t kGZIID_cISC4GameClock = 0x681bd53a;          // the 24-hour clock kept at +0x178
constexpr std::uint32_t kGZIID_cISC4MySimOwner = 0x8bc01ffe;         // the owner's serializable view (Read at +0x170)
constexpr std::uint32_t kGZIID_cISC4OccupantSaved = 0x26bb767e;      // the occupant / model keys serialized by Write

// Messages the dispatch registers in Init and handles in DoMessage (0x1006ae2d6).
constexpr std::uint32_t kMySimDispatchMsgFiveMinuteTick = 0x44778231;   // +0x5c += 5 (sim time)
constexpr std::uint32_t kMySimDispatchMsgFollow = 0x8bbbd22a;           // data1 -> follow camera (+0x20)
constexpr std::uint32_t kMySimDispatchMsgView1 = 0xa6b79601;            // clears follow when no drive view runs
constexpr std::uint32_t kMySimDispatchMsgView2 = 0xa6b79611;
constexpr std::uint32_t kMySimDispatchMsgSimulate = 0x86ad10ee;         // -> updateState()
constexpr std::uint32_t kMySimDispatchMsgReadPending = 0x26d31ec1;      // Read ran first: Init now
constexpr std::uint32_t kMySimDispatchMsgExitPlayerDrive = 0x0bb324bd;  // data2 == 1 -> ExitUDriveIt
constexpr std::uint32_t kMySimDispatchMsgAutomaton = 0x09d92fc3;        // automaton event: data2 = tAutomataEvent
constexpr std::uint32_t kMySimDispatchMsgNotify = kMessageMySimDispatchNotify;   // 0x2ba06b99 sent to the owner

// The automaton event ids DoMessage tests (cISC4Automaton::tAutomataEvent, Automaton.h): 1 animation done,
// 2 removing, 5 route reached, 9 dispatched-returned.
constexpr std::uint32_t kDispatchEventAnimationDone = 1;
constexpr std::uint32_t kDispatchEventRemoving = 2;
constexpr std::uint32_t kDispatchEventRouteReached = 5;
constexpr std::uint32_t kDispatchEventReturned = 9;

enum eState : std::int32_t {
    kMySimTripNone = -1,        // no trip
    kMySimTripTravelling = 2,   // following the route deque
    kMySimTripIdle = 3,         // waiting at the current location
};

enum eModelType : std::int32_t {
    kModelCar = 0,          // the vehicle model (MySim SetCar)
    kModelPedestrian = 1,   // the pedestrian model
    kModelWalk = 2,         // the walking model
    kModelNone = -1,
};

constexpr std::int32_t kTravelTypeWalk = 0xfe;    // walking / on foot
constexpr std::int32_t kTravelTypeNone = 0xff;    // no travel type chosen yet

// Occupant groups the dispatch looks for at a transit switch (tables 0x100a28730 / 0x100a28750, indexed by travel type-2
// for travel types 2..8; the mask 0x73 skips types 4 and 5 which have no table entry).
constexpr std::uint32_t kTransitGroupTable[7] = {0x4200, 0x4215, 0x4200, 0x4200, 0x4217, 0x422c, 0x422e};
constexpr std::uint32_t kTransitStopTable[7] = {0x1301, 0x1300, 0x1301, 0x1301, 0x1302, 0x1303, 0x1307};
constexpr std::uint32_t kTransitMask = 0x73;                 // travel types 2, 3, 6, 7, 8 have a table entry
constexpr std::uint32_t kAutomataGroupFootMale = 0x4149;      // getAutoGroupAndModelType: walk with IsMale() true
constexpr std::uint32_t kAutomataGroupFootFemale = 0x414a;    // walk with IsMale() false
constexpr std::uint32_t kAutomataGroupTransit = 0x4107;       // "ride a transit vehicle" group
constexpr std::uint32_t kAutomataGroupVehicleDefault = 0x4200;
constexpr std::uint32_t kAutomataCreateFlags = 0x214042;      // flags passed to the automata manager's 0x78

// Flag word helpers (see the header comment for the meaning of each bit).
constexpr std::uint32_t kMySimDispatchFlagPlayerDrive = 0x1;
constexpr std::uint32_t kMySimDispatchFlagDriveView = 0x2;
constexpr std::uint32_t kMySimDispatchFlagCellOnly = 0x4;
constexpr std::uint32_t kMySimDispatchFlagAtBuilding = 0x10;
constexpr std::uint32_t kMySimDispatchFlagCommute = 0x20;
constexpr std::uint32_t kMySimDispatchFlagStuck = 0x40;
constexpr std::uint32_t kMySimDispatchFlagHasDefault = 0x80;
constexpr std::uint32_t kMySimDispatchFlagTracking = 0x100;
constexpr std::uint32_t kMySimDispatchFlagPathLeg = 0x200;
constexpr std::uint32_t kMySimDispatchFlagSwitchTravel = 0x400;
constexpr std::uint32_t kMySimDispatchFlagSwitchEnter = 0x800;
constexpr std::uint32_t kMySimDispatchFlagTravelling = 0x1000;
constexpr std::uint32_t kMySimDispatchFlagTeleported = 0x2000;
constexpr std::uint32_t kMySimDispatchFlagNeedPath = 0x4000;
constexpr std::uint32_t kMySimDispatchFlagNearDestination = 0x8000;
constexpr std::uint32_t kMySimDispatchFlagUDriveIt = 0x10000;
constexpr std::uint32_t kMySimDispatchFlagUDriveItPending = 0x20000;

class IMySimDispatchRoute;

// cSC4DispatchVehicleView (0x100-byte object, src/sc4/DispatchManager.h has the full interface) as the dispatch tracks
// the sim's car/pedestrian: TrackOccupant @ 0x100233ae2, UntrackOccupant @ 0x1002342e0.
class IMySimDispatchVehicleView : public cIGZUnknown {
public:
    virtual int TrackOccupant(cISC4Occupant* occupant, int trackerType, std::uint32_t callNumber,
                              const cRZString* label) = 0;
    virtual void UntrackOccupant(int trackId, bool force) = 0;
};

// ------------------------------------------------------------------------------------------------------------------------
// Neighbour seams. One method per original virtual call; the original slot / address is in the comment.
// ------------------------------------------------------------------------------------------------------------------------

// IMySimDispatchOwner (the dispatch's view of cSC4MySim) is declared in MySim.h; cSC4MySim answers it for iid 0x8bc01ffe.

// cISC4TrafficAutomaton's route view (iid 0x8b99ee7c, the same object as cSC4TrafficAutomaton in src/sc4/TrafficAutomaton).
class IMySimDispatchRoute : public cIGZUnknown {
public:
    virtual bool GetRouteInfo(std::deque<RouteData>& out) const = 0;                    // 0x4b8 @ 0x1000e25e0
    virtual bool SetRouteInfo(const std::deque<RouteData>& route, bool begin, bool onlyStart) = 0;   // 0x4c0 @ 0x100119a46
};

// cISC4Automaton (iid 0xa9b40f05) as the dispatch uses it. Only the slots it calls are declared, in vtable order.
class IMySimDispatchAutomaton : public cIGZUnknown {
public:
    virtual cISC4Occupant* AsOccupant() = 0;                                   // 0x30
    virtual void Remove(bool fade) = 0;                                        // 0x68
    virtual bool PlayAnim(std::uint32_t animId) = 0;                           // 0xa0
    virtual void SetAutomataFlag(std::uint32_t flag, bool on) = 0;             // 0xf0
    virtual bool CheckAutomataFlag(std::uint32_t flag) = 0;                    // 0xf8
    virtual void AddCallback(cIGZMessageTarget2* target) = 0;                   // 0x138
    virtual void RemoveCallback(cIGZMessageTarget2* target) = 0;                // 0x140
    virtual int GetStateId() const = 0;                                         // 0x1c0 -> +8 (tAutomataState::state)
    // 0x198 SetState(state, controller, timer, flag, animOffset, sfx, behavior); the dispatch passes the automaton as
    // its own controller and timer = -1.0f.
    virtual bool SetState(std::uint32_t state, IMySimDispatchAutomaton* controller, float timer, bool flag,
                          std::uint32_t animOffset, std::uint32_t sfx, const void* behavior) = 0;
    virtual void SetPriority(std::uint8_t priority) = 0;                       // 0x200
    virtual IMySimDispatchRoute* RouteInterface() = 0;                         // QueryInterface(0x8b99ee7c)
};

// spVehicleManager / spPedestrianManager (the automata managers) as the dispatch uses them.
class IMySimDispatchAutomataManager {
public:
    virtual ~IMySimDispatchAutomataManager() = default;
    // 0x78: CreateAutomaton(kind 0, position (x,z), flags 0x214042, resource key, message target, priority 0xff, 0).
    virtual IMySimDispatchAutomaton* CreateAutomaton(int kind, const float position[2], std::uint32_t flags,
                                                    const cGZPersistResourceKey& key, cIGZMessageTarget2* target,
                                                    std::uint8_t priority, int argument) = 0;
    virtual bool PlayerDriveActive() = 0;                              // vehicle manager 0x118
    virtual bool DriveViewActive() = 0;                                // vehicle manager 0x68
    virtual void SetPlayerDriveAutomaton(IMySimDispatchAutomaton* automaton) = 0;   // vehicle manager 0x110
};

// What the dispatch needs from the running game. One method per original call chain (the global / slot chain is in the
// comment; the concrete class behind each is listed in docs/SEAMS.md).
class IMySimDispatchHost {
public:
    virtual ~IMySimDispatchHost() = default;
    // --- message / input plumbing ---------------------------------------------------------------------------
    virtual cIGZMessageServer2* MessageServer() = 0;                          // SL::spMS2 (borrowed)
    virtual void RegisterInputControlTarget(cIGZMessageTarget2* target) = 0;   // SL::ATM() 0x28(target, 0xff)
    virtual void UnregisterInputControlTarget(cIGZMessageTarget2* target) = 0; // SL::ATM() 0x30(target, 0xff)
    // --- services -------------------------------------------------------------------------------------------
    virtual cISC4PathFinder* PathFinder() = 0;            // SL::spTrafficSim 0x30 (borrowed; Init AddRefs it)
    virtual int FindPath(cISC4PathFinder* pathFinder) = 0;
        // path finder vslot 0xb8 (FindPath; not recovered yet — see the PATHFINDER-* to-do rows). 1 = path found,
        // 2 = still searching / search in progress, anything else = no path.
    virtual cIGZUnknown* Clock() = 0;                     // SL::sp24HourClock (borrowed; Init AddRefs it)
    virtual IMySimDispatchVehicleView* DispatchVehicleView() = 0;   // SL::spDispatchManager 0xb0 (borrowed)
    virtual IMySimDispatchAutomataManager* AutomataManager(std::uint32_t automataGroup) = 0;
        // pickAutomataManager: group in {0x4200,0x420b,0x4215,0x4217,0x422c,0x422e} -> spVehicleManager,
        // {0x4149,0x414a,0x4107} -> spPedestrianManager (mask 0x500000a00801 at 0x100a287xx)
    // --- city / lot / network ------------------------------------------------------------------------------
    virtual int CityCellsX() = 0;                         // SL::spCity 0x2e8
    virtual int CityCellsZ() = 0;                         // SL::spCity 0x2f0
    virtual bool AnyTravelConnection(int cellX, int cellZ) = 0;
        // AnyTravelConnectionExists 0x1006ac84e: spSC4 0x118 -> occupant manager 0x58 -> spNeighborsSim 0x90 over
        // kValidNeighborConnectionTypeArray (13 kinds) until a connection is returned
    virtual bool NeighborConnection(int cellX, int cellZ, int& outCellX, int& outCellZ) = 0;
        // updateNeighborLocation 0x1006b06ec: spSC4 0x118 -> 0x58 -> spNeighborsSim 0x60 -> connection 0x28(&x,&z)
    virtual cISC4Occupant* BuildingAtCell(int cellX, int cellZ) = 0;
        // validateCommuteLocation: spCity 0x318(cell) + spLotManager 0x28(cellX,cellZ,0) -> lot 0x180() -> 0x18()
    virtual bool LotCellRect(int cellX, int cellZ, int outRect[4]) = 0;   // getLocationCellRect -> spLotManager 0x28/0x70 -> lot 0x70
    virtual bool LotCellRectForOccupant(cISC4Occupant* occupant, int outRect[4]) = 0;   // getLocationCellRect -> spLotManager 0x70
    virtual bool TileHasNetwork(int cellX, int cellZ, std::uint32_t mask) = 0;   // spTrafficNetworkMap 0x50(x,z,mask,0)
    virtual bool NetworkTilesInRect(const int rect[4], std::deque<std::pair<int, int>>& tiles) = 0;
        // spTrafficNetworkMap 0xd0(rect, 1, 1, 0x849) plus every returned tile's 0x1d8(&x,&z) cell query
    virtual void OccupantsInCellRect(const int rect[4], std::deque<cISC4Occupant*>& out) = 0;
        // spOccupantManager 0x148(rect, &list) — the transit-switch search in switchTravel
    virtual void ClearOccupantList(std::deque<cISC4Occupant*>& list) = 0;   // spOccupantManager 0x180(&list)
    // --- the automaton the sim rides, by its occupant (the original QIs the occupant at each call site) -----------
    virtual IMySimDispatchAutomaton* AutomatonOf(cISC4Occupant* occupant) = 0;   // QueryInterface(0xa9b40f05), one reference
    virtual IMySimDispatchRoute* RouteOf(cISC4Occupant* occupant) = 0;           // QueryInterface(0x8b99ee7c), one reference
    virtual cISC4Occupant* CallbackOccupant(void* automaton) = 0;
        // the automaton an automaton event message (0x09d92fc3) names in its first pointer, as its occupant (borrowed)
    // --- U-Drive-It view -------------------------------------------------------------------------------------
    virtual bool UDriveViewAvailable() = 0;                                   // SL::spCity 0x188 != null
    virtual void EnterUDriveView(IMySimDispatchAutomaton* automaton, cIGZMessageTarget2* target) = 0;
        // cSC4AutomataManager::EnterPlayerDriveView 0x100ff652 (no view) or the view's 0xc0/0xb0/0x200 path
    virtual void ShowUDriveAdvice() = 0;                                      // SL::spAdvisorSystem 0x38(0x1d) -> 0x108
    virtual void ClearVehiclePlayerDrive() = 0;                               // SL::spVehicleManager 0x110(null)
    // --- serialization helpers -------------------------------------------------------------------------------
    virtual bool WriteSerializable(cIGZOStream& stream, const cIGZUnknown* object) = 0;   // SC4WriteGZSerializable
    virtual bool ReadSerializable(cIGZIStream& stream, std::uint32_t iid, void** out) = 0;   // SC4ReadGZSerializable
};
IMySimDispatchHost* MySimDispatchHost();
void SetMySimDispatchHost(IMySimDispatchHost* host);

// cLocation serialization / validity, the halves that live in this unit (0x1006ac930 / 0x1006ac9e2 / 0x1006ac84e /
// 0x1006ac7da). The type itself is sc4::MySimLocation (MySim.h).
bool MySimLocationWrite(const MySimLocation& location, cIGZOStream& stream);   // 0x1006ac930
bool MySimLocationRead(MySimLocation& location, cIGZIStream& stream);          // 0x1006ac9e2
bool MySimLocationAnyTravelConnection(const MySimLocation& location, int cellX, int cellZ);   // 0x1006ac84e
bool MySimLocationIsValid(const MySimLocation& location);                      // 0x1006ac7da

// cSC4MySimDispatch (interface 0xeb8b6f9a). Implements MySim's IMySimDispatch seam, so cSC4MySim can hold one directly.
class cSC4MySimDispatch final : public IMySimDispatch, public cIGZMessageTarget2, public cIGZSerializable {
public:
    using cLocation = MySimLocation;

    explicit cSC4MySimDispatch(IMySimDispatchOwner* owner = nullptr);   // @ 0x1006acb52 (alias 0x1006acb3c)
    ~cSC4MySimDispatch();                                              // @ 0x1006ace62

    // cIGZUnknown, one refcount for the three views (0x1006ad7f0 / 0x1006ad85e / 0x1006ad884)
    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;

    // cIGZMessageTarget2 / cIGZSerializable
    bool DoMessage(cIGZMessage2* message) override;      // @ 0x1006ae2d6 (thunk 0x1006afdc4)
    bool Write(cIGZOStream& stream) const override;      // @ 0x1006ad8aa (thunk 0x1006adce0)
    bool Read(cIGZIStream& stream) override;             // @ 0x1006adcee (thunk 0x1006ae2c8)
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4MySimDispatch; }   // @ 0x1006b3280

    // IMySimDispatch (the MySim seam; each maps to the cSC4MySimDispatch:: address in the comment)
    bool Init() override;                                                     // @ 0x1006ad11a
    bool Shutdown() override;                                                 // @ 0x1006ad48a
    void SetDefaultDestination(const MySimLocation& location) override;       // @ 0x1006b00de
    bool SetCurrentCommuteDestination(const MySimLocation& location, bool notify) override;   // @ 0x1006b26fe
    bool AddDestinationLocation(const MySimLocation& location, int flags) override;           // @ 0x1006afdd8
    bool GoTo(const MySimLocation& location) override;                        // @ 0x1006afb8c
    void TeleportToCommuteLocation() override;                                // @ 0x1006b2860
    const MySimLocation& GetCurrentLocation() const override { return currentLocation_; }             // +0x70
    const MySimLocation& GetCurrentDestination() const override { return currentDestination_; }       // +0xd0
    const MySimLocation& GetCurrentDefaultDestination() const override { return defaultDestination_; }   // +0xa0
    const MySimLocation& GetCurrentCommuteDestination() const override { return commuteDestination_; }   // +0x100
    bool CheckFlags(int flags) const override { return (flags_ & static_cast<std::uint32_t>(flags)) != 0; }   // @ 0x1006afa4a
    int GetState() const override { return state_; }                          // @ 0x1006b0244
    int GetTravelType() const override { return travelType_; }                // @ 0x1006b024e
    int GetTransitSwitchWaitTimeMunutes() override;                           // @ 0x1006b2dae
    void SetModelOffset(int modelType, int offset, bool snap) override;        // @ 0x1006b000a
    void SetModelBaseResKey(int modelType, const cGZPersistResourceKey& key, int offset) override;   // @ 0x1006aff0e
    void StartTracking(int bitmapId, const cRZString& text) override;          // @ 0x1006b0266
    void StopTracking() override;                                             // @ 0x1006b01d2
    int TrackHandle() const override { return trackHandle_; }                  // +0x4c
    void SetFollowCamera(bool follow) override { follow_ = follow; }           // +0x20
    bool CanUDriveIt() override;                                              // @ 0x1006b2dd6
    bool EnterUDriveIt() override;                                            // @ 0x1006b2e9c
    bool ExitUDriveIt() override;                                             // @ 0x1006b30a6
    void SetDestinationApproachDistance(int value) override { destinationApproachDistance_ = value; }   // +0x24
    void SetDestinationProximityMin(int value) override { destinationProximityMin_ = value; }           // +0x28
    void SetMinTimeIdling(int value) override { minTimeIdling_ = value; }                                // +0x2c
    void SetMinTimeAtLocation(int value) override { minTimeAtLocation_ = value; }                        // +0x30
    void SetMinTimeAtSwitch(int value) override { minTimeAtSwitch_ = value; }                            // +0x34

    // The original's own entry points that are not part of the MySim seam.
    bool Reset();                                                            // @ 0x1006ad584
    std::uint32_t updateState();                                             // @ 0x1006aeb5a
    void switchTravel();                                                     // @ 0x1006b0b16

    // State access for the tests / the game.
    std::uint32_t Flags() const { return flags_; }
    bool FollowCamera() const { return follow_; }
    std::size_t DestinationCount() const { return destinationQueue_.size(); }
    std::size_t RouteSize() const { return route_.size(); }
    std::size_t ModelKeyCount() const { return modelKeys_.size(); }
    bool ReadPending() const { return initPending_; }
    int ChangeCounter() const { return changeCounter_; }
    int Time() const { return time_; }                    // +0x5c (sim minutes)
    int DefaultModelType() const { return defaultModelType_; }   // +0x1d0
    cISC4Occupant* AutomataOccupant() const { return automataOccupant_.get(); }   // +0x168 (the automaton's occupant view)
    IMySimDispatchOwner* Owner() const { return owner_; }

private:
    // --- helpers (the original's private functions) ---------------------------------------------------------
    void notifyMySim(std::uint32_t event, int data2, int data3, int data4);   // @ 0x1006aea60
    bool setState(int state);                                                 // @ 0x1006ad3c8
    void resetPath();                                                         // @ 0x1006ad41c
    bool setFlags(std::uint32_t flags, bool on);                              // @ 0x1006afa56
    void setNextPathLeg();                                                    // @ 0x1006afa98
    cISC4Occupant* getOccupantToTrack();                                      // @ 0x1006b03ec
    bool updateTrackingOccupant();                                            // @ 0x1006b0486
    bool isBuilding(cLocation& location) const;                               // @ 0x1006b052c
    IMySimDispatchAutomataManager* pickAutomataManager(int automataGroup);    // @ 0x1006b059c
    bool setAutomataOccupant(cISC4Occupant* occupant);                        // @ 0x1006b05e8
    bool updateNeighborLocation(int attempt, cLocation& location);            // @ 0x1006b06ec
    bool validateCommuteLocation(cLocation& location);                        // @ 0x1006b074c
    void getLocationCellRect(const cLocation& location, int outRect[4]);      // @ 0x1006b0aac
    bool snapToNearestRoadTile(cLocation& location);                          // @ 0x1006b1d76
    std::uint8_t basicDispatch(std::deque<RouteData>& route, IMySimDispatchAutomaton* automaton, bool noRoute);   // @ 0x1006b1fa2
    int distance_between_locations(const cLocation& a, const cLocation& b);   // @ 0x1006b2442
    std::uint8_t basicDispatchOnNewPathLeg(IMySimDispatchAutomaton* automaton, bool flag);   // @ 0x1006b256a
    bool jumpToNextSwitch();                                                  // @ 0x1006b25f4
    void setStuck();                                                          // @ 0x1006b2668
    bool getNextSwitchPathSegment(std::deque<RouteData>& out, bool append);   // @ 0x1006b29a6
    bool snapCurrentPathSegmentToNearestNetworkTile();                        // @ 0x1006b2a4a
    void getAutoGroupAndModelType(int travelType, int* automataGroup, int* modelType);   // @ 0x1006b2aea
    bool createAutomaton(int travelType, IMySimDispatchAutomaton** automaton, int* modelType);   // @ 0x1006b2c08
    void getModelKey(int modelType, cGZPersistResourceKey& key, int& offset) const;
    // QI the occupant view at +0x168 for the automaton interface (returns one reference, or null).
    IMySimDispatchAutomaton* acquireAutomaton() const;

    int cellFromPosition(float value) const;   // the float add-0.5 / truncate trick in the original

    // --- data (original byte offset in the comment) ---------------------------------------------------------
    std::uint32_t refCount_ = 0;                              // cRZUnknown base
    bool follow_ = false;                                     // +0x20
    int destinationApproachDistance_ = 10;                    // +0x24
    int destinationProximityMin_ = 10;                        // +0x28
    int minTimeIdling_ = 5;                                   // +0x2c
    int minTimeAtLocation_ = 0;                               // +0x30
    int minTimeAtSwitch_ = 5;                                 // +0x34
    std::uint32_t transitGroup_ = 0;                          // +0x38
    int pathSegmentEnd_ = 0;                                  // +0x3c
    int pathSegmentStart_ = 0;                                // +0x40
    int changeCounter_ = 0;                                   // +0x44
    int trackedChangeCounter_ = 0;                            // +0x48
    int trackHandle_ = -1;                                    // +0x4c
    int state_ = kMySimTripNone;                                  // +0x50
    int previousState_ = kMySimTripNone;                          // +0x54
    std::uint32_t flags_ = 0;                                 // +0x58
    int time_ = 0;                                            // +0x5c
    int travelType_ = kTravelTypeNone;                        // +0x60
    int transitSwitchStartTime_ = 0;                          // +0x64
    int neighborAttempts_ = 0;                                // +0x68
    cLocation currentLocation_;                               // +0x70
    cLocation defaultDestination_;                            // +0xa0
    cLocation currentDestination_;                            // +0xd0
    cLocation commuteDestination_;                            // +0x100
    cRZAutoRefCount<cISC4Occupant> trackedOccupant_;          // +0x130
    std::uint32_t trackingBitmapId_ = 0;                      // +0x138
    cRZString trackingText_;                                  // +0x140
    cRZAutoRefCount<cISC4Occupant> automataOccupant_;         // +0x168 (the automaton's occupant view)
    IMySimDispatchOwner* owner_ = nullptr;                    // +0x170 (counted)
    cIGZUnknown* clock_ = nullptr;                            // +0x178 (counted)
    cISC4PathFinder* pathFinder_ = nullptr;                   // +0x180 (counted)
    std::deque<RouteData> route_;                             // +0x188
    std::map<int, std::pair<cGZPersistResourceKey, int>> modelKeys_;   // +0x1b8
    int defaultModelType_ = kModelNone;                       // +0x1d0
    std::deque<cLocation> destinationQueue_;                  // +0x1d8
    bool initPending_ = false;                                // +0x208
};

}  // namespace sc4
