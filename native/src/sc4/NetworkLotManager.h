// cSC4NetworkLotManager — the city's network-lot registry (class id 0xe9a6cd3b, interface
// cISC4NetworkLotManager 0x89a5ba3f). 37 symbols at 0x100404dce..0x100409532, three vtables:
// 0x100aacd70 (cISC4NetworkLotManager, 21 slots), 0x100aace28 (cIGZMessageTarget2, 4 slots),
// 0x100aace48 (cIGZSerializable, 6 slots).
//
// A "network lot" is the 4x4 tile record that holds one plopped civic/service building piece (the
// network occupant) plus its network-lot configuration (the template that lists the building, its
// props and its flora). The manager owns:
//   * the lot registry, one record per occupied city cell (+0x18). The key packs the cell as
//     x << 8 | z, so x and z must fit in a byte (the game's cities are at most 128 cells wide),
//   * the configuration table (+0x40), keyed by the network piece id (cSC4NetworkLotConfiguration
//     +0x50) with a vector of configurations per piece (several styles per piece id, chosen by
//     IsLotCompatible),
//   * the configuration forced for the next piece (SetLotConfigurationForNextPiece, +0x68),
//   * the street-pothole visual effects (+0x90). Their cells come from the city traffic network map.
//
// The developers and the network tools both go through it: cSC4GrowthDeveloper asks
// GetNetworkLot/GetZoneTypeForNetwork/GetWealthForNetwork, the network tools place pieces through
// RedevelopLot/SetLotConfigurationForNextPiece, and the zone developer's lot changes arrive as
// messages that mark the cells around the lot dirty (UpdateRoadsInCell).
//
// Original layout (0xe8 bytes, the size GZCOM records for the class):
//   +0x000/0x008/0x010 the three vptrs
//   +0x018 std::unordered_map<uint16 cellKey, cSC4NetworkLot*> lot registry (0x28 bytes)
//   +0x040 std::unordered_map<uint32 pieceId, std::vector<cSC4NetworkLotConfiguration*>> (0x28)
//   +0x068 cSC4NetworkLotConfiguration* next-piece configuration (one reference, or null)
//   +0x070 uint8  city-dimension flag: ((maxCellX|maxCellZ) >> 30) & 2 (2 when a dimension's top bit
//                 is set, i.e. for absurdly large cities); set by Init, read nowhere else
//   +0x074 int    reserved (zeroed by the ctor and Init; read nowhere)
//   +0x078 int    reserved (ditto)
//   +0x07c int    reserved (1 from the ctor, never written again)
//   +0x080 int    reserved (zeroed by Init)
//   +0x084 int    reserved (int32_min from the ctor, zeroed by Init)
//   +0x088 int    maxCellX = CellCountX() - 1 (clamped to 0 for an empty city)
//   +0x08c int    maxCellZ = CellCountZ() - 1 (clamped to 0)
//   +0x090 std::unordered_map<uint16 cellKey, cISC4PropOccupantVisualEffect*> potholes (0x28)
//   +0x0b8 uint32 reference count (shared by all three views; starts at 0 like the other COM objects)
//   +0x0bc bool   initialised (Init/Shutdown guard)
//   +0x0c0 std::unordered_set<uint16 cellKey> cells whose roads must be refreshed (0x28)
//   +0x0e8 end of object
// (The ctor's odd 8-byte stores at +0x7c/+0x84 and the reserved ints are the original's; they are kept
// so Init's writes land at the documented offsets, and asserted by the test.)
//
// Quirks kept on purpose (each is asserted by the test):
//  * AddRef starts the reference count at 0; a Release() on a never-AddRef'd object destroys it and
//    Release calls vtable +0x80 (the deleting destructor) without decrementing.
//  * QueryInterface tests the iid as a *signed* int: `iid < 1` routes iid 0 to the interface-id
//    comparisons, and iid 1 is the primary interface, not cIGZUnknown (0x00000001 is normally "me").
//  * Init only subscribes and measures the city when it was not initialised; the missing-configuration
//    message is formatted with cRZString::Sprintf and then discarded (the original's output call is
//    gone from the binary).
//  * Shutdown releases every network lot but does not clear the registry map (only the destructor
//    destroys it), so the map is left holding dangling pointers; it also releases every configuration
//    and pothole and clears those two maps.
//  * Read only accepts stream version 2 and restores the configuration *id* (lot +0x18), not the
//    configuration pointer; Init reattaches the configuration by id.
//  * Write dereferences the lot's configuration pointer unconditionally in the binary; the port falls
//    back to the id Read stored (see the comment at the call site) so Write-after-Read does not crash.
//  * The lot-state message (0xe8bda2e7) walks the one-cell border around the lot with a loop that
//    starts at cell (0,0) and begins its first row at x = 1, so it always marks cell 0 and never marks
//    the lot's westmost border cell on the first row. Both are kept.
//  * ReadLotConfiguration takes a *second* reference on the configuration it fills (it is handed a
//    fresh object that already owns one) and releases it before returning; the caller's AddRef +
//    AddLotConfiguration reference keep the object alive.
//  * GetWealthForNetwork's mode breaks ties toward the higher wealth and needs a wealth to count at least as many lots
//    as the lots without one.
//  * GetWealthForNetwork only looks at diagonal neighbours when no orthogonal neighbour had a lot, and
//    GetZoneTypeForNetwork only when the best zone so far is 0, 7 or 14 (0x4081).
//  * A version-1 configuration is written back by ReadLotConfiguration's upgrade path and then appended
//    again by AddLotConfigurationStatic, so the piece table holds the same configuration twice.
//  * UpdateRoadStyle's "has flag 0xd" disjunct can never be reached: 0xd shares bit 2 with the
//    0x40280304 skip mask the same function tests first, so a dirt-road piece returns early.
//  * Shutdown releases every lot but leaves the registry populated, so a second Init (which reattaches
//    configurations by id) dereferences freed lots; the game destroys the object after Shutdown.
//
// Everything outside the object is reached through INetworkLotManagerHost, one method per original
// vtable call (the class and slot are in the comment), the same seam style as cSC4NetworkManager and
// cSC4ZoneDeveloper. The two value records the registry stores (cSC4NetworkLot and
// cSC4NetworkLotConfiguration) and the exemplar helpers (cSCPropertyHelp, cSC4LotConfiguration's
// static converters) are used directly because those classes are recovered.
//
// Not recovered here (see the PARTIAL section of notes/ledger_pending/cSC4NetworkLotManager.txt):
// nothing is left partial — every one of the 37 symbols is recovered; the hook-ups that remain are the
// host seam implementations listed by docs/SEAMS.md.
#pragma once
#include <cstddef>
#include <cstdint>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gz/cGZPersistResourceKey.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZSerializable.h"
#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "rz/cRZRandomSuper.h"
#include "sc4/LotConfiguration.h"
#include "sc4/PropOccupantVisualEffectIds.h"
#include "sc4/SCProperty.h"
#include "sc4/TileNeighborhood.h"

