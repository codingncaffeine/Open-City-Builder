// cSC4PropDeveloper — the developer for props: it walks the city tract by tract, asks the registered prop
// requesters how many props each tract wants, re-uses the props that are already in the lots / network lots
// (matching them by lot tag to the configuration's optional prop entries), removes the ones the requesters no
// longer want, creates the missing ones through the prop manager, and then keeps the created props in lists
// so that the time of day, the date, the power grid, the global-effect triggers and the night switch can
// update them.
//
// Recovered from Sim City 4 Deluxe Editionsub 1.2.1 (0x1001217ce..0x100126490, class id 0x89c48f47). It is one
// COM object with three views (verified with scripts/vtable.py against 0x100a75690 / 0x100a75728 / 0x100a75758):
//   +0x00 cISC4PropDeveloper   iid 0xe9a1f4b6 (and iid 1)   17 slots: Init..AreSignsAndLabelsVisible, the
//                                                          two destructors, Write/Read/GetGZCLSID/DoMessage
//   +0x08 cIGZMessageTarget2   iid 0x452294aa                4 slots (QueryInterface/AddRef/Release/DoMessage)
//   +0x10 cIGZSerializable     iid 0xe4fda3d4                6 slots (QUI/AddRef/Release/Write/Read/GetGZCLSID)
// One reference count at +0x1c is shared by the three views (the secondary AddRef/Release thunks adjust their
// `this` by -8 / -0x10). cSC4PropDeveloper does *not* derive from cSC4PortDeveloper (RTTI: cISC4PropDeveloper :
// cIGZUnknown); it only shares the cIGZMessageTarget2 + cIGZSerializable layout with it.
//
// Original layout (0xb20 bytes — the size GZCOM records for the class):
//   +0x18 uint8  initialised (Init/Shutdown guard)
//   +0x1c uint32 reference count
//   +0x20 uint8  walk phase: 0 not started, 1 armed by Init, 2 walking the tracts (DoIdle)
//   +0x24 uint8  walk cursor state: 0 first tract, 1 advancing, 2 end of the tract rectangle
//   +0x28 int    cursor x (tract), +0x2c int cursor z, +0x30 int step (1)
//   +0x34 SC4Rect<int> the tract rectangle Init measures from the city (minX/minZ/maxX/maxZ);
//         the constructor leaves it INT_MIN/INT_MAX
//   +0x48 cRZRandomSuper (0x9d0 bytes) seeded from the seconds since cRZTime::mRefDate * 28 by the constructor
//   +0xa18 uint8 "the default requester was created" flag
//   +0xa20 std::vector<cISC4PropRequester*> requesters (AddPropRequester/RemovePropRequester)
//   +0xa38 cISC4LotManager*         +0xa40 cISC4PropManager*      +0xa48 cISC4PowerSimulator*
//   +0xa50 cISC4NetworkLotManager*  (taken from the city's slot 0x150)
//   +0xa58 std::vector<cISC4PropOccupant*> props whose state follows the night flag (property 0x49c9c93c == 1)
//   +0xa70 uint32 animation cursor  +0xa74 int32 animation mode: 0 off, 1 lights on, 2 lights off
//   +0xa78 uint32 animation batch (ceil(props/8))   +0xa7c uint32 animation seconds accumulator
//   +0xa80 bool   night        +0xa84 float minute-change threshold (0.4 h)  +0xa88 float last time of day (-100)
//   +0xa90 std::unordered_map<uint32 effect id, std::list<cISC4PropOccupant*>> global-effect props (0x28 bytes)
//   +0xab8 std::list<cISC4PropOccupant*> date-range props   (property 0xa751675)
//   +0xad0 std::list<cISC4PropOccupant*> time-of-day props  (property 0x4a149631)
//   +0xae8 std::list<cISC4PropOccupant*> power props        (property 0x6a959e71)
//   +0xb00 bool   signs and labels visible
//   +0xb08 std::list<cISC4PropOccupant*> sign props (prop types 0xab72fbb3 and 0xcb79919b)
//
// The registry slice (UpdateTract / SelectAndCreateProp) uses the configuration objects the lot configuration and
// the network lot both expose (cSC4LotConfigurationObject, 0x40 bytes): an object of type 1 (prop) whose flags
// bit 0 is set is an *optional* prop slot. A prop placed for such a slot carries the object's id as its lot tag
// and the requester index in property 0x89a1c16c. The 0x20-byte PropSlot below is the original's stack record
// (list node payload): the cell (two bytes), the configuration object array, the object index and the occupant.
//
// Quirks kept on purpose (each is asserted by the test, the address in the comment):
//  * The constructor seeds the generator with ((seconds of day) + (today - cRZTime::mRefDate.julianDay) * 86400) * 28
//    in 32-bit arithmetic; the reference date is the *binary's* static-initialised value 249522, not the 1970
//    epoch the date code uses (0x10012188c). The host supplies it.
//  * Init computes the walk cursor byte at +0x24 as 2 when a city dimension's top bit is set, but clamps the
//    dimension to 0 first, so the byte is always 0 (0x100121d7a). Kept as a zero write.
//  * The tract walk starts by calling UpdateTract(cursorX, cursorZ) and only advances the cursor on the *next*
//    DoIdle; after the last tract it wraps to the origin and starts over in the same call, so it never stops
//    (0x100123602).
//  * DoIdle ignores its `elapsed` argument (0x1001235a0) and the day/idle-time messages both call it.
//  * UpdateTract builds a vector<bool> of "this requester asked for something" and never reads it (0x100124e1c).
//  * UpdateTract divides by the total requested share without checking for zero (0x1001255ea). The port returns
//    early in that impossible-for-the-game case instead of dividing by zero.
//  * A prop whose lot tag does not match any configuration object *decrements* the tract's created-prop count
//    (0x100125254), so the count can go negative.
//  * The remainder of the requested-prop distribution is handed out round-robin starting at requester 1, not 0
//    (0x100125625).
//  * SelectAndCreateProp returns `(idCount & ~0xff) | 1` instead of a plain bool — the compiler only wrote `bl`
//    (0x100126035); the game's prop tables are small, so it is 1 in practice. Kept verbatim.
//  * SelectAndCreateProp returns 0 when the cell has neither a lot nor a network lot (0x10012606c).
//  * HandleInsertOccupant's sign check (0x100123e11) is reached both after the property work and directly when
//    the prop has none of the four properties, so a sign prop is listed even without them.
//  * The lot-state handler posts the "props changed" message with data3 = (newState == 3 && oldState == 2), i.e.
//    "the lot just became livable", read from the *incoming* message's data1/data2 (0x100123470).
//  * UpdateLotStateProps sets the prop state to `property 0x6a5e0727 != lot state` (inverted), 0x100124b06.
//  * Shutdown releases every sign prop's reference but never clears the sign list (0x100121f7e), and it releases
//    the power props without clearing that list either.
//  * Read overwrites the three manager pointers without releasing the Init-time ones (0x1001229xx): a leak, kept.
//  * The wealth requester's constructor starts its reference count at 0 in the port (the original writes 1 at
//    +0x10), so Init takes the creator's reference explicitly before AddPropRequester; nothing observable differs.
//  * PropSlot stores the city cell in two bytes (the original does too), so a city wider than 256 cells would
//    truncate the cell in the registry. Kept.
//
// The neighbours this class reaches through the SL::sp* globals are reached through IPropDeveloperHost, one
// method per original call with its vtable slot in the comment. cSC4Lot, cSC4LotConfiguration,
// cSC4LotConfigurationObject, cSC4NetworkLot, cISC4PropRequester (cSC4WealthRequester), cSCPropertyHelp,
// cISCPropertyHolder/cISCProperty/cIGZVariant and cRZRandomSuper are used directly (all recovered).
//
// The prop occupant interface (cISC4PropOccupant, iid 0xe9793a65, primary table 0x100a76950) is in
// sc4/PropOccupantInterface.h; cSC4PropOccupant derives from it and answers the iid.
#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

