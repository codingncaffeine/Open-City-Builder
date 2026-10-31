// cSC4NetworkTool — the road / rail / highway / pipe / subway / power-line drawing tool: it turns a mouse drag
// into a list of cells (`ComputeDraggedCells`), breaks the drag into straight and diagonal segments, solves the
// tile set (edge subtypes, intersections, bridges, tunnels, vertex heights), inserts the occupants and keeps the
// preview in sync. 196 symbols at 0x1001862bc..0x10071ef55, vtable 0x100a7a918 (+0x0 = cISC4NetworkTool, 83
// slots; +0x8 = cIGZMessageTarget2, 4 slots).
//
// This file recovers the object itself: construction / destruction, the init & shutdown handshake with the
// engine globals, the state accessors the input controls and the UI poll, the failure reason, the anchor /
// placement-parameter copies, the bridge & neighbour-connection queries, the network-type flag helpers, the
// padded-rect helper, the terrain highlight of the valid part of the drag and the straight/diagonal splitter
// (`BreakIntoStraightAndDiagSegments`).  The solving pipeline (SolveNetwork, ApplyNetworkRules, bridges, tunnels,
// highway intersections), the occupant insertion and the preview/occupant drawing are still unrecovered — every
// such function is `recovered-partial` in notes/ledger.csv (applied from notes/ledger_applied/cSC4NetworkTool.txt)
// and named in the vtable-order comment blocks below so the gaps are visible.
//
// Object layout (0x638 bytes through the last stored pointer; offsets are the original ones):
//   +0x000 vtable cISC4NetworkTool        +0x008 vtable cIGZMessageTarget2
//   +0x010 std::vector<uint32_t>          +0x028 AddRef/Release count (starts at 0)
//   +0x02c bool inited (Init guard)       +0x030 cSC4NetworkWorldCache (0x50 B)
//   +0x080 cSTETerrain* (counted)         +0x088 cISC4OccupantManager* (counted)
//   +0x090 city view model* (counted)     +0x098 bool  +0x09a bool (1)  +0x09b bool forceReload
//   +0x0a0 std::vector<tDraggedStep> (12-byte steps)        +0x0b8 std::vector<SC4Point<u32>> cells
//   +0x0d0 bool stepIsHorizontal           +0x0d4 totalSteps
//   +0x0d8 straightSteps                   +0x0dc validSteps   +0x0e0/+0x0e4 int (segment split output)
//   +0x0e8 unordered_set<u32>              +0x110 std::vector<tSolvedCell> (12-byte solved tiles)
//   +0x128 unordered_set<u32>              +0x150 unordered_set<u32> (the fixed-vertex ids)
//   +0x178 std::vector<tIntersection> (0x38)              +0x190 std::vector<tBridge> (0x2c)
//   +0x1a8 std::vector<u32>                +0x1c0 std::vector<tReservedCell> (0x14)
//   +0x1d8 uint32 (renderer altitude from message 0xa6b79602)  +0x1e0 std::vector<u32>
//   +0x1f8 std::vector<tNeighborConnection> (0x15+)       +0x210 std::vector<SC4Point<u32>> anchors
//   +0x228 cSC4VertexHtConstraintSatisfier                +0x328 cSC4VertexHtConstraintSatisfier
//   +0x428 uint32 networkFlags (~kFlagForNetworkType[type] & 0x1f4f)
//   +0x430 cSC4NetworkPreview* (counted)  +0x438 std::vector<int> possibleBridges
//   +0x450 int preferredBridge (-1)       +0x454 float bridgeAltitudeOverride (-1.0f)
//   +0x458 int (1)                        +0x45c int failureReason
//   +0x460 cRZCellMap*                    +0x468 std::set<cRZAutoRefCount<cISC4Occupant>> (highlighted)
//   +0x480 (unused)                       +0x488 std::set<cRZAutoRefCount<cISC4Occupant>> (preview)
//   +0x4a0 bool previewUpdateEnabled (1)  +0x4a4 float cellSize  +0x4a8 float slopeRise
//   +0x4ac int cityWidth  +0x4b0 int cityHeight  +0x4b4 int width+1  +0x4b8 int height+1
//   +0x4bc int[16] initScratch (Init writes width-derived rect/padding values; see the quirk note)
//   +0x500 cISC4NetworkTool::tPlacementParams (0xe8 B, copied from the network type info at Init)
//   +0x5e8 std::vector<int> waterBridgeTypes             +0x600 std::vector<int> landBridgeTypes
//   +0x618 bool hasBridgeTypes            +0x620 const cSC4NetworkTypeInfo* (one 0x180-byte record)
//   +0x628 uint32 networkType (ctor argument)             +0x62c uint32 placementFlagMask
//   +0x630 cISC4NetworkORFilter* (counted)
#pragma once
#include "sc4/CommonIIDs.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "gz/cIGZMessage2.h"
#include "gz/cIGZUnknown.h"
#include "gz/cIGZString.h"
#include "gz/cIGZUnknown.h"
#include "rz/cRZString.h"

