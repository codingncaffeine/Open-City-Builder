// cSC4AutomataManager — owns every automaton of the city (85 symbols, 0x1000e2446..0x1004fde70): it keeps the automata in a
// list, tracks them per automata group, chooses the exemplar (model/tuning) an automaton is built from, creates and destroys
// automata, drives them each tick, follows one with the camera, and translates the dispatch manager's locations into cells and
// world positions. The four per-kind managers (vehicle / aircraft / pedestrian / watercraft) stay the city's own objects; this
// class finds the right one for a group (GetAutomataManagerForGroup, city vslots 0x158/0x160/0x168/0x170) and hands
// creation to it.
//
// Class id 0x498f9cb5; interfaces 0x98f9bf4 (cISC4AutomataManager, primary vtable 0x100a73840), 1 (cIGZMessageTarget legacy,
// vtable 0x100a73a90) and 0x452294aa (cIGZMessageTarget2, vtable 0x100a73b00); cIGZSerializable 0xe4fda3d4 (vtable 0x100a73ac0)
// shares the one refcount. The primary vtable is 74 slots, 0x100104544..0x1004fde70 (GetAutomataCount sits alone at
// 0x1004fde70, the last slot of the class).
//
// Original layout (0xb08 bytes):
//   +0x00/08/10/18 the four vtables  +0x20 "post city init pending" (set by Read, cleared by HandlePostCityInit)
//   +0x24 view rectangle minX/minZ/maxX/maxZ (4 floats), +0x34 the previous one (4 floats), expanded by 64 in HandleViewChanged
//   +0x44 initialised  +0x48 refcount  +0x4c max automaton count  +0x50 that count x1.25
//   +0x58 std::list<cISC4Automaton*> (sentinel, +0x68 size), +0x70 automaton count
//   +0x74 active  +0x75 paused
//   +0x78 cRZRandomSuper (0x9d0 bytes, seeded 0xffffffff)
//   +0xa48 std::vector<tExemplarData> (0x88 bytes each), +0xa60 std::map<u32, std::vector<u32>> group -> exemplar indices
//   +0xa78 last view position used for the scroll-granularity test (+0xa78 x, +0xa7c y, +0xa80 z)
//   +0xa84 min zoom (byte)  +0xa85 "loaded" flag (cheat/message 0x4a7fb807)
//   +0xa88 exemplar group id the scan keeps (property 0x10 of every automata exemplar)
//   +0xa8c default automata group, +0xa90 fallback group  +0xa94 "view scroll in progress"
//   +0xa98 std::unordered_map<u32, std::unordered_set<cISC4Automaton*>> tracked group instances
//   +0xac0 std::unordered_map<u32, u32> tracked building counts (source building id -> count)
//   +0xae8 "recount buildings from the save" flag (versions 5..7 of Read)
//   +0xaf0 city network manager (city vslot 0x140)  +0xaf8 cSC4AutomataScriptSystem (0x180)  +0xb00 24 hour clock (0x178)
//
// Messages (registered in Init with cIGZMessageServer2, always removed in Shutdown): 0xa6b79602 / 0xa6b79612 camera moved,
// 0x06cdb65a view filter changed, 0x26d31ec1 post city init, 0x26d31ec2 city shutdown, 0x99ef1142 / 0x99ef1143 occupant
// inserted / removed, 0x2977ae2f building completed, 0x4a7fb807 automata enabled flag, 0xea298d50 controller removed,
// 0xc9f775bb render properties changed, 0xcad61e00 / 0xead61e16 scroll start / end, 0xebff18a3 occupant group added and
// 0x86ad10ee (the ATM tick, channel 0xff; 0x89d3e9f3 is registered too). The cheat listener (vtable +8) answers
// 0x230e27ac for the five cheats StaticInit registers: DrawPaths 0xe97fcbc8, HidePaths 0xe97fcbc9, udriveit 0xb70c96d,
// NoCSI 0x6bb354c7 and Casper 0xcc478b79.
//
// Quirks kept (each also asserted by the test):
//  * Dispatch(group, ...) and Dispatch(automaton, ...) are empty stubs that return 0 in the shipping binary; the real
//    dispatch lives in the per-kind managers (0x100100d86 / 0x100100d8e).
//  * GetAutomataManagerForGroup formats an error text ("Couldn't find automata manager for group 0x%4x! ...") into a
//    temporary cRZString and drops it; it returns the last manager it probed (null when the city has none).
//  * AdjustCountForAutomaton only ever removes one automaton, and only when the *count is at its limit* and the automaton
//    with the lowest priority is below the caller's priority byte.
//  * SetMaxCount removes automata without checking the priority and stops at the first automaton that has flag 0x200000.
//  * SetMaxCount clamps to [0, 5000] and never goes below 0 (a negative count means 0, not "unlimited").
//  * MaxCount 0 therefore removes every automaton that is not flagged 0x200000.
//  * RemoveAutomaton(force = false) "retires" the automaton instead of removing it: it sends state 2, sets flag 0x4000000,
//    jumps the model to opacity 0 and stops its effects - the automaton stays in the list.
//  * DestroyAutomata calls RemoveAutomatonInternal on every automaton before releasing it (so the automaton is expected to
//    shut itself down twice: Shutdown is called via vslot 0x20 and RemoveAutomatonInternal before it).
//  * DoAnimationTick keeps ticking an automaton with opacity 0.1 or more even when it is marked for removal; below that it
//    removes it with force = true.
//  * GetCollidingAutomaton returns the *last* candidate that beat the running distance (the comparison is <=) and starts the
//    running distance at 0, so an automaton already overlapping never wins.
//  * ReadInternal only accepts stream versions below 7, keeps the automaton count it reads (it can grow past the count
//    when an entry fails to read) and for versions below 5 sets the "recount buildings" flag instead of reading the map.
//  * Write always writes version 6 (the version is a *stream* version, not the class version).
//  * TranslateDispatchLocation(type 0) with a null occupant returns false; type 0 only uses the occupant's cell rectangle
//    for building occupants (type 0x278128a0), anything else goes through GetPosition -> world-to-cell.
//  * GetRandomCreationPoint always resets +4 of the output direction to the terrain height unless flag 0x40000 is passed.
//  * The four camera-follow globals are class statics, not members: only one automaton can be followed in a process.
//
// Seams: everything the manager reached through SL::spCity / SL::spView3D / SL::spViewUtilities / SL::spRenderer /
// SL::spTerrain / SL::spSimulator / SL::spVehicleManager / SL::spOccupantManager / SL::spRM / SL::spCCM / SL::ATM() and the
// cSC4Automaton it created goes through IAutomataManagerHost below (the original slot/address is in each comment). The
// real classes are hooked up in docs/SEAMS.md; the test supplies small fakes.
#pragma once
#include <cstdint>
#include <list>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gz/StreamOps.h"   // cS3DVector3
#include "gz/cGZMessageServer.h"
#include "gz/cGZPersistResourceKey.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZSerializable.h"
#include "rz/cRZRandomSuper.h"
#include "rz/cRZString.h"
#include "rz/cRZVariant.h"
#include "sc4/AutomataTuningParams.h"
#include "sc4/Exemplar.h"
#include "sc4/SCProperty.h"
#include "sc4/cSC4ResponseCurve.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4AutomataManager = 0x498f9cb5;
constexpr std::uint32_t kGZIID_cISC4AutomataManager = 0x98f9bf4;
constexpr std::uint32_t kGZIID_cISC4Automaton = 0xa9b40f05;
#ifndef SC4_GZIID_CISC4BuildingOccupant_DEFINED
#define SC4_GZIID_CISC4BuildingOccupant_DEFINED
constexpr std::uint32_t kGZIID_cISC4BuildingOccupant = 0x87dda3a9;
#endif
constexpr std::uint32_t kGZIID_cISC4NetworkOccupant = 0xa821ef94;
constexpr std::uint32_t kGZIID_cIGZSerializableView = 0xe4fda3d4;
constexpr std::uint32_t kGZIID_cIGZMessageTarget2View = 0x452294aa;

// Cheat codes StaticInit registers (cGZCheatCodeManager, "DebugFunctionalityEnabled" gated).
constexpr std::uint32_t kCheatDrawAutomataPaths = 0xe97fcbc8;
constexpr std::uint32_t kCheatHideAutomataPaths = 0xe97fcbc9;
constexpr std::uint32_t kCheatPlayerDrive = 0x0b70c96d;
constexpr std::uint32_t kCheatNoCsi = 0x6bb354c7;
constexpr std::uint32_t kCheatGhostModels = 0xcc478b79;
#ifndef SC4_MESSAGE_CHEAT_CODE_DEFINED   // defined by both automata halves (AutomataControllerManager.h, AutomataManager.h)
#define SC4_MESSAGE_CHEAT_CODE_DEFINED
constexpr std::uint32_t kMessageCheatCode = 0x230e27ac;
#endif