namespace sc4 {

constexpr std::int32_t kInt32Min = static_cast<std::int32_t>(0x80000000u);

constexpr std::uint32_t kGZIID_cISC4NetworkLotManager = 0x89a5ba3f;
constexpr std::uint32_t kGZCLSID_cSC4NetworkLotManager = 0xe9a6cd3b;
constexpr std::uint32_t kGZIID_cISC4NetworkOccupant = 0xa821ef94;
constexpr std::uint32_t kGZIID_cISC4LotNetworkLot = 0xe818fced;   // cISC4Lot (sc4/Lot.h)
constexpr std::uint32_t kGZIID_cIGZMessageTarget2NetworkLot = 0x452294aa;
constexpr std::uint32_t kGZIID_cIGZSerializableNetworkLot = 0xe4fda3d4;

// Init subscribes to these six messages (slots 0x28/0x30 of SL::spMS2), in this order.
constexpr std::uint32_t kNetworkLotManagerMessages[6] = {
    0x89d3e9f3,   // network occupant usable changed (payload occupant, data1 = usable)
    0x99ef1143,   // occupant removed from the occupant manager
    0xa9b6c2d6,   // re-create a lot's objects (payload = cISC4NetworkLot*)
    0x07699455,   // rectangle of cells changed (data2 = const int[4] x0,z0,x1,z1)
    0xe8bda2e7,   // lot state changed (payload QI 0xe818fced = cISC4Lot)
    0x69247dc8,   // pothole changed (data1 = x, data2 = z, data3 = damaged)
};
constexpr std::uint32_t kNetworkLotOccupantFlagMask = 0x1f4f;    // every "is a network piece" flag
constexpr std::uint32_t kNetworkLotBusyFlagMask = 0x80200000;    // hides the piece from road refresh
constexpr std::uint32_t kNetworkLotAvenueFlag = 0x40;            // the avenue style refresh flag
constexpr std::uint32_t kNetworkLotNoStyleFlagMask = 0x40280304; // pieces the style pass skips
constexpr std::uint32_t kNetworkLotRailFlag = 0x200000;          // excluded from the removal handler
constexpr std::uint32_t kNetworkLotStyleTwoFlag = 0x2;           // has a "style 2" piece
constexpr std::uint32_t kNetworkLotStyleDRoadFlag = 0xd;         // has a dirt-road piece
constexpr std::uint32_t kNetworkLotUnderTextureFlag = 0x6000000;
constexpr std::uint32_t kNetworkLotPotholeRailFlag = 0x2;        // GetNetworkFlag() & 0x1fff

constexpr std::uint32_t kNetworkAvenueNetworkType = 6;           // eNetworkType::kNetworkAvenue

// Exemplar ids and properties of a network-lot configuration (exemplar type 0x21 of class 0xa52160f5).
constexpr std::uint32_t kNetworkLotConfigExemplarClass = 0xa52160f5;
constexpr std::uint32_t kNetworkLotConfigExemplarType = 0x21;
constexpr std::uint32_t kNetworkLotConfigExemplarTypeProperty = 0x10;
constexpr std::uint32_t kNetworkLotConfigKeyType = 0x6534284a;    // exemplar resource type
constexpr std::uint32_t kNetworkLotConfigKeyGroup = 0x89ac5643;   // group of the network-lot configs
constexpr std::uint32_t kNetworkLotConfigName = 0x20;
constexpr std::uint32_t kNetworkLotConfigId = 0x21;
constexpr std::uint32_t kNetworkLotConfigVersion = 0x88edc789;
constexpr std::uint32_t kNetworkLotConfigPieceId = 0xc9a5a1be;
constexpr std::uint32_t kNetworkLotConfigZoneTypes = 0x88edc793;
constexpr std::uint32_t kNetworkLotConfigWealthTypes = 0x88edc795;
constexpr std::uint32_t kNetworkLotConfigNeighborhood = 0x49d55951;   // four bytes, the 4x4 tile mask
constexpr std::uint32_t kNetworkLotConfigSize = 0xca81b8d4;
constexpr std::uint32_t kNetworkLotConfigMinSlope = 0xaa120972;
constexpr std::uint32_t kNetworkLotConfigMaxSlope = 0xaa120973;
constexpr std::uint32_t kNetworkLotConfigRotationMask = 0xec3bd470;
constexpr std::uint32_t kNetworkLotConfigFlipMode = 0xcc3e4755;

// kFlippedRotation @ 0x100a203d0, indexed [object orientation * 4 + occupant rotation].
constexpr int kNetworkLotFlippedRotation[16] = {0, 3, 2, 1, 3, 2, 1, 0, 2, 1, 0, 3, 1, 0, 3, 2};
// 0x100a203c0: the object-position bounds CorrectObjectPositions clamps against (16, 16, 0, 0).
constexpr float kNetworkLotObjectBounds[4] = {16.0f, 16.0f, 0.0f, 0.0f};

// 0x100a20410 / 0x100a20430: the eight neighbour offsets (four edges, then four diagonals), shared by
// GetZoneTypeForNetwork, GetWealthForNetwork and UpdateAvenueStyle. The diagonals are read past the
// four-entry edge tables in the binary, which is why the port declares all eight.
constexpr int kNetworkLotDeltaX[8] = {-1, 0, 1, 0, -1, -1, 1, 1};
constexpr int kNetworkLotDeltaZ[8] = {0, -1, 0, 1, -1, 1, -1, 1};
// 0x100a20450: zone priority ranks (lower wins) for GetZoneTypeForNetwork.
constexpr int kNetworkLotZonePriority[16] = {18, 4, 7, 6, 5, 4, 3, 8, 2, 1, 16, 16, 16, 16, 17, 16};
// 0x100a20490: the under-texture id per zone * 4 + wealth.
constexpr std::uint32_t kNetworkLotUnderTextureId[64] = {
    0x0000000u, 0x08100000u, 0x08200000u, 0x08300000u,   // zone 0
    0x08200000u, 0x08100000u, 0x08200000u, 0x08300000u,  // 1
    0x08200000u, 0x08100000u, 0x08200000u, 0x08300000u,  // 2
    0x08200000u, 0x08100000u, 0x08200000u, 0x08300000u,  // 3
    0x08200000u, 0x08100000u, 0x08200000u, 0x08300000u,  // 4
    0x08200000u, 0x08100000u, 0x08200000u, 0x08300000u,  // 5
    0x08200000u, 0x08100000u, 0x08200000u, 0x08300000u,  // 6
    0x08400000u, 0x08400000u, 0x08400000u, 0x08400000u,  // 7
    0x08200000u, 0x08100000u, 0x08200000u, 0x08200000u,  // 8
    0x08200000u, 0x08100000u, 0x08200000u, 0x08200000u,  // 9
    0x08200000u, 0x08100000u, 0x08200000u, 0x08300000u,  // 10
    0x08200000u, 0x08100000u, 0x08200000u, 0x08300000u,  // 11
    0x08200000u, 0x08100000u, 0x08200000u, 0x08300000u,  // 12
    0x08200000u, 0x08100000u, 0x08200000u, 0x08300000u,  // 13
    0x08400000u, 0x08400000u, 0x08400000u, 0x08400000u,  // 14
    0x08200000u, 0x08100000u, 0x08200000u, 0x08300000u,  // 15
};
// 0x100a20590: the piece variation per zone * 3 + (wealth ? wealth - 1 : 0).
constexpr std::uint8_t kNetworkLotVariation[48] = {
    0, 0, 0,  1, 2, 3,  4, 5, 6,  4, 5, 6,  1, 2, 3,  4, 5, 6,  4, 5, 6,  7, 7, 7,
    1, 1, 2,  1, 1, 2,  1, 1, 1,  1, 1, 1,  1, 1, 1,  1, 1, 1,  7, 7, 7,  1, 1, 1,
};
// 0x100a205c0: the pothole position nudge per GetActiveEdges() mask, in quarter cells.
constexpr std::int8_t kNetworkLotActiveEdgeOffset[16][2] = {
    {0, 0}, {-1, 0}, {0, -1}, {-1, -1}, {1, 0}, {0, 0}, {1, -1}, {0, 0},
    {0, 1}, {-1, 1}, {0, 0}, {0, 0}, {1, 1}, {0, 0}, {0, 0}, {0, 0},
};
constexpr float kNetworkLotQuarterCell = 4.0f;   // 0x100a161fc

// ---- the host seams -------------------------------------------------------------------------------
class INetworkLotOccupantObject;   // cISC4Occupant
class INetworkLotNetworkOccupant;  // cISC4NetworkOccupant
class INetworkLotOccupantFilter;   // cISC4OccupantFilter views (cSC4NetworkORFilter)
class INetworkLotDBSegment;        // cIGZPersistDBSegment
class INetworkLotResourceEnumerator;   // the resource key enumerator

// cISC4Occupant, the two position slots this class uses. Byte offsets are the interface vtable slots.
class INetworkLotOccupantObject : public cIGZUnknown {
public:
    virtual void GetPosition(float xyz[3]) = 0;        // 0x40
    virtual void SetPosition(const float xyz[3]) = 0;  // 0x48
};

// cISC4NetworkOccupant (iid 0xa821ef94); the concrete table is 0x100a7bd08.
class INetworkLotNetworkOccupant : public cIGZUnknown {
public:
    virtual bool HasNetworkFlag(std::uint32_t flags) = 0;        // 0x40
    virtual bool HasAnyNetworkFlag(std::uint32_t flags) = 0;     // 0x48
    virtual void SetNetworkFlag(std::uint32_t flags) = 0;        // 0x50
    virtual std::uint32_t GetNetworkFlag() = 0;                  // 0x60
    virtual bool IsUsable() = 0;                                 // 0x98
    virtual std::uint32_t PieceId() = 0;                         // 0xb8
    virtual std::uint8_t GetRotation() = 0;                      // 0xc0
    virtual bool GetFlip() = 0;                                  // 0xc8
    virtual void SetVariation(std::uint8_t variation) = 0;       // 0xd8
    virtual void SetUnderTexture(std::uint32_t texture) = 0;     // 0xe8
    virtual std::uint32_t GetEdgeTypesFlag(std::uint32_t networkType) = 0;  // 0x130
    virtual std::uint32_t GetActiveEdges() = 0;                  // 0x140
    virtual INetworkLotOccupantObject* AsOccupant() = 0;         // 0x180
    virtual bool IsIntersection() = 0;                           // 0x1b8
    virtual float GetAltitude(float x, float z) = 0;             // 0x1c0
    virtual void GetOccupiedCell(std::uint32_t& x, std::uint32_t& z) = 0;  // 0x1d8
};

// cISC4PropOccupant / the flora occupant as this class tags them (cISC4Occupant slots).
class INetworkLotPropOccupant : public cIGZUnknown {
public:
    virtual void SetLotTag(std::uint32_t tag) = 0;   // 0xe0
    virtual void SetFlag(std::uint32_t flag) = 0;    // 0xe8
};

// cISC4PropOccupantVisualEffect (iid 0x09820792, class 0x2977aa49); primary table 0x100a77228.
class INetworkLotVisualEffect : public cIGZUnknown {
public:
    virtual INetworkLotOccupantObject* AsOccupant() = 0;   // 0x18
    virtual void SetEffectType(const char* name) = 0;      // 0x28
    virtual void StartEffect(bool instant) = 0;            // 0x38
    virtual void StopEffect(bool instant) = 0;             // 0x40
};

// cISC4Lot (iid 0xe818fced) as the lot-state message and GetWealthForNetwork use it.
class INetworkLot : public cIGZUnknown {
public:
    virtual void GetBoundingRect(std::int32_t rect[4]) = 0;   // 0x70
    virtual bool IsInLivableState() = 0;                      // 0x128
    virtual std::uint32_t GetOccupantWealth() = 0;            // 0x148
    virtual std::uint32_t GetBuildingType(bool flag) = 0;     // 0x170
};

// cISC4OccupantFilter: only the reference count is used (the filter is handed to the occupant manager).
class INetworkLotOccupantFilter {
public:
    virtual ~INetworkLotOccupantFilter() = default;
    virtual void AddRef() = 0;     // cIGZUnknown slot 0x08
    virtual void Release() = 0;    // slot 0x10
};

// cIGZPersistDBSegment: WriteLotConfiguration keeps one reference on it while it runs.
class INetworkLotDBSegment {
public:
    virtual ~INetworkLotDBSegment() = default;
    virtual void AddRef() = 0;     // 0x08
    virtual void Release() = 0;    // 0x10
};

// What LoadAllConfigurations does with the resource manager's enumerator (spRM slot 0x90): the callback
// receives every resource key of the type, the enumerator is released afterwards.
class INetworkLotResourceEnumerator {
public:
    virtual ~INetworkLotResourceEnumerator() = default;
    virtual void Enumerate(void (*callback)(const cGZPersistResourceKey&, void*), void* context) = 0;  // 0x38
    virtual void Release() = 0;                                                                        // 0x10
};

// Everything outside the object. One method per original call; the class and vtable slot are in the
// comment. A null return means the game global was null.
class INetworkLotManagerHost {
public:
    virtual ~INetworkLotManagerHost() = default;