namespace sc4 {

class cISC4NetworkToolUI;

// SC4Point<unsigned int>: two u32 at +0x0 (x) and +0x4 (z). The engine's template also has AddClamped etc.,
// none of which the recovered tool functions call.
template <typename T>
struct SC4PointT {
    T x{};
    T z{};
    bool operator==(const SC4PointT& o) const { return x == o.x && z == o.z; }
    bool operator<(const SC4PointT& o) const { return x != o.x ? x < o.x : z < o.z; }
};
using SC4PointU = SC4PointT<std::uint32_t>;

// SC4Rect<unsigned int>: x0, z0, x1, z1 (inclusive), written by GetPaddedRectAroundCell.
struct SC4RectU {
    std::uint32_t x0{}, z0{}, x1{}, z1{};
};

// Interface ids (QueryInterface @ 0x100186cc2): 1 = cIGZUnknown, 0x0a74ca15 = cISC4NetworkTool,
// 0x452294aa = cIGZMessageTarget2 (returns the +8 sub-object).
constexpr std::uint32_t kGZIID_cISC4NetworkTool = 0x0a74ca15;

// The message Init registers on the message server (@ 0x100187752: spMS2 slot +0x28, id 0xa6b79602) and
// DoMessage consumes: payload 2 holds a pointer to the new altitude (+0x1d8).
constexpr std::uint32_t kNetworkToolMessageAltitudes = 0xa6b79602;

// cISC4NetworkManager::eNetworkType, in the order ReloadNetworkTypeInfo (0x100186f9c) initialises the records.
enum eNetworkType : std::uint32_t {
    kNetworkRoad = 0,
    kNetworkRail = 1,
    kNetworkHighway = 2,
    kNetworkStreet = 3,
    kNetworkPipe = 4,
    kNetworkPowerLine = 5,
    kNetworkAvenue = 6,
    kNetworkSubway = 7,
    kNetworkLightRail = 8,
    kNetworkMonorail = 9,
    kNetworkOneWayRoad = 10,
    kNetworkDirtRoad = 11,
    kNetworkGroundHighway = 12,
    kNetworkTypeCount = 13,
};

// kFlagForNetworkType @ 0x100a19aa0 (13 entries; the file-local table the binary instantiates in this
// translation unit): one bit per network type. Note the two flag-scanning loops below only walk the first 12
// entries — ground highway (bit 0x1000) is a quirk of the original, kept on purpose.
constexpr std::uint32_t kFlagForNetworkType[kNetworkTypeCount] = {
    0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040,
    0x0080, 0x0100, 0x0200, 0x0400, 0x0800, 0x1000,
};

// kNetworkPlacementExemplarIDs @ 0x100a19ae0 (passed to ReloadNetworkSpecificInfo in Init).
constexpr std::uint32_t kNetworkPlacementExemplarIDs[kNetworkTypeCount] = {
    0x0781214f, 0x0814e612, 0xa8433f6b, 0x492a03a3, 0x095117db, 0x00000000, 0x2b730a17,
    0x0a367ca7, 0xcb79e089, 0xebe08882, 0x8bda0c2b, 0xebe08cda, 0x0be08c87,
};

// The mask the tool keeps for "networks this tool may place" (+0x62c) and the flags that limit it (+0x428).
constexpr std::uint32_t kNetworkFlagMask = 0x1f4f;

// The network tuning examplar GetOccupantExemplar (0x100188abe) reads: type 0x6534284a, property 0xa52160f5.
constexpr std::uint32_t kNetworkTuningType = 0x6534284a;
constexpr std::uint32_t kNetworkTuningProperty = 0xa52160f5;

// SC4LoadString group used for the failure reasons (0x10018fe7a).
constexpr std::uint32_t kFailureReasonStringGroup = 0x2a592fd1;

// One entry of std::vector<cSC4NetworkBridgeTypeInfo> (element stride 0x200, filled by
// cSC4NetworkTypeInfo::LoadBridgeTypes). Only the fields the recovered functions read are named.
struct cSC4NetworkBridgeTypeInfo {
    std::uint8_t unknown000[0x100];         // +0x000
    // +0x100 / +0x101: "can this bridge be built over land / water". Which byte is which is inferred from the
    // list each one feeds in IdentifyWaterAndLandBridgeTypes (0x1001c2c10) and the flag FindBestBridgeType
    // (0x1001c077e) uses to pick the list.
    bool isLand = false;                    // +0x100 (-> landBridgeTypes_, +0x600)
    bool isWater = false;                   // +0x101 (-> waterBridgeTypes_, +0x5e8)
    std::uint8_t unknown102[0x1b0 - 0x102]; // +0x102
    std::uint32_t bitmapKeyA = 0;           // +0x1b0 (GetBridgeBitmapKey)
    std::uint32_t bitmapKeyB = 0;           // +0x1b4
    std::uint8_t unknown1b8[0x1c8 - 0x1b8]; // +0x1b8
    std::string name;                       // +0x1c8 (libc++ std::string; +0x1d8 is its heap pointer)
    std::uint8_t unknown1e0[0x200 - 0x1e0]; // +0x1e0
};

// cSC4NetworkSupportStructInfo, looked up by id in the network type info's map at +0x58
// (GetSupportStructureInfo @ 0x100188c0e returns `&value`). The type itself is unrecovered; the recovered
// function only hands the pointer out, so the size is not needed here.
struct cSC4NetworkSupportStructInfo {
    std::uint8_t unknown[0x40];
};

// cISC4NetworkTool::tPlacementParams (0xe8 bytes): the block the network type info keeps at +0x80 and the tool
// copies to +0x500 in Init, then hands back in GetPlacementParams (0x1002ed4e8).
struct cSC4NetworkPlacementParams {
    std::uint32_t unknown00[4];             // +0x00
    union {
        std::uint32_t unknown10[2];         // +0x10 (the base tool only ever copies these words)
        float float10[2];                   // +0x14 is read as a float by the underground tool (cSC4NetworkCellInfo +0x70)
    };
    float maxSlope = 0.0f;                  // +0x18: Init stores tan(maxSlope) * cellSize at +0x4a8
    std::uint32_t unknown1c = 0;            // +0x1c
    std::uint32_t unknown20 = 0;            // +0x20
    std::uint8_t unknown24 = 0;             // +0x24
    // +0x25: when set the drag is forced to a pure straight or pure diagonal line
    // (BreakIntoStraightAndDiagSegments 0x10071aa64 reads it as `this[0x525]`, DrawNetworkLine as well).
    std::uint8_t straightOrDiagonalOnly = 0;
    std::uint8_t unknown26[2] = {0, 0};     // +0x26
    std::vector<std::uint32_t> list28;      // +0x28
    union {
        std::uint32_t unknown40[8];         // +0x40 (the base tool only ever copies these words)
        float float40[8];                   // +0x40 is read as a float by the underground tool (cSC4NetworkCellInfo +0x70)
    };
    std::vector<std::uint32_t> list60;      // +0x60
    std::vector<std::uint32_t> list78;      // +0x78
    std::vector<float> list90;              // +0x90
    // +0xa8 (i386 +0x78, ten words: the i386 copy constructor 0x00670036 copies 0x78..0x9c). [6] / [7] are per-cell
    // costs the solution adds for the tunnel / bridge cells, [8] the cost of one unit of terrain change (GetCostOfSolution).
    union {
        std::uint32_t unknownA8[10];
        float floatA8[10];
    };
    std::vector<std::uint32_t> listD0;      // +0xd0
};

// cSC4NetworkTool::tDraggedStep (12 bytes, vector at +0xa0): the half-open cell range of one drag step.
struct tDraggedStep {
    std::uint32_t firstCell = 0;   // +0x0 index into the dragged-cell vector (+0xb8)
    std::uint32_t lastCell = 0;    // +0x4
    std::uint32_t direction = 0;   // +0x8: 0 = regular step (IsRegularStep), 3 = stepped/short step
};

// cSC4NetworkTool::tIntersection (0x38 bytes, vector at +0x178): a highway intersection to place.
struct tIntersection {
    int type = 0;                  // +0x00
    float altitude = 0.0f;         // +0x04 GetMajorBridgeAltitude reads [end-0x28]
    float referenceAltitude = 0.0f;// +0x08 GetMajorBridgeReferenceAltitude reads [end-0x24]
    bool landBridge = false;       // +0x0c IsMajorBridgeLandBridge reads [end-0x20]
    std::uint8_t unknown0d[0x14 - 0x0d];
    int step = 0;                  // +0x14 MarkHighwayIntersectionPieces compares it with validSteps
    std::uint8_t unknown18[0x38 - 0x18];
};

// cSC4NetworkTool::tBridge (0x2c bytes, vector at +0x190): the bridge the drag would build. The getters read
// only the four leading fields of the *last* element.
struct tBridge {
    int type = 0;                   // +0x00
    float altitude = 0.0f;          // +0x04
    float referenceAltitude = 0.0f; // +0x08
    bool landBridge = false;        // +0x0c
    std::uint8_t unknown0d[0x2c - 0x0d];
};

// One entry of the vector at +0x1c0 (CheckForReservedTiles walks it with a stride of 0x14).
struct tReservedCell {
    std::uint32_t x = 0;        // +0x00
    std::uint32_t z = 0;        // +0x04
    std::uint32_t unknown08 = 0;// +0x08
    std::uint32_t network = 0;  // +0x0c (network type passed to CheckForReservedTiles(x, z, type))
    std::uint32_t unknown10 = 0;// +0x10
};

// cISC4NetworkTool::NeighborConnectionInfo, copied out of the first element of the vector at +0x1f8
// (GetPossibleNeighborConnection @ 0x10018800a). The element has an 8-byte header the copy skips.
struct NeighborConnectionInfo {
    std::uint32_t x = 0;         // +0x0
    std::uint32_t z = 0;         // +0x4
    std::uint32_t unknown8 = 0;  // +0x8
    std::uint8_t unknownC = 0;   // +0xc
};
struct tNeighborConnection {
    std::uint32_t unknown0 = 0;  // +0x0
    std::uint32_t unknown4 = 0;  // +0x4
    NeighborConnectionInfo info; // +0x8..+0x14
};

// cSC4NetworkTool::tSolvedCell (12 bytes, the vector at +0x110): one tile the solver produced. The underground
// tool fills it while it propagates the drag (PropagateFromCell adds the neighbours of a solved cell); the
// insertion pass and the smoothing pass consume it.
class cSC4NetworkOccupant;
class cSC4UndergroundNetworkTool;
class cSC4PowerLineTool;

struct tSolvedCell {
    std::uint32_t pieceType = 0;              // +0x00 (cISC4NetworkOccupant vtable +0xb8 at creation time)
    std::uint8_t rotationFlip = 0;            // +0x04 (the same call's +0xd0)
    std::uint8_t unknown05[3] = {};           // +0x05
    std::uint32_t cellKey = 0;                // +0x08 x | z << 16
};
static_assert(sizeof(tSolvedCell) == 0x0c, "tSolvedCell stride");

// cSC4NetworkTool::tImmovablePortion (0x10 bytes, argument of ApplyPlaneConstraintToRegularCells): the vertical
// range one drag segment pins. Only its size is used by the underground tool's empty override.
struct tImmovablePortion {
    std::int32_t x = 0;            // +0x0
    std::int32_t step = 0;         // +0x4
    float valueA = 0.0f;           // +0x8
    float valueB = 0.0f;           // +0xc
};
static_assert(sizeof(tImmovablePortion) == 0x10, "tImmovablePortion stride");

// The two bounds of one tile corner's allowed height (a std::pair<float, float> in the original).
struct tVertexHeightRange {
    float low = 0.0f;              // +0x0
    float high = 0.0f;             // +0x4
};

// cSC4NetworkCellInfo: the per-cell cache the solving pipeline keeps in the world cache at +0x30. The class
// itself (GetCellInfo 0x1001b4bc0, RecomputeEdgesFromAllTypes 0x10079d6e2, ...) is unrecovered, so the tool
// asks the host for the record. The layout below is the original's byte for byte (0xc4 bytes; read from
// CreateCellInfosForCurrentPlacementTiles 0x1001a78ee and InsertRangeConstraintsForCell 0x1001a7616).
struct cSC4NetworkCellInfo {
    std::uint32_t x = 0;                     // +0x00 (SC4Point<u32>)
    std::uint32_t z = 0;                     // +0x04
    std::uint32_t networkFlags = 0;          // +0x08 OccupantExists tests it against its mask
    std::int32_t unknown0c = 0;              // +0x0c
    // +0x10: the counted network occupant of the tile (cRZAutoRefCount<cISC4NetworkOccupant> in the
    // original). InsertNetworkSpecificConstraintsForCell reads the piece id through it; AddCellPreview
    // takes and drops a reference.
    cSC4NetworkOccupant* occupant = nullptr;
    std::uint8_t status = 0;                 // +0x18 1 = the record was (re)solved for the current drag
    std::uint8_t unknown19[3] = {};          // +0x19
    std::int32_t stepIndex = 0;              // +0x1c the drag step (+0xa0 vector) that produced the cell
    // +0x20 + type*4: the packed cSC4TileNeighborhoodCompact of this cell for that network type. OccupantExists
    // returns it as its out value (0x10018fe2e) and IsSimpleDiagonalNetwork classifies its edges (0x10018b0a0).
    std::uint32_t tileNeighborhoods[13] = {};
    std::uint32_t compact54 = 0;             // +0x54 another cSC4TileNeighborhoodCompact
    std::uint8_t unknown58 = 0;              // +0x58
    bool hasOccupant = false;                // +0x59
    std::uint8_t unknown5a = 0;              // +0x5a
    // +0x5b: set when the cell belongs to a step whose direction is not 0 (CreateCellInfosForCurrentPlacementTiles).
    // InsertRangeConstraintsForCell / InsertNetworkSpecificConstraintsForCell treat it as "height is fixed".
    bool solvedOrImmovable = false;
    std::uint8_t occupantId = 0;             // +0x5c the occupant manager handle, only the low byte is kept
    bool shortStep = false;                  // +0x5d the step direction was 3 (a short/stepped step)
    bool buildable = false;                  // +0x5e (IsTerrainOK)
    bool pieceFromSolvedList = false;        // +0x5f read the piece id from the solved-cell vector, not the occupant
    float vertexHeights[4] = {};             // +0x60
    tVertexHeightRange range70[4] = {};      // +0x70 the range the constraints allow this corner
    tVertexHeightRange preferred90[4] = {};  // +0x90 the preferred range
    std::int32_t vertexBaseHeights[4] = {};  // +0xb0 + i*4, the vertex ids the terrain query takes
    std::int32_t solvedCellIndex = -1;       // +0xc0 index into solvedCellList_ (piece id / smoothing seed); the ctor
                                             // 0x10079d6ac sets -1, the occupant, the neighbourhoods and +0x58 to 0