// Messages (the first 14 are registered by Init).
constexpr std::uint32_t kMsgCameraMoved = 0xa6b79602, kMsgCameraTargetDone = 0xa6b79612, kMsgViewFilterChanged = 0x06cdb65a,
                        kMsgPostCityInit = 0x26d31ec1, kMsgCityShutdown = 0x26d31ec2, kMsgOccupantInserted = 0x99ef1142,
                        kMsgOccupantRemoved = 0x99ef1143, kMsgBuildingCompleted = 0x2977ae2f, kMsgAutomataEnabled = 0x4a7fb807,
                        kMsgControllerRemoved = 0xea298d50, kMsgRenderPropertiesChanged = 0xc9f775bb, kMsgScrollStart = 0xcad61e00,
                        kMsgScrollEnd = 0xead61e16, kMsgOccupantGroupAdded = 0xebff18a3, kMsgTick = 0x86ad10ee,
                        kMsgQueuePointsInvalid = 0x89d3e9f3;
constexpr int kAutomataTickChannel = 0xff;

// Occupant property that excludes an occupant from the automata system (HandleInsert/RemoveOccupant).
constexpr std::uint32_t kPropertyNoAutomata = 0x029244db;
// Automaton property: the group ids it belongs to (cRZVariant of u32s).
#ifndef SC4_kPropertyOccupantGroups_DEFINED
#define SC4_kPropertyOccupantGroups_DEFINED
constexpr std::uint32_t kPropertyOccupantGroups = 0xaa1dd396;
#endif
#ifndef SC4_BUILDING_OCCUPANT_TYPE_DEFINED   // defined by both automata halves (AutomataGenerator.h, AutomataManager.h)
#define SC4_BUILDING_OCCUPANT_TYPE_DEFINED
constexpr std::uint32_t kBuildingOccupantType = 0x278128a0;
#endif
constexpr std::uint32_t kNetworkOccupantType = 0xc772bf98;

// The tuning resource: an exemplar with the automata tuning properties (StaticInit -> ReadTuningParameters).
constexpr std::uint32_t kAutomataTuningResourceType = 0x6534284a;   // exemplar
constexpr std::uint32_t kAutomataTuningResourceGroup = 0xa998d30b;
constexpr std::uint32_t kAutomataTuningResourceInstance = 1;

// Camera follow: camera movement (squared) below this per zoom level keeps the stored view position; and the fraction of the
// viewport used as the "nose" dead zone per virtual zoom (0x100a2a2c0 is a different table).
extern const float kAutomataScrollGranularitySquared[8];   // @ DoMessage(cIGZMessage2*)::kScrollGranularitySquared
extern const float kAutomataCameraNoseFraction[5];         // @ UpdateCameraFollow::kNoseFraction

// cISC4AutomataManager::PopulateLayer: the layer index used to key the clock multipliers and the exemplar checks. The names
// are not in the binary; the numbers are the ones ReadTuningParameters stores (properties 0x4998c90c..0x4998c911 and
// 0x6a09f631, 0xec06e7cf) and what GetClockMultiplier maps 6, 9 and 10 onto (4).
enum class PopulateLayer : std::uint32_t {
    kLayerNone = 0,
    kLayer1 = 1,
    kLayer2 = 2,
    kLayer3 = 3,
    kLayer4 = 4,
    kLayer5 = 5,
    kLayer6 = 6,
    kLayer7 = 7,
    kLayer8 = 8,
    kLayer9 = 9,
    kLayer10 = 10,
    kLayer11 = 11,
};

class IAutomataOccupant;
class IAutomataGroupTemplate;

// cISC4AutomataManager::tDispatchLocation (kind + 0x10 bytes of payload): +0x00 kind, +0x04 unused, +0x08 the occupant
// pointer (kind 0) or x (kinds 1..3), +0x0c y, +0x10 z, +0x14 the second cell's z (kind 3). The original aliases the
// occupant pointer and x/y over the same eight bytes; the port keeps them apart (same behaviour, documented offsets).
enum DispatchLocationKind : std::uint32_t {
    kLocationOccupant = 0,
    kLocationWorldPosition = 1,
    kLocationCell = 2,
    kLocationCellRect = 3,
};
struct tAutomataLocation {
    std::uint32_t kind = 0;            // +0x00
    std::uint32_t flags = 0;           // +0x04
    IAutomataOccupant* occupant = nullptr;   // +0x08 when kind == kLocationOccupant
    float x = 0.0f;                    // +0x08 (world x / cell x)
    float y = 0.0f;                    // +0x0c (world y / cell z)
    float z = 0.0f;                    // +0x10 (world z / second cell x)
    float w = 0.0f;                    // +0x14 (second cell z)
};

// SC4Rect<int> (0x10 bytes): the cell rectangle TranslateDispatchLocation fills.
struct tCellRect {
    std::int32_t x0 = 0, z0 = 0, x1 = 0, z1 = 0;   // +0x00, +0x04, +0x08, +0x0c
};

// cISC4AutomataManager::tExemplarData (0x88 bytes): one automata exemplar, prepared by InitExemplarData.
struct tExemplarData {
    std::uint32_t networkMask0 = 0;              // +0x00 property 0x696e88db (byte list -> bit (v-1)); 0xffffffff = none
    std::uint32_t networkMask1 = 0;              // +0x04 property 0x296e88e9
    std::uint32_t networkMask2 = 0;              // +0x08 property 0x496e88f1
    std::uint32_t layerMask = 0;                 // +0x0c property 0xc973c9aa (bit per PopulateLayer, zero bytes ignored)
    cGZPersistResourceKey modelKey;              // +0x10 property 0x9c80416 (exactly three ints)
    cRZString name;                              // +0x20 property 0x20 (the exemplar name)
    cISCResExemplar* exemplar = nullptr;         // +0x48 (counted; set by EnumAutomatonExemplar)
    bool flag50 = false;                         // +0x50 property 0xaa5a9633 (sets automata flag 0x100000 on creation)
    std::uint32_t crimeType = 0;                 // +0x54 property 0x2a62d9f3
    std::uint32_t groupIds[10] = {};             // +0x58 property 0xaa1dd396 plus the parent chain, at most ten
    std::uint8_t groupCount = 0;                 // +0x80
    std::uint8_t pad81_[3] = {};
    std::uint32_t defaultGroup = 0;              // +0x84 the top-level group of groupIds (or property 0x2c23a3a1)
};
static_assert(sizeof(tExemplarData) == 0x88, "tExemplarData layout");

// -------------------------------------------------------------------------------------------------------------------------
// The neighbour seams (only the slots this class calls are declared, in original order, with the byte offset from the vptr).
// -------------------------------------------------------------------------------------------------------------------------

// cISC4Occupant as the manager uses it (vtable of cSC4Occupant 0x100a73f90).
class IAutomataOccupant : public cIGZUnknown {
public:
    virtual bool Init() = 0;                                                    // 0x18
    virtual bool Shutdown() = 0;                                                // 0x20
    virtual bool IsInitialized() = 0;                                           // 0x28
    virtual cISCPropertyHolder* AsPropertyHolder() = 0;                         // 0x30
    virtual std::uint32_t GetType() = 0;                                        // 0x38
    virtual void GetPosition(float* xyz) = 0;                                   // 0x40
    virtual void GetBoundingBox(float* minXyz, float* maxXyz) = 0;              // 0x50
    virtual void GetBoundingCityCells(tCellRect& rect) = 0;                     // 0x58
    virtual bool IsOccupantGroup(std::uint32_t group) = 0;                      // 0xb0
    virtual bool GetOccupantGroups(std::set<std::uint32_t>& groups) = 0;        // 0xc0
    virtual std::uint32_t GetFlags() = 0;                                       // 0x108
};

// cISC4BuildingOccupant as the manager uses it (QueryInterface 0x87dda3a9).
class IAutomataBuildingOccupant : public cIGZUnknown {
public:
    virtual IAutomataOccupant* AsOccupant() const = 0;                          // 0x18
    virtual const float& GetCompletionPercent() = 0;                            // 0x48
};

