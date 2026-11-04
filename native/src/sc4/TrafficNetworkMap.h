// cSC4TrafficNetworkMap — the traffic network data map (class id 0x6a0f82b2, interface
// cISC4TrafficNetworkMap 0xca0f81b9). 97 symbols at 0x100298a0e..0x1002a052c, object size 0xb08.
//
// The map owns one TileInfo chain per city cell (occupant, per-lane volume/congestion counters, live
// automata lists and the per-edge occupancy values the data views read), the intersection arrays
// (stoplight phases, train junctions) and the map of roads that became unusable.
//
// Original layout (offsets in the 0xb08-byte object):
//   +0x00 / +0x08 / +0x10 / +0x18  four COM sub-object vptrs (map, message target 2, message target,
//                                  serializable); their refcounts are +0x24, +0x1c, +0x14, +0xc
//   +0x20  bool initialized (Init guard)          +0x28 TileInfo* grid (cellsX * cellsZ pointers)
//   +0x30  cellsX (a power of two)                +0x34 cellsZ
//   +0x38  u8 log2(cellsX)                        +0x39 u8 tunnel paths enabled
//   +0x40  unordered_map<cISC4NetworkOccupant*, IntersectionArray>
//   +0x68  bool intersection map dirty            +0x70 unordered_map<..., TrainJunctionArray>
//   +0xa0  bool tile occupancy dirty              +0xa4 SC4Rect visible cells
//   +0xb4  float camera position [x, y, z]        +0xc0 cRZRandomSuper
//   +0xa90 view utilities (spViewUtilities)       +0xa98 occupant manager (spOccupantManager)
//   +0xaa0 traffic-network occupant filter        +0xaa8 prop/stoplight occupant filter
//   +0xab0 bool camera scroll active              +0xab4 / +0xab8 tile cursor x / z
//   +0xabc float stoplight phase A                +0xac0 bool stoplight group A
//   +0xac4 float stoplight phase B                +0xac8 bool stoplight group B
//   +0xad0 unordered_map<u32 cell, vector<cISC4NetworkOccupant*>> unusable roads (key x0 << 16 | z0)
//   +0xaf8 / +0xafc garbage-collector cursor x / z  +0xb00 float garbage-collector timer
//
// TileInfo (0x90 bytes, one per occupant per cell, chained through +0x88):
//   +0x00 occupant (counted)                      +0x08 vector<vector<LaneTickEntry>>
//   +0x20 vector<FrequencyEntry>                  +0x38 std::list<cSC4TrafficAutomaton*> (+0x48 size)
//   +0x50 / +0x54 / +0x58 road / building / occupancy bit masks (from CheckCellOccupancy)
//   +0x5c u16[4] occupancy per edge               +0x64 u16[4][4] occupancy distribution
//   +0x88 next TileInfo
// LaneTickEntry (0x10): +0 key (lane | edge << 8 | path type << 0x18), +4 lanes tick, +8 autos/second,
//   +0xc automata count. FrequencyEntry (0xc): +0 key, +4 autos/second, +8 automata count.
//
// IntersectionArray (0x90, map value):
//   +0x00 IntersectionInfo approaches[4] (0x14 each: +0 type 0..3, +4 green flag, +5 about-to-switch flag,
//         +8 current-state duration, +0xc other-state duration, +0x10 time left)
//   +0x50 u8 unused, +0x58 vector<IStoplightProp*>, +0x70 std::list automata (trains), +0x80 list size,
//   +0x88 bool dirty.
// TrainJunctionArray (0x80): four {vector<u32> keys (0x18), u16 cursor, pad} blocks, one per path type.
//
// Everything that reaches an unrecovered neighbour goes through ITrafficNetworkMapHost (city, occupant
// manager, building-development, terrain, view, message bus) and the occupant seams
// ITrafficNetworkMapOccupant / ITrafficNetworkMapProp / ITrafficNetworkMapAutomaton.
//
// Quirks kept: the log2(cellsX) probe starts at 1, so cellsX == 1 leaves the shift at 32; the path-type
// index clamps every type outside 1..12 to slot 0; GetTileInfoByNetwork walks the chain for a pointer
// equality; TickSingleIntersection writes a zero automaton list count after a switch.
#pragma once
#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

#include "gz/StreamOps.h"
#include "gz/cGZMessageServer.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZSerializable.h"
#include "rz/cRZRandomSuper.h"
#include "sc4/CommonIIDs.h"
#include "sc4/MiscTypes.h"
#include "sc4/Occupant.h"