    // @ 0x10079d6e2: the all-types neighbourhood (+0x54) is the union of the per-type ones.
    void RecomputeEdgesFromAllTypes() {
        std::uint32_t all = 0;
        for (std::uint32_t compact : tileNeighborhoods) all |= compact;
        compact54 = all;
    }
};
static_assert(offsetof(cSC4NetworkCellInfo, occupant) == 0x10, "cell info offsets");
static_assert(offsetof(cSC4NetworkCellInfo, tileNeighborhoods) == 0x20, "cell info offsets");
static_assert(offsetof(cSC4NetworkCellInfo, vertexHeights) == 0x60, "cell info offsets");
static_assert(offsetof(cSC4NetworkCellInfo, vertexBaseHeights) == 0xb0, "cell info offsets");
static_assert(offsetof(cSC4NetworkCellInfo, solvedCellIndex) == 0xc0, "cell info offsets");
static_assert(sizeof(cSC4NetworkCellInfo) == 0xc8, "cell info size (GetAllCells copies 200 bytes)");

class INetworkPreview;
class INetworkWorldCache;
class INetworkConstraintSatisfier;
class INetworkTypeInfo;

// The preview layer the tool draws its cells into (cSC4NetworkPreview, 19 functions at 0x100718d8c.., not
// recovered). One method per original call; `Release` is the COM-style counted release ([2] +0x10).
class INetworkPreview {
public:
    virtual ~INetworkPreview() = default;
    virtual std::uint32_t AddRef() = 0;                                                       // [1] +0x08
    virtual std::uint32_t Release() = 0;                                                      // [2] +0x10
    virtual void Init() = 0;                                                                  // 0x100718d8c
    virtual void Shutdown() = 0;                                                              // 0x1007190b8
    virtual void Clear() = 0;                                                                 // 0x1007195bc
    virtual void Erase(const SC4PointU& cell) = 0;                                            // 0x100719538
    // 0x1007190ea: (cell, color 0x80ff0000, heights = null, elevation, lit)
    virtual void AddUntexturedSelection(const SC4PointU& cell, std::uint32_t color, const float* heights,
                                        float elevation, bool lit) = 0;
    virtual void AdjustCellHeights(const SC4PointU& cell, const float* heights, bool lit) = 0; // 0x100719612
};

// cSC4NetworkWorldCache (member at +0x30, 0x50 bytes; ctor 0x10079d6fe, dtor 0x10079d76a, not recovered).
class INetworkWorldCache {
public:
    virtual ~INetworkWorldCache() = default;
    virtual void Clear() = 0;                                 // 0x10079d868 (Reset)
    virtual cSC4NetworkCellInfo* GetCell(std::uint32_t cellKey) = 0;  // 0x10079d774
    // 0x10079ddbe: the whole cache as a flat copy (cSC4UndergroundNetworkTool::StartPlacing walks it).
    virtual void GetAllCells(std::vector<cSC4NetworkCellInfo>& out) = 0;
    // 0x10079d7a4: the record for cellKey, created (and stored) when it does not exist yet. The pointer stays
    // valid until Clear (the underground tool keeps several of them across a loop).
    virtual cSC4NetworkCellInfo* CreateCell(std::uint32_t cellKey) = 0;
};

// cSC4VertexHtConstraintSatisfier (members at +0x228 / +0x328, 0x100 bytes each, not recovered). The
// underground tool drives one of them as it walks the drag: the methods below are the ones it calls, in the
// original's own order of appearance.
class INetworkConstraintSatisfier {
public:
    virtual ~INetworkConstraintSatisfier() = default;
    // 0x1001bc9c2: put the vertex into the solve set with an initial height.
    virtual void AddVertexToSolve(int vertexId, float height) = 0;
    // 0x1001bd0ea: the height the solver settled on; false when the vertex is not in the system.
    virtual bool GetHeight(int vertexId, float& out) = 0;
    // 0x1001bd6b2 / 0x1001bd7c2 / 0x1001bd78a / 0x1001bd67c / 0x1001bd178: the constraints of one vertex.
    virtual void InsertRangeConstraint(int vertexId, float low, float high) = 0;
    virtual void InsertPreferredRangeConstraint(int vertexId, float low, float high) = 0;
    virtual void InsertPreferredValueConstraint(int vertexId, float value) = 0;
    virtual void InsertFixedVertex(int vertexId, float height) = 0;
    virtual void InsertEqualityConstraint(std::uint32_t vertexA, std::uint32_t vertexB) = 0;
};

// One cSC4NetworkTypeInfo record (&sNetworkTypeInfo + networkType * 0x180, @ 0x100b23190; the record is
// filled at start-up by ReloadNetworkTypeInfo 0x100186f9c from the network ini, which is not recovered).
class INetworkTypeInfo {
public:
    virtual ~INetworkTypeInfo() = default;
    virtual std::uint32_t ViewInputControlId() const = 0;            // +0x08 (ViewInputControlId)
    virtual std::uint32_t ToolViewId() const = 0;                    // +0x10 (GetToolViewId)
    virtual std::uint32_t CursorId() const = 0;                      // +0x14 (CursorId)
    virtual std::uint32_t DirectionalCursorBase() const = 0;         // +0x18 (DirectionalCursorBase)
    virtual std::uint32_t DefaultExemplarId() const = 0;             // +0x0c (GetOccupantExemplar default)
    virtual int PenWidthX() const = 0;                               // +0x1c (DAT_100b231ac)
    virtual int PenWidthZ() const = 0;                               // +0x20 (DAT_100b231b0)
    virtual std::uint32_t CostPerCell() const { return 0; }          // i386 sNetworkTypeInfo +0xdc
    virtual const std::vector<cSC4NetworkBridgeTypeInfo>& BridgeTypes() const = 0;  // +0x40
    virtual const cSC4NetworkPlacementParams& PlacementParams() const = 0;          // +0x80
    // The unordered_map<uint32, cSC4NetworkSupportStructInfo> at +0x58: the value pointer, or null.
    virtual const cSC4NetworkSupportStructInfo* FindSupportStructure(std::uint32_t id) const = 0;
    virtual std::uint32_t PlacementExemplarId() const = 0;           // kNetworkPlacementExemplarIDs[type]
};

// Every engine global / unrecovered neighbour the recovered functions reach. One method per original call, with
// the vtable slot or address in the comment; scripts/tests/sc4_networktool_test.cpp supplies a fake.
class INetworkToolHost {
public:
    virtual ~INetworkToolHost() = default;
    // SL::spSC4 slot +0x218: register the static shutdown callback (StaticInit 0x100186d04).
    virtual void RegisterShutdownCallback(bool (*fn)(void*), void* user) = 0;
    // SL::spTerrain: counted acquire (Init 0x100187752 / Shutdown 0x100187ce8 release it again).
    virtual cIGZUnknown* AcquireTerrain() = 0;
    // SL::spTerrain slot +0x30: one cell's edge length in metres (Init -> +0x4a4).
    virtual float TerrainCellSize() = 0;
    // SL::spTerrain i386 vslot 0x34: the terrain height at a vertex id (the cell info's vertex ids, +0xb0).
    virtual float TerrainVertexHeight(std::int32_t /*vertex*/) { return 0.0f; }
    // SL::spTerrain slot +0x110: may a network cell be built here (IsTerrainOK 0x10071e4ca).
    virtual bool TerrainAllowsNetwork(std::uint32_t x, std::uint32_t z) = 0;
    // SL::spCity slots +0x2e8 / +0x2f0: city size in cells (Init -> +0x4ac / +0x4b0).
    virtual int CityWidth() = 0;
    virtual int CityHeight() = 0;
    // SL::spOccupantManager, counted (Init -> +0x88).
    virtual cIGZUnknown* AcquireOccupantManager() = 0;
    // SL::spCity slot +0x108: the city view model, counted (Init -> +0x90).
    virtual cIGZUnknown* AcquireCityViewModel() = 0;
    // SL::spRenderer slots +0x100 / +0x108: preview layer 0xa / priority 1000 (Init / Shutdown).
    virtual void AddRenderLayer(INetworkPreview* layer, std::uint32_t layerId, std::int32_t priority) = 0;
    virtual void RemoveRenderLayer(INetworkPreview* layer) = 0;
    // SL::spRenderer slot +0x50 -> +0x14: the renderer's altitude scale (Init -> +0x1d8).
    virtual std::uint32_t RendererAltitudeScale() = 0;
    // SL::spMS2 slots +0x28 / +0x30 (target is the +8 sub-object, message id 0xa6b79602).
    virtual void RegisterMessage(cIGZMessageTarget2* target, std::uint32_t messageType) = 0;
    virtual void UnregisterMessage(cIGZMessageTarget2* target, std::uint32_t messageType) = 0;
    // cSC4NetworkORFilter (ctor 0x1002eee0a): the occupant filter at +0x630; `flags` goes to its +0x14.
    virtual cIGZUnknown* CreateNetworkOrFilter(std::uint32_t flags) = 0;
    // cSC4NetworkPreview: a new preview (ctor 0x100718d8c), returned with one reference.
    virtual INetworkPreview* CreatePreview() = 0;
    // cSC4NetworkWorldCache: the cache member at +0x30 (ctor 0x10079d6fe).
    virtual INetworkWorldCache* CreateWorldCache() = 0;
    // cSC4VertexHtConstraintSatisfier: one of the two members at +0x228 / +0x328 (ctor 0x1001bca8c).
    virtual INetworkConstraintSatisfier* CreateConstraintSatisfier() = 0;
    // &sNetworkTypeInfo + type * 0x180: the network type record.
    virtual const INetworkTypeInfo* NetworkTypeInfo(std::uint32_t networkType) = 0;
    // GetCellInfo (0x1001b4bc0): the cell record for x | z << 16, or null.
    virtual cSC4NetworkCellInfo* GetCellInfo(std::uint32_t cellKey) = 0;
    // SL::spRM slot +0x18: the network tuning exemplar's property value (GetOccupantExemplar).
    virtual cIGZUnknown* GetTuningProperty(std::uint32_t type, std::uint32_t group, std::uint32_t instance,
                                           std::uint32_t propertyId) = 0;
    // SC4LoadString(group, instance, out): the localized failure reason.
    virtual bool LoadString(std::uint32_t group, std::uint32_t instance, cIGZString& out) = 0;
};

void SetNetworkToolHost(INetworkToolHost* host);
INetworkToolHost* GetNetworkToolHost();

// cISC4NetworkTool: the tool's interface (the binary's primary vtable, 83 slots at 0x100a7a918).
// Only the virtuals this slice recovers are declared, always in the original relative order; the slot numbers in
// the comments are the original vtable offsets (0x000 = slot 0). Because the unrecovered slots are omitted the
// ported interface's own vtable is shorter than the binary's, so it is NOT layout-compatible with the game — it
// exists to keep the recovered methods and their original order documented. [0]/[1]/[2] (QueryInterface /
// AddRef / Release) come from cIGZUnknown, [51]/[52] are the destructors (declared at the end of the interface
// in the binary; here they sit first because the remaining slots are missing).
class cISC4NetworkTool : public cIGZUnknown {
public:
    virtual ~cISC4NetworkTool() = default;