// cISC4NetworkOccupant: the manager's network occupant handlers never touch it.
class IAutomataNetworkOccupant : public cIGZUnknown {};

// cISC4Automaton as the manager uses it (primary vtable 0x100a73840; the automaton's dtor finishes at 0x248 and the
// remaining slots live in the same array past it). The occupant-view slots (0x28 / 0x108 of the occupant base at +8) are on
// IAutomataOccupant above.
class IAutomataAutomaton : public cIGZUnknown {
public:
    virtual bool Init() = 0;                                                    // 0x18
    virtual bool Shutdown() = 0;                                                // 0x20
    virtual IAutomataOccupant* Occupant() = 0;                                  // 0x30
    virtual void GetPosition(float* xyz) = 0;                                   // 0x38
    virtual void SetPosition(const float* xyz) = 0;                             // 0x40
    virtual void SetVisible(bool visible) = 0;                                  // 0x48
    virtual bool Remove(bool force) = 0;                                        // 0x68
    virtual void SetOrientation(const float* direction) = 0;                    // 0x70
    virtual void GetOrientation(float* direction) = 0;                          // 0x78
    virtual float GetRadius() const = 0;                                        // 0x88
    virtual void SetAutomataFlag(std::uint32_t flag, bool on) = 0;              // 0xf0
    virtual bool CheckAutomataFlag(std::uint32_t flag) = 0;                     // 0xf8
    virtual float GetOpacity() const = 0;                                       // 0x100
    virtual void SetOpacity(float opacity, float arg) = 0;                      // 0x108
    virtual void AddCallback(cIGZMessageTarget2* target) = 0;                   // 0x138
    virtual void* GetState() = 0;                                               // 0x1c0 (state->target at +0x38)
    virtual void* FindState(std::uint32_t state) = 0;                           // 0x1c8
    virtual bool PushState(std::uint32_t state, void* controller, float speed, bool flag, std::uint32_t a,
                           std::uint32_t b, const void* behavior) = 0;          // 0x1a0
    virtual std::uint8_t GetPriority() = 0;                                     // 0x1f8
    virtual void SetPriority(std::uint8_t priority) = 0;                        // 0x200
    virtual std::uint32_t GetAutomataGroup() = 0;                               // 0x218
    virtual bool DoAnimationTick(std::uint32_t data, float deltaMilliseconds) = 0;   // 0x278
    virtual void SetZoomAndRotation(int zoom, int rotation) = 0;                // 0x288
    virtual void HandleRemoveController(void* controller) = 0;                  // 0x2c0
    virtual bool GetMarkedForRemove() = 0;                                      // 0x2e0
    virtual void SetMarkedForRemove(bool marked) = 0;                           // 0x300
    virtual bool PostInit() = 0;                                                // 0x360
    virtual void SendCallbackMessage(std::uint32_t event, std::uint32_t data) = 0;   // 0x368 (the removal events 2 and 3)
    virtual bool ShouldCollide(IAutomataAutomaton* other) = 0;                  // 0x280
    virtual bool PlaceInOccupantManager(bool place) = 0;                        // 0x268
    virtual void PlaceInRenderer(bool place) = 0;                               // 0x270
    virtual bool IsPlacedInOccupantManager() = 0;                               // +0x5b bit 0
    virtual bool IsPlacedInRenderer() = 0;                                      // +0x5b bit 1
    virtual void StopEffectsAndClearModel() = 0;                                // cSC4ModelRegEffects at +0x290 (StopAllEffects + SetModel(null))
    virtual IAutomataAutomaton* StateTarget() = 0;                              // the state returned by GetState(), field +0x38
    virtual void SetStateTarget(IAutomataAutomaton* target) = 0;                // the same field, with the AddRef/Release the original does
    // Property helper the manager uses to stamp the group onto a new automaton (the automaton's +8 property-holder view,
    // vslot 0x58; the original passes variant 0xaa1dd396 straight through).
    virtual void SetPropertyVariant(std::uint32_t id, cRZVariant& value, bool replace) = 0;
    // The automaton's +0x18 view, vslot 0xd0 (the original passes the exemplar stored in tExemplarData+0x48).
    virtual void SetExemplar(cISCResExemplar* exemplar) = 0;
    virtual void AddModelResourceKey(const cGZPersistResourceKey& key) = 0;      // cSC4Automaton::AddModelResourceKey @ 0x100100a94
    virtual void SetAutomataGroup(IAutomataGroupTemplate* groupTemplate) = 0;    // 0x330 (the +0x18 view's SetAutomataGroup)
};

// cSC4AutomataScriptSystem as the manager uses it (primary vtable 0x100a83e40: 0x38 GetAutomataGroup(u32),
// 0x60 GetOccupantGroup(u32), 0x90 GetTrackBuildingGroups).
class IAutomataGroupTemplate {
public:
    virtual ~IAutomataGroupTemplate() = default;
    virtual std::uint32_t GroupId() const = 0;                                  // +0x20
    virtual std::uint32_t ClassId() const = 0;                                  // GetClassID @ 0x1001de042
    virtual cRZVariant& GroupIdVariant() = 0;                                   // GetGroupIDVariant @ 0x1001de05a
    virtual IAutomataGroupTemplate* Parent() const = 0;                         // +0x18
    virtual std::uint32_t SourceBuilding() const = 0;                           // +0xf4
    virtual bool IsOccupantGroup(std::uint32_t group) const = 0;                // @ 0x1001ddffc
};
class IAutomataScriptGroups : public cIGZUnknown {
public:
    ~IAutomataScriptGroups() = default;
    virtual IAutomataGroupTemplate* GetAutomataGroup(std::uint32_t id) = 0;      // 0x38
    virtual IAutomataGroupTemplate* GetOccupantGroup(std::uint32_t id) = 0;     // 0x60
    virtual const std::list<std::uint32_t>& GetTrackBuildingGroups() = 0;       // 0x90
};

// The 24 hour clock (city vslot 0x178).
class IAutomataClock : public cIGZUnknown {
public:
    ~IAutomataClock() = default;
    virtual int Hour() = 0;                                                     // 0x50
    virtual int Minute() = 0;                                                   // 0x68
};

// cISC4AutomataManager (the per-kind managers): HandlesGroup (0xf0), the creation entry the manager calls for a new group
// automaton (0x78) and the two player-drive entry points (0x48 / 0x50).
class IAutomataKindManager {
public:
    virtual ~IAutomataKindManager() = default;
    virtual bool HandlesGroup(std::uint32_t group) = 0;                         // 0xf0
    virtual IAutomataAutomaton* CreateAutomaton(std::uint32_t group, const cS3DVector3& position, std::uint32_t flags,
                                                const cGZPersistResourceKey& key, cIGZMessageTarget2* listener,
                                                std::uint8_t argument, const cS3DVector3* velocity) = 0;   // 0x78
    virtual void BeginPlayerDrive() = 0;                                        // 0x48
    virtual void EndPlayerDrive() = 0;                                          // 0x50
};

// The city object at vslot 0x188: "is this group being used by the city situation, and how many are there".
class IAutomataGroupBudget {
public:
    virtual ~IAutomataGroupBudget() = default;
    virtual bool HandlesGroup(std::uint32_t group) = 0;                         // 0xa0
    virtual std::uint32_t CurrentCount() = 0;                                   // 0xe0
    virtual std::uint8_t MaxCount() = 0;                                        // 0xd8
    virtual void ClearTrackedAutomata() = 0;                                  // 0x108
};

// The network manager (city vslot 0x140).
class IAutomataNetworkManager : public cIGZUnknown {
public:
    ~IAutomataNetworkManager() = default;
    virtual bool CellMasks(int cellX, int cellZ, std::uint32_t masks[3]) = 0;   // 0x70 -> cell +0x50/+0x54/+0x58
    virtual float Height(float x, float z) = 0;                                 // 0x90
};

// SC4ViewUtilities as the manager reads it (the globals are read through the host).
class IAutomataViewUtilities {
public:
    virtual ~IAutomataViewUtilities() = default;
    virtual void GetZoomAndRotation(int& zoom, int& rotation) = 0;               // 0x10017375a
    virtual void ViewPosition(float out[3]) = 0;                                // +0x44
    virtual void ViewRect(float out[4]) = 0;                                    // +0x3f4 (minX, minZ, maxX, maxZ)
    virtual bool IsPointInViewFrustum(const float point[3]) = 0;                // 0x100173f8a
    virtual void ProjectPointToViewCoordinates(float point[3]) = 0;             // 0x10017383c
    virtual void ProjectPointFromViewCoordinatesToViewportCoordinates(float point[3]) = 0;   // 0x1001738e0
    virtual void UnProjectPointFromViewportCoordinatesToWorldCoordinates(float point[3]) = 0;   // 0x100173b1a
    // SL::spView3D's GetCurrentViewLevel (i386 vslot 0x58): 1 above ground, 2 underground.
    virtual int ViewLevel() { return 1; }
};