// the window/message struct of gz/WinInterfaces.h (an elaborated `class cGZMessage` inside namespace sc4 used to declare a
// second, unrelated sc4::cGZMessage that broke every header that later named the real one)
struct cGZMessage;

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4TrafficNetworkMap = 0x6a0f82b2;
constexpr std::uint32_t kGZIID_cISC4TrafficNetworkMap = 0xca0f81b9;
constexpr std::uint32_t kGZIID_MessageTarget2 = 0x452294aa;
constexpr std::uint32_t kTrafficNetworkFlagTunnel = 0x00080000;
constexpr std::uint32_t kTrafficNetworkFlagUsable = 0x10000000;

// Messages the map subscribes to (Init / Shutdown: cIGZMessageServer2 slot 0x28 / 0x30).
constexpr std::uint32_t kTrafficMessageNetworkUsableChanged = 0x89d3e9f3;   // 0x-762c160d
constexpr std::uint32_t kTrafficMessageTileOccupancyDirty = 0x69247dc7;     // 0x69247dc7
constexpr std::uint32_t kTrafficMessageScrollStart = 0xa6b79602;            // 0x-594869fe
constexpr std::uint32_t kTrafficMessageScrollEnd = 0xa6b79612;              // 0x-594869ee
constexpr std::uint32_t kTrafficMessageScrollActive = 0xcad61e00;           // 0x-3529e200
constexpr std::uint32_t kTrafficMessageScrollInactive = 0xead61e16;         // 0x-1529e1ea
constexpr std::uint32_t kTrafficMessageIntersectionsChanged = 0x26d31ec1;
constexpr std::uint32_t kTrafficMessageAnimationTick = 0x86ad10ee;          // 0x-7952ef12
constexpr std::uint32_t kTrafficMessageOccupantInserted = 0x99ef1142;       // 0x-6610eebe
constexpr std::uint32_t kTrafficMessageOccupantRemoved = 0x99ef1143;        // 0x-6610eebd

// Network edge order used by the map: 0 west, 1 north, 2 east, 3 south (kEdgeDeltas at 0x100a1bfd0).
constexpr int kTrafficEdgeDeltaX[4] = {-1, 0, 1, 0};
constexpr int kTrafficEdgeDeltaZ[4] = {0, -1, 0, 1};

// Building-mask tables (0x100a1c068 / 0x100a1c080 / 0x100a1c0a0).
constexpr std::uint32_t kTrafficFootprintMaskZ[3] = {1, 2, 4};
constexpr std::uint32_t kTrafficFootprintMaskX[8] = {1, 2, 4, 8, 16, 32, 64, 128};
constexpr std::uint32_t kTrafficBuildingTypeMask[8] = {1, 2, 2, 2, 4, 4, 4, 4};

// cISC4PathInfo::tPathType (values 0..3 are the road/rail/pedestrian path types the map uses).
enum TrafficPathType : std::uint32_t { kPathTypeAny = 0, kPathTypeCar = 1, kPathTypePedestrian = 2, kPathTypeTrain = 3 };

// ---- occupant seams -------------------------------------------------------------------------------
class ITrafficNetworkMapPathInfo;

// cISC4NetworkOccupant (iid 0xa821ef94) calls the map makes. Slot numbers are byte offsets into the
// interface vtable (concrete table 0x100a7bd08).
class ITrafficNetworkMapOccupant : public cIGZUnknown {
public:
    virtual bool HasNetworkFlag(std::uint32_t flags) const = 0;                           // 0x40
    virtual bool HasAnyNetworkFlag(std::uint32_t flags) const = 0;                        // 0x48
    virtual void SetNetworkFlag(std::uint32_t flags) = 0;                                 // 0x50
    virtual void ClearNetworkFlag(std::uint32_t flags) = 0;                               // 0x58
    virtual bool ConnectsToNeighbor(int edge, std::uint32_t networkType) const = 0;        // 0x178
    virtual cISC4Occupant* AsOccupant() = 0;                                              // 0x180
    virtual ITrafficNetworkMapPathInfo* GetPathInfo() = 0;                                // 0x188
    virtual void ReleasePathInfo() = 0;                                                   // 0x190
    virtual bool PathConnectionExists(std::uint32_t pathType, std::uint8_t entry, std::uint8_t exit) const = 0;  // 0x1a8
    virtual bool IsIntersection() const = 0;                                              // 0x1b8
    virtual float GetAltitude(float x, float z) const = 0;                                // 0x1c0
    virtual void GetOccupiedCell(std::uint32_t& x, std::uint32_t& z) const = 0;           // 0x1d8
    virtual bool IsOfType(std::uint32_t networkType) const = 0;                           // 0xb0
    virtual bool IsUsable() const = 0;                                                    // 0x98
};