#include "gz/cGZDate.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZSerializable.h"
#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "rz/cRZRandomSuper.h"
#include "sc4/Lot.h"
#include "sc4/Occupant.h"
#include "sc4/PropManager.h"
#include "sc4/PropOccupantInterface.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/SCProperty.h"
#include "sc4/TileNeighborhood.h"
#include "sc4/WealthRequester.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4PropDeveloper = 0x89c48f47;
constexpr std::uint32_t kGZIID_cISC4PropDeveloper = 0xe9a1f4b6;
constexpr std::uint32_t kGZIID_cIGZMessageTarget2PropDeveloper = 0x452294aa;
constexpr std::uint32_t kGZIID_cIGZSerializablePropDeveloper = 0xe4fda3d4;

// The messages Init registers on SL::spMS2 (in this order, 0x100121f04..0x100121f4a).
enum PropDeveloperMessage : std::uint32_t {
    kPropMessageOccupantInserted = 0x99ef1142,     // void1 = occupant, data3 = occupant type
    kPropMessageOccupantRemoved = 0x99ef1143,      // void1 = occupant, data3 = occupant type
    kPropMessageLightsOff = 0xc9da96e8,            // night begins: props -> anim mode 2
    kPropMessageLightsOn = 0xc9da96e9,             // day begins: props -> anim mode 1
    kPropMessageMinuteChanged = 0x44778231,        // clock: 5-minute step
    kPropMessageDayNightChanged = 0x44778234,      // data1 = 1 night / 0 day
    kPropMessageCityInit = 0x26d31ec1,             // re-read the lighting manager
    kPropMessageLotStateChanged = 0xe8bda2e7,      // unknown slot 0x100 = lot snapshot, data1/2 = old/new state
    kPropMessagePowerChanged = 0x8a07ae00,         // data1..4 = SC4Rect<int> in cells
    kPropMessageGlobalEffectChanged = 0xaa375fe8,  // data1 = effect id, data2 = on
    kPropMessagePropsChanged = 0xca5e5027,         // data1 = x, data2 = z (the developer posts it to itself)
    kPropMessageTerrainChanged = 0x278e6003,
    kPropMessageTerrainChanged2 = 0x678e6e3a,
    kPropMessageDay = 0x66956814,
    kPropMessageIdleTime = 0x6695681a,
    kPropMessageAnimationTick = 0x86ad10ee,        // data2 = elapsed milliseconds
};
constexpr std::uint32_t kPropAnimationChannel = 0xc0;      // SL::ATM() notification channel
constexpr int kPropMessageAnimationInterval = 10;          // simulator agent intervals 0x98
constexpr int kPropMessageDayInterval = 4;
constexpr std::uint32_t kPropAgentFlags = 2;

