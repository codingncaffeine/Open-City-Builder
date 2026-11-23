// cSC4PowerLineTool — the power-line drawing tool: it turns a chain of drag cells into power poles (`tPoleInfo`),
// solves the pole positions along the drag (`DeterminePolePositions`), creates / connects / removes the pole
// occupants (`PlacePoles`, `CreatePowerPole`) and keeps the preview in sync. 32 function symbols at
// 0x1002e9c30..0x1002ed8e0 (33 exported, two of them compiler-generated aliases), vtable 0x100a92ca0
// (+0x0 = cISC4NetworkTool, 85 slots; +0x8 = cIGZMessageTarget2, 4 slots). It derives from cSC4NetworkTool
// with network type 5 (kNetworkPowerLine).
//
// Slot map of the overrides (scripts/vtable.py cSC4PowerLineTool):
//   [3] 0x018 Init                        [4] 0x020 Shutdown
//   [10] 0x050 UpdateOnZoneChange          [16] 0x080 ClearCellPreviews
//   [20] 0x0a0 GetCostOfSolution           [43] 0x158 CursorId
//   [44] 0x160 DirectionalCursorBase       [51]/[52] 0x198/0x1a0 destructors
//   [54] 0x1b0 GetFailureReasonString(int, cRZString&)
//   [55] 0x1b8 DoNetworkSpecificPlacement  [83] 0x298 CheckForReservedTiles(int&)
//   [84] 0x2a0 SetTerrainHilight
// The base port (NetworkTool.h) deliberately omits the unrecovered slots [16], [20], [55], [83] and [84], so
// the last five are plain members here, kept in the original slot order with the slot in each comment.
//
// Object layout of the derived part (the cSC4NetworkTool base occupies +0x000..+0x637):
//   +0x638 bool powerLineInited (Init / Shutdown guard)
//   +0x640 std::vector<tPoleInfo> poleInfos (0x50 bytes each, the poles of the current drag)
//   +0x658 std::vector<std::vector<tLineInfo>> lineInfos (one list of drag cells per pole, 0x18 each)
//   +0x670 int wireCellCount   (cells added to the wire preview; GetCostOfSolution)
//   +0x674 int poleCount       (number of tPoleInfo entries solved; GetCostOfSolution)
//   +0x678 std::list<IPowerPoleOccupant*> createdPoles (poles placed by the current drag; 0x18 list header)
//   +0x690 std::list<IPowerPoleOccupant*> previewPoles (poles reused from the city by the drag)
//   +0x6a8 std::list<tIntersectionInfo> connectionList (pairs whose connection was (re)drawn, 0x18 each)
//   +0x6c0 std::vector<std::vector<int>> connectionGroups (pole-index chains; consumed and cleared by PlacePoles)
//   +0x6d8 int poleSpacing = 0x14 (cells; spaced this far the solver forces a pole)
//   +0x6dc float poleHeightOffset = 7.0f (added to the computed pole height)
//   +0x6e0 int poleCost, +0x6e4 float wireCost (tuning exemplar; GetCostOfSolution)
#pragma once
#include <cstddef>
#include <cstdint>
#include <list>
#include <vector>

#include "gz/cIGZMessage2.h"
#include "gz/cIGZUnknown.h"
#include "gz/cIGZString.h"
#include "rz/cRZIniFile.h"
#include "rz/cRZString.h"
#include "sc4/NetworkTool.h"
#include "sc4/PowerLineOccupant.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {

class cSC4PowerLineTool;
class IPowerPoleOccupant;

// One connection of a pole (cSC4PowerLineTool::tConnectionInfo, 8 bytes, the vector at tPoleInfo +0x20):
// the index of the other pole and the value the connection was created with (the number of solved pole
// positions at the time, or -1 for a wire inside one segment).
struct tConnectionInfo {
    std::uint32_t poleIndex = 0;   // +0x00
    std::int32_t value = 0;        // +0x04
};
static_assert(sizeof(tConnectionInfo) == 0x08, "tConnectionInfo stride");