// cISC4Lot calls (slots 0x70 / 0x128 / 0x148 / 0x1c0 of the recovered cSC4Lot).
class ITrafficNetworkMapLot {
public:
    virtual ~ITrafficNetworkMapLot() = default;
    virtual void GetBoundingRect(std::int32_t rect[4]) = 0;              // 0x70
    virtual bool IsLivable() = 0;                                       // 0x128
    virtual std::uint8_t OccupantWealth() = 0;                          // 0x148
    virtual std::uint16_t TravelPopulation(std::uint32_t developerType) = 0;   // 0x1c0
};

// cISC4BuildingOccupant::GetBuildingProfile (slot 0x58): only the first two words are read.
class ITrafficNetworkMapBuilding {
public:
    virtual ~ITrafficNetworkMapBuilding() = default;
    virtual std::uint32_t Purpose() = 0;   // BuildingProfile +0
    virtual std::uint32_t Wealth() = 0;    // BuildingProfile +4
};

// cISC4PathInfo (0xa9667793) calls the map makes (slots 0x68 / 0xa0 / 0x108 / 0x130 / 0x100).
class ITrafficNetworkMapPathInfo {
public:
    virtual ~ITrafficNetworkMapPathInfo() = default;
    virtual bool IsIntersection(std::uint32_t pathType) = 0;   // 0x68
    virtual int SnapPointToAnyPath(cS3DVector3& point, std::uint32_t pathType, std::uint32_t* key) = 0;   // 0xa0
    virtual int GetStopPointCount(std::uint32_t pathType, std::uint8_t entry, std::uint8_t exit, std::uint8_t lane) = 0;   // 0x108
    virtual bool GetJunctionKeys(std::uint32_t pathType, std::uint8_t entry, std::vector<std::uint32_t>& keys) = 0;   // 0x130
    virtual void MakeTunnelPaths(ITrafficNetworkMapOccupant* from, ITrafficNetworkMapOccupant* to) = 0;   // 0x100
};

// cISC4NetworkTunnelOccupant (iid 0x8a4bd52b) — only the far end is read.
class ITrafficNetworkMapTunnel : public cIGZUnknown {
public:
    virtual ITrafficNetworkMapOccupant* GetOtherEndOccupant() = 0;   // cSC4NetworkTunnelOccupant::GetOtherEndOccupant
};

// cISC4PropOccupant (iid 0xe9793a65) as used by the stoplight logic: slots 0x18 / 0x30 / 0x48 / 0x58 / 0x60.
class ITrafficNetworkMapProp : public cIGZUnknown {
public:
    virtual bool IsOccupantGroup(std::uint32_t group) = 0;   // AsOccupant() slot 0xb0
    virtual bool HasProperty(std::uint32_t propertyId) = 0;  // AsPropertyHolder() slot 0x18
    virtual int GetState() = 0;                              // 0x60: 0..3 stoplight state
    virtual void SetState(int state) = 0;                    // 0x58
    virtual void GetPosition(float* xyz) = 0;                // AsOccupant() slot 0x40
    virtual int GetOrientation() = 0;                        // 0x48
    virtual bool IsFlagSet(std::uint32_t flag) = 0;          // AsOccupant() slot 0x100
};

// cSC4TrafficAutomaton — the parts of the vehicle the map stores (slots 0x2e0 / 0x478 and the +0x308 word).
class ITrafficNetworkMapAutomaton {
public:
    virtual ~ITrafficNetworkMapAutomaton() = default;
    virtual std::uint32_t AddRef() = 0;
    virtual std::uint32_t Release() = 0;
    virtual bool IsRemoved() = 0;                     // 0x2e0
    virtual std::uint32_t GetLane() = 0;              // 0x478
    virtual std::uint32_t PathTypeWord() const = 0;   // +0x308
};