// Occupant types carried by the insert/remove messages.
constexpr std::uint32_t kPropOccupantType = 0xa823821e;         // cSC4PropOccupant
constexpr std::uint32_t kPropNetworkOccupantType = 0xc772bf98;  // a network occupant (signs on network pieces)

// Prop types the developer keeps in the sign list (cISC4PropOccupant::GetPropType).
constexpr std::uint32_t kPropTypeSignpost = 0xab72fbb3;
constexpr std::uint32_t kPropTypeNetworkSign = 0xcb79919b;

// Prop timer bits (cISC4PropOccupant::SetPropTimerState masks). The name differs from PropOccupant.h's
// PropTimerMask because the two headers share the sc4 namespace.
enum PropTimerBit : std::uint32_t {
    kPropTimerBitTimeOfDay = 1,
    kPropTimerBitDate = 2,
    kPropTimerBitEffect = 4,
    kPropTimerBitPower = 8,
};

// Exemplar properties of a prop.
constexpr std::uint32_t kPropPropertyNoShadow = 0x29244db;          // present -> the developer ignores the prop
constexpr std::uint32_t kPropPropertyNightState = 0x49c9c93c;       // u8; 1 -> the state follows the night flag
constexpr std::uint32_t kPropPropertyEffectId = 0x6a3fcc37;         // u32 global effect id
constexpr std::uint32_t kPropPropertyTimeWindow = 0x4a149631;       // float[2] time-of-day window
constexpr std::uint32_t kPropPropertyDatePropType = 0xa751675;      // u32 prop type of the date-range variant
constexpr std::uint32_t kPropPropertyDateMonthDay = 0xca7515cc;     // u8[2] month, day
constexpr std::uint32_t kPropPropertyDateSpan = 0x4a764564;         // u32 span in days (default 1)
constexpr std::uint32_t kPropPropertyRandomChance = 0x4a751ad5;     // u8 percent (default 100)
constexpr std::uint32_t kPropPropertyPowerTimer = 0x6a959e71;       // u8: timer bit 8 follows the power grid
constexpr std::uint32_t kPropPropertyRequester = 0x89a1c16c;        // u32 requester index (written by the developer)
constexpr std::uint32_t kPropPropertyLotState = 0x6a5e0727;         // u8 compared with the lot's state
// Prop properties that make HandleInsertOccupant take the prop into its lists at all.
constexpr std::uint32_t kPropPropertyLotStateValue = 1;             // the value of kPropPropertyNightState wanted