// One cell of the wire between two poles (cSC4PowerLineOccupant::tLineInfo, 0x18 bytes). DeterminePolePositions
// appends the drag cells between two poles to the per-pole lists at +0x658; PlacePoles creates the wire occupant of
// each cell and the pole's AddConnection keeps a reference to it (UpdateLineOccupants).
struct tLineInfo {
    std::uint32_t x = 0;                          // +0x00
    std::uint32_t z = 0;                          // +0x04
    cSC4PowerLineOccupant* line = nullptr;        // +0x08 the wire occupant of the cell (one reference)
    bool first = false;                           // +0x10 the cell is the step's primary cell
    std::uint8_t pad11[7] = {};                   // +0x11
};
static_assert(sizeof(tLineInfo) == 0x18, "tLineInfo stride");
static_assert(offsetof(tLineInfo, first) == 0x10, "tLineInfo offsets");

// cSC4PowerLineTool::tIntersectionInfo (0x18 bytes): the two pole occupants a placed piece connects, plus the
// per-pole "the connection to it is new" flags. It is also the tail of a tPoleInfo (+0x38..+0x49), which is why
// IntersectionIsInList compares the same four fields against both structures.
struct tIntersectionInfo {
    IPowerPoleOccupant* first = nullptr;   // +0x00 (a tPoleInfo stores it at +0x38)
    IPowerPoleOccupant* second = nullptr;  // +0x08 (a tPoleInfo stores it at +0x40)
    bool firstNew = false;                 // +0x10 (a tPoleInfo stores it at +0x48)
    bool secondNew = false;                // +0x11 (a tPoleInfo stores it at +0x49)
    std::uint8_t pad12[6] = {};            // +0x12
};
static_assert(sizeof(tIntersectionInfo) == 0x18, "tIntersectionInfo stride");
static_assert(offsetof(tIntersectionInfo, firstNew) == 0x10, "tIntersectionInfo offsets");

// cSC4PowerLineTool::tPoleInfo (0x50 bytes, the vector at +0x640): one pole the drag wants to place.
struct tPoleInfo {
    std::int32_t index = 0;                        // +0x00 position in the vector (AddConnection writes it)
    std::uint32_t x = 0;                           // +0x04 cell
    std::uint32_t z = 0;                           // +0x08
    std::int32_t cellIndex = 0;                    // +0x0c index into the dragged-cell vector (+0xb8)
    IPowerPoleOccupant* occupant = nullptr;        // +0x10 counted cISC4PowerPoleOccupant
    bool created = false;                          // +0x18 false when an existing city pole was reused
    std::uint8_t connections = 0;                  // +0x19 direction-connection bit mask (1 << Get0To3Direction)
    std::uint8_t pad1a[6] = {};                    // +0x1a
    std::vector<tConnectionInfo> connectionList;   // +0x20
    IPowerPoleOccupant* poleA = nullptr;           // +0x38 counted (from the tIntersectionInfo argument)
    IPowerPoleOccupant* poleB = nullptr;           // +0x40 counted
    bool flagA = false;                            // +0x48
    bool flagB = false;                            // +0x49
    std::uint8_t pad4a[6] = {};                    // +0x4a
};
static_assert(sizeof(tPoleInfo) == 0x50, "tPoleInfo stride");
static_assert(offsetof(tPoleInfo, occupant) == 0x10, "tPoleInfo offsets");
static_assert(offsetof(tPoleInfo, connectionList) == 0x20, "tPoleInfo offsets");
static_assert(offsetof(tPoleInfo, poleA) == 0x38, "tPoleInfo offsets");
static_assert(offsetof(tPoleInfo, flagB) == 0x49, "tPoleInfo offsets");

// sPowerPoleForDirectionsFlag @ 0x100b24930 (cSC4PowerLineTool::sPowerPoleForDirectionsFlag): 16 exemplar ids,
// indexed by the 4-bit direction-connection mask (bit 0 = +x, bit 1 = the +x/+z diagonal, bit 2 = +z, bit 3 = the
// other diagonal). ReadPowerPoleInfos fills it from the "PowerPoles" ini section; until then every entry is 0.
// These are the exemplars CreatePowerPole asks cSC4NetworkTool::GetOccupantExemplar for.

// --- seams ---------------------------------------------------------------------------------------------------
// Engine / neighbour calls, one method per original call with the vtable slot or address in the comment. The
// occupant manager and the world caches are real interfaces in src/; the classes that are not recovered yet
// (the pole occupant, a city occupant as the tool sees it) are replaced by narrow interfaces.