// ---- tuning ---------------------------------------------------------------------------------------
// The values the original keeps in cSC4AutomataManager::sTuningParams (0x100b21880, filled from the
// simulator exemplar, property 0x4998c89f). The exemplar data is not in the checkout, so the defaults
// below are placeholders; the game's ReadTuningParameters fills them.
struct TrafficNetworkMapTuning {
    float junctionDuration = 6.0f;        // +0x124 (0x100b219a4)
    float junctionTimeLeft = 3.0f;        // +0x128 (0x100b219a8)
    float stoplightPeriodA = 10.0f;       // +0x12c (0x100b219ac)
    float stoplightPeriodB = 10.0f;       // +0x130 (0x100b219b0)
    float unused134 = 0.0f;               // +0x134 (0x100b219b4)
    double occupancyJitterLow = 0.9;      // +0x140 (0x100b219c0)
    double occupancyJitterHigh = 1.1;     // +0x144 (0x100b219c4)
    std::uint8_t neighborDivisor = 4;     // +0x148 (0x100b219c8)
    std::uint32_t footprintMaskA = 0xffffffffu;  // +0x14c (0x100b219cc)
    std::uint32_t footprintMaskB = 0xffffffffu;  // +0x150 (0x100b219d0)
};

// ---- host seam ------------------------------------------------------------------------------------
class ITrafficNetworkMapHost {
public:
    virtual ~ITrafficNetworkMapHost() = default;
    // SL::spCity
    virtual int CityCellsX() = 0;                                               // slot 0x2e8
    virtual int CityCellsZ() = 0;                                               // slot 0x2f0
    virtual void PositionToCell(float x, float z, int& cellX, int& cellZ) = 0;  // slot 0x2f8
    virtual bool SimulationActive() = 0;   // city slots 0x158 / 0x160, each slot 0x30
    // lot manager (city slot 0x108) -> 0x28(x, z, 0): the lot at the cell, borrowed
    virtual ITrafficNetworkMapLot* LotAtCell(int cellX, int cellZ) = 0;
    // lot manager -> 0x70(occupant): the lot that owns the occupant, borrowed
    virtual ITrafficNetworkMapLot* LotOf(cISC4Occupant* occupant) = 0;
    // occupant manager filtered query (city slot 0x108 -> 0x128, release via 0x180); counted references
    virtual std::vector<cISC4Occupant*> NetworkOccupantsInBox(const float boxMin[3], const float boxMax[3]) = 0;
    virtual void ReleaseOccupants(const std::vector<cISC4Occupant*>& occupants) = 0;
    // occupant manager slot 0x148 with the stoplight-prop filter: props at the cell, counted references
    virtual std::vector<ITrafficNetworkMapProp*> PropsAtCell(int cellX, int cellZ) = 0;
    // terrain (city slots 0x310 / 0x2a0 -> 0x60)
    virtual bool HasTerrain() = 0;
    virtual float TerrainHeightAt(float x, float z) = 0;
    // lot slot 0x180: the lot's building occupant, borrowed; null when the lot has none
    virtual ITrafficNetworkMapBuilding* LotBuilding(ITrafficNetworkMapLot* lot) = 0;
    // building development simulator slot 0xf0(purpose, wealth): active edge mask of the building type
    virtual int ActiveEdgesForBuilding(std::uint32_t purpose, std::uint8_t wealth) = 0;
    // view utilities: camera position (+0x3f4) and look-at (+0x3fc)
    virtual void GetViewPosition(float& x, float& y, float& z) = 0;
    virtual void GetViewTarget(float& x, float& y, float& z) = 0;
    // message bus (SL::spMS2 slots 0x28 / 0x30)
    virtual void SubscribeMessage(std::uint32_t message) = 0;
    virtual void UnsubscribeMessage(std::uint32_t message) = 0;
    // cISC4PathInfo::MakeTunnelPaths (slot 0x100)
    virtual void MakeTunnelPaths(ITrafficNetworkMapPathInfo* pathInfo, ITrafficNetworkMapOccupant* from,
                                 ITrafficNetworkMapOccupant* to) = 0;
    // Port seam: the map's view of a network occupant. The default is the original's QueryInterface 0xa821ef94; the game's
    // host overrides it because the port's network occupants do not answer that iid (NetworkManager.h). One reference, as
    // the QueryInterface gives; null for anything that is not a network piece.
    virtual ITrafficNetworkMapOccupant* AsMapOccupant(cISC4Occupant* occupant);
};
// The host a map made without one uses (the GZCOM creator passes none).
ITrafficNetworkMapHost* DefaultTrafficNetworkMapHost();
void SetDefaultTrafficNetworkMapHost(ITrafficNetworkMapHost* host);