    virtual bool Init() = 0;                                                               // [3] 0x018
    virtual bool Shutdown() = 0;                                                           // [4] 0x020
    virtual void Reset() = 0;                                                              // [5] 0x028
    virtual bool GetUITool(cISC4NetworkToolUI** out) = 0;                                  // [6] 0x030
    // [7] 0x038 PlaceNetworkOccupants .. [8] 0x040 PlaceNetworkOccupantById — not recovered.
    virtual bool OccupantExists(int x, int z, std::uint32_t typeMask, std::uint32_t& occupantId) = 0;  // [9] 0x048
    virtual void UpdateOnZoneChange(SC4RectU rect, std::uint32_t network) = 0;              // [10] 0x050
    virtual void ForceReload() = 0;                                                        // [11] 0x058
    virtual void SetTraceFinal(bool final) = 0;                                            // [12] 0x060
    virtual void ClearFailureReason() = 0;                                                 // [13] 0x068
    virtual bool GetFailureState() = 0;                                                    // [14] 0x070
    virtual void GetFailureReasonString(cIGZString& out) = 0;                              // [15] 0x078
    virtual void ClearCellPreviews() = 0;                                                  // [16] 0x080
    virtual void ClearOccupantHighlights() = 0;                                            // [17] 0x088
    // [18] 0x090: the drag from `start` to `end` as cells and steps; `end` moves to the end of the allowed part.
    virtual bool DrawNetworkLine(SC4PointU& start, SC4PointU& end, bool allowDiagonals, std::uint32_t networkType) = 0;
    // [19] 0x098: solve every anchor segment (and place it when `place`).
    virtual void PlaceAllSegments(bool place, bool allowDiagonals, bool connectNeighbor, std::uint32_t networkType) = 0;
    virtual int GetCostOfSolution() = 0;                                                   // [20] 0x0a0
    virtual void EnablePreviewUpdate(bool enabled) = 0;                                    // [21] 0x0a8
    virtual void SetAnchorPoints(const std::vector<SC4PointU>& anchors) = 0;               // [22] 0x0b0
    virtual void GetDraggedCells(std::vector<SC4PointU>& out) = 0;                         // [23] 0x0b8
    virtual void SetValidStepCount(int count) = 0;                                         // [24] 0x0c0
    virtual int GetValidStepCount() = 0;                                                   // [25] 0x0c8
    virtual int GetStraightStepCount() = 0;                                                // [26] 0x0d0
    virtual int GetTotalStepCount() = 0;                                                   // [27] 0x0d8
    virtual bool IsRegularStep(int step) = 0;                                              // [28] 0x0e0
    virtual void GetLastValidPoint(SC4PointU& out) = 0;                                    // [29] 0x0e8
    virtual bool GetPossibleBridges(std::vector<int>& out) = 0;                            // [30] 0x0f0
    virtual int GetPreferredBridge() = 0;                                                  // [31] 0x0f8
    virtual void SetPreferredBridge(int bridge) = 0;                                       // [32] 0x100
    virtual float GetBridgeAltitudeOverride() = 0;                                         // [33] 0x108
    virtual void SetBridgeAltitudeOverride(float altitude) = 0;                            // [34] 0x110
    virtual void GetBridgeName(cIGZString& out, int bridge) = 0;                           // [35] 0x118
    virtual int GetMajorBridge() = 0;                                                      // [36] 0x120
    virtual bool GetMajorBridgeAltitude(float& out) = 0;                                   // [37] 0x128
    virtual bool GetMajorBridgeReferenceAltitude(float& out) = 0;                          // [38] 0x130
    virtual bool IsMajorBridgeLandBridge() = 0;                                            // [39] 0x138
    virtual bool GetBridgeBitmapKey(int bridge, std::uint32_t& keyA, std::uint32_t& keyB) = 0;  // [40] 0x140
    virtual bool GetPossibleNeighborConnection(NeighborConnectionInfo& out) = 0;            // [41] 0x148
    virtual std::uint32_t ViewInputControlId() = 0;                                        // [42] 0x150
    virtual std::uint32_t CursorId() = 0;                                                  // [43] 0x158
    virtual std::uint32_t DirectionalCursorBase() = 0;                                     // [44] 0x160
    virtual void PenWidth(int& widthX, int& widthZ) = 0;                                   // [45] 0x168
    virtual bool StopPickAtWater() = 0;                                                    // [46] 0x170
    virtual std::uint32_t GetToolViewId() = 0;                                             // [47] 0x178
    virtual void GetPlacementParams(cSC4NetworkPlacementParams& out) = 0;                  // [48] 0x180
    virtual void QueryUserForPotentialIntersections() = 0;                                 // [49] 0x188
    // [50] 0x190 DestroyOccupantsAndResolve — not recovered.
    // [51] / [52] 0x198 / 0x1a0 the complete / deleting destructor.
    virtual void GetFailureReasonString(std::uint32_t reason, cRZString& out) = 0;         // [54] 0x1b0
    // [55] 0x1b8 DoNetworkSpecificPlacement .. [62] 0x1f0 DestroyHilightedOccupants — not recovered.
    virtual bool AllowBridges() = 0;                                                       // [63] 0x1f8
    virtual bool AllowTunnels() = 0;                                                       // [64] 0x200
    virtual bool IsStoppedByWater() = 0;                                                   // [65] 0x208
    virtual bool ShouldAutoConnect() = 0;                                                  // [66] 0x210
    // [67] 0x218 TestForAutoConnectOccupant .. [71] 0x238 CheckForGoodTerrain — not recovered.
    virtual void StartPlacing() = 0;                                                       // [72] 0x240
    // [73] 0x248 CreateCellInfosForCurrentPlacementTiles .. [76] 0x260 GetNetworkHeightForTerrainHeight,
    // [78..80] 0x270..0x280 the constraint inserters, [82..83] the remaining placement helpers — not recovered.
    virtual void PropagateSolvedCellsForSmoothing() = 0;                                   // [77] 0x268
    virtual void ReloadNetworkSpecificInfo(std::uint32_t exemplarId) = 0;                  // [81] 0x288
};

class cSC4NetworkTool : public cISC4NetworkTool, public cIGZMessageTarget2 {
public:
    explicit cSC4NetworkTool(std::uint32_t networkType);
    cSC4NetworkTool(const cSC4NetworkTool&) = delete;
    cSC4NetworkTool& operator=(const cSC4NetworkTool&) = delete;
    ~cSC4NetworkTool() override;