// cSC4CameraControl as the camera follow uses it (SL::spRenderer vslot 0x80).
class IAutomataCameraControl {
public:
    virtual ~IAutomataCameraControl() = default;
    virtual int VirtualZoom() = 0;                                              // +0x110
    virtual bool SetZoom(float zoom) = 0;                                       // 0x10076bbe2
    virtual void GetViewportSize(int& width, int& height) = 0;                   // 0x10076c07a
    virtual bool SetViewTargetPosition(const cS3DVector3& position) = 0;        // 0x10076bf2a
    virtual bool SetViewTargetVelocity(const cS3DVector3& velocity) = 0;        // 0x10076bffc
    virtual void TargetVelocity(float out[3]) = 0;                              // +0x4c/+0x50/+0x54 (the current target)
};

// cSC4ViewFilterInformation (only the "does this filter include the group" call is used; the original takes the first
// member of the record and calls its vslot 0x18 with the automaton's group).
class IAutomataViewFilterInformation {
public:
    virtual ~IAutomataViewFilterInformation() = default;
    virtual bool Includes(std::uint32_t group) const = 0;                       // vslot 0x18
};

// The player drive view input control (spPlayerDriveView, 0x1000ff652 creates it).
class IAutomataPlayerDriveControl : public cIGZUnknown {
public:
    virtual void EnterChoosingMode() = 0;                                       // 0x1006fad1a
    virtual void EnterDrivingMode(IAutomataAutomaton* automaton, bool arg) = 0; // 0x1006fa2ee
    virtual void AddCallbackTarget(cIGZMessageTarget2* target) = 0;             // 0x1006fad6a
    virtual bool HandlesGroup(std::uint32_t group) = 0;                         // vslot 0x18
    virtual bool HasDrivenAutomaton() = 0;                                      // +0x50 != 0
    virtual std::uint32_t DrivenGroup() = 0;                                    // the object at +0x50, vslot 0x218 -> +0x20
};

// SC4PathInfoView (operator new(0x28) + ctor at 0x1000ff2aa; the manager only sets its filter at +0x20).
class IAutomataPathView : public cIGZUnknown {
public:
    virtual bool Init() = 0;
    virtual void SetShouldIgnoreDepthBuffer(bool ignore) = 0;
    virtual void SetPathFilter(int filter) = 0;                                 // +0x20
    virtual void Shutdown() = 0;
};

// cSC4DispatchVehicleView (only the cheat uses it).
class IAutomataDispatchView {
public:
    virtual ~IAutomataDispatchView() = default;
    virtual void ShowAllIndicators(int type, bool show) = 0;
};

// The simulator's speed panel (SL::spSimulator and SL::spSC4).
class IAutomataSimulator {
public:
    virtual ~IAutomataSimulator() = default;
    virtual bool IsPaused() = 0;                                                // 0x88
    virtual bool IsFastForward() = 0;                                           // 0x78
    virtual int SpeedIndex() = 0;                                               // 0xd8
    virtual bool SpeedPanelVisible() = 0;                                       // spSC4 0x130 -> +0xf18
};

// cSC4AutomataTuningParams and the loose tuning values that do not belong to it. The field names are the original
// addresses (0x100b218xx / 0x100b219xx / 0x100b21axx) because the scripts own the meaning; the property id each field is
// read from is in the comment. Only the fields the manager itself reads have proper names.
struct AutomataTuningGlobals {
    cSC4AutomataTuningParams params;   // 0x100b21880 (property 0x4998c89f reads into its first field; see the .cpp note)

    cSC4ResponseCurve f218b0;   float f218d0 = 0.0f;   // 0x4998c89c / 0x4998c89e
    cSC4ResponseCurve f218d8;   float f218f8 = 0.0f;   // 0x4a511873 / 0x4998c89e (second target)
    cSC4ResponseCurve f21900;   float f21920 = 0.0f;   // 0xa51188a  / 0xea511de0
    cSC4ResponseCurve f21928;   float f21948 = 0.0f;   // 0x2a511893 / 0xca511de6
    cSC4ResponseCurve f21950;   float f21970 = 0.0f;   // 0xca51189a / 0xca511df2
    cSC4ResponseCurve f21980;   float f219a0 = 0.0f;   // 0x4998c901 / 0x4998c903

    std::uint32_t f21884 = 0, f21888 = 0;      // 0x4998c900 (u32 array, first two)
    std::uint8_t f2188c = 0, f21890 = 0;       // 0x4998c902 (byte array, first two)
    std::uint32_t f21894 = 0;                  // 0x4998c904
    std::uint32_t f21898 = 0, f2189c = 0;      // 0x4998c905 (u32 array, first two)
    std::uint8_t f218a0 = 0, f218a4 = 0;       // 0x4998c909 (byte array, first two)
    float f218a8 = 0.0f;                       // 0x4998c90b
    float f219a4 = 0.0f, f219a8 = 0.0f, f219ac = 0.0f, f219b0 = 0.0f;   // 0x4998c912..0x4998c915
    float f219b4 = 0.0f;                       // 0x4998c916 (the opacity/duration argument the manager passes to automata)
    float f219b8 = 0.0f;                       // 0x2a511cbc
    float f219bc = 0.0f;                       // 0xca5545ea (u32 count / 3600)
    std::uint32_t f219c0 = 0, f219c4 = 0;      // 0x4998c90a (u32 array, first two)
    std::uint32_t f219c8 = 0;                  // 0x4998c91d
    std::int32_t f219cc = 0, f219d0 = 0;       // 0xa9af1e54 / 0xa9af1e55 (byte list -> bit mask, 0xffffffff when missing)
    std::uint8_t f219d4 = 0, f219d5 = 0;       // 0x4998c918 / 0x4998c919 minus one
    std::vector<float> f219d8;                 // 0x4bae8fe0
    float f219e0 = 0.0f;                       // written from f219d8 (the vector's first element, see the .cpp note)
    float f219f0 = 0.0f;                       // 0x89aec88b
    std::uint32_t f219f4 = 0;                  // 0x89aec88c
    std::uint8_t f219f5 = 0;                   // 0x89aec88d
    float f219f8 = 0.0f;                       // 0x89aec88e
    std::uint32_t f219fc = 0;                  // 0xc9ee640d
    bool f219fd = false;                       // 0x49be9565
    float f21a00 = 0.0f;                       // 0xc9c18eae
    float f21a04 = 0.0f, f21a08 = 0.0f;        // 0xa9d6cc17 (float array, first two)
    float f21a0c = 0.0f, f21a10 = 0.0f, f21a14 = 0.0f;   // 0xa9d6cc18 (float array, first three)
    float f21a18 = 0.0f;                       // 0xc9ee640c
    float f21a1c = -90.0f, f21a20 = 90.0f;     // 0xc9da95d4 (float array, defaults -90 / 90)
    float f21a24 = -1.0f, f21a28 = 1.0f;       // 0x29dd40c1
    float f21a2c = -1.0f, f21a30 = 1.0f;       // 0x29dd40c2
    float f21a34 = 0.0f, f21a38 = 0.0f;        // 0xc9ee640a (degrees, converted to radians) / 0xc9ee640b
    float f21a3c = 0.0f;                       // 0x6a09f62f
    std::uint8_t f21a40 = 0, f21a41 = 0;       // 0x6a09f630
    std::uint8_t f21a42 = 0, f21a43 = 0;       // 0xea441c97
    std::uint8_t f21a44 = 0, f21a45 = 0;       // 0x8a441c9c
    std::uint32_t f21a48 = 0, f21a4c = 0;      // 0x6a440d40 (u32 array, first two)
    std::uint32_t f21a50 = 20;                 // 0xa53fd32
    float f21a54 = 0.0f, f21a58 = 0.0f, f21a5c = 0.0f, f21a60 = 0.0f;   // 0x6b42922c (the four simulator speed modifiers)
    std::uint32_t f21a64 = 0;                  // 0x6bac0b3e
    float f21a68 = 0.0f, f21a6c = 0.0f;        // 0x4bae8fe1 / 0x4bae8fe2
    float f21a70 = 0.0f;                       // 0xebfb4a0a
    float f21a74 = 0.0f;                       // 0xcc101142
    float f21a78 = 0.0f;                       // 0x6c200139 (the speed the player drive unlocks with)
    float f21a7c = 0.0f;                       // 0x8c3e8185 (1 / count)
    float f21974 = 0.0f, f21978 = 0.0f;        // 0xec0700c0 / 0x6c0700c4
    bool f21aa8 = false;                       // 0x4998c91c "clock multipliers enabled"
    std::int32_t f21aac = 0;                   // 0x4998c89d
    std::unordered_map<std::uint32_t, cSC4ResponseCurve> clockCurves;   // 0x100b21a80 PopulateLayer -> curve
    struct SpawnRecallParams { float f0 = 0.25f, f4 = 0.5f, f8 = 0.5f, fc = 0.5f; cSC4ResponseCurve curve; };
    std::unordered_map<std::int32_t, SpawnRecallParams> spawnRecall;    // 0x100b21ab0 network type -> params
};