// ---- data -----------------------------------------------------------------------------------------
struct TrafficLaneTick {
    std::uint32_t key = 0;   // +0: lane (bits 0..7) | entry edge (8..15) | path type (24..27)
    std::uint32_t laneTick = 0;      // +4
    std::uint32_t autosPerSecond = 0;   // +8
    std::uint32_t automataCount = 0;    // +0xc
};
struct TrafficFrequency {
    std::uint32_t key = 0;   // +0
    std::uint32_t automataCount = 0;   // +4
    std::uint32_t autosPerSecond = 0;  // +8
};
struct TrafficTileInfo {
    ITrafficNetworkMapOccupant* occupant = nullptr;             // +0x00 counted
    std::vector<std::vector<TrafficLaneTick>> laneTicks;         // +0x08
    std::vector<TrafficFrequency> frequencies;                  // +0x20
    std::list<ITrafficNetworkMapAutomaton*> automata;           // +0x38 (+0x48 size)
    std::uint32_t roadMask = 0;                                 // +0x50
    std::uint32_t occupancyMask = 0;                            // +0x54
    std::uint32_t buildingMask = 0;                             // +0x58
    std::uint16_t edgeOccupancy[4] = {};                        // +0x5c
    std::uint16_t distribution[4][4] = {};                      // +0x64
    std::uint32_t pad84 = 0;                                    // +0x84
    TrafficTileInfo* next = nullptr;                            // +0x88
};

struct TrafficIntersectionInfo {
    int type = 0;             // +0: 0 none, 1 junction, 2 stoplight, 3 train junction
    std::uint8_t green = 0;   // +4
    std::uint8_t nearSwitch = 0;   // +5
    std::uint16_t pad = 0;    // +6
    float stateDuration = 0;  // +8
    float otherDuration = 0;  // +0xc
    float timeLeft = 0;       // +0x10
};
struct TrafficIntersectionArray {
    TrafficIntersectionInfo approaches[4];                    // +0x00
    std::uint8_t flag50 = 0;                                  // +0x50
    std::vector<ITrafficNetworkMapProp*> props;                // +0x58
    std::list<ITrafficNetworkMapAutomaton*> automata;          // +0x70 (+0x80 size)
    std::uint8_t dirty = 0;                                   // +0x88
};
struct TrafficTrainJunctionArray {
    std::vector<std::uint32_t> keys[4];   // +0x00, +0x20, +0x40, +0x60
    std::uint16_t cursor[4] = {};          // +0x18, +0x38, +0x58, +0x78
};

// The map's own interface (class id 0x6a0f82b2, iid 0xca0f81b9). Only the COM identity is recovered;
// the concrete methods are called directly, so the interface carries no extra slots.
class cISC4TrafficNetworkMap : public cIGZUnknown {
public:
    virtual ~cISC4TrafficNetworkMap() = default;
};