// cRZTime::mRefDate.julianDay as static-initialised in the binary (0x100b73888).
constexpr std::uint32_t kPropTimeReferenceJulianDay = 249522;
constexpr std::uint32_t kSecondsPerDay = 0x15180;
constexpr std::uint32_t kPropSeedMultiplier = 0x1c;
constexpr float kPropMinutesPerHour = 60.0f;
constexpr float kPropCellSizeReciprocal = 0.0625f;    // 1/16: metres -> city cells (0x100a158b0)
constexpr float kPropMinuteThreshold = 0.4f;          // 0x3ecccccd, +0xa84
constexpr float kPropLastTimeOfDayInitial = -100.0f;  // 0xc2c80000, +0xa88
constexpr std::uint32_t kPropAnimationStepSeconds = 200;
constexpr float kPropSignHeightEpsilon = 0.01f;
constexpr std::int32_t kPropTractCells = 8;           // a tract is 8x8 city cells
constexpr std::uint32_t kPropSignVisibleFlag = 2;     // occupant flag ShowSignsAndLabels sets/clears (vslot 0xe8/0xf8)

// cISC4PropOccupant (iid 0xe9793a65) lives in sc4/PropOccupantInterface.h; cSC4PropOccupant implements it.

// ---------------------------------------------------------------------------------------------------------
// PropSlot: the 0x20-byte record UpdateTract builds for one configuration prop slot (list node payload).
// ---------------------------------------------------------------------------------------------------------
struct PropSlot {
    std::uint8_t x = 0;                      // +0x00 city cell x (the original stores a byte; cities <= 128)
    std::uint8_t z = 0;                      // +0x01 city cell z
    std::uint8_t unused[6] = {};             // +0x02..0x07 never read; the original copies uninitialised stack bytes
    std::vector<cSC4LotConfigurationObject>* objects = nullptr;   // +0x08 the config object array
    std::uint32_t objectIndex = 0;           // +0x10 index into `objects`
    cISC4Occupant* occupant = nullptr;       // +0x18 the prop already placed for the slot (a counted reference)
};
static_assert(sizeof(PropSlot) == 0x20, "PropSlot must stay 0x20 bytes");

// ---------------------------------------------------------------------------------------------------------
// Services (one small view per original global; AddRef/Release/Init/Shutdown mirror the COM calls the
// original makes on the stored pointer, like cSC4GrowthDeveloper's IGrowthDeveloperService).
// ---------------------------------------------------------------------------------------------------------
class IPropDeveloperService {
public:
    virtual ~IPropDeveloperService() = default;
    virtual void AddRef() {}       // cIGZUnknown slot 0x08
    virtual void Release() {}      // cIGZUnknown slot 0x10
};

class IPropLotManagerApi : public IPropDeveloperService {
public:
    // lot manager vslot 0x28: the lot whose origin is (x, z); `exactCell` asks for the origin cell only.
    // The returned lot is borrowed (the original does not release it).
    virtual cSC4Lot* GetLot(int x, int z, bool exactCell) = 0;
};