    virtual cIGZMessageServer2* MessageServer() = 0;                       // SL::spMS2 (AddNotification 0x28 / RemoveNotification 0x30)
    virtual int CityCellCountX() = 0;                                     // city 0x2e8
    virtual int CityCellCountZ() = 0;                                     // city 0x2f0
    virtual void CityPositionToCell(float x, float z, int& cellX, int& cellZ) = 0;      // city 0x2f8
    virtual void CityCellCenterToPosition(int cellX, int cellZ, float& x, float& z) = 0;  // city 0x308
    virtual float TerrainAltitude(float x, float z) = 0;                  // terrain 0x60
    // zone manager 0x28 (GetZoneGrid) -> grid 0xc0 -> data[x][z] (the raw grid access of the binary)
    virtual int ZoneTypeAt(int x, int z) = 0;
    virtual INetworkLot* LotAt(int x, int z, bool exactCell) = 0;         // lot manager 0x28 (borrowed)
    // traffic network map 0x50 FindNetworkOccupant(x, z, flags, includeUnusable) -> borrowed occupant
    virtual INetworkLotNetworkOccupant* FindNetworkOccupant(int x, int z, std::uint32_t flags,
                                                            bool includeUnusable) = 0;
    virtual void UpdateIntersection(INetworkLotNetworkOccupant* occupant) = 0;   // traffic map 0x120
    // occupant manager 0x108 GetFirstOccupant(x, z, filter) -> the occupant view (borrowed, one
    // reference; the binary's extra QI to 0xa821ef94 is folded into this return).
    virtual INetworkLotNetworkOccupant* GetFirstNetworkOccupant(int x, int z,
                                                                INetworkLotOccupantFilter* filter) = 0;
    virtual void InsertOccupant(INetworkLotOccupantObject* occupant) = 0;             // occupant manager 0xa0
    virtual void RemoveOccupant(INetworkLotOccupantObject* occupant, bool arg, std::uint32_t reason) = 0;  // 0xa8
    // cSC4NetworkORFilter: a fresh filter (reference count 0, like the original constructor); the class
    // AddRefs it and releases it once.
    virtual INetworkLotOccupantFilter* CreateNetworkOrFilter(std::uint32_t flags) = 0;
    // cSC4PropManager 0x100 AddStreetProp: the original returns the created occupant (the recovered
    // bool-returning port API drops it), so the pointer comes through the seam.
    virtual INetworkLotPropOccupant* AddStreetProp(std::uint32_t type, const float* position, int orientation) = 0;
    virtual void RemoveStreetPropsFromTile(int x, int z) = 0;             // prop manager 0x108
    // cSC4FloraSimulator 0x90 AddNewFloraOccupant / 0xa0 DemolishFloraInCellArea (neither recovered).
    virtual INetworkLotPropOccupant* AddFlora(std::uint32_t type, const float* position, bool arg) = 0;
    virtual void DemolishFloraInCellArea(const std::int32_t rect[4], int arg) = 0;
    // cSC4LotConfigurationManager 0xd8 GetObjectIDArray(object, out)
    virtual void GetObjectIDArray(const cSC4LotConfigurationObject* object, std::vector<std::uint32_t>& out) = 0;
    // cSC4LotDeveloper 0xe8 GetAreaSlope(rect, includeWater)
    virtual float GetAreaSlope(const std::int32_t rect[4], bool includeWater) = 0;
    // cSC4BuildingDevelopmentSimulator 0x140 GetBuildingProfilePtr(type) -> profile +0x4 (the wealth)
    virtual std::uint32_t GetBuildingWealth(std::uint32_t buildingType) = 0;
    // GZCOM()->0x18(0x2977aa49, 0x09820792, &out): a fresh visual-effect occupant, reference count 0.
    virtual INetworkLotVisualEffect* CreateVisualEffect() = 0;
    virtual bool HasResourceManager() = 0;                                // SL::spRM != null
    virtual INetworkLotResourceEnumerator* GetExemplarEnumerator(std::uint32_t type) = 0;   // spRM 0x90
    // spRM 0x18(key, 0xa52160f5, &exemplar, 0, 0) + exemplar 0x30 (its property holder). The holder is
    // returned with one reference; ReleaseExemplar drops it (exemplar 0x10).
    virtual cISCPropertyHolder* AcquireExemplar(const cGZPersistResourceKey& key, std::uint32_t exemplarType) = 0;
    virtual void ReleaseExemplar(cISCPropertyHolder* holder) = 0;
    virtual INetworkLotDBSegment* GetConfigDBSegment(const cGZPersistResourceKey& key) = 0;   // spRM 0xf8(key, &segment)
    // holder QI 0x456b8f1d -> resource 0x20(key) -> spRM 0x68(resource, key, segment) -> release.
    virtual bool StoreExemplar(cISCPropertyHolder* holder, const cGZPersistResourceKey& key,
                               INetworkLotDBSegment* segment) = 0;
    virtual bool WriteGZSerializable(cIGZOStream& stream, const cIGZUnknown* object) = 0;   // 0x1000435e3
    virtual bool ReadGZSerializable(cIGZIStream& stream, std::uint32_t iid, void** out) = 0;  // 0x100043267