class cSC4TrafficNetworkMap : public cISC4TrafficNetworkMap,
                              public cIGZMessageTarget,
                              public cIGZMessageTarget2,
                              public cIGZSerializable {
public:
    explicit cSC4TrafficNetworkMap(ITrafficNetworkMapHost* host, const TrafficNetworkMapTuning* tuning = nullptr);
    ~cSC4TrafficNetworkMap() override;

    // ---- object life cycle (0x100298a0e .. 0x100299312) ----
    bool Init();                                   // 0x100298c72
    bool Shutdown();                               // 0x100299006
    void InitializeTunnelPaths();                  // 0x100298f2a
    void ClearNetworkTileInfo();                   // 0x10029917e
    void ClearIntersectionMap();                   // 0x100299218
    void ClearTrainJunctionMap();                  // 0x100299288
    void ClearUnusableRoadsMap();                  // 0x1002992c2

    // ---- COM surface (the original has four sub-objects; one refcount here) ----
    bool QueryInterface(std::uint32_t iid, void** out) override;   // 0x1002a034c
    std::uint32_t AddRef() override;                                // 0x1002a03c0
    std::uint32_t Release() override;                               // 0x1002a03f8
    std::uint32_t RefCount() const { return refCount_; }
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4TrafficNetworkMap; }   // 0x1002a051a
    bool DoMessage(cIGZMessage2* message) override;                 // 0x100299312 (message target 2)

    // ---- tile storage and accessors (0x10029a228 .. 0x10029dc54) ----
    bool HandleInsertNetworkOccupant(ITrafficNetworkMapOccupant* occupant);   // 0x10029a228
    bool HandleRemoveNetworkOccupant(ITrafficNetworkMapOccupant* occupant);   // 0x10029b202
    bool HandleNetworkOccupantUsable(ITrafficNetworkMapOccupant* occupant, bool unusable);   // 0x10029a5ea
    TrafficTileInfo* GetTileInfoAtCell(int cellX, int cellZ) const;           // 0x1002a04ea
    TrafficTileInfo* SafeGetTileInfoAtCell(int cellX, int cellZ) const;       // 0x10029d7a6
    TrafficTileInfo* GetTileInfoByNetwork(ITrafficNetworkMapOccupant* occupant) const;   // 0x10029d9b0
    TrafficTileInfo* GetTileInfoAtNeighborEdge(ITrafficNetworkMapOccupant* occupant, std::uint8_t edge,
                                                bool secondDirection, std::uint32_t pathType) const;   // 0x10029d7ca
    ITrafficNetworkMapOccupant* FindNetworkOccupant(int cellX, int cellZ) const;   // 0x10029da12
    ITrafficNetworkMapOccupant* FindNetworkOccupant(int cellX, int cellZ, float y0, float y1) const;   // 0x10029da2a
    ITrafficNetworkMapOccupant* FindNetworkOccupant(const float* position) const;   // 0x10029da42
    ITrafficNetworkMapOccupant* FindNetworkOccupant(int cellX, int cellZ, std::uint32_t flags, bool unusable) const;   // 0x10029dad6
    ITrafficNetworkMapOccupant* FindNetworkOccupant(const float* position, std::uint32_t flags, bool unusable) const;   // 0x10029db98
    TrafficTileInfo* FindNetworkTile(int cellX, int cellZ, float y0, float y1) const;   // 0x10029dbd0
    bool ConnectionExists(int cellX, int cellZ, std::uint8_t entry, std::uint8_t exit, std::uint32_t pathType) const;   // 0x10029d70a
    static bool ConnectionExists(ITrafficNetworkMapOccupant* occupant, std::uint8_t entry, std::uint8_t exit,
                                 std::uint32_t pathType);   // 0x10029d77c
    void ReleaseTileInfo(TrafficTileInfo* tile);   // 0x10029b414
    void RemoveUnusableNetworkOccupant(ITrafficNetworkMapOccupant* occupant, const SC4RectInt32* rect);   // 0x10029a790
    SC4RectInt32 GetVisibleCellRect() const { return visible_; }   // 0x1002a0502
    const std::unordered_map<ITrafficNetworkMapOccupant*, TrafficIntersectionArray>& GetIntersectionMap() const {
        return intersections_;   // 0x1002a0510
    }
    std::size_t TileCount() const;   // test helper: live TileInfo nodes

    // ---- volume / congestion accumulation (0x10029ae0 .. 0x10029e5f0) ----
    void UpdateRoadNeighborInfo(ITrafficNetworkMapOccupant* occupant, TrafficTileInfo* tile);   // 0x100299ae0
    void UpdateNetworkOccupancyInfo(ITrafficNetworkMapOccupant* occupant, TrafficTileInfo* tile);   // 0x100299c4c
    bool CheckCellOccupancy(int cellX, int cellZ, std::uint8_t edge, TrafficTileInfo* tile);   // 0x10029cc22
    void WalkNeighborOccupancy(ITrafficNetworkMapOccupant* occupant, std::uint8_t edge, std::uint8_t depth,
                               std::uint16_t& occupancy);   // 0x10029cdbc
    std::uint32_t* GetLaneTick(ITrafficNetworkMapOccupant* occupant, std::uint32_t layer, std::uint32_t pathType,
                               std::uint8_t a, std::uint8_t b);   // 0x10029dc54
    std::uint32_t* GetAutosPerSecond(ITrafficNetworkMapOccupant* occupant, std::uint32_t layer, std::uint32_t pathType,
                                      std::uint8_t a, std::uint8_t b);   // 0x10029de12
    std::uint32_t* GetAutomataCount(ITrafficNetworkMapOccupant* occupant, std::uint32_t layer, std::uint32_t pathType,
                                    std::uint8_t a, std::uint8_t b);   // 0x10029df44
    std::uint32_t* GetAutomataCount(ITrafficNetworkMapOccupant* occupant, std::uint32_t layer, std::uint32_t key);   // 0x10029df60
    std::uint32_t GetAutomataCountForLane(ITrafficNetworkMapOccupant* occupant, std::uint32_t lane);   // 0x10029e082
    std::uint32_t* GetAutomataCount(ITrafficNetworkMapOccupant* occupant, std::uint32_t pathType);   // 0x10029e104
    std::uint32_t* GetAutosPerSecond(ITrafficNetworkMapOccupant* occupant, std::uint32_t pathType);   // 0x10029e1a8
    int ChooseWeightedEdge(const float* weights, std::uint8_t* edges);   // 0x10029e24c
    bool EnterNetwork(ITrafficNetworkMapAutomaton* automaton, ITrafficNetworkMapOccupant* occupant);   // 0x10029e74a
    bool ExitNetwork(ITrafficNetworkMapAutomaton* automaton, ITrafficNetworkMapOccupant* occupant, std::uint32_t lane);   // 0x10029e862

    // ---- intersections / train junctions (0x10029979c .. 0x10029cb16) ----
    void UpdateIntersectionProps(ITrafficNetworkMapOccupant* occupant, TrafficIntersectionArray& intersections, bool flag);   // 0x10029979c
    void InitializeIntersection(ITrafficNetworkMapOccupant* occupant, TrafficIntersectionArray& intersections);   // 0x10029a186
    void InitializeIntersectionEdge(ITrafficNetworkMapOccupant* occupant, ITrafficNetworkMapOccupant* neighbor,
                                    TrafficIntersectionArray& intersections, TrafficIntersectionInfo& info,
                                    std::uint8_t edge);   // 0x10029c42a
    void AddIntersection(ITrafficNetworkMapOccupant* occupant);   // 0x10029a872
    void RemoveIntersection(ITrafficNetworkMapOccupant* occupant);   // 0x10029b31a
    void AddTrainJunction(ITrafficNetworkMapOccupant* occupant);   // 0x10029ad1e
    void RemoveTrainJunction(ITrafficNetworkMapOccupant* occupant);   // 0x10029b3ca
    bool IsIntersection(ITrafficNetworkMapOccupant* occupant);   // 0x10029c2a0
    int CheckIntersection(ITrafficNetworkMapOccupant* occupant, ITrafficNetworkMapAutomaton* automaton,
                          std::uint32_t edge, float& time);   // 0x10029c2cc
    void UpdateIntersection(ITrafficNetworkMapOccupant* occupant);   // 0x10029c256
    void InitIntersectionProps(ITrafficNetworkMapOccupant* occupant, TrafficIntersectionArray& intersections);   // 0x10029b628
    void UpdateIntersectionMap();   // 0x10029b9e0
    void TickIntersections(float milliseconds);   // 0x10029b7dc
    void TickSingleIntersection(ITrafficNetworkMapOccupant* occupant, TrafficIntersectionArray& intersections, float seconds);   // 0x10029ba20
    void UpdateIntersectionTiming(ITrafficNetworkMapOccupant* occupant, TrafficIntersectionArray& intersections);   // 0x10029baaa (partial)
    void SwitchStoplightStates(ITrafficNetworkMapProp* prop);   // 0x10029cb16
    void MarkTrainInCell(ITrafficNetworkMapAutomaton* automaton, int cellX, int cellZ);   // 0x10029c786
    void UnmarkTrainInCell(ITrafficNetworkMapAutomaton* automaton, int cellX, int cellZ);   // 0x10029c8e2
    std::uint32_t GetTrainJunctionPath(ITrafficNetworkMapOccupant* occupant, std::uint8_t pathType) const;   // 0x10029c9d2
    std::size_t GetTrainJunctionKeys(ITrafficNetworkMapOccupant* occupant, std::uint8_t pathType,
                                     std::vector<std::uint32_t>& keys, std::uint16_t& cursor) const;   // 0x10029ca1e
    bool ToggleTrainJunction(ITrafficNetworkMapOccupant* occupant);   // 0x10029cab0

    // ---- geometry and surrounding-road queries (0x10029cf42 .. 0x10029e684) ----
    bool ConstrainPointToNetworkTile(cS3DVector3& point, std::uint32_t flags) const;   // 0x10029cf42
    bool ConstrainVectorToNetworkTile(const cS3DVector3& from, cS3DVector3& point, std::uint32_t flags) const;   // 0x10029d174
    float GetRoadAltitude(float x, float z) const;   // 0x10029d538
    bool GetSurroundingRoads(std::vector<ITrafficNetworkMapOccupant*>& out, const SC4RectInt32& rect, int margin,
                             int flag, std::uint32_t networkFlags);   // 0x10029e40e
    bool GetSurroundingRoads(std::vector<ITrafficNetworkMapOccupant*>& out, cISC4Occupant* occupant, int margin,
                             int flag, std::uint32_t networkFlags);   // 0x10029e684

    // ---- ticks and messages (0x100299322 .. 0x100299f86) ----
    bool DoMessage(::cGZMessage& message) override;                          // 0x100299312 (message target)
    bool DoMessage2(std::uint32_t messageId, std::uint32_t dataA, float dataB); // 0x100299322 (partial)
    void DoAnimationTick(std::uint32_t frame, float milliseconds);             // 0x100299982
    void GarbageCollectPathInfo();                                             // 0x100299dc0
    bool HandleViewChanged();                                                  // 0x100299f86
    bool HandlePostCityInit();                                                 // 0x10029b464
    bool HandleTuningParametersChanged();                                      // 0x10029b7ae

    // ---- persistence (0x10029e972 / 0x10029eeaa) ----
    bool Write(cIGZOStream& stream) const override;   // partial
    bool Read(cIGZIStream& stream) override;          // partial

    // ---- accessors ----
    int CellsX() const { return cellsX_; }
    int CellsZ() const { return cellsZ_; }
    bool Initialized() const { return initialized_; }
    bool TunnelPathsEnabled() const { return tunnelPaths_; }
    void SetTunnelPathsEnabled(bool on) { tunnelPaths_ = on; }
    const SC4RectInt32& VisibleRect() const { return visible_; }
    float StoplightPhaseA() const { return stoplightPhaseA_; }
    float StoplightPhaseB() const { return stoplightPhaseB_; }
    bool& StoplightGroupA() { return stoplightGroupA_; }
    bool& StoplightGroupB() { return stoplightGroupB_; }
    const TrafficNetworkMapTuning& Tuning() const { return *tuning_; }
    ITrafficNetworkMapHost* Host() const { return host_; }
    const std::unordered_map<ITrafficNetworkMapOccupant*, TrafficTrainJunctionArray>& TrainJunctions() const {
        return trainJunctions_;
    }
    const std::unordered_map<std::uint32_t, std::vector<ITrafficNetworkMapOccupant*>>& UnusableRoads() const {
        return unusableRoads_;
    }