// The wire between two poles (seam over cISC4PowerLineOccupant for the calls the tool makes). The real
// cSC4PowerLineOccupant is recovered in PowerLineOccupant.h but drags the whole occupant model in; the tool
// only asks it for its two ends.
class IPowerLineOccupant : public cIGZUnknown {
public:
    virtual ~IPowerLineOccupant() = default;
    // cSC4PowerLineOccupant [3] +0x18 GetConnectedPoles: one reference per returned pole.
    virtual void GetConnectedPoles(IPowerPoleOccupant*& first, IPowerPoleOccupant*& second, bool& connected) = 0;
    // [5] GetCell: the wire's cell.
    virtual void GetCell(std::uint32_t& x, std::uint32_t& z) { x = 0; z = 0; }
};

// One occupant of the city (seam over cISC4Occupant for the calls the tool makes).
class ICityOccupant : public cIGZUnknown {
public:
    virtual ~ICityOccupant() = default;
    virtual std::uint32_t GetType() = 0;                         // cISC4Occupant [7] +0x38
    virtual void GetPosition(float* xyz) = 0;                     // cISC4Occupant [8] +0x40
    virtual IPowerPoleOccupant* QueryPole() = 0;                  // QI 0xa8c86142 (one reference, or null)
    virtual IPowerLineOccupant* QueryPowerLine() = 0;             // QI 0xc98f3b24 (one reference, or null)
    virtual bool QueriesAs(std::uint32_t iid) = 0;                // QI iid, then Release; true when it answers
};

// cISC4PowerPoleOccupant as cSC4PowerLineTool calls it; cSC4PowerPoleOccupant (sc4/PowerPoleOccupant.h) implements
// it. The slot offsets are the 64-bit ones of the recovered call sites (i386: half).
class IPowerPoleOccupant : public cISC4PowerPoleOccupant {
public:
    virtual ~IPowerPoleOccupant() = default;
    virtual int GetNumberOfConnections() = 0;                                 // +0x20
    virtual IPowerPoleOccupant* GetConnectionOccupant(int index) = 0;         // +0x38 (not counted)
    virtual void GetCell(std::uint32_t& x, std::uint32_t& z) = 0;             // +0x40
    virtual void InitFromExemplar(cIGZUnknown* exemplar, std::uint32_t directionMask) = 0;  // +0x90 Init(exemplar, int)
    virtual void SetExemplar(cIGZUnknown* exemplar) = 0;                      // +0x130 SetDefaultExemplar
    virtual void SetPosition(const float* xyz) = 0;                           // +0xd0
    virtual void SetPreview(bool preview) = 0;                                // +0x70
    virtual void SetModelState(int state, bool immediate) = 0;                // +0xe0 SetHighlight
    virtual void OnInserted() = 0;                                            // +0x48 InsertAllPowerLineOccupants
    virtual std::uint32_t DirectionFlags() const = 0;                         // field +0x100
    virtual void SetDirectionFlags(std::uint32_t flags) = 0;                  // field +0x100
    // UpdateOnZoneChange's calls (i386 slots): the cells of a connection's wire (+0x14), of the wire to a pole (+0x5c),
    // dropping every connection (+0x18) and one connection (cSC4PowerPoleOccupant::RemoveConnection).
    virtual bool GetWireCells(int connection, std::vector<SC4PointU>& out) { (void)connection; out.clear(); return false; }
    virtual bool GetWireCellsTo(IPowerPoleOccupant* pole, std::vector<SC4PointU>& out) { (void)pole; out.clear(); return false; }
    virtual void BreakAllConnectionsAtDestination(bool quiet) { (void)quiet; }
    virtual void RemoveConnectionTo(IPowerPoleOccupant* pole, bool quiet) { (void)pole; (void)quiet; }
};

// Everything cSC4PowerLineTool reaches outside itself. scripts/tests/sc4_powerlinetool_test.cpp supplies a fake.
class IPowerLineToolHost {
public:
    virtual ~IPowerLineToolHost() = default;