    // PORT: the message objects' views. The original QueryInterfaces the message's object for cISC4NetworkOccupant
    // (0xa821ef94) or cISC4Lot (0xe818fced) and calls their tables directly; the port's network pieces and lots answer
    // neither (other seams read those ids), so a host may hand out its own views. One reference each, or null.
    virtual INetworkLotNetworkOccupant* NetworkOccupantOf(cIGZUnknown* object) {
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kGZIID_cISC4NetworkOccupant, &raw)) return nullptr;
        return static_cast<INetworkLotNetworkOccupant*>(raw);
    }
    virtual INetworkLot* LotOf(cIGZUnknown* object) {
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kGZIID_cISC4LotNetworkLot, &raw)) return nullptr;
        return static_cast<INetworkLot*>(raw);
    }
};

void SetNetworkLotManagerHost(INetworkLotManagerHost* host);
INetworkLotManagerHost* NetworkLotManagerHost();

// The process-wide sRandom (cRZRandomSuper) the configuration pick and the object pick use.
cRZRandomSuper& NetworkLotManagerRandom();

// cISC4NetworkLotManager: the primary interface (vtable 0x100aacd70). Only the slots this class
// implements are declared, in the original order; slots 15/16 are the destructors.
class cISC4NetworkLotManager : public cIGZUnknown {
public:
    virtual bool Init() = 0;                                                       // [3]  0x018
    virtual bool Shutdown() = 0;                                                   // [4]  0x020
    virtual cSC4NetworkLot* GetNetworkLot(int x, int z) const = 0;                 // [5]  0x028
    virtual void GetNetworkLotConfigurationSet(std::set<cSC4NetworkLotConfiguration*>& out) const = 0;  // [6] 0x030
    virtual bool GetNetworkLotConfigurationById(std::uint32_t id) const = 0;       // [7]  0x038
    virtual INetworkLotPropOccupant* CreateProp(cSC4NetworkLot* lot, std::uint32_t objectIndex,
                                                std::uint32_t propType) = 0;       // [8]  0x040
    virtual std::uint32_t GetZoneTypeForNetwork(int x, int z) = 0;                 // [9]  0x048
    virtual std::uint32_t GetWealthForNetwork(int x, int z) = 0;                   // [10] 0x050
    virtual bool IterateAllNetworkLots(bool (*callback)(std::uint32_t, std::uint32_t, cSC4NetworkLot*, void*),
                                       void* context) = 0;                         // [11] 0x058
    virtual void RedevelopLot(std::uint32_t x, std::uint32_t z, cSC4NetworkLot* lot) = 0;  // [12] 0x060
    virtual void SetLotConfigurationForNextPiece(cSC4NetworkLotConfiguration* configuration) = 0;  // [13] 0x068
    virtual bool WriteLotConfiguration(cSC4NetworkLotConfiguration* configuration,
                                       INetworkLotDBSegment* segment) = 0;         // [14] 0x070
    virtual ~cISC4NetworkLotManager() = default;                                   // [15]/[16] 0x078/0x080
    // [17] Write, [18] Read, [19] GetGZCLSID are cIGZSerializable's; [20] 0x0a0 DoMessage is
    // cIGZMessageTarget2's.
};