    // --- cIGZUnknown / cISC4NetworkTool ---
    bool QueryInterface(std::uint32_t iid, void** out) override;          // 0x100186cc2
    std::uint32_t AddRef() override;                                      // 0x100186c62
    std::uint32_t Release() override;                                     // 0x100186c7e
    bool Init() override;                                                 // 0x100187752
    bool GetUITool(cISC4NetworkToolUI** out) override;                    // i386 0x00122c58
    void ClearCellPreviews() override;                                    // i386 0x0012563e
    void ClearOccupantHighlights() override;                              // i386 0x0012519a
    bool DrawNetworkLine(SC4PointU& start, SC4PointU& end, bool allowDiagonals, std::uint32_t networkType) override;  // i386 0x004cdd0a
    void PlaceAllSegments(bool place, bool allowDiagonals, bool connectNeighbor, std::uint32_t networkType) override;  // i386 0x004cf1ce
    int GetCostOfSolution() override;                                     // i386 0x00124dbe
    void QueryUserForPotentialIntersections() override;                   // i386 0x0012b18c
    bool Shutdown() override;                                             // 0x100187ce8
    void Reset() override;                                                // 0x10018fdaa
    bool OccupantExists(int x, int z, std::uint32_t typeMask, std::uint32_t& occupantId) override;  // 0x10018fe2e
    void UpdateOnZoneChange(SC4RectU rect, std::uint32_t network) override;  // 0x100190604 (empty)
    void ForceReload() override;                                          // 0x100193650
    void SetTraceFinal(bool final) override;                              // 0x100190618 (empty)
    void ClearFailureReason() override;                                   // 0x1002ed3e0
    bool GetFailureState() override;                                      // 0x1002ed3f0
    void GetFailureReasonString(cIGZString& out) override;                // 0x1002ed400
    void EnablePreviewUpdate(bool enabled) override;                      // 0x100193668
    void SetAnchorPoints(const std::vector<SC4PointU>& anchors) override; // 0x1002ed4ac
    void GetDraggedCells(std::vector<SC4PointU>& out) override;           // 0x100193678
    void SetValidStepCount(int count) override;                           // 0x100190628
    int GetValidStepCount() override;                                     // 0x100190638
    int GetStraightStepCount() override;                                  // 0x1001936b0
    int GetTotalStepCount() override;                                     // 0x100190650
    bool IsRegularStep(int step) override;                                // 0x1001936c8
    void GetLastValidPoint(SC4PointU& out) override;                      // 0x100190668
    bool GetPossibleBridges(std::vector<int>& out) override;              // 0x100187e88
    int GetPreferredBridge() override;                                    // 0x100187ec8
    void SetPreferredBridge(int bridge) override;                         // 0x100187ed4
    float GetBridgeAltitudeOverride() override;                           // 0x100187ee0
    void SetBridgeAltitudeOverride(float altitude) override;              // 0x100187eee
    void GetBridgeName(cIGZString& out, int bridge) override;             // 0x100187efc
    int GetMajorBridge() override;                                        // 0x100187f42
    bool GetMajorBridgeAltitude(float& out) override;                     // 0x100187f84
    bool GetMajorBridgeReferenceAltitude(float& out) override;            // 0x100187f62
    bool IsMajorBridgeLandBridge() override;                              // 0x100187fa6
    bool GetBridgeBitmapKey(int bridge, std::uint32_t& keyA, std::uint32_t& keyB) override;  // 0x100187fc8
    bool GetPossibleNeighborConnection(NeighborConnectionInfo& out) override;  // 0x10018800a
    std::uint32_t ViewInputControlId() override;                          // 0x1001906a0
    std::uint32_t CursorId() override;                                    // 0x1001906b0
    std::uint32_t DirectionalCursorBase() override;                      // 0x1001906c0
    void PenWidth(int& widthX, int& widthZ) override;                     // 0x1001906d0
    bool StopPickAtWater() override;                                      // 0x1002ed4d0 (returns true)
    std::uint32_t GetToolViewId() override;                               // 0x1002ed4d8
    void GetPlacementParams(cSC4NetworkPlacementParams& out) override;    // 0x1002ed4e8
    void GetFailureReasonString(std::uint32_t reason, cRZString& out) override;  // 0x10018fe7a
    bool AllowBridges() override;                                         // 0x1002ed6dc (returns true)
    bool AllowTunnels() override;                                         // 0x1002ed6e4 (returns true)
    bool IsStoppedByWater() override;                                     // 0x1002ed6ec (returns true)
    bool ShouldAutoConnect() override;                                    // 0x1002ed6f4 (returns false)
    void StartPlacing() override;                                         // 0x1002ed6fc (empty)
    void PropagateSolvedCellsForSmoothing() override;                     // 0x1002ed708 (empty)
    void ReloadNetworkSpecificInfo(std::uint32_t exemplarId) override;    // 0x1001906fc (empty in the base tool)