    // SL::spOccupantManager. +0x148 GetOccupantsByStandardCityCells(filter = null, max = -1); every returned
    // entry holds one reference the caller releases.
    virtual void GetOccupantsInCellRect(std::vector<ICityOccupant*>& out, int x0, int z0, int x1, int z1) = 0;
    virtual void PositionToCityCell(float x, float z, int& cx, int& cz) = 0;   // +0x98 PositionToStandardCityCell
    virtual void InsertPoleOccupant(IPowerPoleOccupant* pole) = 0;             // +0xa0 InsertOccupant(occ, 0)
    virtual void RemovePoleOccupant(IPowerPoleOccupant* pole, bool shutdown) = 0;  // +0xa8 RemoveOccupant(occ,..)

    // SL::spZoneManager +0x60 GetZoneTypeAt(x, z).
    virtual int ZoneAt(int x, int z) = 0;
    // SL::spLotManager +0x28 GetLotAt(x, z, 0), reduced to the null test the tool does.
    virtual bool HasLotAt(int x, int z) = 0;

    // SL::spTerrain.
    virtual float TerrainCellSize() = 0;                                       // +0x30
    virtual bool TerrainAllowsNetwork(std::uint32_t x, std::uint32_t z) = 0;   // +0x110
    virtual void TerrainCornerHeights(int x, int z, float out[4]) = 0;         // +0x88
    virtual float TerrainHeightAt(float x, float z) = 0;                       // +0xf8

    // SL::spRenderer +0x1e0: the line-of-sight test from `start` along `direction`; writes the visible end.
    virtual bool RendererRayTest(const float* start, const float* direction, float* end) = 0;

    // SL::spRM +0x18 GetPropertyExemplar({0x6534284a, 0xe7e2c2db, 0xc911e35b}, 0xa52160f5), then QI 0x30.
    // Not owned; null when the exemplar or its property holder is missing.
    virtual const cISCPropertyHolder* TuningPropertyHolder() = 0;
    // SC4GetResourceAsFile({0x8a5971c5, 0, 0x8a5993b9}) opened as an ini file. The caller deletes it.
    virtual cRZIniFile* OpenPowerPoleIni() = 0;
    // SC4LoadString(group, instance, out) @ 0x100120e10.
    virtual bool LoadString(std::uint32_t group, std::uint32_t instance, cIGZString& out) = 0;

    // cSC4NetworkRoutines::Get0To3Direction @ 0x1002ee7ca: 0 = +x, 1 = same-sign diagonal, 2 = +z, 3 = the other.
    virtual int Get0To3Direction(int x1, int z1, int x2, int z2) = 0;

    // Calls on the cSC4NetworkTool base object that the base port does not define.
    virtual void DrawNetworkLine(SC4PointU& a, SC4PointU& b, bool preview, std::uint32_t networkType) = 0;  // [18]
    // the base tool's AttemptNeighborConnections / PlaceNeighborConnections (i386 0x00125a22 / 0x00123e36) on `tool`
    virtual bool AttemptNeighborConnections(cSC4NetworkTool& tool, bool first, std::uint32_t networkType) = 0;
    virtual void PlaceNeighborConnections(cSC4NetworkTool& tool) = 0;

    // cSC4PowerPoleOccupant.
    virtual IPowerPoleOccupant* CreatePoleOccupant() = 0;                      // new(0x1a0) + ctor 0x100320cf6
    virtual void ConnectPoles(IPowerPoleOccupant* from, IPowerPoleOccupant* to,
                              const std::vector<tLineInfo>* lineInfos) = 0;    // AddConnection 0x10031f4aa
    virtual void RemovePoleConnection(IPowerPoleOccupant* from, IPowerPoleOccupant* to, bool preview) = 0;  // 0x100320340
    virtual void DrawPoleConnection(IPowerPoleOccupant* from, IPowerPoleOccupant* to, bool preview) = 0;    // 0x1003204a4
};

void SetPowerLineToolHost(IPowerLineToolHost* host);
IPowerLineToolHost* GetPowerLineToolHost();

class cSC4PowerLineTool : public cSC4NetworkTool {
public:
    cSC4PowerLineTool();
    cSC4PowerLineTool(const cSC4PowerLineTool&) = delete;
    cSC4PowerLineTool& operator=(const cSC4PowerLineTool&) = delete;
    ~cSC4PowerLineTool() override;