class cSC4NetworkLotManager final : public cISC4NetworkLotManager,
                                    public cIGZMessageTarget2,
                                    public cIGZSerializable {
public:
    cSC4NetworkLotManager();                       // 0x100404dce
    ~cSC4NetworkLotManager() override;             // 0x100404ebc

    // ---- cIGZUnknown (one shared reference count at +0xb8) ----
    bool QueryInterface(std::uint32_t iid, void** out) override;   // 0x100405072
    std::uint32_t AddRef() override;                                // 0x100404fbc
    std::uint32_t Release() override;                               // 0x100404ff8

    // ---- cIGZSerializable ----
    bool Write(cIGZOStream& stream) const override;                 // 0x10040555a
    bool Read(cIGZIStream& stream) override;                        // 0x10040562a
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4NetworkLotManager; }   // 0x100409526

    // ---- cIGZMessageTarget2 ----
    bool DoMessage(cIGZMessage2* message) override;                 // 0x100406428

    // ---- cISC4NetworkLotManager ----
    bool Init() override;                                           // 0x1004050d2
    bool Shutdown() override;                                       // 0x100405422
    cSC4NetworkLot* GetNetworkLot(int x, int z) const override;     // 0x100407f2c
    void GetNetworkLotConfigurationSet(std::set<cSC4NetworkLotConfiguration*>& out) const override;  // 0x1004062a6
    bool GetNetworkLotConfigurationById(std::uint32_t id) const override;   // 0x100407f60
    INetworkLotPropOccupant* CreateProp(cSC4NetworkLot* lot, std::uint32_t objectIndex,
                                        std::uint32_t propType) override;   // 0x100407e66
    std::uint32_t GetZoneTypeForNetwork(int x, int z) override;     // 0x10040800c
    std::uint32_t GetWealthForNetwork(int x, int z) override;       // 0x100408130
    bool IterateAllNetworkLots(bool (*callback)(std::uint32_t, std::uint32_t, cSC4NetworkLot*, void*),
                               void* context) override;             // 0x1004063de
    void RedevelopLot(std::uint32_t x, std::uint32_t z, cSC4NetworkLot* lot) override;   // 0x100407df4
    void SetLotConfigurationForNextPiece(cSC4NetworkLotConfiguration* configuration) override;   // 0x100407fbe
    bool WriteLotConfiguration(cSC4NetworkLotConfiguration* configuration,
                               INetworkLotDBSegment* segment) override;   // 0x100405d3e

    // ---- non-virtual entry points of the original ----
    void LoadAllConfigurations();                                          // 0x1004053c8
    static void AddLotConfigurationStatic(const cGZPersistResourceKey& key, void* context);   // 0x100405868
    bool ReadLotConfiguration(cISCPropertyHolder* holder,
                              cSC4NetworkLotConfiguration* configuration);  // 0x100405946
    void AddLotConfiguration(cSC4NetworkLotConfiguration* configuration);   // 0x100405c50
    void UpdateRoadsInCell(int x, int z);                                   // 0x100406d10
    void DoNetworkOccupantRemoved(INetworkLotNetworkOccupant* occupant);    // 0x100406de6
    void CreateLotObjects(cSC4NetworkLot* lot);                             // 0x100406ee8
    void ProcessUpdateList();                                               // 0x100407106
    void DoRoadDamageChange(int x, int z, bool damaged);                    // 0x100407166
    void UpdateRoadStyle(int x, int z, INetworkLotNetworkOccupant* occupant);   // 0x1004074f4
    void UpdateLot(INetworkLotNetworkOccupant* occupant, int x, int z, std::uint32_t zoneType,
                   std::uint32_t wealth);                                   // 0x100407640
    bool IsLotCompatible(cSC4NetworkLotConfiguration* configuration, int x, int z, std::uint32_t zoneType,
                         std::uint32_t wealth, std::uint8_t rotation, bool flipped);   // 0x100407750
    void DoNetworkOccupantInserted(INetworkLotNetworkOccupant* occupant, std::uint32_t zoneType,
                                   std::uint32_t wealth);                   // 0x10040784e
    void GetPositionForLotConfigurationObject(cSC4NetworkLot* lot, cSC4LotConfigurationObject* object,
                                              float position[3], int& rotation);   // 0x100407c02
    void UpdateAvenueStyle(int x, int z);                                   // 0x100408330

    // The original field offsets, asserted by the test (the reserved words are part of the layout).
    static constexpr std::uint32_t kOffsetLots = 0x18;
    static constexpr std::uint32_t kOffsetConfigurations = 0x40;
    static constexpr std::uint32_t kOffsetNextConfiguration = 0x68;
    static constexpr std::uint32_t kOffsetDimensionFlag = 0x70;
    static constexpr std::uint32_t kOffsetReserved74 = 0x74;
    static constexpr std::uint32_t kOffsetMaxCellX = 0x88;
    static constexpr std::uint32_t kOffsetMaxCellZ = 0x8c;
    static constexpr std::uint32_t kOffsetPotholes = 0x90;
    static constexpr std::uint32_t kOffsetRefCount = 0xb8;
    static constexpr std::uint32_t kOffsetInited = 0xbc;
    static constexpr std::uint32_t kOffsetUpdateCells = 0xc0;
    static constexpr std::uint32_t kClassSize = 0xe8;

    // scripts/tests/sc4_networklotmanager_test.cpp reaches the private state through this.
    friend struct NetworkLotManagerTestAccess;