class IPropManagerApi : public IPropDeveloperService {
public:
    virtual bool IsPropFamilyTypeValid(std::uint32_t type) = 0;                                  // vslot 0x40
    virtual int GetPropTypesByFamily(std::vector<std::uint32_t>& out, std::uint32_t family) = 0;  // vslot 0x48
    // vslot 0x68: the prop record of a type. Only +0x10 (valueA, property 0x49a1e05a) is read: it is the
    // satisfaction id of the requester the prop was made for.
    virtual const PropProfile* GetPropProfile(std::uint32_t type) = 0;
    // vslot 0xb8: the props placed in a lot. The list receives one reference per prop.
    virtual int PropsInLot(std::vector<cISC4Occupant*>& out, cSC4Lot* lot) = 0;
    // vslot 0xe8: remove a prop occupant from the city.
    virtual void RemoveProp(cISC4Occupant* occupant) = 0;
    // vslot 0xf0: the props in one cell (network lots). One reference per prop.
    virtual int PropsInCell(std::vector<cISC4Occupant*>& out, int x, int z) = 0;
    // vslot 0xf8: the props inside a cell rectangle (power change). One reference per prop.
    virtual bool PropsInRect(std::vector<cISC4Occupant*>& out, const SC4Rect<int>& rect) = 0;
};

class IPropPowerApi : public IPropDeveloperService {
public:
    virtual bool IsPowered(int x, int z) = 0;    // cSC4PowerSimulator vslot 0x128
};

class IPropNetworkLotManagerApi : public IPropDeveloperService {
public:
    virtual cSC4NetworkLot* GetNetworkLot(int x, int z) = 0;    // vslot 0x28 (borrowed)
    // vslot 0x40: create the prop of a network-lot configuration object; returns a counted cISC4Occupant.
    virtual cISC4Occupant* CreateProp(cSC4NetworkLot* lot, int objectIndex, std::uint32_t propType) = 0;
};

class IPropLotDeveloperApi {
public:
    virtual ~IPropLotDeveloperApi() = default;
    // cSC4LotDeveloper vslot 0x1f8: create the prop of a lot configuration object for a city lot.
    virtual cISC4Occupant* CreateLotProp(cSC4Lot* lot, int objectIndex, std::uint32_t propType) = 0;
};

class IPropBuildingDevelopmentApi : public IPropDeveloperService {
public:
    virtual IPropLotDeveloperApi* GetLotDeveloper() = 0;    // cSC4BuildingDevelopmentSimulator vslot 0x120
};

class IPropSimulatorApi : public IPropDeveloperService {
public:
    virtual cGZDate Date() = 0;                                // vslot 0x30 GetSimDate
    virtual void RemoveAgent(cIGZMessageTarget2* agent) = 0;   // vslot 0xa8
};

class IPropClockApi : public IPropDeveloperService {
public:
    virtual int VisualTimeMode() const = 0;   // vslot 0x30 (0 real, 1 noon, 2 midnight)
    virtual std::uint32_t Hour() const = 0;   // vslot 0x50
    virtual std::uint32_t Minute() const = 0; // vslot 0x68
};

class IPropLightingApi : public IPropDeveloperService {
public:
    virtual bool IsNight() = 0;    // SL::spLightingManager vslot 0x48
};

class IPropView3DApi : public IPropDeveloperService {
public:
    // cISC4View3DWin vslot 0x170 GetGlobalEffectTriggerState(effectId) (iid 0xfa47b3f9).
    virtual bool GlobalEffectTriggered(std::uint32_t effectId) = 0;
};

class IPropTerrainApi : public IPropDeveloperService {
public:
    virtual float Height(float x, float z) = 0;       // terrain vslot 0x60 (returns the altitude)
    virtual float WaterHeight(float x, float z) = 0;  // terrain vslot 0xf8 (the larger of the two is the base)
};

class IPropOccupantManagerApi : public IPropDeveloperService {
public:
    // occupant manager vslot 0xc0: re-place an occupant after its position changed.
    virtual void UpdateOccupant(cISC4Occupant* occupant, bool placed) = 0;
};