    // --- cIGZMessageTarget2 ---
    bool DoMessage(cIGZMessage2* message) override;                       // 0x100188822

    // --- static state ---
    static bool StaticInit();                                             // 0x100186d04
    static bool StaticShutdown(void* user);                               // 0x100186d3e
    static void ReloadCheck();                                            // 0x10018774c (empty)
    static void ReloadRules();                                            // 0x100186d56 — NOT RECOVERED (stub)
    static int GetNetworkTypeFromFlag(std::uint32_t flag);                // 0x10018f58a
    static int GetFirstNetworkTypeFromFlags(std::uint32_t flags);         // 0x100190386
    // 0x1001c2c10: fill waterBridgeTypes/landBridgeTypes from a bridge-type list (both are cleared first).
    static void IdentifyWaterAndLandBridgeTypes(const std::vector<cSC4NetworkBridgeTypeInfo>* bridgeTypes,
                                                std::vector<int>& waterBridgeTypes,
                                                std::vector<int>& landBridgeTypes);

    // --- plain members ---
    void GetNetworkHeightForTerrainHeight(float /*terrainHeight*/) {}     // 0x1001906f0 (empty)
    void DebugDumpEdgeTypes(const cIGZString& /*dump*/) {}                // 0x10041530c (empty)
    bool IsTerrainOK(std::uint32_t x, std::uint32_t z);                   // 0x10071e4ca
    bool IdentifyStartingConditions(int& failureReason);                  // 0x10071e47a
    void SetTerrainHilight(int param);                                    // 0x10018cd24
    // i386 0x004cc96a: the drag's cells (+0xb8) and steps (+0xa0); zMajor / xMajor = the splitter's two flags.
    void ComputeDraggedCells(const SC4PointU& start, const SC4PointU& end, int zMajor, int xMajor, std::uint32_t networkType);
    // i386: solve the current line and insert its occupants (or previews). Not ported yet.
    bool PlaceNetwork(bool place, bool allowDiagonals, bool connectNeighbor, std::uint32_t networkType);
    // 0x10071aa64: split the drag from `start` to `end` into straight and diagonal parts and move `end` to the
    // end of the allowed part. See the .cpp for the meaning of every out-parameter.
    void BreakIntoStraightAndDiagSegments(const SC4PointU& start, SC4PointU& end, int& totalSteps,
                                          int& straightSteps, int& directionA, int& directionB, int& isDiagonal,
                                          int& isStraight, bool allowDiagonals, int penWidthX, int penWidthZ);
    // 0x10018b0a0: true when the cell's edges for that network type are one or two diagonals and no straight.
    static bool IsSimpleDiagonalNetwork(const cSC4NetworkCellInfo& cell, std::uint32_t networkType);
    // 0x100188b2c: the cell rectangle around (x, z) padded by `pad`, clamped to the city.
    void GetPaddedRectAroundCell(std::uint32_t x, std::uint32_t z, SC4RectU& out, std::uint32_t pad);
    // 0x100188abe: the exemplar `exemplarId` in `groupOrZero` (0 = the type info's default exemplar id).
    cIGZUnknown* GetOccupantExemplar(std::uint32_t exemplarId, std::uint32_t groupOrZero);
    // 0x100188c0e: the support-structure record for id, or null.
    const cSC4NetworkSupportStructInfo* GetSupportStructureInfo(std::uint32_t id);

private:
    // Fields in original order; the comments carry the original offsets.
    std::vector<std::uint32_t> unknown10_;                  // +0x010
    std::uint32_t refCount_ = 0;                            // +0x028
    bool inited_ = false;                                   // +0x02c (Init / Shutdown guard)
    std::uint8_t unknown2d_[3] = {0, 0, 0};                 // +0x02d
    INetworkWorldCache* worldCache_ = nullptr;              // +0x030 (cSC4NetworkWorldCache, 0x50 bytes)
    cIGZUnknown* terrain_ = nullptr;                        // +0x080 (counted)
    cIGZUnknown* occupantManager_ = nullptr;                // +0x088 (counted)
    cIGZUnknown* cityViewModel_ = nullptr;                  // +0x090 (counted)
    bool unknown98_ = false;                                // +0x098
    std::uint8_t unknown99_ = 0;                            // +0x099
    bool unknown9a_ = true;                                 // +0x09a
    bool forceReload_ = false;                              // +0x09b
    std::vector<tDraggedStep> steps_;                       // +0x0a0
    std::vector<SC4PointU> cells_;                          // +0x0b8
    bool stepIsHorizontal_ = false;                         // +0x0d0
    int totalSteps_ = 0;                                    // +0x0d4
    int straightSteps_ = 0;                                 // +0x0d8
    int validSteps_ = 0;                                    // +0x0dc
    int directionA_ = 0;                                    // +0x0e0
    int directionB_ = 0;                                    // +0x0e4
    std::unordered_set<std::uint32_t> solvedCells_;         // +0x0e8
    // +0x110 std::vector<tSolvedCell>: the tiles the solver produced this drag. The base port was missing this
    // member (its comments jumped from +0x0e8 to +0x128, which shifted every following offset by 0x18); it is
    // restored here because the underground tool is its main user (0x1001a7616, 0x1001a913c, 0x1001a932a, ...).
    std::vector<tSolvedCell> solvedCellList_;               // +0x110
    std::unordered_set<std::uint32_t> unknown128_;          // +0x128
    std::unordered_set<std::uint32_t> unknown150_;          // +0x150
    std::vector<tIntersection> intersections_;               // +0x178
    std::vector<tBridge> bridges_;                          // +0x190
    std::vector<std::uint32_t> unknown1a8_;                 // +0x1a8
    std::vector<tReservedCell> reservedCells_;              // +0x1c0
    std::uint32_t rendererAltitudeScale_ = 0;               // +0x1d8
    std::vector<std::uint32_t> unknown1e0_;                 // +0x1e0
    std::vector<tNeighborConnection> neighborConnections_;  // +0x1f8
    std::vector<SC4PointU> anchors_;                        // +0x210
    INetworkConstraintSatisfier* constraintA_ = nullptr;    // +0x228 (cSC4VertexHtConstraintSatisfier)
    INetworkConstraintSatisfier* constraintB_ = nullptr;    // +0x328
    std::uint32_t networkFlags_ = 0;                        // +0x428
    INetworkPreview* preview_ = nullptr;                    // +0x430 (counted)
    std::vector<int> possibleBridges_;                      // +0x438
    int preferredBridge_ = -1;                              // +0x450
    float bridgeAltitudeOverride_ = -1.0f;                  // +0x454
    int unknown458_ = 1;                                    // +0x458
    int failureReason_ = 0;                                 // +0x45c
    // +0x460 cRZCellMap*, +0x468 / +0x488 std::set<cRZAutoRefCount<cISC4Occupant>> (the highlighted and the
    // preview occupants). No function in this slice touches them, so they stay opaque until the occupant
    // insertion path (CreateNetworkOccupant / InsertOccupantsForSolvedCells) is recovered.
    std::uint8_t unknown460_[0x20] = {};                    // +0x460 .. +0x47f
    std::int32_t baseCost_ = 0;                             // i386 +0x254: added to every GetCostOfSolution
    std::int32_t unknown484_ = 0;                           // i386 +0x258
    std::uint8_t unknown488_[0x18] = {};
    bool previewUpdateEnabled_ = true;                      // +0x4a0
    float cellSize_ = 0.0f;                                 // +0x4a4
    float slopeRise_ = 0.0f;                                // +0x4a8
    int cityWidth_ = 0;                                     // +0x4ac
    int cityHeight_ = 0;                                    // +0x4b0
    int widthPlusOne_ = 0;                                  // +0x4b4
    int heightPlusOne_ = 0;                                 // +0x4b8
    // +0x4bc: Init writes 16 ints here (width derived). Meaning unrecovered; preserved verbatim so the offsets
    // after it stay right.
    std::int32_t initScratch_[16] = {};
    cSC4NetworkPlacementParams placement_;                  // +0x500
    std::vector<int> waterBridgeTypes_;                     // +0x5e8
    std::vector<int> landBridgeTypes_;                      // +0x600
    bool hasBridgeTypes_ = false;                           // +0x618
    const INetworkTypeInfo* typeInfo_ = nullptr;            // +0x620 (not owned)
    std::uint32_t networkType_ = 0;                         // +0x628
    std::uint32_t placementFlagMask_ = 0;                   // +0x62c
    cIGZUnknown* networkOrFilter_ = nullptr;                // +0x630 (counted; cISC4NetworkORFilter)

    // 0x100186d3e's shared flag (sbInited @ 0x100b2... in the binary).
    static bool sbInited_;

    // scripts/tests/sc4_networktool_test.cpp reads the private state below to check the offsets/quirk cases the
    // public interface cannot reach (the original has the same members private).
    friend struct NetworkToolTestAccess;
    // The underground tool is a derived class in the original and drives this state directly (solved-cell
    // vector +0x110, the fixed-vertex set +0x150, the constraint satisfier +0x228, the world cache +0x30, the
    // terrain +0x80, the occupant manager +0x88, the placement block +0x500, network type +0x628, +0x458).
    friend class cSC4UndergroundNetworkTool;
    // The power-line tool derives from this class and reaches the same base state directly in the original
    // (the dragged steps +0xa0 and cells +0xb8, the step counters +0xd4..+0xdc, the anchors +0x210, the
    // preview +0x430, the occupant manager +0x88, the network type +0x628 and the +0x98 update flag).
    friend class cSC4PowerLineTool;
};

}  // namespace sc4