// -------------------------------------------------------------------------------------------------------------------------
// The host: every SL::sp* global and every engine object the original reached directly.
// -------------------------------------------------------------------------------------------------------------------------
class IAutomataManagerHost {
public:
    virtual ~IAutomataManagerHost() = default;
    // ---- framework
    virtual cIGZMessageServer2* MessageServer() = 0;                                  // SL::spMS2 (borrowed)
    virtual void AddTickNotification(cIGZMessageTarget2* target, int channel) = 0;    // SL::ATM() vslot 0x28
    virtual void RemoveTickNotification(cIGZMessageTarget2* target, int channel) = 0; // vslot 0x30
    virtual bool CheatManagerAvailable() = 0;                                         // SL::spCCM != 0
    virtual bool DebugFunctionalityEnabled() = 0;                                     // SL::spSC4 vslot 0xa8
    virtual void RegisterCheat(std::uint32_t id, const char* name) = 0;               // spCCM vslot 0x18
    virtual void UnregisterCheat(std::uint32_t id) = 0;                               // spCCM vslot 0x20
    virtual void AddCheatListener(cIGZMessageTarget* target) = 0;                     // spCCM vslot 0x28
    virtual void RemoveCheatListener(cIGZMessageTarget* target) = 0;                  // spCCM vslot 0x30
    virtual void PostMessage(cIGZMessage2* message, bool front) = 0;                  // spMS2 vslot 0x20
    // ---- city (SL::spCity, all borrowed)
    virtual IAutomataNetworkManager* NetworkManager() = 0;                            // vslot 0x140
    virtual IAutomataScriptGroups* ScriptGroups() = 0;                                // vslot 0x180
    virtual IAutomataClock* CityClock() = 0;                                          // vslot 0x178
    virtual IAutomataKindManager* KindManager(int index) = 0;                          // 0x158 / 0x160 / 0x168 / 0x170
    virtual IAutomataGroupBudget* GroupBudget() = 0;                                  // vslot 0x188
    virtual bool CellToWorld(int cellX, int cellZ, float& x, float& z) = 0;           // vslot 0x308
    virtual bool WorldToCell(float x, float z, int& cellX, int& cellZ) = 0;           // vslot 0x2f8
    virtual float CellSize() = 0;                                                     // vslot 0x2d8
    virtual int CityCellCount() = 0;                                                  // vslot 0x2e8
    virtual bool OccupantCells(IAutomataOccupant* occupant, tCellRect& rect) = 0;     // vslot 0x108 -> 0x28 -> 0x70
    virtual bool LotCellMasks(int cellX, int cellZ, std::uint32_t masks[3]) = 0;      // vslot 0x108 -> 0x28 -> +0x50..+0x58
    // ---- occupants (SL::spOccupantManager)
    virtual cIGZUnknown* CreateOccupantGroupFilter(const std::vector<std::uint32_t>& groups) = 0;   // cSC4OccupantGroupOccupantFilter
    virtual bool EnumerateOccupants(cIGZUnknown* filter, std::list<IAutomataOccupant*>& out) = 0;   // vslot 0x128
    virtual void ReleaseOccupantList(std::list<IAutomataOccupant*>& list) = 0;                      // vslot 0x180
    // ---- views / rendering
    virtual IAutomataViewUtilities* ViewUtilities() = 0;                              // SL::spViewUtilities
    virtual IAutomataPlayerDriveControl* ActiveInputControl() = 0;                    // SL::spView3D vslot 0x90 (the current input control)
    virtual IAutomataPlayerDriveControl* PlayerDriveControl() = 0;                    // the file-static spPlayerDriveView
    virtual void SetPlayerDriveControl(IAutomataPlayerDriveControl* control) = 0;     // stores it (the original writes the global)
    virtual IAutomataPlayerDriveControl* CreatePlayerDriveControl() = 0;              // operator new(0x148) + ctor (counted)
    virtual void AddViewInputControl(IAutomataPlayerDriveControl* control, int layer) = 0;   // SL::spView3D vslot 0xc8
    virtual void SetVehicleManagerSpeed(float speed) = 0;                             // SL::spVehicleManager vslot 0xb8
    virtual void SetGhostModels(bool enabled) = 0;                                    // cSC4Automaton::sbGhostModelsEnabled
    virtual bool CurrentInputControlIsPlayerDrive() = 0;                              // SL::spView3D 0xc0 -> 0x28 == 0x2b709676
    virtual IAutomataCameraControl* CameraControl() = 0;                              // SL::spRenderer vslot 0x80
    virtual bool CameraFollowFast() = 0;                                              // SL::spRenderProperties +0x18 -> +0x978
    virtual float TerrainHeight(float x, float z) = 0;                                // SL::spTerrain vslot 0x60
    virtual bool TerrainShadowEnabled() = 0;                                          // SL::spTerrain vslot 0x58
    virtual float TerrainShadowHeight(float x, float z) = 0;                          // SL::spTerrain vslot 0xe8 / 0xf8
    virtual IAutomataPathView* CreatePathView() = 0;                                  // operator new(0x28) + SC4PathInfoView
    virtual void AddRendererLayer(IAutomataPathView* view) = 0;                       // SL::spRenderer vslot 0x100(view, 5, 1000)
    virtual void RemoveRendererLayer(IAutomataPathView* view) = 0;                    // SL::spRenderer vslot 0x108(view)
    virtual IAutomataDispatchView* DispatchView() = 0;                                // SL::spDispatchManager 0xb0
    virtual void ReleaseObject(cIGZUnknown* object) = 0;                              // vslot 0x10 on any object
    // ---- simulator / misc
    virtual IAutomataSimulator* Simulator() = 0;                                      // SL::spSimulator (null in headless)
    virtual bool VehicleManagerPaused() = 0;                                          // SL::spVehicleManager vslot 0x68
    virtual bool LandfillDeveloperPresent() = 0;                                      // SL::spLandfillDeveloper != 0
    virtual int LandfillSourceBuildingCount() = 0;                                    // spLandfillDeveloper vslot 0x58
    virtual bool GetTuningProperties(cISCPropertyHolder** out) = 0;                   // spRM vslot 0x18 -> exemplar 0x30
    virtual bool GetExemplar(const cGZPersistResourceKey& key, cISCResExemplar** out) = 0;   // spRM vslot 0x18 (counted)
    virtual bool EnumerateResourceKeys(std::uint32_t type, std::vector<cGZPersistResourceKey>& out) = 0;   // spRM 0x90
    virtual IAutomataAutomaton* CreateAutomatonByClassId(std::uint32_t classId) = 0;   // GZCOM()->CreateInstance(classId, 0xa9b40f05)
    virtual void DebugAutomatonRemoved(IAutomataAutomaton* automaton) = 0;             // spDebugView->AutomatonRemoved
    virtual void DebugViewSetEnabled(bool enabled) = 0;                                // the object at spDebugView vslot 0x110
    virtual void DebugViewShutdown() = 0;                                              // spDebugView 0x20 / 0x10
    virtual void Report(const std::string& message) { (void)message; }                 // Sprintf'd error text
    // Port seam: the manager's view of an automaton read from a city file (the object SC4ReadGZSerializable gave for
    // cISC4Automaton; the original stores it as it is). Null drops it.
    virtual IAutomataAutomaton* LoadedAutomaton(void* object) { (void)object; return nullptr; }
    // Port seams: this manager's views of the city's own objects that messages and queries hand it (the original uses the
    // objects themselves). The defaults take the object as the view (the tests' fakes implement the views directly).
    virtual IAutomataOccupant* OccupantView(void* cityOccupant) { return static_cast<IAutomataOccupant*>(cityOccupant); }   // cISC4Occupant*
    virtual IAutomataBuildingOccupant* BuildingView(void* building) { return static_cast<IAutomataBuildingOccupant*>(building); }   // QI 0x87dda3a9 answer
    virtual IAutomataAutomaton* AutomatonView(void* automaton) { return static_cast<IAutomataAutomaton*>(automaton); }   // QI 0xa9b40f05 answer
};
IAutomataManagerHost* AutomataManagerHost();
void SetAutomataManagerHost(IAutomataManagerHost* host);