class IPropCityApi : public IPropDeveloperService {
public:
    virtual float CellSizeX() = 0;                                             // vslot 0x2d8 (metres)
    virtual float CellSizeZ() = 0;                                             // vslot 0x2e0
    virtual int CellCountX() = 0;                                              // vslot 0x2e8
    virtual int CellCountZ() = 0;                                              // vslot 0x2f0
    virtual void PositionToCell(float x, float z, int& cellX, int& cellZ) = 0; // vslot 0x2f8
};

// Everything outside the object. One method per original call; the global/slot is in the comment.
class IPropDeveloperHost {
public:
    virtual ~IPropDeveloperHost() = default;

    virtual IPropLotManagerApi* LotManager() = 0;                       // SL::spLotManager
    virtual IPropManagerApi* PropManager() = 0;                         // SL::spPropManager
    virtual IPropPowerApi* PowerSimulator() = 0;                        // SL::spPowerSim
    virtual IPropNetworkLotManagerApi* NetworkLotManager() = 0;         // SL::spCity vslot 0x150
    virtual IPropBuildingDevelopmentApi* BuildingDevelopment() = 0;     // SL::spBuildingDevelopmentSim
    virtual IPropSimulatorApi* Simulator() = 0;                         // SL::spSimulator / SL::spCity vslot 0x190
    // lot vslot 0x48 (GetLotConfiguration) -> configuration vslot 0xb8 (GetLotConfigurationObjectArray).
    // The port's cSC4Lot hands out the configuration through the narrow cISC4LotConfigurationRef view, so the
    // host resolves the concrete cSC4LotConfiguration and returns its array (borrowed).
    virtual const std::vector<cSC4LotConfigurationObject>* LotConfigurationObjects(cSC4Lot* lot) = 0;
    // message slot 0x100 (GetIGZUnknown): the lot snapshot a lot-state message carries (borrowed).
    virtual cSC4Lot* MessageLot(cIGZMessage2* message) = 0;
    // SC4WriteGZSerializable / SC4ReadGZSerializable on the three manager/store objects the stream contains
    // (lot manager, prop manager, network lot manager). The read returns one counted reference.
    virtual bool WriteService(cIGZOStream& stream, IPropDeveloperService* service) = 0;
    virtual IPropDeveloperService* ReadService(cIGZIStream& stream, std::uint32_t iid) = 0;
    virtual cIGZMessageServer2* MessageServer() = 0;                    // SL::spMS2
    virtual IPropClockApi* Clock() = 0;                                 // SL::sp24HourClock (null: no clock)
    virtual IPropLightingApi* Lighting() = 0;                           // SL::spLightingManager
    virtual IPropView3DApi* View3D() = 0;                               // SL::spView3DWin QI 0xfa47b3f9 (null: none)
    virtual IPropTerrainApi* Terrain() = 0;                             // SL::spTerrain
    virtual IPropOccupantManagerApi* OccupantManager() = 0;             // SL::spOccupantManager
    virtual IPropCityApi* City() = 0;                                   // SL::spCity
    // SL::ATM() vslot 0x28 / 0x30 (the animation tick source, channel 0xc0).
    virtual void AddTickNotification(cIGZMessageTarget2* target, int channel) = 0;
    virtual void RemoveTickNotification(cIGZMessageTarget2* target, int channel) = 0;
    // simulator vslot 0x98 AddAgent(target, interval, "cSC4PropDeveloper", 2).
    virtual void AddSimulatorAgent(cIGZMessageTarget2* target, int interval, const char* name,
                                   std::uint32_t flags) = 0;
    // The wealth requester Init creates (cSC4WealthRequester takes its host in the port).
    virtual IWealthHost* WealthRequesterHost() = 0;
    // cRZTime::mRefDate.julianDay @ 0x100b73888 (249522 in the shipped binary).
    virtual std::uint32_t TimeReferenceJulianDay() = 0;
    // The generator the wealth requester Init creates uses (cSC4WealthRequester takes a cRZRandom*; the
    // original requester draws from the process-wide generator).
    virtual cRZRandom* RequesterRandom() = 0;
};
IPropDeveloperHost* PropDeveloperHost();
void SetPropDeveloperHost(IPropDeveloperHost* host);