private:
    void InsertUpdateCell(std::uint16_t key);

    // Declared in the original's order so every offset below is the binary's (the test asserts them).
    std::unordered_map<std::uint16_t, cSC4NetworkLot*> networkLots_;    // +0x18
    std::unordered_map<std::uint32_t, std::vector<cSC4NetworkLotConfiguration*>> configurations_;  // +0x40
    cSC4NetworkLotConfiguration* nextPieceConfiguration_ = nullptr;     // +0x68
    std::uint8_t dimensionFlag_ = 0;                   // +0x70
    std::int32_t reserved74_ = 0;                      // +0x74
    std::int32_t reserved78_ = 0;                      // +0x78
    std::int32_t reserved7c_ = 1;                      // +0x7c
    std::int32_t reserved80_ = kInt32Min;              // +0x80
    std::int32_t reserved84_ = kInt32Min;              // +0x84
    std::int32_t maxCellX_ = 0x7fffffff;               // +0x88
    std::int32_t maxCellZ_ = 0x7fffffff;               // +0x8c
    std::unordered_map<std::uint16_t, INetworkLotVisualEffect*> potholes_;   // +0x90
    std::uint32_t refCount_ = 0;                       // +0xb8
    bool inited_ = false;                              // +0xbc
    std::unordered_set<std::uint16_t> updateCells_;    // +0xc0
};

// The cell key the registry and the pothole map use: x << 8 | z (the x in the high byte).
constexpr std::uint16_t NetworkLotCellKey(int x, int z) {
    return static_cast<std::uint16_t>((static_cast<std::uint32_t>(x) << 8) |
                                      (static_cast<std::uint32_t>(z) & 0xffu));
}

}  // namespace sc4