private:
    TrafficTileInfo* TileAtGrid(int cellX, int cellZ) const;
    TrafficTileInfo* FindOrCreateTile(ITrafficNetworkMapOccupant* occupant, int cellX, int cellZ);
    void UnlinkTile(ITrafficNetworkMapOccupant* occupant, int cellX, int cellZ);
    TrafficIntersectionArray* FindIntersection(ITrafficNetworkMapOccupant* occupant);
    const TrafficIntersectionArray* FindIntersection(ITrafficNetworkMapOccupant* occupant) const;
    TrafficTrainJunctionArray* FindTrainJunction(ITrafficNetworkMapOccupant* occupant);
    const TrafficTrainJunctionArray* FindTrainJunction(ITrafficNetworkMapOccupant* occupant) const;
    static std::size_t PathTypeIndex(std::uint32_t pathType);
    static std::uint16_t FloatToUShort(float value);
    static void AddOccupantRef(ITrafficNetworkMapOccupant* occupant);
    static void ReleaseOccupant(ITrafficNetworkMapOccupant* occupant);

    ITrafficNetworkMapHost* host_;
    const TrafficNetworkMapTuning* tuning_;
    TrafficNetworkMapTuning ownedTuning_;
    std::uint32_t refCount_ = 1;                 // +0x24
    bool initialized_ = false;                   // +0x20
    int cellsX_ = 0, cellsZ_ = 0;                // +0x30 / +0x34
    int shift_ = 1;                              // +0x38
    bool tunnelPaths_ = false;                   // +0x39
    std::vector<TrafficTileInfo*> grid_;         // +0x28
    std::unordered_map<ITrafficNetworkMapOccupant*, TrafficIntersectionArray> intersections_;   // +0x40
    bool intersectionsDirty_ = false;            // +0x68
    std::unordered_map<ITrafficNetworkMapOccupant*, TrafficTrainJunctionArray> trainJunctions_;   // +0x70
    bool tileOccupancyDirty_ = false;            // +0xa0
    SC4RectInt32 visible_{};                     // +0xa4
    float camera_[3] = {};                       // +0xb4
    cRZRandomSuper random_{0xffffffffu};         // +0xc0
    bool scrollActive_ = false;                  // +0xab0
    int cursorX_ = 0, cursorZ_ = 0;              // +0xab4 / +0xab8
    float stoplightPhaseA_ = 0;                  // +0xabc
    bool stoplightGroupA_ = true;                // +0xac0
    float stoplightPhaseB_ = 0;                  // +0xac4
    bool stoplightGroupB_ = true;                // +0xac8
    std::unordered_map<std::uint32_t, std::vector<ITrafficNetworkMapOccupant*>> unusableRoads_;   // +0xad0
    int gcX_ = -1, gcZ_ = -1;                    // +0xaf8 / +0xafc
    float gcTimer_ = 0;                          // +0xb00
};

}  // namespace sc4