// The class-static tuning table (one per process; StaticInit fills it).
AutomataTuningGlobals& AutomataTuning();

// cISC4AutomataManager: the primary vtable, slots 3..0x248 in the original order (the two destructor slots at 0x130/0x138
// come from the virtual destructor this interface declares there).
class cISC4AutomataManager : public cIGZUnknown {
public:
    virtual ~cISC4AutomataManager() = default;                                                  // 0x130 / 0x138
    virtual bool Init() = 0;                                                                    // 0x18 @0x1000fee58
    virtual bool Shutdown() = 0;                                                                // 0x20 @0x1000ff06a
    virtual void Activate(bool active) = 0;                                                     // 0x28 @0x1001005d4
    virtual bool IsActive() = 0;                                                                // 0x30 @0x1000f3b80
    virtual bool AddAutomaton(IAutomataAutomaton* automaton) = 0;                                // 0x38 @0x100101848
    virtual bool RemoveAutomaton(IAutomataAutomaton* automaton, bool force) = 0;                 // 0x40 @0x100101c2c
    virtual void Pause() = 0;                                                                   // 0x48 @0x1000e2446
    virtual void Resume() = 0;                                                                  // 0x50 @0x1000e2450
    virtual bool IsPaused() = 0;                                                                // 0x58 @0x1000e245a
    virtual int GetAutomataCount() = 0;                                                         // 0x60 @0x1004fde70
    virtual bool IsDrivingMode() = 0;                                                           // 0x68 @0x1000ff79e
    virtual int GetMinZoom() = 0;                                                               // 0x70 @0x1000e246e
    virtual IAutomataAutomaton* CreateAutomaton(std::uint32_t group, const cS3DVector3& position, std::uint32_t flags,
                                                const cGZPersistResourceKey& key, cIGZMessageTarget2* listener,
                                                std::uint8_t argument, const cS3DVector3* velocity) = 0;   // 0x78 @0x100100698
    virtual bool GetRandomCreationPoint(std::uint32_t group, const cS3DVector3& origin, float minDistance,
                                        float maxDistance, const cS3DVector3& center, cS3DVector3& direction,
                                        std::uint32_t flags) = 0;                           // 0x80 @0x100100ba0
    virtual IAutomataAutomaton* Dispatch(std::uint32_t group, const tAutomataLocation& from, const tAutomataLocation& to,
                                         std::uint32_t flags, std::uint8_t type, const cGZPersistResourceKey& key,
                                         std::uint8_t argument) = 0;                        // 0x88 @0x100100d86 (stub)
    virtual bool Dispatch(IAutomataAutomaton* automaton, const tAutomataLocation& from, const tAutomataLocation& to,
                          std::uint32_t flags, std::uint8_t type) = 0;                      // 0x90 @0x100100d8e (stub)
    virtual bool TranslateDispatchLocation(const tAutomataLocation& location, tCellRect& rect) = 0;   // 0x98 @0x100100d96
    virtual bool TranslateDispatchLocation(const tAutomataLocation& location, cS3DVector3& position) = 0;   // 0xa0 @0x100100f6c
    virtual bool ChaseAutomaton(IAutomataAutomaton* automaton, IAutomataAutomaton* target) = 0;   // 0xa8 @0x100101118
    virtual bool ChaseAutomaton(std::uint32_t group, float radius, IAutomataAutomaton* target, bool all) = 0;   // 0xb0 @0x10010127c
    virtual void SetMaxCount(int count) = 0;                                                    // 0xb8 @0x100102580
    virtual bool HandleTuningParametersChanged() = 0;                                           // 0xc0 @0x100102f6e
    virtual tExemplarData* ChooseRandomExemplar(std::uint32_t group, PopulateLayer layer, int cellX, int cellZ) = 0;   // 0xc8
    virtual tExemplarData* ChooseRandomExemplarByFilter(std::uint32_t group, std::uint32_t layerMask, std::uint32_t mask0,
                                                        std::uint32_t mask1, std::uint32_t mask2) = 0;   // 0xd0
    virtual tExemplarData* ChooseExemplarByCrimeType(std::uint32_t group, std::uint32_t crimeType, PopulateLayer layer) = 0;   // 0xd8
    virtual std::vector<tExemplarData>& GetExemplarData() = 0;                                   // 0xe0 @0x1000f3b90
    virtual std::map<std::uint32_t, std::vector<std::uint32_t>>& GetExemplarDataMap() = 0;       // 0xe8 @0x1000e248a
    virtual bool CanCreateGroup(std::uint32_t group) = 0;                                       // 0xf0 @0x100104e1c
    virtual bool TrackAutomataGroupInstances(std::uint32_t group, bool track) = 0;              // 0xf8 @0x100104e62
    virtual bool GetInstancesOfGroup(std::uint32_t group, std::unordered_set<IAutomataAutomaton*>& out) = 0;   // 0x100 @0x10010543a
    virtual void ReleaseAutomatonSet(std::unordered_set<IAutomataAutomaton*>& set) = 0;         // 0x108 @0x1001059ba
    virtual void SetCameraFollow(IAutomataAutomaton* automaton) = 0;                            // 0x110 @0x100101e44
    virtual bool IsCameraFollowUpdating() = 0;                                                  // 0x118 @0x100101e38
    virtual std::uint32_t GetSourceBuildingCount(std::uint32_t group) = 0;                      // 0x120 @0x100103efc
    virtual std::uint32_t GetSourceBuildingCountForAutomataGroup(std::uint32_t group) = 0;      // 0x128 @0x100103f4e
    virtual bool Write(cIGZOStream& stream) const = 0;                                          // 0x140 @0x1001047a0
    virtual bool Read(cIGZIStream& stream) = 0;                                                 // 0x148 @0x100104870
    virtual std::uint32_t GetGZCLSID() const = 0;                                               // 0x150 @0x10011b6e0
    virtual bool StaticInit() = 0;                                                              // 0x158 @0x1000fdcba
    virtual bool StaticShutdown() = 0;                                                          // 0x160 @0x1000fed8a
    virtual bool DoMessage(cGZMessage& message) = 0;                                            // 0x168 @0x1000ff2aa
    virtual bool DoMessage(cIGZMessage2* message) = 0;                                          // 0x170 @0x1000ff7d4
    virtual bool IsAutomatonOnscreen(IAutomataAutomaton* automaton) = 0;                        // 0x178 @0x100102626
    virtual float GetClockMultiplier(PopulateLayer layer) = 0;                                  // 0x180 @0x1001016cc
    virtual void PostNotificationMessage(std::uint32_t type, IAutomataAutomaton* automaton, std::uint32_t data1,
                                         std::uint32_t data2) = 0;                              // 0x188 @0x1001046e6
    virtual tExemplarData* FindExemplarFromModelKey(const cGZPersistResourceKey& key) = 0;       // 0x190 @0x100104506
    virtual void DestroyAutomata() = 0;                                                        // 0x198 @0x1001017a0
    virtual IAutomataAutomaton* CreateAutomatonInternal(std::uint32_t group, std::uint32_t flags,
                                                        const cGZPersistResourceKey& key, const tExemplarData* data,
                                                        std::uint8_t priority) = 0;              // 0x1a0 @0x100100728
    virtual bool AdjustCountForAutomaton(std::uint8_t priority) = 0;                            // 0x1a8 @0x100100b3a
    virtual void HandleViewChanged() = 0;                                                       // 0x1b0 @0x1000ffba0
    virtual bool HandleViewZoomChanged(int oldZoom, int newZoom) = 0;                            // 0x1b8 @0x1000ffbf0
    virtual bool HandleViewFilterChanged(const IAutomataViewFilterInformation* oldFilter,
                                        const IAutomataViewFilterInformation* newFilter) = 0;   // 0x1c0 @0x10010027a
    virtual bool HandleInsertNetworkOccupant(IAutomataNetworkOccupant* occupant) = 0;            // 0x1c8 @0x100100340
    virtual bool HandleRemoveNetworkOccupant(IAutomataNetworkOccupant* occupant) = 0;            // 0x1d0 @0x100100348
    virtual bool HandleNewBuilding(IAutomataBuildingOccupant* building) = 0;                     // 0x1d8 @0x100100350
    virtual bool HandleInsertOccupant(IAutomataOccupant* occupant) = 0;                          // 0x1e0 @0x10010042e
    virtual bool HandleRemoveOccupant(IAutomataOccupant* occupant) = 0;                          // 0x1e8 @0x1001004ce
    virtual void HandlePostCityInit() = 0;                                                      // 0x1f0 @0x1000ffc06
    virtual bool HandleRemoveController(void* controller) = 0;                                  // 0x1f8 @0x100100586
    virtual bool HandleRenderPropertiesChanged() = 0;                                           // 0x200 @0x1001005cc
    virtual void HandleOccupantGroupChanged(IAutomataAutomaton* automaton) = 0;                  // 0x208 @0x1000ffefe
    virtual void UpdateZoomAndRotation() = 0;                                                   // 0x210 @0x10010019c
    virtual bool AddAutomatonInternal(IAutomataAutomaton* automaton) = 0;                        // 0x218 @0x100101bc2
    virtual bool RemoveAutomatonInternal(IAutomataAutomaton* automaton) = 0;                     // 0x220 @0x100101df6
    virtual void DoAnimationTick(std::uint32_t data, float deltaMilliseconds) = 0;              // 0x228 @0x100101424
    virtual void UpdateCameraFollow(float deltaMilliseconds) = 0;                                // 0x230 @0x100101f70
    virtual void SeedTrackedBuildingsMap() = 0;                                                 // 0x238 @0x100102f76
    virtual void ScanExemplars() = 0;                                                           // 0x240 @0x100103230
    virtual void InitExemplarData(tExemplarData& data, cISCResExemplar* exemplar) = 0;           // 0x248 @0x1001037a8
};