    // --- cISC4NetworkTool overrides (recovered slots) ---
    bool Init() override;                                             // [3] 0x1002ecf2a
    bool Shutdown() override;                                         // [4] 0x1002ed32a
    void UpdateOnZoneChange(SC4RectU rect, std::uint32_t network) override;  // [10] 0x1002ec3c2 (i386 0x0022e388)
    std::uint32_t CursorId() override;                                // [43] 0x1002ea220
    std::uint32_t DirectionalCursorBase() override;                   // [44] 0x1002ea22c
    void GetFailureReasonString(std::uint32_t reason, cRZString& out) override;  // [54] 0x1002ed37c

    // --- original virtual slots the base port does not declare (see the header comment) ---
    void ClearCellPreviews();                                         // [16] 0x1002ed0c6
    int GetCostOfSolution();                                          // [20] 0x1002ed348
    bool DoNetworkSpecificPlacement(bool first, bool checkNeighbors);  // [55] 0x1002ecde4
    void CheckForReservedTiles(int& step);                            // [83] 0x1002ecaec
    void SetTerrainHilight(int step);                                 // [84] 0x1002ecc80

    // --- original methods ---
    static void AddConnection(tPoleInfo& a, tPoleInfo& b, int value);       // 0x1002e9c30
    static void GetLineSlope(int x1, int z1, int x2, int z2, int& dx, int& dz);  // i386 0x0022b798
    static void AddPowerPoleInfo(const cRZString& key, const cRZString& value, void* context);  // 0x1002e9cd2
    void ComputePolePosition(int x, int z, float* out);                     // 0x1002e9d28
    void CreatePowerPole(tPoleInfo& info);                                  // 0x1002e9e2c
    bool FindPoleOccupant(int x, int z, IPowerPoleOccupant*& out);          // 0x1002e9f9e
    void PlacePole(int cellIndex, bool connect, int value, const tIntersectionInfo& info);  // 0x1002ea238
    bool CellIsOccupiedByPowerLine(std::uint32_t x, std::uint32_t z, IPowerLineOccupant*& out);  // 0x1002ea38c
    bool IntersectionIsInList(const tIntersectionInfo& info);               // 0x1002ea4a2
    void ReleasePoleInfos();                                                // 0x1002ea4f2
    int GetPrimaryCell(int step, int anchorIndex);                          // 0x1002ea594
    bool PoleIsInList(IPowerPoleOccupant* pole);                            // 0x1002ea616
    void DeterminePolePositions();                                          // 0x1002ea67e
    std::uint8_t CellIsOKForPlacement(int x, int z);                        // 0x1002eb380
    void FindNewPolePosition(SC4CellRect rect, int startIndex, int endIndex, int step,
                             std::vector<SC4PointU>& cells, IPowerPoleOccupant* existing,
                             IPowerPoleOccupant* other);                    // 0x1002eb536
    void PlacePoles(bool resolve);                                          // 0x1002eb7de
    static bool StaticInit();                                               // 0x1002ed0a4
    static void ReadPowerPoleInfos();                                       // 0x1002ed208

    // --- members (see the layout comment at the top) ---
    bool powerLineInited_ = false;                          // +0x638
    std::vector<tPoleInfo> poleInfos_;                      // +0x640
    std::vector<std::vector<tLineInfo>> lineInfos_;         // +0x658
    int wireCellCount_ = 0;                                 // +0x670
    int poleCount_ = 0;                                     // +0x674
    std::list<IPowerPoleOccupant*> createdPoles_;           // +0x678
    std::list<IPowerPoleOccupant*> previewPoles_;           // +0x690
    std::list<tIntersectionInfo> connectionList_;            // +0x6a8
    std::vector<std::vector<int>> connectionGroups_;        // +0x6c0
    int poleSpacing_ = 0;                                   // +0x6d8 (Init: 0x14)
    float poleHeightOffset_ = 0.0f;                         // +0x6dc (Init: 7.0f)
    int poleCost_ = 0;                                      // +0x6e0
    float wireCost_ = 0.0f;                                 // +0x6e4

    // 0x100b24920: the static-init guard of the power-line tool (separate from cSC4NetworkTool::sbInited_).
    static bool sbInited_;
    // 0x100b24930: the 16 exemplar ids, indexed by the direction-connection mask (see above).
    static std::uint32_t sPowerPoleForDirectionsFlag[16];

private:
    friend struct PowerLineToolTestAccess;
};

}  // namespace sc4