// ---------------------------------------------------------------------------------------------------------
// cISC4PropDeveloper: the primary COM view (17 slots, vtable 0x100a756a0).
// ---------------------------------------------------------------------------------------------------------
class cISC4PropDeveloper : public cIGZUnknown {
public:
    virtual bool Init() = 0;                                                    // [3]  0x18
    virtual bool Shutdown() = 0;                                                // [4]  0x20
    virtual void AddPropRequester(cISC4PropRequester* requester) = 0;           // [5]  0x28
    virtual void RemovePropRequester(cISC4PropRequester* requester) = 0;        // [6]  0x30
    virtual void UpdateAllOptionalProps() = 0;                                  // [7]  0x38
    virtual void ShowSignsAndLabels(bool show) = 0;                             // [8]  0x40
    virtual void ToggleSignsAndLabels() = 0;                                    // [9]  0x48
    virtual bool AreSignsAndLabelsVisible() const = 0;                          // [10] 0x50
    virtual ~cISC4PropDeveloper() = default;                                    // [11]/[12] 0x58/0x60
    virtual bool Write(cIGZOStream& stream) const = 0;                          // [13] 0x68
    virtual bool Read(cIGZIStream& stream) = 0;                                 // [14] 0x70
    virtual std::uint32_t GetGZCLSID() const = 0;                               // [15] 0x78
    virtual bool DoMessage(cIGZMessage2* message) = 0;                          // [16] 0x80
};

// ---------------------------------------------------------------------------------------------------------
// The object.
// ---------------------------------------------------------------------------------------------------------
class cSC4PropDeveloper final : public cISC4PropDeveloper,
                                public cIGZMessageTarget2,
                                public cIGZSerializable {
public:
    cSC4PropDeveloper();
    ~cSC4PropDeveloper() override;

    // cIGZUnknown (one shared count at +0x1c)
    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override;
    std::uint32_t Release() override;

    // cISC4PropDeveloper
    bool Init() override;
    bool Shutdown() override;
    void AddPropRequester(cISC4PropRequester* requester) override;       // @ 0x100122d70
    void RemovePropRequester(cISC4PropRequester* requester) override;    // @ 0x100122dc8
    void UpdateAllOptionalProps() override;                              // @ 0x1001260b2
    void ShowSignsAndLabels(bool show) override;                         // @ 0x10012612a
    void ToggleSignsAndLabels() override;                                // @ 0x100126448
    bool AreSignsAndLabelsVisible() const override { return signsVisible_; }   // @ 0x100126464
    bool Write(cIGZOStream& stream) const override;                      // @ 0x1001222c8
    bool Read(cIGZIStream& stream) override;                             // @ 0x1001225d0
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4PropDeveloper; }   // @ 0x100126470

    // cIGZMessageTarget2 / cISC4PropDeveloper
    bool DoMessage(cIGZMessage2* message) override;                      // @ 0x100122e36

    // ---- the rest of the original, in address order ----
    void DoIdle(std::uint32_t elapsed);                                  // @ 0x1001235a0
    void HandleInsertOccupant(cISC4Occupant* occupant);                  // @ 0x100123632
    void HandleRemoveOccupant(cISC4Occupant* occupant);                  // @ 0x100123edc
    void HandleRemoveNetworkOccupant(cISC4Occupant* occupant);           // @ 0x10012428c
    void DoAnimationTick(std::uint32_t seconds);                         // @ 0x1001243ae
    void SwitchNightChangeProps(bool night);                             // @ 0x1001245e6
    void RemoveOptionalPropsInLot(cSC4Lot* lot);                         // @ 0x10012471c
    void DoPowerChanged(const SC4Rect<int>& rect);                       // @ 0x100124842
    void UpdateLotStateProps(cSC4Lot* lot);                              // @ 0x100124b06
    void UpdateSignpostHeights();                                        // @ 0x100124bfe
    void UpdateTract(int tractX, int tractZ);                            // @ 0x100124d32
    std::uint32_t SelectAndCreateProp(PropSlot* slot, std::uint32_t requesterIndex);   // @ 0x100125d30

    // Accessors the test uses (not part of the original interface).
    bool IsInitialized() const { return initialized_; }
    bool IsNight() const { return night_; }
    int AnimationMode() const { return animationMode_; }
    std::uint32_t AnimationBatch() const { return animationBatch_; }
    std::uint32_t ReferenceCount() const { return refCount_; }
    const std::vector<cISC4PropRequester*>& Requesters() const { return requesters_; }
    const std::vector<cISC4PropOccupant*>& StateProps() const { return stateProps_; }
    const std::list<cISC4PropOccupant*>& TimeProps() const { return timeProps_; }
    const std::list<cISC4PropOccupant*>& DateProps() const { return dateProps_; }
    const std::list<cISC4PropOccupant*>& PowerProps() const { return powerProps_; }
    const std::list<cISC4PropOccupant*>& SignProps() const { return signProps_; }
    const std::unordered_map<std::uint32_t, std::list<cISC4PropOccupant*>>& EffectProps() const {
        return effectProps_;
    }
    const SC4Rect<int>& TractRect() const { return tractRect_; }
    std::int32_t CursorX() const { return cursorX_; }
    std::int32_t CursorZ() const { return cursorZ_; }
    std::uint8_t CursorState() const { return cursorState_; }
    std::uint8_t Phase() const { return phase_; }
    cRZRandomSuper& Random() { return random_; }