// Non-virtual helpers (the original declares them as plain members).
class cSC4AutomataManager : public cISC4AutomataManager, public cIGZMessageTarget, public cIGZSerializable,
                            public cIGZMessageTarget2 {
public:
    cSC4AutomataManager();                                                    // @ 0x1000fda34
    ~cSC4AutomataManager();                                                   // @ 0x1000fdb9c (deleting 0x1000fdc8c)

    // cIGZUnknown (four views, one refcount)
    bool QueryInterface(std::uint32_t iid, void** out) override;              // @ 0x100104544 (thunks 0x100104592..)
    std::uint32_t AddRef() override { return ++refCount_; }                   // @ 0x1001045bc
    std::uint32_t Release() override;                                         // @ 0x1001045f4 (thunks 0x100104638..)

    bool Init() override;                                                     // @ 0x1000fee58
    bool Shutdown() override;                                                 // @ 0x1000ff06a
    bool StaticInit() override;                                               // @ 0x1000fdcba
    bool StaticShutdown() override;                                           // @ 0x1000fed8a

    // ---- registry
    void Activate(bool active) override;                                      // @ 0x1001005d4
    bool IsActive() override { return active_; }                              // @ 0x1000f3b80
    int GetMinZoom() override { return minZoom_; }                            // @ 0x1000e246e
    bool AddAutomaton(IAutomataAutomaton* automaton) override;                // @ 0x100101848
    bool AddAutomatonInternal(IAutomataAutomaton* automaton) override;        // @ 0x100101bc2
    bool RemoveAutomaton(IAutomataAutomaton* automaton, bool force) override; // @ 0x100101c2c
    bool RemoveAutomatonInternal(IAutomataAutomaton* automaton) override;     // @ 0x100101df6
    void DestroyAutomata() override;                                          // @ 0x1001017a0
    void Pause() override { paused_ = true; }                                 // @ 0x1000e2446
    void Resume() override { paused_ = false; }                               // @ 0x1000e2450
    bool IsPaused() override { return paused_; }                              // @ 0x1000e245a
    int GetAutomataCount() override { return static_cast<int>(automata_.size()); }   // @ 0x1004fde70
    void SetMaxCount(int count) override;                                     // @ 0x100102580
    bool AdjustCountForAutomaton(std::uint8_t priority) override;             // @ 0x100100b3a
    bool IsAutomatonOnscreen(IAutomataAutomaton* automaton) override;         // @ 0x100102626
    IAutomataAutomaton* GetCollidingAutomaton(IAutomataAutomaton* automaton, float margin);   // @ 0x100102666

    // ---- creation / dispatch
    IAutomataAutomaton* CreateAutomaton(std::uint32_t group, const cS3DVector3& position, std::uint32_t flags,
                                        const cGZPersistResourceKey& key, cIGZMessageTarget2* listener,
                                        std::uint8_t argument, const cS3DVector3* velocity) override;   // @ 0x100100698
    IAutomataAutomaton* CreateAutomatonInternal(std::uint32_t group, std::uint32_t flags, const cGZPersistResourceKey& key,
                                                const tExemplarData* data, std::uint8_t priority) override;   // @ 0x100100728
    static IAutomataAutomaton* StaticCreateAutomaton(std::uint32_t group, const cS3DVector3& position, std::uint32_t flags,
                                                     const cGZPersistResourceKey& key, cIGZMessageTarget2* listener,
                                                     std::uint8_t argument);   // @ 0x100104da8
    bool GetRandomCreationPoint(std::uint32_t group, const cS3DVector3& origin, float minDistance, float maxDistance,
                                const cS3DVector3& center, cS3DVector3& direction, std::uint32_t flags) override;   // @ 0x100100ba0
    IAutomataAutomaton* Dispatch(std::uint32_t, const tAutomataLocation&, const tAutomataLocation&, std::uint32_t,
                                 std::uint8_t, const cGZPersistResourceKey&, std::uint8_t) override {
        return nullptr;   // @ 0x100100d86 (stub in the shipping binary)
    }
    bool Dispatch(IAutomataAutomaton*, const tAutomataLocation&, const tAutomataLocation&, std::uint32_t,
                  std::uint8_t) override {
        return false;   // @ 0x100100d8e (stub in the shipping binary)
    }
    bool TranslateDispatchLocation(const tAutomataLocation& location, tCellRect& rect) override;   // @ 0x100100d96
    bool TranslateDispatchLocation(const tAutomataLocation& location, cS3DVector3& position) override;   // @ 0x100100f6c
    static IAutomataKindManager* GetAutomataManagerForGroup(std::uint32_t group);   // @ 0x100104c7e (no this used)

    // ---- tick / camera
    void DoAnimationTick(std::uint32_t data, float deltaMilliseconds) override;   // @ 0x100101424
    bool CalculateSimSpeedModifier();                                        // @ 0x1001015f2
    float GetClockMultiplier(PopulateLayer layer) override;                    // @ 0x1001016cc
    bool ChaseAutomaton(IAutomataAutomaton* automaton, IAutomataAutomaton* target) override;   // @ 0x100101118
    bool ChaseAutomaton(std::uint32_t group, float radius, IAutomataAutomaton* target, bool all) override;   // @ 0x10010127c
    void HandleViewChanged() override;                                        // @ 0x1000ffba0
    bool HandleViewZoomChanged(int oldZoom, int newZoom) override;            // @ 0x1000ffbf0
    bool HandleViewFilterChanged(const IAutomataViewFilterInformation* oldFilter,
                                 const IAutomataViewFilterInformation* newFilter) override;   // @ 0x10010027a
    void UpdateZoomAndRotation() override;                                    // @ 0x10010019c
    void SetCameraFollow(IAutomataAutomaton* automaton) override;             // @ 0x100101e44
    bool IsCameraFollowUpdating() override;                                   // @ 0x100101e38
    void UpdateCameraFollow(float deltaMilliseconds) override;                // @ 0x100101f70

    // ---- groups / buildings
    bool CanCreateGroup(std::uint32_t group) override;                        // @ 0x100104e1c
    bool TrackAutomataGroupInstances(std::uint32_t group, bool track) override;   // @ 0x100104e62
    bool GetInstancesOfGroup(std::uint32_t group, std::unordered_set<IAutomataAutomaton*>& out) override;   // @ 0x10010543a
    void ReleaseAutomatonSet(std::unordered_set<IAutomataAutomaton*>& set) override;   // @ 0x1001059ba
    void HandleOccupantGroupChanged(IAutomataAutomaton* automaton) override;   // @ 0x1000ffefe
    void SeedTrackedBuildingsMap() override;                                  // @ 0x100102f76
    std::uint32_t GetSourceBuildingCount(std::uint32_t group) override;       // @ 0x100103efc
    std::uint32_t GetSourceBuildingCountForAutomataGroup(std::uint32_t group) override;   // @ 0x100103f4e

    // ---- exemplars
    std::vector<tExemplarData>& GetExemplarData() override { return exemplarData_; }   // @ 0x1000f3b90
    std::map<std::uint32_t, std::vector<std::uint32_t>>& GetExemplarDataMap() override { return exemplarDataMap_; }   // @ 0x1000e248a
    tExemplarData* ChooseRandomExemplar(std::uint32_t group, PopulateLayer layer, int cellX, int cellZ) override;   // @ 0x100103fa2
    tExemplarData* ChooseRandomExemplarByFilter(std::uint32_t group, std::uint32_t layerMask, std::uint32_t mask0,
                                                std::uint32_t mask1, std::uint32_t mask2) override;   // @ 0x10010423e
    tExemplarData* ChooseExemplarByCrimeType(std::uint32_t group, std::uint32_t crimeType, PopulateLayer layer) override;   // @ 0x1001043aa
    tExemplarData* FindExemplarFromModelKey(const cGZPersistResourceKey& key) override;   // @ 0x100104506
    void ScanExemplars() override;                                            // @ 0x100103230
    static void EnumAutomatonExemplar(const cGZPersistResourceKey& key, void* context);   // @ 0x1001032c6
    void InitExemplarData(tExemplarData& data, cISCResExemplar* exemplar) override;   // @ 0x1001037a8

    // ---- messages / queries
    bool DoMessage(cIGZMessage2* message) override;                           // @ 0x1000ff7d4 (thunk 0x1000ffb8e)
    bool DoMessage(cGZMessage& message) override;                             // @ 0x1000ff2aa (thunk 0x1000ff74c)
    bool HandleInsertNetworkOccupant(IAutomataNetworkOccupant*) override { return true; }   // @ 0x100100340 (empty)
    bool HandleRemoveNetworkOccupant(IAutomataNetworkOccupant*) override { return true; }   // @ 0x100100348 (empty)
    bool HandleNewBuilding(IAutomataBuildingOccupant* building) override;     // @ 0x100100350
    bool HandleInsertOccupant(IAutomataOccupant* occupant) override;          // @ 0x10010042e
    bool HandleRemoveOccupant(IAutomataOccupant* occupant) override;          // @ 0x1001004ce
    bool HandleRemoveController(void* controller) override;                   // @ 0x100100586
    bool HandleRenderPropertiesChanged() override { return true; }            // @ 0x1001005cc (empty)
    void HandlePostCityInit() override;                                       // @ 0x1000ffc06
    void EnterPlayerDriveView(IAutomataAutomaton* automaton, cIGZMessageTarget2* callback);   // @ 0x1000ff652
    void ExitPlayerDriveView();                                               // @ 0x1000ff75c
    bool IsDrivingMode() override;                                            // @ 0x1000ff79e
    void PostNotificationMessage(std::uint32_t type, IAutomataAutomaton* automaton, std::uint32_t data1,
                                 std::uint32_t data2) override;               // @ 0x1001046e6
    bool HandleTuningParametersChanged() override { return true; }            // @ 0x100102f6e (empty)
    void ReadTuningParameters();                                              // @ 0x1000fe11c
    void ReadTuningClock(cISCPropertyHolder* holder, std::uint32_t property, PopulateLayer layer);   // @ 0x10010279c
    void ReadSpawnRecallParameters(cISCPropertyHolder* holder, std::uint32_t property, std::int32_t networkType);   // @ 0x100102afa

    // ---- persistence
    bool Write(cIGZOStream& stream) const override;                            // @ 0x1001047a0 (thunk 0x10010485e)
    bool Read(cIGZIStream& stream) override;                                   // @ 0x100104870 (thunk 0x100104c36)
    bool ReadInternal(cIGZIStream& stream, std::uint16_t version);             // @ 0x1001048aa
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4AutomataManager; }   // @ 0x10011b6e0

    // ---- test / engine access
    bool Initialized() const { return initialized_; }
    // The per-kind managers (cSC4VehicleManager / the traffic manager) share the base's +0x78 random stream and read the
    // base's default group (+0xa8c); both are private, so expose them.
    cRZRandomSuper& Random() { return random_; }                       // +0x78
    std::uint32_t DefaultGroup() const { return defaultGroup_; }        // +0xa8c
    bool PostCityInitPending() const { return postCityInitPending_; }
    int MinZoom() const { return minZoom_; }
    std::uint32_t RefCount() const { return refCount_; }
    const std::list<IAutomataAutomaton*>& Automata() const { return automata_; }
    const std::map<std::uint32_t, std::vector<std::uint32_t>>& ExemplarDataMap() const { return exemplarDataMap_; }
    const std::unordered_map<std::uint32_t, std::unordered_set<IAutomataAutomaton*>>& TrackedGroupInstances() const {
        return trackedGroups_;
    }
    const std::unordered_map<std::uint32_t, std::uint32_t>& TrackedBuildings() const { return trackedBuildings_; }
    void SetMinZoom(int zoom) { minZoom_ = static_cast<std::uint8_t>(zoom); }
    void SetDefaultGroup(std::uint32_t group) { defaultGroup_ = group; }
    void SetFallbackGroup(std::uint32_t group) { fallbackGroup_ = group; }
    void SetExemplarGroup(std::uint32_t group) { exemplarGroup_ = group; }
    std::uint32_t MaxCount() const { return maxCount_; }
    std::uint32_t MaxCountLimit() const { return maxCountLimit_; }
    float ViewRect(int index) const { return viewRect_[index]; }
    std::uint32_t LoadedFlag() const { return loadedFlag_; }
    bool ScrollInProgress() const { return scrollInProgress_; }
    static int StaticInstanceCount();
    static IAutomataAutomaton* CameraFollowTarget();
    static bool CameraChanging();
    static void ResetCameraFollowState();
    static bool SimSpeedPaused();
    static float SimSpeedModifier();
    static IAutomataPathView* PathView();