private:
    static constexpr std::uint32_t kIntMin = 0x80000000u;

    void PushRequesterVector(std::vector<cISC4PropRequester*>& list, cISC4PropRequester* requester);
    void ReleaseList(std::list<cISC4PropOccupant*>& list, bool clear);
    void ReleaseEffectProps();
    void ApplyTimeOfDay(float time);

    // +0x18
    bool initialized_ = false;
    // +0x1c
    std::uint32_t refCount_ = 0;
    // +0x20
    std::uint8_t phase_ = 0;
    // +0x24
    std::uint8_t cursorState_ = 0;
    std::uint8_t pad25_[3] = {};
    // +0x28
    std::int32_t cursorX_ = 0;
    std::int32_t cursorZ_ = 0;
    std::int32_t step_ = 1;            // +0x30
    // +0x34
    SC4Rect<int> tractRect_{static_cast<std::int32_t>(kIntMin), static_cast<std::int32_t>(kIntMin),
                            static_cast<std::int32_t>(0x7fffffffu), static_cast<std::int32_t>(0x7fffffffu)};
    std::int32_t pad44_ = 0;
    // +0x48
    cRZRandomSuper random_;
    // +0xa18
    bool requesterCreated_ = false;
    std::uint8_t padA19_[3] = {};
    // +0xa20
    std::vector<cISC4PropRequester*> requesters_;
    // +0xa38
    IPropLotManagerApi* lotManager_ = nullptr;
    IPropManagerApi* propManager_ = nullptr;        // +0xa40
    IPropPowerApi* powerSimulator_ = nullptr;       // +0xa48
    IPropNetworkLotManagerApi* networkLotManager_ = nullptr;   // +0xa50
    // +0xa58
    std::vector<cISC4PropOccupant*> stateProps_;
    // +0xa70
    std::uint32_t animationIndex_ = 0;
    std::int32_t animationMode_ = 0;    // +0xa74
    std::uint32_t animationBatch_ = 0;  // +0xa78
    std::uint32_t animationSeconds_ = 0;    // +0xa7c
    bool night_ = false;                // +0xa80
    std::uint8_t padA81_[3] = {};
    float minuteThreshold_ = kPropMinuteThreshold;          // +0xa84
    float lastTimeOfDay_ = kPropLastTimeOfDayInitial;       // +0xa88
    std::uint32_t padA8c_ = 0;
    // +0xa90
    std::unordered_map<std::uint32_t, std::list<cISC4PropOccupant*>> effectProps_;
    // +0xab8
    std::list<cISC4PropOccupant*> dateProps_;
    // +0xad0
    std::list<cISC4PropOccupant*> timeProps_;
    // +0xae8
    std::list<cISC4PropOccupant*> powerProps_;
    // +0xb00
    bool signsVisible_ = true;
    std::uint8_t padB01_[7] = {};
    // +0xb08
    std::list<cISC4PropOccupant*> signProps_;
};

}  // namespace sc4