private:
    void ClearExemplars();
    void AddExemplarToGroupMap(const tExemplarData& data, std::size_t index);

    std::uint32_t maxCount_ = 0;             // +0x4c
    std::uint32_t maxCountLimit_ = 0;        // +0x50
    bool postCityInitPending_ = false;       // +0x20
    bool initialized_ = false;               // +0x44
    std::uint32_t refCount_ = 0;             // +0x48
    float viewRect_[4] = {0, 0, 0, 0};       // +0x24
    float previousViewRect_[4] = {0, 0, 0, 0};   // +0x34
    std::list<IAutomataAutomaton*> automata_;   // +0x58
    bool active_ = true;                     // +0x74
    bool paused_ = false;                    // +0x75
    cRZRandomSuper random_{0xffffffffu};     // +0x78
    std::vector<tExemplarData> exemplarData_;                     // +0xa48
    std::map<std::uint32_t, std::vector<std::uint32_t>> exemplarDataMap_;   // +0xa60
    float lastViewPosition_[3] = {0, 0, 0};  // +0xa78
    std::uint8_t minZoom_ = 0;               // +0xa84
    std::uint32_t loadedFlag_ = 0;           // +0xa85 (a byte in the original, kept as the message writes a word)
    std::uint32_t exemplarGroup_ = 0;        // +0xa88
    std::uint32_t defaultGroup_ = 0;         // +0xa8c
    std::uint32_t fallbackGroup_ = 0;        // +0xa90
    bool scrollInProgress_ = false;          // +0xa94
    std::unordered_map<std::uint32_t, std::unordered_set<IAutomataAutomaton*>> trackedGroups_;   // +0xa98
    std::unordered_map<std::uint32_t, std::uint32_t> trackedBuildings_;   // +0xac0
    bool recountBuildings_ = false;          // +0xae8
    IAutomataNetworkManager* networkManager_ = nullptr;   // +0xaf0 (counted)
    IAutomataScriptGroups* scriptGroups_ = nullptr;       // +0xaf8 (counted)
    IAutomataClock* cityClock_ = nullptr;                 // +0xb00 (counted)
};

}  // namespace sc4
